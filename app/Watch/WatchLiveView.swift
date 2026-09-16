//
//  WatchLiveView.swift
//  좌우 3페이지 (watchOS page)
//
//  ★ 좌우로 넘기는 이유
//    애플 운동 앱과 같은 조작이라 익숙하다는 것도 있지만, 실질적인 이유는
//    Digital Crown 이다. 세로 페이지(verticalPage)로 두면 크라운이 페이지를
//    넘겨 버려서 페이지 안을 스크롤할 때 손가락만 써야 한다.
//    좌우로 두면 크라운이 페이지 안 스크롤에 쓰인다.
//    1페이지 — 항해 중 보는 화면. 속도 · HDG · 힐 세 개만, 최대한 크게.
//               요트 계기처럼 뱃머리 방향을 크게 둔다. COG 는 2페이지.
//    2페이지 — 항해 상세: SOG·HDG / COG·DIF / HEEL·PITCH
//    3페이지 — 설정: 보드 / 기록 / 항해 세션 / 나침반 보정 / 센서 상태

//
//  1페이지에는 숫자 전환 애니메이션을 쓰지 않는다.
//  10 Hz 로 갱신되는 값에 애니메이션을 걸면 글자가 계속 꿈틀거려 못 읽는다.
//
//  Always On (손목을 내려 화면이 어두워진 상태)
//   · isLuminanceReduced 가 true 가 된다.
//   · 이때는 갱신 한도가 초당 1회다(세션이 있을 때. 없으면 분당 1회).
//   · 그래서 어두워지면 속도만 남기고 나머지를 지운다. 정보를 줄이는 게 아니라
//     초당 1회로 갱신되는 값만 남기는 것이다. 애니메이션도 전부 끈다.
//

import SwiftUI
import WatchKit

struct WatchLiveView: View {

    /// 지금 보고 있는 페이지. 물 잠금을 걸기 전에 1페이지로 옮기려고 붙잡아 둔다.
    ///
    /// 물 잠금은 화면 터치를 **전부** 막는다. 그래서 3페이지에서 그냥 걸면
    /// 설정 화면에 갇힌다. 옆으로 못 넘기고, 크라운을 길게 누르면 잠금이 풀려서
    /// 그것도 길이 아니다. 걸기 직전에 항해 화면으로 옮겨 놓아야 한다.
    @State private var page = 0

    @EnvironmentObject private var ble: BLEManager

    /// 보드의 SD 기록이 저절로 멈추면 세 페이지 배경을 빨갛게 (flags bit5).
    /// TabView 페이지 배경은 containerBackground(_:for: .tabView) 로만 바뀐다
    /// [확인: watchOS SDK SwiftUI.swiftinterface, watchOS 10.0].
    private var pageBackground: Color {
        ble.sample?.recordingFailed == true ? Color.red.opacity(0.6) : Color.black
    }

    var body: some View {
        TabView(selection: $page) {
            MainPage().tag(0)
                .containerBackground(pageBackground, for: .tabView)
            DebugPage().tag(1)
                .containerBackground(pageBackground, for: .tabView)
            SettingsPage(page: $page).tag(2)
                .containerBackground(pageBackground, for: .tabView)
        }
        .tabViewStyle(.page)
    }
}

// MARK: - 2페이지 · 항해 상세
//
// 여섯 값만 3행 2열로 크게 보여준다. 9축 원시값은 3페이지의 `상태`로 옮겼다.

private struct DebugPage: View {
    @EnvironmentObject private var ble: BLEManager
    @Environment(\.isLuminanceReduced) private var isDim

    private var extra: TelemetryExtra? { ble.sample?.extra }

    /// HDG는 기본 자북(M) 그대로 보여주고, DIF를 구하는 이 순간에만 진북으로 바꾼다.
    /// 조류가 섞인 값이라 실제 leeway로 확정하지 않고 DIF라고 표시한다.
    private var courseDeltaText: String {
        guard let e = extra,
              let heading = e.headingDegrees,
              let course = ble.sample?.cogDegrees else { return "—" }
        let headingTrue: Double
        if e.headingIsTrue {
            headingTrue = heading // 이전 펌웨어와 호환
        } else if let declination = e.magneticDeclinationDegrees {
            headingTrue = (heading + declination + 360.0).truncatingRemainder(dividingBy: 360.0)
        } else {
            return "—"
        }
        let delta = (course - headingTrue + 540.0).truncatingRemainder(dividingBy: 360.0) - 180.0
        return String(format: "%+.0f°", delta)
    }

