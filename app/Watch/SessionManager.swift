//
//  SessionManager.swift
//  watchOS — 항해 중 계기판처럼 동작하게 만드는 런타임 세션
//
//  왜 필요한가 (버튼 하나 살리자고 넣은 게 아니다):
//
//   1. Always On — 손목을 내려도 시계 화면으로 안 넘어가고 내 앱이 어둡게 남는다.
//      갱신 한도가 결정적으로 다르다:
//        · 워크아웃 세션 있음 → 초당 1회
//        · 없음               → 분당 1회  (사실상 멈춘 숫자)
//   2. 백그라운드 실행 — 화면이 꺼져도 앱이 suspend 되지 않아 BLE 가 유지된다.
//
//  설계 선택
//   · 버튼 없이 앱을 켜면 자동 시작한다. 계기판은 켜면 켜져 있는 게 맞다.
//   · HKLiveWorkoutBuilder(데이터 수집기)를 **쓰지 않는다.** 빌더가 건강 앱에
//     기록을 남기는데, 앱을 열 때마다 "세일링 운동"이 쌓이면 지저분하다.
//     Always On·백그라운드 권한은 세션 자체가 주므로 빌더 없이도 목적을 달성한다.
//

import Foundation
import HealthKit
import WatchKit

@MainActor
final class SessionManager: NSObject, ObservableObject {

    @Published private(set) var isRunning = false
    @Published private(set) var startedAt: Date?
    @Published var errorMessage: String?

    private let healthStore = HKHealthStore()
    private var session: HKWorkoutSession?

    var elapsedText: String {
        guard let startedAt else { return "--:--" }
        let t = Int(Date().timeIntervalSince(startedAt))
        return String(format: "%02d:%02d", t / 60, t % 60)
    }

    // MARK: 시작 / 종료

    /// 앱 진입 시 한 번 호출. 이미 돌고 있으면 아무것도 하지 않는다.
    func startIfNeeded() async {
        guard !isRunning else { return }
        guard HKHealthStore.isHealthDataAvailable() else {
            errorMessage = "이 기기에서 HealthKit 을 쓸 수 없습니다."
            return
        }

        // 세션 생성에는 workoutType 공유 권한이 필요하다. 첫 실행에 한 번 팝업이 뜬다.
        do {
            try await healthStore.requestAuthorization(
                toShare: [HKQuantityType.workoutType()], read: [])
        } catch {
            errorMessage = "건강 권한 요청 실패: \(error.localizedDescription)"
            return
        }

        let config = HKWorkoutConfiguration()
        config.activityType = .sailing
        config.locationType = .outdoor

        do {
            let s = try HKWorkoutSession(healthStore: healthStore, configuration: config)
            s.delegate = self
            s.startActivity(with: Date())
            session = s
            startedAt = Date()
            isRunning = true
            errorMessage = nil
        } catch {
            errorMessage = "세션 시작 실패: \(error.localizedDescription)"
            cleanup()
        }
    }

    /// 설정에서 수동 종료. 종료하면 Always On 갱신이 분당 1회로 떨어지고
    /// 손목을 내리면 BLE 가 끊긴다.
    func stop() {
        session?.end()
        cleanup()
    }

    // MARK: 물 잠금 — 세일링용

    /// 켜면 화면 터치가 막히고, 디지털 크라운을 돌려야 풀린다.
    /// 해제 시 스피커로 물을 빼낸다.
    func enableWaterLock() {
        WKInterfaceDevice.current().enableWaterLock()
    }

    var isWaterLocked: Bool {
        WKInterfaceDevice.current().isWaterLockEnabled
    }

    private func cleanup() {
        session = nil
        startedAt = nil
        isRunning = false
    }
}

// MARK: - HKWorkoutSessionDelegate

extension SessionManager: HKWorkoutSessionDelegate {

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didChangeTo toState: HKWorkoutSessionState,
                                    from fromState: HKWorkoutSessionState,
                                    date: Date) {
        Task { @MainActor in
            switch toState {
            case .running:
                self.isRunning = true
            case .ended, .stopped:
                self.cleanup()
            default:
                break
            }
        }
    }

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didFailWithError error: Error) {
        Task { @MainActor in
            self.errorMessage = "세션 오류: \(error.localizedDescription)"
            self.cleanup()
        }
    }
}
