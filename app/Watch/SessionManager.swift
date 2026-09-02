//
//  SessionManager.swift
//  watchOS — 항해 중 계기판처럼 동작하게 만드는 런타임 세션
//
//  왜 필요한가 (버튼 하나 살리자고 넣은 게 아니다):
//
//   애플 DTS 답변이 규칙을 그대로 적어 놨다.
//   developer.apple.com/forums/thread/775151
//
//     · 앱이 앞에 남아 있는 시간은 워치 설정 → 일반 → 시계로 돌아가기 가 정한다.
//       **기본값이 2분이다.**
//     · 백그라운드 세션(운동 · 위치 · 오디오 재생 · 녹음) 중 하나를 돌리지 않으면
//       2분쯤 뒤에 시계 화면으로 돌아간다.
//
//   그래서 운동 세션을 건다. 그게 유일한 열쇠다. 세션이 안 걸려 있으면
//   무슨 짓을 해도 2분에 시계로 돌아간다.
//
//   덤으로 Always On 갱신 한도도 달라진다.
//     · 세션 있음 → 초당 1회      · 없음 → 분당 1회 (사실상 멈춘 숫자)
//   화면이 꺼져도 앱이 suspend 되지 않아 BLE 도 유지된다.
//
//  설계 선택
//   · 버튼 없이 앱을 켜면 자동 시작한다. 계기판은 켜면 켜져 있는 게 맞다.
//   · HKLiveWorkoutBuilder(데이터 수집기)를 **쓰지 않는다.** 빌더가 건강 앱에
//     기록을 남기는데, 앱을 열 때마다 "세일링 운동"이 쌓이면 지저분하다.
//     Always On·백그라운드 권한은 세션 자체가 주므로 빌더 없이도 목적을 달성한다.
//
//  ★ 상태를 지어내지 않는다
//   전에는 startActivity 를 부른 직후 isRunning = true 로 적어 놨다. 그런데
//   HKWorkoutSession.h 를 보면 startActivityWithDate: 는 **비동기**로 시작하고,
//   초기화 함수는 권한이 없어도 에러를 내지 않는다. 즉 권한이 막혀 있으면
//   화면에는 "항해 중" 이라고 떠 있는데 실제로는 아무것도 안 도는 상태가 된다.
//   지금은 대리자(delegate)가 알려 준 state 만 믿는다.
//

import Foundation
import HealthKit
import WatchKit

@MainActor
final class SessionManager: NSObject, ObservableObject {

    /// 칩이 알려 준 진짜 상태. 화면은 이 값만 보고 그린다.
    @Published private(set) var state: HKWorkoutSessionState = .notStarted
    @Published private(set) var startedAt: Date?
    @Published var errorMessage: String?

    /// 무슨 일이 언제 있었는지 남긴다. 앱이 죽었다 살아나도 남아 있어야
    /// "언제 끊겼나" 를 볼 수 있으므로 UserDefaults 에 적는다.
    @Published private(set) var trail: [String] = SessionManager.loadTrail()

    private let healthStore = HKHealthStore()
    private var session: HKWorkoutSession?

    /// 사용자가 직접 끝낸 것인지, 시스템이 끊은 것인지 가른다.
    private var stoppedByUser = false
    /// 시스템이 끊었을 때 다시 걸어 보는 횟수. 권한이 막혀 있으면 무한 반복이 된다.
    private var restarts = 0
    private static let kRestartMax = 3

    var isRunning: Bool { state == .running }

    var elapsedText: String {
        guard let startedAt else { return "--:--" }
        let t = Int(Date().timeIntervalSince(startedAt))
        return String(format: "%02d:%02d", t / 60, t % 60)
    }

    /// 세션 상태를 한 낱말로.
    var stateText: String { Self.word(state) }

    /// 운동 기록 쓰기 권한이 실제로 열려 있는지. 여기가 막히면 세션이 안 돈다.
    var authText: String {
        switch healthStore.authorizationStatus(for: HKQuantityType.workoutType()) {
        case .sharingAuthorized: return "허용"
        case .sharingDenied:     return "거부됨"
        case .notDetermined:     return "안 물어봄"
        @unknown default:        return "?"
        }
    }

    // MARK: 시작 / 종료