    var body: some View {
        VStack(spacing: 4) {
            BoardStatusHeader(isDim: isDim)

            VStack(spacing: 5) {
                metricRow(
                    "SOG", ble.sample?.sogKnots.map { String(format: "%.2f kn", $0) } ?? "—",
                    extra?.headingIsTrue == true ? "HDG T" : "HDG M",
                    extra?.headingDegrees.map { String(format: "%.0f°", $0) } ?? "—"
                )
                metricRow(
                    "COG T", ble.sample?.cogDegrees.map { String(format: "%.0f°", $0) } ?? "—",
                    "DIF", courseDeltaText
                )
                metricRow(
                    "HEEL", ble.sample?.heelDegrees.map { String(format: "%+d°", $0) } ?? "—",
                    "PITCH", extra.map { String(format: "%+.1f°", $0.pitchDegrees) } ?? "—"
                )
            }
            .opacity(ble.isLive ? 1 : 0.45)
        }
        .padding(.horizontal, 2)
        .padding(.bottom, 3)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func metricRow(_ leftLabel: String, _ leftValue: String,
                           _ rightLabel: String, _ rightValue: String) -> some View {
        HStack(spacing: 6) {
            metric(leftLabel, leftValue)
            metric(rightLabel, rightValue)
        }
        .frame(maxHeight: .infinity)
    }

    private func metric(_ label: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 4) {
            Text(label)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
                .lineLimit(1)
            Spacer(minLength: 1)
            Text(value)
                .font(.system(size: 20, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.55)
                .lineLimit(1)
        }
        .padding(.horizontal, 7)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(Color.white.opacity(0.075))
        )
    }
}

// MARK: - 1페이지 · 항해 화면

private struct MainPage: View {
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var session: SessionManager
    /// 손목을 내려 화면이 어두워진 상태 (Always On)
    @Environment(\.isLuminanceReduced) private var isDim

    /// 어두워진 횟수. 손목을 내린 화면은 스크린샷으로 못 찍기 때문에,
    /// Always On 이 실제로 걸리는지 이 값으로 확인한다. (설정 페이지에 표시)
    @AppStorage("dimCount") private var dimCount = 0

    private var stale: Bool { !ble.isLive }



    var body: some View {
        VStack(spacing: 0) {

            BoardStatusHeader(isDim: isDim)

            Group {
                // 잠겨 있으면 푸는 법을 적어 둔다. 워치는 물방울 아이콘만 그려 주고
                // 화면을 눌러도 아무 안내가 안 뜬다. 눌러도 안 먹는 이유를 여기서 말해 준다.
                if session.waterLocked {
                    HStack(spacing: 3) {
                        Image(systemName: "drop.fill")
                        Text("크라운 길게 눌러 풀기")
                    }
                    .font(.system(size: isDim ? 11 : 10, weight: .medium))
                    .foregroundStyle(.tint)
                    .padding(.vertical, 1)
                }

                if !ble.hasPinnedModule {
                    Spacer()
                    VStack(spacing: 6) {
                        Image(systemName: "sailboat")
                            .font(.title)
                            .foregroundStyle(.orange)
                        Text("모듈을 고르세요")
                            .font(.headline)
                        Text("옆으로 스와이프")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                } else {
                    Spacer(minLength: 0)

                    // 속도 — 화면에서 제일 큰 것.
                    // 지어낸 값이면 숫자를 빨갛게 칠한다. 단위와 라벨은 그대로 둔다.
                    Text(ble.sample?.sogText ?? "—.—")
                        .font(.system(size: isDim ? 84 : 68, weight: .semibold, design: .rounded))
                        .monospacedDigit()
                        .minimumScaleFactor(0.4)
                        .lineLimit(1)
                        .foregroundStyle(Color.primary)
                    Text("kn")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Spacer(minLength: 0)

                    // Always On 에서는 속도만 남긴다.
                    if !isDim {
                        // 이 자리는 항상 HDG 다. 값이 없으면 대시를 보여주지,
                        // 다른 값으로 바꿔 채우지 않는다. COG 는 2페이지에 있다.
                        HStack(spacing: 0) {
                            bigPair(ble.sample?.headingText ?? "—", ble.sample?.headingLabel ?? "HDG")
                            bigPair(ble.sample.map { $0.heelText } ?? "—", "HEEL")
                        }
                        Spacer(minLength: 0)
                    }
                }
            }
            .opacity(stale ? 0.45 : 1)
        }
        .padding(.horizontal, 2)
        .onChange(of: isDim) { _, nowDim in
            if nowDim { dimCount += 1 }
            let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
            print("[DIM] \(f.string(from: Date())) \(nowDim ? "어두워짐" : "밝아짐") (\(dimCount)회)")
        }
    }

    private func bigPair(_ value: String, _ label: String,
                         warn: Bool = false) -> some View {
        VStack(spacing: -2) {
            Text(value)
                .font(.system(size: 34, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.4)
                .lineLimit(1)
                .foregroundStyle(warn ? Color.sailWarn : Color.primary)
            Text(label)
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }

}

/// 1·2페이지가 공유하는 보드·기록 상태 헤더.
/// 주황=통신 없음, 녹색=수신 중, 빨강 점멸=SD 기록 중.
private struct BoardStatusHeader: View {
    @EnvironmentObject private var ble: BLEManager
    let isDim: Bool

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 5) {
                RecordingDot(communicating: ble.isLive,
                             recording: ble.isLive && ble.sample?.recording == true,
                             isDim: isDim)
                Text(recordingLabel)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(recordingColor)
                    .lineLimit(1)
            }

            if !isDim, let boardName = ble.pinnedModule?.displayName {
                Text(boardName)
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
            }
        }
        .frame(maxWidth: .infinity, alignment: .center)
    }

