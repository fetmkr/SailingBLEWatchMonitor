//
//  WatchLiveView.swift
//  세로 2페이지 구성 (watchOS 표준 verticalPage 스타일)
//    1페이지 — 속도 / COG / 연결상태 / 훈련 시작·종료
//    2페이지 — 설정: 모듈 선택·해제 + 진단
//

import SwiftUI

struct WatchLiveView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var workout: WorkoutManager

    var body: some View {
        TabView {
            MainPage()
            SettingsPage()
        }
        .tabViewStyle(.verticalPage)
    }
}

// MARK: - 1페이지 · 라이브

private struct MainPage: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var workout: WorkoutManager

    private var dimmed: Bool { !ble.isLive }

    var body: some View {
        VStack(spacing: 4) {

            StatusLine()

            if !ble.hasPinnedModule {
                // 모듈을 아직 안 골랐으면 값 대신 안내를 띄운다.
                VStack(spacing: 6) {
                    Image(systemName: "sailboat")
                        .font(.title2)
                        .foregroundStyle(.orange)
                    Text("연결할 모듈을\n골라주세요")
                        .font(.footnote)
                        .multilineTextAlignment(.center)
                    Text("아래로 스와이프 → 설정")
                        .font(.system(size: 9))
                        .foregroundStyle(.secondary)
                }
                .frame(maxHeight: .infinity)
            } else {
                liveContent
            }
        }
        .padding(.horizontal, 2)
    }

    @ViewBuilder
    private var liveContent: some View {
            // 속도
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(ble.sample?.sogText ?? "—.—")
                    .font(.system(size: 46, weight: .bold, design: .rounded))
                    .monospacedDigit()
                    .minimumScaleFactor(0.5)
                    .lineLimit(1)
                    .contentTransition(.numericText())
                    .animation(.linear(duration: 0.2), value: ble.sample?.sogKnots)
                Text("kn")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(.secondary)
            }
            .opacity(dimmed ? 0.5 : 1)
            .animation(.easeInOut(duration: 0.25), value: dimmed)

            // COG / HEEL
            HStack(spacing: 6) {
                MiniCard(title: "COG",
                         value: ble.sample.map { $0.cogText } ?? "—",
                         caption: ble.sample.map { compassPoint($0.cogDegrees) } ?? " ")
                MiniCard(title: "HEEL",
                         value: ble.sample.map { $0.heelText } ?? "—",
                         caption: ble.sample.map { $0.heelSideLabel } ?? " ")
            }
            .opacity(dimmed ? 0.5 : 1)

            // 워크아웃
            if workout.isActive {
                HStack(spacing: 4) {
                    Image(systemName: "figure.sailing").foregroundStyle(.green)
                    Text(workout.elapsedText)
                        .font(.system(size: 11).monospacedDigit().weight(.semibold))
                    if let hr = workout.heartRate {
                        Text("· \(Int(hr))bpm")
                            .font(.system(size: 10))
                            .foregroundStyle(.secondary)
                    }
                }
            }

            Button {
                Task {
                    if workout.isActive { await workout.end() } else { await workout.start() }
                }
            } label: {
                Label(workout.isActive ? "훈련 종료" : "훈련 시작",
                      systemImage: workout.isActive ? "stop.circle.fill" : "play.circle.fill")
                    .font(.system(size: 13, weight: .semibold))
                    .frame(maxWidth: .infinity)
            }
            .tint(workout.isActive ? .red : .green)
            .disabled(workout.isBusy)

            if let err = workout.errorMessage {
                Text(err)
                    .font(.system(size: 9))
                    .foregroundStyle(.red)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
            }
    }
}

// MARK: - 2페이지 · 설정

