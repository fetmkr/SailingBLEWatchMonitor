//
//  SettingsView.swift
//  모듈 선택 / 해제 — iOS
//
//  고정된 모듈이 없으면 주변 모듈 목록을 띄우고, 고르면 저장한다.
//  다른 모듈에 붙고 싶으면 "고정 해제" 로 목록을 다시 연다.
//

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var ble: BLEManager
    @Environment(\.scenePhase) private var scenePhase
    @State private var showUnpinConfirm = false
    @State private var showLog = false

    var body: some View {
        NavigationStack {
            List {
                if let pin = ble.pinnedModule {
                    pinnedSection(pin)
                } else {
                    pickerSection
                }

                Section {
                    LabeledContent("연결 상태", value: ble.state.displayText)
                    LabeledContent("데이터 경로", value: ble.source.displayText)
                    LabeledContent("연결 수신율",
                                   value: ble.connLastAt == nil ? "—"
                                        : String(format: "%.2f Hz", ble.connRateHz))
                    LabeledContent("광고 수신율",
                                   value: ble.advLastAt == nil ? "—"
                                        : String(format: "%.2f Hz", ble.advRateHz))
                    LabeledContent("RSSI", value: ble.rssi.map { "\($0) dBm" } ?? "—")
                    LabeledContent("배터리", value: ble.sample.map { "\($0.batteryPercent)%" } ?? "—")
                    LabeledContent("모듈 uptime",
                                   value: ble.sample?.uptimeSeconds.map { String(format: "%.0f초", $0) } ?? "—")
                    LabeledContent("마지막 수신",
                                   value: ble.lastPacketAt.map { String(format: "%.1f초 전", -$0.timeIntervalSinceNow) } ?? "—")
                    Button {
                        showLog = true
                    } label: {
                        Label("디버그 로그 (\(ble.log.count))", systemImage: "text.alignleft")
                    }
                    Button {
                        ble.forceReconnect()
                    } label: {
                        Label("강제 재연결", systemImage: "arrow.clockwise")
                    }
                } header: {
                    Text("진단")
                } footer: {
                    Text("연결 중이면 GATT notify(보드 설정값, 기본 10 Hz), 끊기면 광고에서 1 Hz 로 "
                         + "값을 계속 받습니다. 화면이 죽지 않고 품질만 내려앉습니다.")
                }
            }
            .navigationTitle("설정")
            .navigationBarTitleDisplayMode(.inline)
            .sheet(isPresented: $showLog) { LogSheet(ble: ble) }
        }
        .onAppear { ble.refreshDiscovery() }
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { ble.refreshDiscovery() }
        }
    }

    // MARK: 고정된 상태

    @ViewBuilder
    private func pinnedSection(_ pin: ModulePin) -> some View {
        Section {
            HStack(spacing: 12) {
                Image(systemName: "sailboat.fill")
                    .font(.title2)
                    .foregroundStyle(.tint)
                VStack(alignment: .leading, spacing: 2) {
                    Text(pin.displayName)
                        .font(.headline)
                    Text("module_id \(pin.moduleID) · \(pin.fullName)")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if ble.isLive {
                    Text("수신 중")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 8).padding(.vertical, 3)
                        .background(.green.opacity(0.2), in: Capsule())
                }
            }
            .padding(.vertical, 4)
        } header: {
            Text("연결할 모듈")
        } footer: {
            Text("이 모듈에만 연결합니다. 다른 모듈이 같은 서비스를 광고해도 무시하고, "
                 + "연결된 뒤에도 매 패킷의 module_id 를 확인해 다르면 즉시 끊습니다.")
        }

        Section {
            Button(role: .destructive) {
                showUnpinConfirm = true
            } label: {
                Label("고정 해제하고 다시 고르기", systemImage: "arrow.triangle.2.circlepath")
            }
            .confirmationDialog("이 모듈 고정을 해제할까요?",
                                isPresented: $showUnpinConfirm, titleVisibility: .visible) {
                Button("해제", role: .destructive) { ble.unpinModule() }
                Button("취소", role: .cancel) {}
            } message: {
                Text("연결이 끊기고 주변 모듈 목록이 다시 나타납니다.")
            }
        }
    }

    // MARK: 선택 대기 상태

    @ViewBuilder
    private var pickerSection: some View {
        Section {
            if !ble.bluetoothPoweredOn {
                Label("블루투스를 켜주세요", systemImage: "antenna.radiowaves.left.and.right.slash")
                    .foregroundStyle(.secondary)
            } else if ble.discoveredModules.isEmpty {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("주변 모듈을 찾는 중…")
                        .foregroundStyle(.secondary)
                }
                .padding(.vertical, 6)
            } else {
                ForEach(ble.discoveredModules) { module in
                    Button {
                        ble.selectModule(module)
                    } label: {
                        ModuleCandidateRow(module: module)
                    }
                    .buttonStyle(.plain)
                }
            }
        } header: {
            Text("연결할 모듈 고르기")
        } footer: {
            Text("가까운 순으로 정렬됩니다. 한 번 고르면 저장되어 다음부터는 자동으로 연결합니다.")
        }
    }
}

// MARK: - 후보 한 줄

struct ModuleCandidateRow: View {
    let module: DiscoveredModule

    var body: some View {
        HStack(spacing: 12) {
            SignalBars(bars: module.signalBars)
                .frame(width: 22, height: 18)

            VStack(alignment: .leading, spacing: 2) {
                Text(module.displayName)
                    .font(.body.weight(.medium))
                HStack(spacing: 6) {
                    if let id = module.moduleID {
                        Text("id \(id)")
                    }
                    Text("\(module.rssi) dBm")
                    if let s = module.sample {
                        Text("· \(s.sogText) kn")
                    }
                }
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
            }

            Spacer()

            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 4)
        .opacity(module.isStale ? 0.4 : 1)
    }
}

// MARK: - 신호 막대

struct SignalBars: View {
    let bars: Int

    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(0..<4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 1)
                    .fill(i < bars ? AnyShapeStyle(.tint) : AnyShapeStyle(.quaternary))
                    .frame(width: 3, height: 5 + CGFloat(i) * 4)
            }
        }
    }
}