    private var recordingLabel: String {
        if !ble.isLive { return "통신 없음" }
        if ble.sample?.recordingFailed == true && ble.sample?.recording != true { return "REC 끊김" }
        if ble.sample?.recording == true { return "REC" }
        return "연결됨"
    }

    private var recordingColor: Color {
        if !ble.isLive { return .orange }
        if ble.sample?.recording == true || ble.sample?.recordingFailed == true { return .red }
        return .green
    }
}

/// 통신은 고정색, 실제 SD 기록만 빨간색으로 깜박인다.
/// Always On에서는 애니메이션이 보장되지 않으므로 기록 중이면 빨간 점을 계속 켜 둔다.
private struct RecordingDot: View {
    let communicating: Bool
    let recording: Bool
    let isDim: Bool

    var body: some View {
        if recording && !isDim {
            TimelineView(.periodic(from: .now, by: 0.5)) { timeline in
                let on = Int(timeline.date.timeIntervalSinceReferenceDate * 2) % 2 == 0
                dot(color: .red, opacity: on ? 1.0 : 0.15)
            }
        } else {
            dot(color: recording ? .red : (communicating ? .green : .orange), opacity: 1.0)
        }
    }

    private func dot(color: Color, opacity: Double) -> some View {
        Circle()
            .fill(color)
            .frame(width: 12, height: 12)
            .opacity(opacity)
    }
}

// MARK: - 3페이지 · 설정

private struct SettingsPage: View {
    @Binding var page: Int
    @EnvironmentObject private var ble: BLEManager
    @EnvironmentObject private var session: SessionManager
    @State private var showUnpinConfirm = false
    @State private var showCalibration = false
    @State private var showStatus = false
    @State private var boatDraft = 0

