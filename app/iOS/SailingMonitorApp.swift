//
//  SailingMonitorApp.swift
//  Sailing Monitor 텔레메트리 수신기 (iOS)
//

import SwiftUI

@main
struct SailingMonitorApp: App {
    @StateObject private var ble = BLEManager.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(ble)
                .onAppear { ble.start() }
                .onChange(of: scenePhase) { _, newPhase in
                    // 앱이 다시 활성화됐는데 연결이 안 되어 있으면 즉시 재시도.
                    if newPhase == .active { ble.appBecameActive() }
                }
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var ble: BLEManager

    var body: some View {
        TabView {
            LiveView()
                .tabItem { Label("라이브", systemImage: "speedometer") }

            // 라이브 탭은 속도·COG·힐 세 개만 크게 보여준다.
            // 9축과 GPS 상태처럼 자세한 값은 바로 옆 탭에서 본다.
            SensorDetailView(ble: ble)
                .tabItem {
                    Label("상세", systemImage: "gauge.with.dots.needle.bottom.50percent")
                }

            SettingsView()
                .tabItem { Label("설정", systemImage: "gearshape") }
                // 아직 모듈을 안 골랐으면 설정 탭에 배지를 띄워 유도한다.
                .badge(ble.hasPinnedModule ? 0 : 1)
        }
    }
}
