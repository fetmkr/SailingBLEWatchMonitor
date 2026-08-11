//
//  AppShortcuts.swift
//  App Intents — iOS / watchOS 공용
//
//  왜 필요한가:
//  Apple Watch Ultra 의 **액션 버튼**은 앱을 직접 지정하는 게 아니라
//  Shortcuts 를 실행한다. Shortcuts 목록에 뜨려면 앱이 App Intent 를 노출해야 한다.
//  Intent 가 하나도 없으면 설정 → 액션 버튼 → 바로가기 목록에 앱이 아예 안 보인다.
//  (앱스토어 등록과는 무관하다. 개발 설치본도 메타데이터만 있으면 잡힌다)
//
//  아이폰 15 Pro 이상의 액션 버튼과 Siri, 단축어 앱에도 같은 Intent 가 함께 노출된다.
//
//  주의: 액션 버튼 **길게 누르기는 못 쓴다.** watchOS 가 긴급 SOS 로 예약해 뒀다.
//

import AppIntents
import Foundation

// MARK: - 앱 열기

struct OpenSpeedIntent: AppIntent {
    static let title: LocalizedStringResource = "속도계 열기"
    static let description = IntentDescription("연결된 모듈의 속도·침로·힐 각을 봅니다.")

    // 실행하면 앱을 띄운다.
    // watchOS 26 부터는 supportedModes 로 대체됐지만, 배포 타깃이 watchOS 10 이라
    // 전 버전에서 동작하는 openAppWhenRun 을 쓴다. (기본 구현이 있어 컴파일된다)
    static let openAppWhenRun = true

    func perform() async throws -> some IntentResult {
        .result()
    }
}

// MARK: - Shortcuts 등록

struct SailingMonitorShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: OpenSpeedIntent(),
            phrases: [
                "\(.applicationName) 열기",
                "\(.applicationName) 속도",
                "Open \(.applicationName)",
            ],
            shortTitle: "속도계",
            systemImageName: "speedometer"
        )
    }
}