    var body: some View {
        ScrollView {
            VStack(spacing: 8) {
                card { boardSection }
                card { recSection }
                card { loraSection }
                card { waterLockSection }
                card {
                    VStack(alignment: .leading, spacing: 0) {
                        Button {
                            withAnimation(.easeInOut(duration: 0.2)) {
                                showCalibration.toggle()
                            }
                        } label: {
                            HStack {
                                Label("나침반 보정", systemImage: "circle.dotted")
                                    .font(.caption.weight(.semibold))
                                Spacer()
                                Image(systemName: showCalibration ? "chevron.up" : "chevron.down")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)

                        if showCalibration {
                            magcalControls.padding(.top, 6)
                        }
                    }
                }
                card {
                    VStack(alignment: .leading, spacing: 0) {
                        Button {
                            withAnimation(.easeInOut(duration: 0.2)) {
                                showStatus.toggle()
                            }
                        } label: {
                            HStack {
                                Label("상태", systemImage: "waveform.path.ecg")
                                    .font(.caption.weight(.semibold))
                                Spacer()
                                Image(systemName: showStatus ? "chevron.up" : "chevron.down")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)

                        if showStatus {
                            IMUStatusView(extra: ble.sample?.extra)
                                .padding(.top, 7)
                        }
                    }
                }
            }
            .padding(.horizontal, 4)
            .padding(.bottom, 8)
        }
        .scrollBounceBehavior(.basedOnSize)
        .onAppear {
            ble.refreshDiscovery()
            if let id = ble.sample?.extra?.boatID, (0...32).contains(id) { boatDraft = id }
        }
        .onChange(of: ble.sample?.extra?.boatID) { _, id in
            if let id, (0...32).contains(id) { boatDraft = id }
        }
    }

    @ViewBuilder
    private var boardSection: some View {
        if ble.pinnedModule != nil {
            VStack(spacing: 6) {
                HStack(spacing: 7) {
                    Circle()
                        .fill(ble.isLive ? Color.green : Color.orange)
                        .frame(width: 10, height: 10)
                    Text(ble.isLive ? "보드 연결됨" : "보드 연결 대기")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(ble.isLive ? Color.primary : Color.orange)
                    Spacer()
                }

                Button { showUnpinConfirm = true } label: {
                    Text("보드 변경")
                        .font(.caption2)
                        .frame(maxWidth: .infinity, minHeight: 30)
                }
                .buttonStyle(.bordered)
            }
            .confirmationDialog("다른 보드를 선택할까요?", isPresented: $showUnpinConfirm) {
                Button("선택 해제", role: .destructive) { ble.unpinModule() }
                Button("취소", role: .cancel) {}
            }
        } else {
            VStack(alignment: .leading, spacing: 6) {
                Label("보드 선택", systemImage: "sailboat.fill")
                    .font(.caption.weight(.semibold))
                if !ble.bluetoothPoweredOn {
                    Text("블루투스를 켜주세요")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                } else if ble.discoveredModules.isEmpty {
                    HStack(spacing: 6) {
                        ProgressView()
                        Text("주변 보드를 찾는 중…").font(.footnote).foregroundStyle(.secondary)
                    }
                } else {
                    ForEach(ble.discoveredModules) { module in
                        Button {
                            ble.selectModule(module)
                        } label: {
                            HStack {
                                Text(module.displayName).lineLimit(1)
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
    }

    private var recSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                RecordingDot(communicating: ble.isLive,
                             recording: ble.isLive && ble.sample?.recording == true,
                             isDim: false)
                Text(recStatusText)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(recStatusColor)
                Spacer()
            }

            Button {
                WKInterfaceDevice.current().play(.click)
                ble.sendControl(ble.sample?.recording == true ? "rec off" : "rec on")
            } label: {
                Label(ble.sample?.recording == true ? "REC 정지" : "REC 시작",
                      systemImage: ble.sample?.recording == true ? "stop.fill" : "record.circle")
                    .font(.system(size: 15, weight: .semibold))
                    .frame(maxWidth: .infinity, minHeight: 42)
            }
            .buttonStyle(.borderedProminent)
            .tint(ble.sample?.recording == true ? .red : .green)

            if !ble.controlReady {
                Text("연결되면 명령을 자동으로 보냅니다")
                    .font(.system(size: 9))
                    .foregroundStyle(.orange)
            }
            if let reply = recentRecReply {
                Text(reply)
                    .font(.system(size: 9))
                    .foregroundStyle(reply.hasPrefix("실패") ? .red : .secondary)
            }
        }
    }

    private var recentRecReply: String? {
        guard ble.lastControlCommand.hasPrefix("rec"),
              let at = ble.controlReplyAt,
              Date().timeIntervalSince(at) < 10,
              !ble.controlReply.isEmpty else { return nil }
        if ble.controlReply.hasPrefix("ok rec on") { return "기록을 시작했습니다" }
        if ble.controlReply.hasPrefix("ok rec off") { return "기록을 멈추는 중입니다" }
        if ble.controlReply.hasPrefix("err rec") { return "실패: \(ble.controlReply)" }
        return ble.controlReply
    }

    private var recStatusText: String {
        if !ble.isLive { return "통신 없음" }
        if ble.sample?.recordingFailed == true && ble.sample?.recording != true { return "REC가 끊겼습니다" }
        if ble.sample?.recording == true { return "SD 기록 중" }
        return "SD 기록 대기"
    }

    private var recStatusColor: Color {
        if !ble.isLive { return .orange }
        if ble.sample?.recording == true || ble.sample?.recordingFailed == true { return .red }
        return .green
    }

    private var loraSection: some View {
        let extra = ble.sample?.extra
        let boat = extra?.boatID ?? 0
        let enabled = extra?.loraEnabled == true
        return VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 6) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .foregroundStyle(enabled ? Color.green : Color.secondary)
                Text(loraStatusText)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(enabled ? Color.primary : Color.secondary)
                Spacer()
            }

            Stepper(value: $boatDraft, in: 0...32) {
                HStack {
                    Text("배 번호").font(.caption2).foregroundStyle(.secondary)
                    Spacer()
                    Text(boatDraft == 0 ? "수신 전용" : String(format: "B%02d", boatDraft))
                        .font(.caption.weight(.semibold).monospacedDigit())
                }
            }

            if boatDraft != boat {
                Button {
                    WKInterfaceDevice.current().play(.click)
                    ble.sendControl("boat \(boatDraft)")
                } label: {
                    Text("배 번호 저장")
                        .font(.caption2.weight(.semibold))
                        .frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.bordered)
            }

            Button {
                WKInterfaceDevice.current().play(.click)
                ble.sendControl(enabled ? "lora live off" : "lora live on")
            } label: {
                Label(enabled ? "장거리 통신 종료" : "장거리 통신 시작",
                      systemImage: enabled ? "antenna.radiowaves.left.and.right.slash" : "antenna.radiowaves.left.and.right")
                    .font(.system(size: 14, weight: .semibold))
                    .frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.borderedProminent)
            .tint(enabled ? .orange : .blue)

            Text(boat == 0 ? "0번은 코치용 수신 전용입니다" : "켜면 자기 위치를 보내면서 다른 배도 받습니다")
                .font(.system(size: 9))
                .foregroundStyle(.secondary)

            if !ble.controlReady {
                Text("보드 연결 후 명령을 자동으로 보냅니다")
                    .font(.system(size: 9))
                    .foregroundStyle(.orange)
            }

            if let reply = recentLoraReply {
                Text(reply)
                    .font(.system(size: 9))
                    .foregroundStyle(reply.hasPrefix("실패") ? .red : .secondary)
            }
        }
    }

    private var loraStatusText: String {
        guard ble.isLive, let extra = ble.sample?.extra else { return "장거리 상태 확인 불가" }
        guard extra.loraEnabled else { return "장거리 꺼짐 · 절전" }
        if extra.boatID == 0 { return "장거리 수신 중 · 코치" }
        return extra.loraPPSReady
            ? String(format: "장거리 송수신 중 · B%02d", extra.boatID)
            : "수신 중 · GPS 시각 대기"
    }

    private var recentLoraReply: String? {
        guard ble.lastControlCommand.hasPrefix("lora") || ble.lastControlCommand.hasPrefix("boat"),
              let at = ble.controlReplyAt,
              Date().timeIntervalSince(at) < 10,
              !ble.controlReply.isEmpty else { return nil }
        if ble.controlReply.hasPrefix("ok lora live on") { return "장거리 통신을 시작했습니다" }
        if ble.controlReply.hasPrefix("ok lora live off") { return "장거리 통신을 종료했습니다" }
        if ble.controlReply.hasPrefix("ok boat") { return "배 번호를 저장했습니다" }
        if ble.controlReply.hasPrefix("err") { return "실패: \(ble.controlReply)" }
        return ble.controlReply
    }

    private var waterLockSection: some View {
        VStack(alignment: .leading, spacing: 7) {
            if let error = session.errorMessage {
                Text(error)
                    .font(.system(size: 9))
                    .foregroundStyle(.red)
                    .lineLimit(3)
            }

            Button {
                page = 0
                Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 500_000_000)
                    session.enableWaterLock()
                }
            } label: {
                Label("물 잠금", systemImage: "drop.fill")
                    .font(.caption2)
                    .frame(maxWidth: .infinity, minHeight: 34)
            }
            .buttonStyle(.bordered)
            .disabled(!session.isRunning)
        }
    }

