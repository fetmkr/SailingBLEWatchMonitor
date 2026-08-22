//
//  LiveView.swift
//  항해 중 보는 화면. 속도 · HDG · 힐 세 개만 크게, 스크롤 없이 한 화면.
//  요트 계기처럼 뱃머리 방향을 크게 둔다. COG 는 상세 화면에 있다.
//
//  진단값(수신율·uptime·RSSI·로그)은 설정 탭으로 뺐다.
//  10 Hz 로 갱신되는 숫자에 전환 애니메이션을 걸면 글자가 계속 꿈틀거려서
//  오히려 읽기 어렵다. 값만 바로 바꾼다.
//  연결이 끊기면 마지막 값을 회색으로 유지하고 "재연결 중…" 을 띄운다.
//

import SwiftUI
import UIKit

struct LiveView: View {
    @EnvironmentObject private var ble: BLEManager
    @Environment(\.scenePhase) private var scenePhase

    private var dimmed: Bool { !ble.isLive }

    /// 값이 없는가. 없으면 sogText 가 대시를 돌려주므로 숫자를 칠할 일은 없지만,
    /// 라벨 쪽을 흐리게 두는 데 쓴다.
    private var noFix: Bool { ble.sample?.sogKnots == nil }
    private var noHeel: Bool { ble.sample?.heelDegrees == nil }

    /// 숫자에만 칠하는 색. 라벨과 단위는 건드리지 않는다.
    private func numberStyle(_ warn: Bool) -> AnyShapeStyle {
        if dimmed { return AnyShapeStyle(.secondary) }
        return warn ? AnyShapeStyle(Color.sailWarn) : AnyShapeStyle(.primary)
    }

    // 항해 중에는 화면이 저절로 꺼지면 안 된다.
    // iOS 에는 watchOS 의 Always On 같은 게 없으므로 자동 잠금을 막는 것이 전부다.
    // 값을 실제로 받고 있을 때만 막는다 — 연결도 안 된 화면을 켜둘 이유는 없다.
    private func updateIdleTimer(active: Bool) {
        UIApplication.shared.isIdleTimerDisabled = active && ble.isLive
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {

                statusLine
                    .padding(.top, 4)

                if !ble.hasPinnedModule {
                    Spacer()
                    NoModuleBanner()
                    Spacer()
                } else {
                    Spacer(minLength: 8)

                    // 속도 — 지어낸 값이면 숫자를 빨갛게 칠한다.
                    bigValue(ble.sample?.sogText ?? "—.—",
                             unit: "knots",
                             size: 96,
                             warn: false)

                    Spacer(minLength: 8)

                    Divider().padding(.horizontal, 24)

                    Spacer(minLength: 8)

                    // 방위 · 힐
                    //
                    // 이 자리는 항상 HDG 다. 값이 없으면 대시를 보여주지,
                    // 다른 값으로 바꿔 채우지 않는다. COG 는 상세 화면에 있다.
                    HStack(alignment: .top, spacing: 0) {
                        smallerValue("HDG",
                                     ble.sample?.headingText ?? "—",
                                     sub: ble.sample?.headingDegrees.map { compassPoint($0) } ?? " ",
                                     warn: false)
                        Divider().frame(height: 90)
                        smallerValue("HEEL",
                                     ble.sample.map { $0.heelText } ?? "—",
                                     sub: ble.sample.map { $0.heelSideLabel } ?? " ",
                                     warn: false)
                    }

                    Spacer(minLength: 8)
                }
            }
            .navigationTitle(ble.pinnedModule?.displayName ?? "모듈 미선택")
            .navigationBarTitleDisplayMode(.inline)
        }
        .onAppear { updateIdleTimer(active: true) }
        .onDisappear { updateIdleTimer(active: false) }
        .onChange(of: ble.isLive) { _, _ in
            updateIdleTimer(active: scenePhase == .active)
        }
        .onChange(of: scenePhase) { _, phase in
            // 백그라운드로 나가면 반드시 되돌린다. 안 그러면 다른 앱에서도 화면이 안 꺼진다.
            updateIdleTimer(active: phase == .active)
        }
    }

    // MARK: 조각

    private func bigValue(_ text: String, unit: String, size: CGFloat,
                          warn: Bool = false) -> some View {
        VStack(spacing: 0) {
            Text(text)
                .font(.system(size: size, weight: .semibold))
                .monospacedDigit()
                .minimumScaleFactor(0.4)
                .lineLimit(1)
                .foregroundStyle(numberStyle(warn))
            Text(unit)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    private func smallerValue(_ title: String, _ value: String, sub: String,
                              warn: Bool = false) -> some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(size: 54, weight: .semibold))
                .monospacedDigit()
                .minimumScaleFactor(0.4)
                .lineLimit(1)
                .foregroundStyle(numberStyle(warn))
            Text(sub)
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity)
    }

    // 색은 "무엇을 받고 있나" 기준. 연결 상태보다 이게 더 중요하다.
    private var statusColor: Color {
        switch ble.source {
        case .connection:  return .green
        case .advertising: return .orange
        case .none:        return ble.state == .idle ? .gray : .blue
        }
    }

    private var statusText: String {
        switch ble.source {
        case .connection:
            return ble.source.displayText(rateHz: ble.packetRateHz)
        case .advertising:
            // 연결은 끊겼지만 광고로 값을 계속 받고 있는 상태
            let base = ble.source.displayText(rateHz: ble.packetRateHz)
            return ble.disconnectedFor > 0.5
                ? base + String(format: " · 재연결 중 %.0f초", ble.disconnectedFor)
                : base
        case .none:
            if ble.state == .reconnecting && ble.disconnectedFor > 0.5 {
                return String(format: "재연결 중… %.0f초", ble.disconnectedFor)
            }
            return ble.state.displayText
        }
    }

    private var statusLine: some View {
        HStack(spacing: 6) {
            Circle().fill(statusColor).frame(width: 8, height: 8)
            Text(statusText)
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

// MARK: - 모듈 미선택 안내

struct NoModuleBanner: View {
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "sailboat")
                .font(.largeTitle)
                .foregroundStyle(.orange)
            Text("연결할 모듈을 고르세요")
                .font(.headline)
            Text("설정 탭에서 내 모듈을 선택하면\n자동으로 연결됩니다.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding()
    }
}

// MARK: - 로그 시트 (설정 탭에서 연다)

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
