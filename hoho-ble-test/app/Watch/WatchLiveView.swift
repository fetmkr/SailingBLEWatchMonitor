//
//  WatchLiveView.swift
//  세로 2페이지 구성 (watchOS 표준 verticalPage 스타일)
//    1페이지 — 속도 큰 숫자 + COG/HEEL + 연결상태 + 훈련 시작/종료
//    2페이지 — 진단값 (수신율 Hz, 배터리, RSSI, uptime)
//

import SwiftUI

struct WatchLiveView: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var workout: WorkoutManager

    var body: some View {
        TabView {
            MainPage()
            DiagnosticsPage()
        }
        .tabViewStyle(.verticalPage)
    }
}

// MARK: - 1페이지

private struct MainPage: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var workout: WorkoutManager

    private var dimmed: Bool { !ble.isLive }

    var body: some View {
        VStack(spacing: 4) {

            StatusLine()

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
        .padding(.horizontal, 2)
    }
}

// MARK: - 2페이지

private struct DiagnosticsPage: View {
    @EnvironmentObject private var ble: BLEManager

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text("진단")
                    .font(.headline)

                row("수신율", String(format: "%.2f Hz", ble.packetRateHz), hint: "기대 4.00")
                row("상태", ble.state.rawValue, hint: nil)
                row("배터리", ble.sample.map { "\($0.batteryPercent)%" } ?? "—", hint: nil)
                row("RSSI", ble.rssi.map { "\($0) dBm" } ?? "—", hint: nil)
                row("uptime",
                    ble.sample?.uptimeSeconds.map { String(format: "%.0f초", $0) } ?? "—",
                    hint: "ESP32")
                row("마지막 수신",
                    ble.lastPacketAt.map { String(format: "%.1f초 전", -$0.timeIntervalSinceNow) } ?? "—",
                    hint: nil)

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
            .padding(.horizontal, 2)
        }
    }

    private func row(_ label: String, _ value: String, hint: String?) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Spacer(minLength: 4)
            VStack(alignment: .trailing, spacing: 0) {
                Text(value)
                    .font(.system(size: 12).monospacedDigit().weight(.semibold))
                if let hint {
                    Text(hint)
                        .font(.system(size: 8))
                        .foregroundStyle(.tertiary)
                }
            }
        }
    }
}

// MARK: - 조각들

private struct StatusLine: View {
    @EnvironmentObject private var ble: BLEManager

    private var color: Color {
        switch ble.state {
        case .connected:                 return ble.isLive ? .green : .yellow
        case .reconnecting, .connecting: return .orange
        case .scanning:                  return .blue
        case .idle:                      return .gray
        }
    }

    private var text: String {
        if ble.state == .reconnecting && ble.disconnectedFor > 0.5 {
            return String(format: "재연결 중… %.0f초", ble.disconnectedFor)
        }
        return ble.state.displayText
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
