//
//  ScannerView.swift
//  연결 없이 광고만 관측하는 탭.
//
//  펌웨어가 1 Hz 로 갱신하는 Manufacturer Data 를 파싱해
//  모듈별 sog / cog / heel / seq / RSSI 를 보여주고,
//  seq 점프로 패킷 유실을 추정해 수신율(%)을 계산한다.
//

import SwiftUI

struct ScannerView: View {
    @StateObject private var scanner = ScannerManager()
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        NavigationStack {
            Group {
                if !scanner.bluetoothPoweredOn {
                    ContentUnavailableView("블루투스를 켜주세요",
                                           systemImage: "antenna.radiowaves.left.and.right.slash")
                } else if scanner.modules.isEmpty {
                    ContentUnavailableView {
                        Label("광고를 찾는 중", systemImage: "dot.radiowaves.left.and.right")
                    } description: {
                        Text("\(HohoProtocol.namePrefix)* 모듈의 광고 패킷을 기다리는 중입니다.\n연결은 하지 않습니다.")
                    }
                } else {
                    List {
                        Section {
                            ForEach(scanner.modules) { module in
                                ModuleRow(module: module)
                            }
                        } header: {
                            Text("모듈 \(scanner.modules.count)개")
                        } footer: {
                            Text("allowDuplicates 를 켜고 스캔합니다. 광고 인터벌 200 ms → 콜백은 약 5 Hz, "
                                 + "페이로드 갱신은 1 Hz 이므로 seq 가 바뀐 것만 새 패킷으로 셉니다.")
                        }

                        if scanner.undecodableCount > 0 {
                            Section("진단") {
                                LabeledContent("파싱 실패 광고", value: "\(scanner.undecodableCount)")
                                    .font(.caption)
                            }
                        }
                    }
                }
            }
            .navigationTitle("스캐너")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("초기화") { scanner.reset() }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(scanner.isScanning ? Color.blue : Color.gray)
                            .frame(width: 8, height: 8)
                        Text(scanner.isScanning ? "스캔 중" : "정지")
                            .font(.caption)
                    }
                }
            }
        }
        .onAppear { scanner.startScanning() }
        .onChange(of: scenePhase) { _, phase in
            // 백그라운드에서 allowDuplicates 스캔은 어차피 동작하지 않으므로 아낀다.
            if phase == .active { scanner.startScanning() } else { scanner.stopScanning() }
        }
    }
}

// MARK: - 모듈 한 줄

struct ModuleRow: View {
    let module: ScannedModule

    private var rateColor: Color {
        switch module.receptionRate {
        case 98...:  return .green
        case 90..<98: return .yellow
        default:     return .red
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {

            // 헤더
            HStack {
                Text("모듈 #\(module.id)")
                    .font(.headline)
                if module.isStale {
                    Text("신호 끊김")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(.red.opacity(0.2), in: Capsule())
                }
                Spacer()
                Label("\(module.rssi)", systemImage: "antenna.radiowaves.left.and.right")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            // 값
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(module.sample.sogText)
                    .font(.system(size: 42, weight: .bold, design: .rounded))
                    .monospacedDigit()
                    .contentTransition(.numericText())
                Text("kn")
                    .font(.headline)
                    .foregroundStyle(.secondary)

                Spacer()

                VStack(alignment: .trailing, spacing: 2) {
                    HStack(spacing: 4) {
                        Text("COG").font(.caption2).foregroundStyle(.secondary)
                        Text(module.sample.cogText)
                            .font(.callout.monospacedDigit().weight(.semibold))
                        Text(compassPoint(module.sample.cogDegrees))
                            .font(.caption2).foregroundStyle(.tertiary)
                    }
                    HStack(spacing: 4) {
                        Text("HEEL").font(.caption2).foregroundStyle(.secondary)
                        Text(module.sample.heelText)
                            .font(.callout.monospacedDigit().weight(.semibold))
                        Text(module.sample.heelSideLabel)
                            .font(.caption2).foregroundStyle(.tertiary)
                    }
                }
            }
            .opacity(module.isStale ? 0.5 : 1)

            Divider()

            // 통계
            HStack(spacing: 0) {
                stat("seq", "\(module.sample.sequence.map(String.init) ?? "—")")
                stat("수신", "\(module.receivedPackets)")
                stat("유실", "\(module.lostPackets)",
                     color: module.lostPackets > 0 ? .orange : nil)
                stat("수신율", String(format: "%.1f%%", module.receptionRate), color: rateColor)
                stat("갱신", String(format: "%.2fHz", module.updateRateHz))
                stat("배터리", "\(module.sample.batteryPercent)%")
            }
        }
        .padding(.vertical, 6)
    }

    private func stat(_ label: String, _ value: String, color: Color? = nil) -> some View {
        VStack(spacing: 2) {
            Text(label)
                .font(.caption2)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.caption.monospacedDigit().weight(.semibold))
                .foregroundStyle(color ?? .primary)
        }
        .frame(maxWidth: .infinity)
    }
}