    private var magcalControls: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("보드가 든 장치를 앞뒤·좌우로 기울이며 천천히 돌리세요")
                .font(.system(size: 9))
                .foregroundStyle(.secondary)

            HStack(spacing: 5) {
                magcalButton(ble.magcal.isFull ? "다시 모으기" : "수집 시작",
                             ble.magcal.isFull ? "magcal reset" : "magcal on")
                magcalButton("검사·저장", "magcal stop")
            }

            if let progress = ble.magcal.progress {
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text("\(progress.done) / \(progress.total) 점")
                            .font(.system(size: 11, weight: .semibold).monospacedDigit())
                        Spacer()
                        if progress.done >= progress.total {
                            Text("수집 끝").font(.system(size: 9)).foregroundStyle(.secondary)
                        }
                    }
                    GeometryReader { geometry in
                        ZStack(alignment: .leading) {
                            Capsule().fill(Color.gray.opacity(0.25))
                            Capsule()
                                .fill(Color.accentColor)
                                .frame(width: geometry.size.width * CGFloat(progress.done) / CGFloat(progress.total))
                        }
                    }
                    .frame(height: 6)
                }
            }

            if !ble.controlReady {
                Text("보드 연결을 기다리는 중입니다")
                    .font(.system(size: 9))
                    .foregroundStyle(.orange)
            } else if !ble.magcal.message.isEmpty {
                Text(ble.magcal.message)
                    .font(.system(size: 9))
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func magcalButton(_ title: String, _ command: String) -> some View {
        Button {
            WKInterfaceDevice.current().play(.click)
            ble.sendControl(command)
        } label: {
            Text(title)
                .font(.system(size: 12, weight: .semibold))
                .frame(maxWidth: .infinity, minHeight: 40)
                .contentShape(Rectangle())
                .background(
                    RoundedRectangle(cornerRadius: 8)
                        .fill(ble.controlReady ? Color.accentColor.opacity(0.28)
                                               : Color.orange.opacity(0.22))
                )
        }
        .buttonStyle(.plain)
    }

    private func card<Content: View>(@ViewBuilder content: () -> Content) -> some View {
        content()
            .padding(8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                RoundedRectangle(cornerRadius: 12)
                    .fill(Color.white.opacity(0.07))
            )
    }
}

