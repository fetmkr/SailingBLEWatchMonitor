//
//  SensorDetailView.swift
//  상세 라이브 — iOS 두 번째 탭
//
//  라이브 탭은 속도 · COG · 힐 세 개만 크게 보여준다. 항해 중에 봐야 하는 건
//  그게 전부다. 9축이나 GPS 상태처럼 자세한 값은 이 탭으로 뺐다.
//
//  ★ 스크롤이 없어야 한다. 흔들리는 배 위에서 손가락으로 밀어 가며 읽을 수는
//    없다. 그래서 한 화면에 다 넣고, 남는 높이는 블록들이 나눠 갖는다.
//    항목을 늘리고 싶으면 무엇을 뺄지 먼저 정해야 한다.
//    (module_id 나 uptime 은 설정 탭 진단에 있으므로 여기서 뺐다)
//
//  10 Hz 로 바뀌는 숫자에는 전환 애니메이션을 걸지 않는다. 글자가 계속
//  꿈틀거려서 오히려 못 읽는다. (라이브 탭과 같은 이유)
//

import SwiftUI

struct SensorDetailView: View {
    @ObservedObject var ble: BLEManager

    private var sample: TelemetrySample? { ble.sample }
    private var extra: TelemetryExtra? { ble.sample?.extra }
    private var dimmed: Bool { !ble.isLive }

    var body: some View {
        NavigationStack {
            Group {
                if sample == nil {
                    emptyState
                } else {
                    VStack(spacing: 8) {
                        statusBanner
                        navRow.frame(maxHeight: .infinity)
                        attitudeRow.frame(maxHeight: .infinity)
                        axisTable.frame(maxHeight: .infinity)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .opacity(dimmed ? 0.5 : 1)
                }
            }
            .navigationTitle("상세")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    // MARK: - 맨 위 한 줄
    //
    // 값이 지어낸 것인지 아닌지를 제일 먼저 알린다.
    // 지어낸 숫자를 실측으로 오해하면 바다에서 위험하다.

    private var statusBanner: some View {
        let sim = extra?.sogIsSimulated ?? false
        let tint = sim ? Color.sailWarn : Color.green
        return HStack(spacing: 8) {
            Image(systemName: sim ? "exclamationmark.triangle.fill" : "location.fill")
            Text(sim ? "속도·침로는 시뮬레이터 값" : "속도·침로는 GPS 실측")
                .font(.subheadline.weight(.medium))
            Spacer()
            if let s = sample {
                Text("\(s.batteryPercent)%")
                    .font(.subheadline.monospacedDigit())
                    .foregroundStyle(s.batteryPercent <= 20 ? Color.sailWarn : .secondary)
            }
        }
        .foregroundStyle(tint)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 10).fill(tint.opacity(0.14)))
    }

    // MARK: - 항해

    private var navRow: some View {
        HStack(spacing: 8) {
            card("SOG", sample?.sogText ?? "—", sub: "kn",
                 warn: extra?.sogIsSimulated ?? false)
            card("COG", sample.map { String(format: "%.0f°", $0.cogDegrees) } ?? "—",
                 sub: sample.map { compassPoint($0.cogDegrees) } ?? " ",
                 warn: extra?.sogIsSimulated ?? false)
            // COG 는 가는 방향(GPS), HDG 는 뱃머리 방향(자력계). 서로 다른 값이다.
            card("HDG", headingText, sub: "뱃머리")
        }
    }

    private var headingText: String {
        guard let h = extra?.headingDegrees else { return "—" }
        return String(format: "%.0f°", h)
    }

    // MARK: - 자세와 위성

    private var attitudeRow: some View {
        HStack(spacing: 8) {
            card("HEEL", sample?.heelText ?? "—",
                 sub: sample?.heelSideLabel ?? " ")
            card("PITCH",
                 extra.map { String(format: "%+.1f°", $0.pitchDegrees) } ?? "—",
                 sub: " ")
            card("위성", extra.map { "\($0.satellites)" } ?? "—",
                 sub: satSub,
                 warn: !(extra?.gpsFix ?? false))
        }
    }

    private var satSub: String {
        guard let e = extra else { return " " }
        if !e.gpsFix { return "fix 없음" }
        if let h = e.hdop { return String(format: "HDOP %.1f", h) }
        return "fix 있음"
    }

    // MARK: - 9축
    //
    // 세 축 × 세 종류라 카드로 아홉 개를 깔면 화면을 다 먹는다.
    // 표로 눕혀서 한 블록에 넣는다.

    @ViewBuilder
    private var axisTable: some View {
        if let e = extra, e.imuOK {
            VStack(spacing: 2) {
                HStack(spacing: 6) {
                    Text("").frame(width: 44, alignment: .leading)
                    axisHeader("X"); axisHeader("Y"); axisHeader("Z")
                    Text("").frame(width: 28)
                }
                axisLine("가속", e.accel, "%+.2f", "g")
                axisLine("자이로", e.gyro, "%+.1f", "°/s")
                if e.magOK {
                    axisLine("자력", e.mag, "%+.0f", "µT")
                } else {
                    HStack {
                        Text("자력계 없음")
                            .font(.subheadline)
                            .foregroundStyle(Color.sailWarn)
                        Spacer()
                    }
                    .frame(maxHeight: .infinity)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 12).fill(.quaternary.opacity(0.35)))
        } else {
            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill")
                Text(extra == nil
                     ? "이 보드는 9축을 보내지 않습니다"
                     : "IMU 가 응답하지 않습니다")
                    .font(.subheadline)
                Spacer()
            }
            .foregroundStyle(Color.sailWarn)
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(RoundedRectangle(cornerRadius: 12).fill(Color.sailWarn.opacity(0.14)))
        }
    }

    private func axisHeader(_ t: String) -> some View {
        Text(t)
            .font(.caption2)
            .foregroundStyle(.tertiary)
            .frame(maxWidth: .infinity)
    }

    private func axisLine(_ label: String, _ v: Vector3,
                          _ fmt: String, _ unit: String) -> some View {
        HStack(spacing: 6) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: 44, alignment: .leading)
            axisValue(v.x, fmt); axisValue(v.y, fmt); axisValue(v.z, fmt)
            Text(unit)
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .frame(width: 28, alignment: .leading)
        }
        .frame(maxHeight: .infinity)
    }

    private func axisValue(_ value: Double, _ fmt: String) -> some View {
        Text(String(format: fmt, value))
            .font(.system(size: 19, weight: .medium, design: .rounded))
            .monospacedDigit()
            .minimumScaleFactor(0.5)
            .lineLimit(1)
            .frame(maxWidth: .infinity)
    }

    // MARK: - 조각

    private func card(_ label: String, _ value: String,
                      sub: String, warn: Bool = false) -> some View {
        VStack(spacing: 1) {
            Text(label)
                .font(.caption2)
                .foregroundStyle(.secondary)
            Spacer(minLength: 0)
            Text(value)
                .font(.system(size: 34, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .minimumScaleFactor(0.4)
                .lineLimit(1)
                .foregroundStyle(warn ? AnyShapeStyle(Color.sailWarn) : AnyShapeStyle(.primary))
            Spacer(minLength: 0)
            Text(sub)
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 12).fill(.quaternary.opacity(0.35)))
    }

    private var emptyState: some View {
        VStack(spacing: 10) {
            Image(systemName: "antenna.radiowaves.left.and.right.slash")
                .font(.largeTitle)
                .foregroundStyle(.secondary)
            Text("아직 받은 값이 없습니다")
                .font(.headline)
            Text("설정 탭에서 모듈을 고르면\n값이 채워집니다.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
    }
}
