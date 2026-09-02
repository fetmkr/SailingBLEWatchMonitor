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
//    2페이지 — 센서 상세: HDG · COG · 위성 · PITCH · 9축
//    3페이지 — 설정: 모듈 선택 / 세션 / 진단

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

struct WatchLiveView: View {
    var body: some View {
        TabView {
            MainPage()
            DebugPage()
            SettingsPage()
        }
        .tabViewStyle(.page)
    }
}

// MARK: - 2페이지 · 센서 상세
//
// 1페이지는 속도·HDG·힐 세 개만 크게 둔다. 그 밖의 값은 여기에 모은다.
// 시계는 화면이 작다. Ultra 에서는 스크롤 없이 한 화면에 들어오도록 줄을
// 아꼈고, 작은 시계에서는 알아서 스크롤된다.
//
// 위성을 못 잡으면 SAT 숫자가 빨갛게 뜬다. 그때 1페이지의 속도는 대시로
// 나온다 — 시뮬레이터는 없앴고, 값이 없으면 숫자를 아예 안 그린다.

private struct DebugPage: View {
    @EnvironmentObject private var ble: BLEManager

    private var extra: TelemetryExtra? { ble.sample?.extra }

    var body: some View {
        ScrollView {
            VStack(spacing: 4) {
                if let e = extra {
                    // 방향 두 개를 나란히 둔다.
                    //   HDG 뱃머리가 보는 방향 (자력계). 멈춰 있어도 나온다.
                    //   COG 배가 실제로 가는 방향 (GPS). 멈추면 의미가 없다.
                    // 조류와 바람 때문에 둘이 벌어진다. 1페이지에는 HDG 만 크게 둔다.
                    HStack(spacing: 0) {
                        pair(e.headingDegrees.map { String(format: "%.0f°", $0) } ?? "—",
                             "HDG")
                        pair(ble.sample?.cogDegrees.map { String(format: "%.0f°", $0) } ?? "—",
                             "COG")
                    }
                    HStack(spacing: 0) {
                        pair("\(e.satellites)", "SAT", warn: !e.gpsFix)
                        pair(e.hdop.map { String(format: "%.1f", $0) } ?? "—", "HDOP")
                    }
                    HStack(spacing: 0) {
                        pair(String(format: "%+.1f°", e.pitchDegrees), "PITCH")
                        Color.clear.frame(maxWidth: .infinity)
                    }

                    if e.imuOK {
                        axisRow("ACC", e.accel, "%+.2f")
                        axisRow("GYR", e.gyro, "%+.1f")
                        if e.magOK {
                            axisRow("MAG", e.mag, "%+.0f")
                        } else {
                            note("NO MAG")
                        }
                    } else {
                        note("NO IMU")
                    }
                } else if ble.sample != nil {
                    note("NO 9-AXIS DATA")
                } else {
                    note("NO DATA")
                }
            }
            .padding(.horizontal, 3)
            .padding(.bottom, 2)
        }
        // 내용이 화면보다 작으면 아예 스크롤되지 않게 한다.
        // 그냥 ScrollView 로 두면 다 보이는데도 손가락에 튕기는 반응이 와서
        // "뭔가 더 있나" 하고 계속 밀어 보게 된다.
        // 그래도 ScrollView 를 남기는 이유: 작은 시계에서는 실제로 넘치기 때문에
        // 그때는 스크롤이 있어야 한다.
        .scrollBounceBehavior(.basedOnSize)
    }

    // 1페이지의 HDG/HEEL 보다 조금 작게 잡는다.
    // 오른쪽 위에 시스템 시계가 얹히는 만큼 세로 자리가 줄어들기 때문이다.
    private func pair(_ value: String, _ label: String, warn: Bool = false) -> some View {
        VStack(spacing: -2) {
            Text(value)
                .font(.system(size: 25, weight: .semibold, design: .rounded))
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

    // 축 이름을 값과 같은 줄에 둔다.
    //
    // 이름을 윗줄에 따로 두면 그것만으로 세 줄을 잡아먹어서 화면이 넘쳤다.
    // 단위(g / °/s / µT)는 뺐다. 값이 어느 센서 것인지만 알면 단위는 고정이고,
    // 시계 화면에서는 한 글자가 아깝다. 단위는 아이폰 상세 화면에 적혀 있다.
    private func axisRow(_ label: String, _ v: Vector3, _ fmt: String) -> some View {
        HStack(spacing: 2) {
            Text(label)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.secondary)
                .frame(width: 30, alignment: .leading)
            axisValue(v.x, fmt)
            axisValue(v.y, fmt)
            axisValue(v.z, fmt)
        }
    }

    private func axisValue(_ value: Double, _ fmt: String) -> some View {
        Text(String(format: fmt, value))
            .font(.system(size: 19, weight: .medium, design: .rounded))
            .monospacedDigit()
            .minimumScaleFactor(0.4)
            .lineLimit(1)
            .frame(maxWidth: .infinity)
    }

    private func note(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 13))
            .foregroundStyle(Color.sailWarn)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 6)
    }
}

