//
//  HohoBLEApp.swift
//  HOHO-01 텔레메트리 수신기 (iOS)
//

import SwiftUI

@main
struct HohoBLEApp: App {
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

            ScannerView()
                .tabItem { Label("스캐너", systemImage: "dot.radiowaves.left.and.right") }

            SettingsView()
                .tabItem { Label("설정", systemImage: "gearshape") }
                // 아직 모듈을 안 골랐으면 설정 탭에 배지를 띄워 유도한다.
                .badge(ble.hasPinnedModule ? 0 : 1)
        }
    }
}
