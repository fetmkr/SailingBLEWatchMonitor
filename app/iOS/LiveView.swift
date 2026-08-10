//
//  LiveView.swift
//  연결해서 4 Hz notify 를 받아 실시간 표시하는 탭.
//
//  연결이 끊기면 마지막 값을 회색으로 유지하고 "재연결 중…" 배지를 띄운다.
//  다시 붙으면 그 순간 라이브 값으로 복귀한다.
//

import SwiftUI

struct LiveView: View {
    @EnvironmentObject private var ble: BLEManager
    @State private var showLog = false

    private var dimmed: Bool { !ble.isLive }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    if !ble.hasPinnedModule {
                        NoModuleBanner()
                    }

                    StatusBadge(state: ble.state, isLive: ble.isLive,
                                disconnectedFor: ble.disconnectedFor)

                    SpeedBlock(sample: ble.sample, dimmed: dimmed)

                    HStack(spacing: 12) {
                        MetricCard(title: "COG",
                                   value: ble.sample.map { $0.cogText } ?? "—",
                                   caption: ble.sample.map { compassPoint($0.cogDegrees) } ?? " ",
                                   systemImage: "safari",
                                   dimmed: dimmed)

                        MetricCard(title: "HEEL",
                                   value: ble.sample.map { $0.heelText } ?? "—",
                                   caption: ble.sample.map { $0.heelSideLabel } ?? " ",
                                   systemImage: "sailboat",
                                   dimmed: dimmed)
                    }

                    HStack(spacing: 12) {
                        MetricCard(title: "배터리",
                                   value: ble.sample.map { "\($0.batteryPercent)%" } ?? "—",
                                   caption: batteryCaption,
                                   systemImage: batteryIcon,
                                   dimmed: dimmed)

                        MetricCard(title: "RSSI",
                                   value: ble.rssi.map { "\($0)" } ?? "—",
                                   caption: "dBm",
                                   systemImage: "antenna.radiowaves.left.and.right",
                                   dimmed: dimmed)
                    }

                    DiagnosticsPanel(ble: ble)

                    Button {
                        showLog = true
                    } label: {
                        Label("디버그 로그 (\(ble.log.count))", systemImage: "text.alignleft")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)

                    Button(role: .none) {
                        ble.forceReconnect()
                    } label: {
                        Label("강제 재연결", systemImage: "arrow.clockwise")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                }
                .padding()
            }
            .navigationTitle(ble.pinnedModule?.displayName ?? "모듈 미선택")
            .navigationBarTitleDisplayMode(.inline)
            .sheet(isPresented: $showLog) { LogSheet(ble: ble) }
        }
    }

    private var batteryIcon: String {
        guard let pct = ble.sample?.batteryPercent else { return "battery.0percent" }
        switch pct {
        case 76...:  return "battery.100percent"
        case 51...75: return "battery.75percent"
        case 26...50: return "battery.50percent"
        case 1...25:  return "battery.25percent"
        default:      return "battery.0percent"
        }
    }

    private var batteryCaption: String {
        guard let s = ble.sample, let up = s.uptimeSeconds else { return " " }
        return String(format: "uptime %.0f분", up / 60)
    }
}

// MARK: - 모듈 미선택 안내

struct NoModuleBanner: View {
    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "sailboat")
                .font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                Text("연결할 모듈을 고르세요")
                    .font(.subheadline.weight(.semibold))
                Text("설정 탭에서 내 모듈을 선택하면 자동으로 연결됩니다.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(14)
        .background(.orange.opacity(0.15), in: RoundedRectangle(cornerRadius: 14))
    }
}

// MARK: - 연결 상태 배지

struct StatusBadge: View {
    let state: BLEConnectionState
    let isLive: Bool
    let disconnectedFor: TimeInterval