// MARK: - 1페이지 · 항해 화면

private struct MainPage: View {
    @EnvironmentObject private var ble: BLEManager
    /// 손목을 내려 화면이 어두워진 상태 (Always On)
    @Environment(\.isLuminanceReduced) private var isDim

    /// 어두워진 횟수. 손목을 내린 화면은 스크린샷으로 못 찍기 때문에,
    /// Always On 이 실제로 걸리는지 이 값으로 확인한다. (설정 페이지에 표시)
    @AppStorage("dimCount") private var dimCount = 0

    private var stale: Bool { !ble.isLive }



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
                        bigPair(ble.sample?.headingText ?? "—", "HDG")
                        bigPair(ble.sample.map { $0.heelText } ?? "—", "HEEL")
                    }
                    Spacer(minLength: 0)
                }
            }
        }
        .opacity(stale ? 0.45 : 1)
        .padding(.horizontal, 2)
        .onChange(of: isDim) { _, nowDim in
            if nowDim { dimCount += 1 }
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

    // 상단 한 줄 — 점 + 모듈 이름
    private var statusLine: some View {
        HStack(spacing: 4) {
            Circle().fill(statusColor).frame(width: 6, height: 6)
            if !isDim {
                Text(statusLabel)
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
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
    @EnvironmentObject private var session: SessionManager
    @AppStorage("dimCount") private var dimCount = 0
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

                Divider()
                sessionSection
                Divider()
                diagnostics
            }
            .padding(.horizontal, 4)
        }
        .onAppear { ble.refreshDiscovery() }
    }

    // ── 세션 (앱 켜면 자동 시작)
    //
    // 모듈을 아직 안 고른 상태에서도 보여야 한다. 세션이 도는지 여부는
    // 모듈과 아무 상관이 없고, "왜 시계로 돌아가나" 를 여기서만 볼 수 있다.
    private var sessionSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 5) {
                Image(systemName: session.isRunning ? "figure.sailing" : "moon.zzz")
                    .foregroundStyle(session.isRunning ? .green : .secondary)
                Text(session.isRunning ? "항해 중 \(session.elapsedText)" : "세션 \(session.stateText)")
                    .font(.caption)
                Spacer()
            }
            Text(session.isRunning
                 ? "손목을 내려도 화면과 연결이 유지됩니다"
                 : "2분 뒤 시계 화면으로 돌아갑니다")
                .font(.system(size: 9))
                .foregroundStyle(.tertiary)

            row("운동 권한", session.authText)

            if let err = session.errorMessage {
                Text(err).font(.system(size: 9)).foregroundStyle(.red).lineLimit(4)
            }

            // 언제 끊겼는지 눈으로 본다. 앱이 죽었다 살아나도 남는다.
            if !session.trail.isEmpty {
                ForEach(session.trail.reversed(), id: \.self) { line in
                    Text(line)
                        .font(.system(size: 9).monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                Button {
                    session.clearTrail()
                } label: {
                    Text("기록 지우기").font(.system(size: 9)).frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }

            Button {
                session.enableWaterLock()
            } label: {
                Label("물 잠금", systemImage: "drop.fill")
                    .font(.caption2)
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)

            if session.isRunning {
                Button(role: .destructive) {
                    session.stop()
                } label: {
                    Text("세션 종료").font(.caption2).frame(maxWidth: .infinity)
                }
            }
        }
    }

    private var diagnostics: some View {
        VStack(alignment: .leading, spacing: 4) {
            // 0 이면 Always On 이 한 번도 안 걸린 것이다.
            // 워치 설정 → 디스플레이 및 밝기 → 항상 켜짐 을 확인할 것.
            row("어두워짐", "\(dimCount)회")
            row("경로", ble.source.displayText)
            row("연결", ble.connLastAt == nil ? "—" : String(format: "%.1f Hz", ble.connRateHz))
            row("광고", ble.advLastAt == nil ? "—" : String(format: "%.1f Hz", ble.advRateHz))
            row("RSSI", ble.rssi.map { "\($0)" } ?? "—")
            row("배터리", ble.sample.map { $0.batteryText } ?? "—")

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
