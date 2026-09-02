//
//  WatchApp.swift
//  Sailing Monitor 텔레메트리 수신기 (watchOS, 독립 실행)
//

import SwiftUI

@main
struct SailingMonitorWatchApp: App {
    @StateObject private var ble = BLEManager.shared
    @StateObject private var session = SessionManager()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            WatchLiveView()
                .environmentObject(ble)
                .environmentObject(session)
                .onAppear {
                    ble.start()
                    // 계기판은 켜면 켜져 있어야 한다. 버튼 없이 자동 시작.
                    Task { await session.startIfNeeded() }
                }
                .onChange(of: scenePhase) { _, newPhase in
                    // 앞에 있는지 뒤로 갔는지를 시각과 함께 찍는다.
                    // 시계 화면으로 넘어가는 순간이 여기 background 로 찍힌다.
                    //   xcrun devicectl device process launch --console <bundle id>
                    let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
                    let word: String
                    switch newPhase {
                    case .active:     word = "앞 (active)"
                    case .inactive:   word = "가려짐 (inactive)"
                    case .background: word = "뒤 (background)"
                    @unknown default: word = "?"
                    }
                    print("[PHASE] \(f.string(from: Date())) \(word)")

                    // 손목을 들어 화면이 다시 켜졌을 때(active) 연결이 끊겨 있으면 즉시 복구.
                    if newPhase == .active { ble.appBecameActive() }
                }
        }
    }
}