    /// 앱 진입 시 한 번 호출. 이미 돌고 있으면 아무것도 하지 않는다.
    func startIfNeeded() async {
        guard session == nil else { return }
        print("[SESSION] startIfNeeded — 권한 \(authText), HealthKit \(HKHealthStore.isHealthDataAvailable())")
        guard HKHealthStore.isHealthDataAvailable() else {
            errorMessage = "이 기기에서 HealthKit 을 쓸 수 없습니다."
            note("HealthKit 없음")
            return
        }

        // 세션 생성에는 workoutType 공유 권한이 필요하다. 첫 실행에 한 번 팝업이 뜬다.
        do {
            try await healthStore.requestAuthorization(
                toShare: [HKQuantityType.workoutType()], read: [])
        } catch {
            errorMessage = "건강 권한 요청 실패: \(error.localizedDescription)"
            note("권한 요청 실패")
            print("[SESSION] requestAuthorization 던짐 — \(error)")
            return
        }
        print("[SESSION] requestAuthorization 돌아옴 — 권한 \(authText)")

        // 권한을 물어본 뒤 실제로 열렸는지 확인한다. 막혀 있으면 세션이
        // 조용히 안 돌기 때문에 여기서 미리 말해 준다.
        if healthStore.authorizationStatus(for: HKQuantityType.workoutType()) != .sharingAuthorized {
            errorMessage = "운동 기록 쓰기 권한이 없습니다. 워치 설정 → 건강 → 앱 에서 켜주세요."
            note("권한 \(authText)")
        }

        let config = HKWorkoutConfiguration()
        config.activityType = .sailing
        config.locationType = .outdoor

        do {
            let s = try HKWorkoutSession(healthStore: healthStore, configuration: config)
            s.delegate = self
            s.startActivity(with: Date())
            session = s
            stoppedByUser = false
            note("시작 요청")
            print("[SESSION] startActivity 부름 — 지금 state \(Self.word(s.state))")
        } catch {
            errorMessage = "세션 시작 실패: \(error.localizedDescription)"
            note("시작 실패")
            cleanup()
        }
    }

    /// 설정에서 수동 종료. 종료하면 Always On 갱신이 분당 1회로 떨어지고
    /// 2분 뒤에 시계 화면으로 돌아간다.
    func stop() {
        stoppedByUser = true
        note("사용자 종료")
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
        state = .notStarted
    }

    // MARK: 기록

    private static let kTrailKey = "sessionTrail"
    private static let kTrailMax = 10

    private static func loadTrail() -> [String] {
        UserDefaults.standard.stringArray(forKey: kTrailKey) ?? []
    }

    private func note(_ text: String) {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        var t = trail
        t.append("\(f.string(from: Date())) \(text)")
        if t.count > Self.kTrailMax { t.removeFirst(t.count - Self.kTrailMax) }
        trail = t
        UserDefaults.standard.set(t, forKey: Self.kTrailKey)
        // 콘솔로도 뱉는다. 워치는 화면을 못 보므로 이 줄이 유일한 창이다.
        //   xcrun devicectl device process launch --console <bundle id>
        print("[SESSION] \(t.last ?? text)")
    }

    func clearTrail() {
        trail = []
        UserDefaults.standard.removeObject(forKey: Self.kTrailKey)
    }

    fileprivate static func word(_ s: HKWorkoutSessionState) -> String {
        switch s {
        case .notStarted: return "없음"
        case .prepared:   return "준비됨"
        case .running:    return "도는 중"
        case .paused:     return "멈춤"
        case .stopped:    return "정지"
        case .ended:      return "끝남"
        @unknown default: return "?"
        }
    }
}

// MARK: - HKWorkoutSessionDelegate

extension SessionManager: HKWorkoutSessionDelegate {

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didChangeTo toState: HKWorkoutSessionState,
                                    from fromState: HKWorkoutSessionState,
                                    date: Date) {
        Task { @MainActor in
            self.state = toState
            self.note("\(Self.word(fromState)) → \(Self.word(toState))")

            switch toState {
            case .running:
                self.startedAt = workoutSession.startDate ?? date
                self.errorMessage = nil
                self.restarts = 0
            case .ended, .stopped:
                let wasUser = self.stoppedByUser
                self.cleanup()
                // 시스템이 끊었으면 다시 건다. 안 그러면 2분 뒤 시계로 돌아간다.
                if !wasUser, self.restarts < Self.kRestartMax {
                    self.restarts += 1
                    self.note("다시 걸기 \(self.restarts)/\(Self.kRestartMax)")
                    await self.startIfNeeded()
                }
            default:
                break
            }
        }
    }

    nonisolated func workoutSession(_ workoutSession: HKWorkoutSession,
                                    didFailWithError error: Error) {
        Task { @MainActor in
            self.errorMessage = "세션 오류: \(error.localizedDescription)"
            self.note("오류 \(( error as NSError).code)")
            self.cleanup()
        }
    }
}