private struct SettingsPage: View {
    @EnvironmentObject private var ble: BLEManager
    @State private var showUnpinConfirm = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {

                if let pin = ble.pinnedModule {
                    Text("내 모듈")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(.secondary)

                    HStack(spacing: 6) {
                        Image(systemName: "sailboat.fill").foregroundStyle(.tint)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(pin.displayName)
                                .font(.system(size: 15, weight: .semibold))
                            Text("id \(pin.moduleID)")
                                .font(.system(size: 9).monospaced())
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                    .padding(6)
                    .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))

                    Button(role: .destructive) {
                        showUnpinConfirm = true
                    } label: {
                        Label("다른 모듈 선택", systemImage: "arrow.triangle.2.circlepath")
                            .font(.system(size: 12))
                            .frame(maxWidth: .infinity)
                    }
                    .confirmationDialog("고정을 해제할까요?", isPresented: $showUnpinConfirm) {
                        Button("해제", role: .destructive) { ble.unpinModule() }
                        Button("취소", role: .cancel) {}
                    }

                    Divider().padding(.vertical, 2)
                    diagnostics

                } else {
                    Text("모듈 고르기")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(.secondary)

                    if !ble.bluetoothPoweredOn {
                        Text("블루투스를 켜주세요")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
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
                                WatchModuleRow(module: module)
                            }
                            .buttonStyle(.plain)
                            .padding(.vertical, 1)
                        }
                        Text("가까운 순")
                            .font(.system(size: 9))
                            .foregroundStyle(.tertiary)
                    }
                }
            }
            .padding(.horizontal, 2)
        }
        .onAppear { ble.refreshDiscovery() }
    }

    private var diagnostics: some View {
        VStack(alignment: .leading, spacing: 3) {
            row("상태", ble.state.displayText)
            row("경로", ble.source.displayText)
            row("연결", ble.connLastAt == nil ? "—" : String(format: "%.1f Hz", ble.connRateHz))
            row("광고", ble.advLastAt == nil ? "—" : String(format: "%.1f Hz", ble.advRateHz))
            row("배터리", ble.sample.map { "\($0.batteryPercent)%" } ?? "—")
            row("RSSI", ble.rssi.map { "\($0) dBm" } ?? "—")
            row("uptime", ble.sample?.uptimeSeconds.map { String(format: "%.0f초", $0) } ?? "—")

            Button {
                ble.forceReconnect()
            } label: {
                Label("강제 재연결", systemImage: "arrow.clockwise")
                    .font(.system(size: 12))
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .padding(.top, 4)
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.system(size: 11)).foregroundStyle(.secondary)
            Spacer(minLength: 4)
            Text(value).font(.system(size: 12).monospacedDigit().weight(.semibold))
        }
    }
}

// MARK: - 조각들

private struct WatchModuleRow: View {
    let module: DiscoveredModule

    var body: some View {
        HStack(spacing: 6) {
            HStack(alignment: .bottom, spacing: 1.5) {
                ForEach(0..<4, id: \.self) { i in
                    RoundedRectangle(cornerRadius: 1)
                        .fill(i < module.signalBars ? AnyShapeStyle(.tint)
                                                    : AnyShapeStyle(.quaternary))
                        .frame(width: 2.5, height: 4 + CGFloat(i) * 3)
                }
            }
            .frame(width: 16, alignment: .leading)

            VStack(alignment: .leading, spacing: 0) {
                Text(module.displayName)
                    .font(.system(size: 13, weight: .medium))
                    .lineLimit(1)
                Text("\(module.rssi)dBm" + (module.sample.map { " · \($0.sogText)kn" } ?? ""))
                    .font(.system(size: 9).monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            Spacer(minLength: 2)
            Image(systemName: "chevron.right")
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 5)
        .padding(.horizontal, 6)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
        .opacity(module.isStale ? 0.4 : 1)
    }
}

private struct StatusLine: View {
    @EnvironmentObject private var ble: BLEManager

    private var color: Color {
        switch ble.source {
        case .connection:  return .green
        case .advertising: return .orange
        case .none:        return ble.state == .idle ? .gray : .blue
        }
    }

    private var text: String {
        switch ble.source {
        case .connection:
            return ble.pinnedModule?.displayName ?? "연결"
        case .advertising:
            return ble.source.displayText(rateHz: ble.packetRateHz)
        case .none:
            if ble.state == .reconnecting && ble.disconnectedFor > 0.5 {
                return String(format: "재연결 중… %.0f초", ble.disconnectedFor)
            }
            return ble.state.displayText
        }
    }

    var body: some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(text)
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity)
    }
}

private struct MiniCard: View {
    let title: String
    let value: String
    let caption: String

    var body: some View {
        VStack(spacing: 0) {
            Text(title)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(size: 16, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.6)
                .lineLimit(1)
                .contentTransition(.numericText())
            Text(caption)
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 4)
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
    }
}
