//
//  WorkoutManager.swift
//  watchOS — HKWorkoutSession(.sailing) 으로 백그라운드 실행 권한을 확보한다.
//
//  이게 핵심인 이유:
//  워크아웃 세션이 돌고 있으면 손목을 내려 화면이 꺼져도 앱이 suspend 되지 않아
//  BLE 연결과 notify 수신이 그대로 유지된다. 세션 없이는 몇 초 만에 끊긴다.
//
//  필요 설정: HealthKit capability, Info.plist 의 NSHealthShare/UpdateUsageDescription,
//            WKBackgroundModes = [workout-processing]
//

import Foundation
import HealthKit

@MainActor
final class WorkoutManager: NSObject, ObservableObject {

    @Published private(set) var isActive = false
    @Published private(set) var isBusy = false
    @Published private(set) var elapsed: TimeInterval = 0
    @Published private(set) var heartRate: Double?
    @Published var errorMessage: String?

    private let healthStore = HKHealthStore()
    private var session: HKWorkoutSession?
    private var builder: HKLiveWorkoutBuilder?
    private var startedAt: Date?
    private var ticker: Timer?

    var elapsedText: String {
        let total = Int(elapsed)
        return String(format: "%02d:%02d", total / 60, total % 60)
    }

    // MARK: 권한

    func requestAuthorization() async {
        guard HKHealthStore.isHealthDataAvailable() else {
            errorMessage = "이 기기에서 HealthKit 을 쓸 수 없습니다."
            return
        }

        let share: Set<HKSampleType> = [HKQuantityType.workoutType()]
        let read: Set<HKObjectType> = [
            HKQuantityType.workoutType(),
            HKQuantityType(.heartRate),
            HKQuantityType(.activeEnergyBurned)
        ]

        do {
            try await healthStore.requestAuthorization(toShare: share, read: read)
        } catch {
            errorMessage = "HealthKit 권한 요청 실패: \(error.localizedDescription)"
        }
    }

    // MARK: 시작 / 종료

    func start() async {
        guard !isActive, !isBusy else { return }
        isBusy = true
        defer { isBusy = false }

        await requestAuthorization()

        let config = HKWorkoutConfiguration()
        config.activityType = .sailing
        config.locationType = .outdoor

        do {
            let s = try HKWorkoutSession(healthStore: healthStore, configuration: config)
            let b = s.associatedWorkoutBuilder()
            b.dataSource = HKLiveWorkoutDataSource(healthStore: healthStore,
                                                   workoutConfiguration: config)
            s.delegate = self
            b.delegate = self

            let now = Date()
            s.startActivity(with: now)
            try await b.beginCollection(at: now)

            session = s
            builder = b
            startedAt = now
            isActive = true
            errorMessage = nil
            startTicker()
        } catch {
            errorMessage = "워크아웃 시작 실패: \(error.localizedDescription)"
            cleanup()
        }
    }

    func end() async {
        guard isActive, !isBusy, let s = session, let b = builder else { return }
        isBusy = true
        defer { isBusy = false }

        s.end()
        do {
            try await b.endCollection(at: Date())
            _ = try await b.finishWorkout()
        } catch {
            errorMessage = "워크아웃 종료 처리 실패: \(error.localizedDescription)"
        }
        cleanup()
    }

    // MARK: 내부

    private func startTicker() {
        ticker?.invalidate()
        let t = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, let start = self.startedAt else { return }
                self.elapsed = Date().timeIntervalSince(start)
            }
        }
        RunLoop.main.add(t, forMode: .common)
        ticker = t
    }

    private func cleanup() {
        ticker?.invalidate()
        ticker = nil
        session = nil
        builder = nil
        startedAt = nil
        elapsed = 0
        heartRate = nil
        isActive = false
    }
}

// MARK: - HKWorkoutSessionDelegate

extension WorkoutManager: HKWorkoutSessionDelegate {

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didChangeTo toState: HKWorkoutSessionState,
                                    from fromState: HKWorkoutSessionState,
                                    date: Date) {
        Task { @MainActor in
            switch toState {
            case .running:
                self.isActive = true
            case .ended, .stopped:
                self.isActive = false
            default:
                break
            }
        }
    }

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didFailWithError error: Error) {
        Task { @MainActor in
            self.errorMessage = "워크아웃 오류: \(error.localizedDescription)"
            self.cleanup()
        }
    }
}

// MARK: - HKLiveWorkoutBuilderDelegate

extension WorkoutManager: HKLiveWorkoutBuilderDelegate {

    nonisolated func workoutBuilderDidCollectEvent(_ workoutBuilder: HKLiveWorkoutBuilder) {
        // 이벤트는 이 테스트에서 쓰지 않는다.
    }

    nonisolated func workoutBuilder(_ workoutBuilder: HKLiveWorkoutBuilder,
                                    didCollectDataOf collectedTypes: Set<HKSampleType>) {
        // 심박수는 "세션이 실제로 살아있다"는 것을 눈으로 확인하는 용도.
        let hrType = HKQuantityType(.heartRate)
        guard collectedTypes.contains(hrType),
              let stats = workoutBuilder.statistics(for: hrType),
              let value = stats.mostRecentQuantity()?
                .doubleValue(for: .count().unitDivided(by: .minute())) else { return }

        Task { @MainActor in
            self.heartRate = value
        }
    }
}
