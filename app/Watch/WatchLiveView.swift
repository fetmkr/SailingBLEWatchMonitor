//
//  WatchLiveView.swift
//  세로 2페이지 (watchOS verticalPage)
//    1페이지 — 항해 중 보는 화면. 속도 · COG · 힐 세 개만, 최대한 크게.
//    2페이지 — 설정: 모듈 선택 / 진단
//
//  1페이지에는 숫자 전환 애니메이션을 쓰지 않는다.
//  10 Hz 로 갱신되는 값에 애니메이션을 걸면 글자가 계속 꿈틀거려 못 읽는다.
//

import SwiftUI

struct WatchLiveView: View {
    var body: some View {
        TabView {
            MainPage()
            SettingsPage()
        }
        .tabViewStyle(.verticalPage)
    }
}

// MARK: - 1페이지 · 항해 화면

private struct MainPage: View {
    @EnvironmentObject private var ble: BLEManager

    private var dimmed: Bool { !ble.isLive }

    var body: some View {
        VStack(spacing: 0) {

            statusLine

            if !ble.hasPinnedModule {
                Spacer()
                VStack(spacing: 6) {
                    Image(systemName: "sailboat")
                        .font(.title)
                        .foregroundStyle(.orange)
                    Text("모듈을 고르세요")
                        .font(.headline)
                    Text("아래로 스와이프")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            } else {
                Spacer(minLength: 0)

                // 속도 — 화면에서 제일 큰 것
                Text(ble.sample?.sogText ?? "—.—")
                    .font(.system(size: 68, weight: .semibold, design: .rounded))
                    .monospacedDigit()
                    .minimumScaleFactor(0.4)
                    .lineLimit(1)
                Text("kn")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer(minLength: 0)

                // COG · 힐
                HStack(spacing: 0) {
                    bigPair(ble.sample.map { $0.cogText } ?? "—", "COG")
                    bigPair(ble.sample.map { $0.heelText } ?? "—", "HEEL")
                }

                Spacer(minLength: 0)
            }
        }
        .opacity(dimmed ? 0.45 : 1)
        .padding(.horizontal, 2)
    }

    private func bigPair(_ value: String, _ label: String) -> some View {
        VStack(spacing: -2) {
            Text(value)
                .font(.system(size: 34, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.4)
                .lineLimit(1)
            Text(label)
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }

    // 상단 한 줄 — 점 + 모듈 이름
    private var statusLine: some View {
        HStack(spacing: 4) {
            Circle().fill(statusColor).frame(width: 6, height: 6)
            Text(statusLabel)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
    }

    private var statusColor: Color {
        switch ble.source {
        case .connection:  return .green
        case .advertising: return .orange
        case .none:        return ble.state == .idle ? .gray : .blue
        }
    }

    private var statusLabel: String {
        switch ble.source {
        case .connection:
            return ble.pinnedModule?.displayName ?? "연결"
        case .advertising:
            return "광고"
        case .none:
            if ble.state == .reconnecting { return "재연결 중…" }
            return ble.state.displayText
        }
    }
}

// MARK: - 2페이지 · 설정

private struct SettingsPage: View {
    @EnvironmentObject private var ble: BLEManager
    @State private var showUnpinConfirm = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {

                if let pin = ble.pinnedModule {
                    // ── 내 모듈
                    HStack(spacing: 6) {
                        Image(systemName: "sailboat.fill").foregroundStyle(.tint)
                        Text(pin.displayName)
                            .font(.headline)
                            .lineLimit(1)
                        Spacer()
                    }

                    Button(role: .destructive) {
                        showUnpinConfirm = true
                    } label: {
                        Text("다른 모듈")
                            .font(.footnote)
                            .frame(maxWidth: .infinity)
                    }
                    .confirmationDialog("고정을 해제할까요?", isPresented: $showUnpinConfirm) {
                        Button("해제", role: .destructive) { ble.unpinModule() }
                        Button("취소", role: .cancel) {}
                    }

                    Divider()
                    diagnostics

                } else {
                    Text("모듈 고르기").font(.headline)

                    if !ble.bluetoothPoweredOn {
                        Text("블루투스를 켜주세요")
                            .font(.footnote).foregroundStyle(.secondary)
                    } else if ble.discoveredModules.isEmpty {
                        HStack(spacing: 6) {
                            ProgressView()
                            Text("찾는 중…").font(.footnote).foregroundStyle(.secondary)
                        }
                    } else {
                        ForEach(ble.discoveredModules) { module in
                            Button {
                                ble.selectModule(module)
                            } label: {
                                HStack {
                                    Text(module.displayName)
                                        .font(.body)
                                        .lineLimit(1)
                                    Spacer()
                                    Text("\(module.rssi)")
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            .padding(.horizontal, 4)
        }
        .onAppear { ble.refreshDiscovery() }
    }

    private var diagnostics: some View {
        VStack(alignment: .leading, spacing: 4) {
            row("경로", ble.source.displayText)
            row("연결", ble.connLastAt == nil ? "—" : String(format: "%.1f Hz", ble.connRateHz))
            row("광고", ble.advLastAt == nil ? "—" : String(format: "%.1f Hz", ble.advRateHz))
            row("RSSI", ble.rssi.map { "\($0)" } ?? "—")
            row("배터리", ble.sample.map { "\($0.batteryPercent)%" } ?? "—")

            Button {
                ble.forceReconnect()
            } label: {
                Text("강제 재연결").font(.caption2).frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .padding(.top, 2)
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.caption2).foregroundStyle(.secondary)
            Spacer(minLength: 4)
            Text(value).font(.caption.monospacedDigit())
        }
    }
}
