//
//  WorkoutIntents.swift
//  watchOS — Apple Watch Ultra 액션 버튼의 "운동" 프리셋에 우리 앱을 올린다
//
//  액션 버튼에는 두 가지 경로가 있다.
//
//   ① 바로가기(Shortcut)  — 아무 App Intent 나 노출하면 목록에 뜬다.
//                          Shared/AppShortcuts.swift 가 담당
//   ② 운동(Workout)      — Strava 처럼 "운동 → 앱" 목록에 뜨는 것. ★ 이 파일
//
//  ②의 조건 (실제로 확인한 것):
//   · Info.plist 의 WKBackgroundModes 에 `workout-processing` 이 있어야 한다
//   · StartWorkoutIntent 를 구현하고
//   · workoutStyle 을 **@Parameter 로 선언**해야 한다.
//     안 그러면 설정 앱에 "앱 열기" 동작만 뜨고 운동 목록에는 안 나온다
//
//  참고: github.com/KhaosT/WatchActionButtonExample
//

import AppIntents
import Foundation

// MARK: - 운동 종류

/// 액션 버튼 설정에서 고를 수 있는 항목. 지금은 세일링 한 종류다.
enum SailingStyle: String, AppEnum, CustomStringConvertible {
    case sailing

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "세일링"
    static let caseDisplayRepresentations: [SailingStyle: DisplayRepresentation] = [
        .sailing: "세일링"
    ]

    var description: String { "세일링" }
}

// MARK: - 액션 버튼 → 운동 → Sailing Monitor

struct BeginSailingIntent: StartWorkoutIntent {

    static let title: LocalizedStringResource = "세일링 시작"

    /// ★ @Parameter 가 반드시 있어야 운동 목록에 노출된다.
    @Parameter(title: "종류")
    var workoutStyle: SailingStyle

    /// 실행하면 앱을 띄운다. 세션은 앱 진입 시 SessionManager 가 자동으로 건다.
    static let openAppWhenRun = true

    /// 설정 화면에 제안으로 보여줄 항목들
    static var suggestedWorkouts: [BeginSailingIntent] {
        [BeginSailingIntent(style: .sailing)]
    }

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(workoutStyle)")
    }

    init() {
        self.workoutStyle = .sailing
    }

    func perform() async throws -> some IntentResult {
        // 앱이 뜨면 SessionManager.startIfNeeded() 가 세션을 시작한다.
        // 액션 버튼을 다시 누르면 아래 PauseSailingIntent 가 불린다.
        .result(actionButtonIntent: PauseSailingIntent())
    }
}

// MARK: - 세션 중 액션 버튼 재입력

struct PauseSailingIntent: PauseWorkoutIntent {
    static let title: LocalizedStringResource = "일시정지"

    func perform() async throws -> some IntentResult {
        .result(actionButtonIntent: ResumeSailingIntent())
    }
}

struct ResumeSailingIntent: ResumeWorkoutIntent {
    static let title: LocalizedStringResource = "재개"

    func perform() async throws -> some IntentResult {
        .result(actionButtonIntent: PauseSailingIntent())
    }
}