/// 항해 화면에는 필요 없는 9축 원시값. 설정의 `상태`를 열었을 때만 보인다.
private struct IMUStatusView: View {
    let extra: TelemetryExtra?

    var body: some View {
        if let extra, extra.imuOK {
            VStack(spacing: 4) {
                axisHeader
                axisRow("ACC", extra.accel, "%+.2f")
                axisRow("GYR", extra.gyro, "%+.1f")
                if extra.magOK {
                    axisRow("MAG", extra.mag, "%+.0f")
                } else {
                    note("자력계 데이터 없음")
                }
            }
        } else {
            note(extra == nil ? "보드 데이터 없음" : "IMU 데이터 없음")
        }
    }

    private var axisHeader: some View {
        HStack(spacing: 2) {
            Color.clear.frame(width: 28, height: 1)
            axisName("X")
            axisName("Y")
            axisName("Z")
        }
    }

    private func axisName(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .medium))
            .foregroundStyle(.tertiary)
            .frame(maxWidth: .infinity)
    }

    private func axisRow(_ label: String, _ value: Vector3, _ format: String) -> some View {
        HStack(spacing: 2) {
            Text(label)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(width: 28, alignment: .leading)
            axisValue(value.x, format)
            axisValue(value.y, format)
            axisValue(value.z, format)
        }
    }

    private func axisValue(_ value: Double, _ format: String) -> some View {
        Text(String(format: format, value))
            .font(.system(size: 14, weight: .medium, design: .rounded))
            .monospacedDigit()
            .minimumScaleFactor(0.45)
            .lineLimit(1)
            .frame(maxWidth: .infinity)
    }

    private func note(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(Color.sailWarn)
            .frame(maxWidth: .infinity, alignment: .center)
    }
}