    private var color: Color {
        switch state {
        case .connected:    return isLive ? .green : .yellow
        case .connecting:   return .orange
        case .reconnecting: return .orange
        case .scanning:     return .blue
        case .choosing:     return .orange
        case .idle:         return .gray
        }
    }

    private var text: String {
        if state == .connected && !isLive { return "연결됨 (데이터 대기)" }
        if state == .reconnecting && disconnectedFor > 0.5 {
            return String(format: "재연결 중… %.0f초", disconnectedFor)
        }
        return state.displayText
    }

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 10, height: 10)
                .overlay(
                    Circle().stroke(color.opacity(0.35), lineWidth: 6)
                        .scaleEffect(state == .connected && isLive ? 1.0 : 1.4)
                        .animation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true),
                                   value: state)
                )
            Text(text)
                .font(.subheadline.weight(.medium))
            Spacer()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(color.opacity(0.12), in: Capsule())
    }
}

// MARK: - 속도 블록

struct SpeedBlock: View {
    let sample: TelemetrySample?
    let dimmed: Bool

    var body: some View {
        VStack(spacing: 0) {
            Text(sample?.sogText ?? "—.—")
                .font(.system(size: 96, weight: .bold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.5)
                .lineLimit(1)
                .contentTransition(.numericText())
                .animation(.linear(duration: 0.2), value: sample?.sogKnots)

            Text("knots")
                .font(.title3.weight(.semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
                .tracking(2)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 24)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 20))
        .foregroundStyle(dimmed ? AnyShapeStyle(.secondary) : AnyShapeStyle(.primary))
        .opacity(dimmed ? 0.55 : 1.0)
        .animation(.easeInOut(duration: 0.25), value: dimmed)
    }
}

// MARK: - 지표 카드

struct MetricCard: View {
    let title: String
    let value: String
    let caption: String
    let systemImage: String
    let dimmed: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(title, systemImage: systemImage)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)

            Text(value)
                .font(.system(size: 32, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.6)
                .lineLimit(1)
                .contentTransition(.numericText())

            Text(caption)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 16))
        .opacity(dimmed ? 0.55 : 1.0)
        .animation(.easeInOut(duration: 0.25), value: dimmed)
    }
}

// MARK: - 진단 패널 (실측값 확인용)

struct DiagnosticsPanel: View {
    @ObservedObject var ble: BLEManager

    var body: some View {
        VStack(spacing: 8) {
            row("수신율", String(format: "%.2f Hz", ble.packetRateHz),
                hint: "기대 4.00 Hz")
            Divider()
            row("상태 머신", ble.state.rawValue, hint: nil)
            Divider()
            row("마지막 수신",
                ble.lastPacketAt.map { String(format: "%.1f초 전", -$0.timeIntervalSinceNow) } ?? "—",
                hint: nil)
            Divider()
            row("uptime",
                ble.sample?.uptimeSeconds.map { String(format: "%.1f초", $0) } ?? "—",
                hint: "ESP32 기준")
        }
        .padding(14)
        .background(.quaternary.opacity(0.25), in: RoundedRectangle(cornerRadius: 16))
    }

    private func row(_ label: String, _ value: String, hint: String?) -> some View {
        HStack {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Spacer()
            VStack(alignment: .trailing, spacing: 1) {
                Text(value).font(.caption.monospaced().weight(.semibold))
                if let hint {
                    Text(hint).font(.caption2).foregroundStyle(.tertiary)
                }
            }
        }
    }
}

// MARK: - 로그 시트

struct LogSheet: View {
    @ObservedObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(ble.log.reversed()) { line in
                VStack(alignment: .leading, spacing: 2) {
                    Text(line.timeText)
                        .font(.caption2.monospaced())
                        .foregroundStyle(.tertiary)
                    Text(line.text)
                        .font(.caption.monospaced())
                }
            }
            .listStyle(.plain)
            .navigationTitle("BLE 로그")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("지우기") { ble.clearLog() }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("닫기") { dismiss() }
                }
            }
        }
    }
}
