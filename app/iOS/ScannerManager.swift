//
//  ScannerManager.swift
//  스캐너 탭 전용 — 연결하지 않고 광고만 관측한다.
//
//  BLEManager 와는 완전히 별개의 CBCentralManager 를 쓴다.
//  (하나의 앱에서 여러 개의 central 인스턴스를 두는 것은 허용된다.)
//
//  allowDuplicates 를 켜므로 광고 인터벌(200ms)마다 콜백이 온다.
//  반면 펌웨어는 광고 페이로드를 1 Hz 로만 갱신하므로 대부분의 콜백은
//  직전과 같은 seq 를 가진 "중복"이다. seq 가 실제로 바뀐 것만 새 패킷으로 센다.
//

import Combine
import CoreBluetooth
import Foundation

// MARK: - 모듈 하나에 대한 관측 상태

struct ScannedModule: Identifiable {
    let id: UInt8            // module_id
    var sample: TelemetrySample
    var rssi: Int
    var firstSeen: Date
    var lastSeen: Date

    /// seq 가 바뀐 횟수 = 실제로 받은 서로 다른 광고 패킷 수
    var receivedPackets: Int
    /// seq 점프로 추정한 유실 패킷 수
    var lostPackets: Int
    /// allowDuplicates 로 들어온 전체 콜백 수 (중복 포함)
    var advertisementCallbacks: Int

    /// 수신율 (%)
    var receptionRate: Double {
        let total = receivedPackets + lostPackets
        guard total > 0 else { return 100 }
        return Double(receivedPackets) / Double(total) * 100
    }

    /// 실측 광고 갱신율 (Hz). 펌웨어 기대치 1 Hz.
    var updateRateHz: Double {
        let span = lastSeen.timeIntervalSince(firstSeen)
        guard span > 0.5, receivedPackets > 1 else { return 0 }
        return Double(receivedPackets - 1) / span
    }

    /// 콜백 자체가 들어오는 비율 (Hz). 광고 인터벌 200ms → 이론상 5 Hz.
    var callbackRateHz: Double {
        let span = lastSeen.timeIntervalSince(firstSeen)
        guard span > 0.5 else { return 0 }
        return Double(advertisementCallbacks) / span
    }

    var isStale: Bool { Date().timeIntervalSince(lastSeen) > 3.0 }
}

// MARK: - ScannerManager

final class ScannerManager: NSObject, ObservableObject {

    @Published private(set) var modules: [ScannedModule] = []
    @Published private(set) var isScanning = false
    @Published private(set) var bluetoothPoweredOn = false
    /// 우리 서비스 UUID 를 광고하지만 manufacturer data 를 못 읽은 횟수(진단용)
    @Published private(set) var undecodableCount = 0

    private var central: CBCentralManager!
    private var lastSeq: [UInt8: UInt8] = [:]
    private var refreshTimer: Timer?
    /// 델리게이트 콜백이 초당 수십 번 오므로 UI 갱신은 별도 주기로 모아서 한다.
    private var pending: [UInt8: ScannedModule] = [:]
    private var dirty = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: 공개 API

    func startScanning() {
        guard central.state == .poweredOn, !central.isScanning else { return }
        central.scanForPeripherals(
            withServices: [SailProtocol.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
        isScanning = true
        startRefreshTimer()
    }

    func stopScanning() {
        central.stopScan()
        isScanning = false
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    func reset() {
        pending.removeAll()
        lastSeq.removeAll()
        modules.removeAll()
        undecodableCount = 0
    }

    // MARK: 내부

    private func startRefreshTimer() {
        refreshTimer?.invalidate()
        let t = Timer(timeInterval: 0.2, repeats: true) { [weak self] _ in
            guard let self else { return }
            // stale 표시를 갱신해야 하므로 dirty 가 아니어도 주기적으로 publish 한다.
            self.modules = self.pending.values.sorted { $0.id < $1.id }
            self.dirty = false
        }
        RunLoop.main.add(t, forMode: .common)
        refreshTimer = t
    }
}

// MARK: - CBCentralManagerDelegate

extension ScannerManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothPoweredOn = central.state == .poweredOn
        if central.state == .poweredOn {
            startScanning()
        } else {
            isScanning = false
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {

        guard let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data,
              let decoded = TelemetrySample.decodeManufacturerData(mfg) else {
            undecodableCount += 1
            return
        }

        let moduleID = decoded.moduleID
        let seq = decoded.sequence ?? 0
        let now = decoded.receivedAt

        var entry = pending[moduleID] ?? ScannedModule(
            id: moduleID,
            sample: decoded,
            rssi: RSSI.intValue,
            firstSeen: now,
            lastSeen: now,
            receivedPackets: 0,
            lostPackets: 0,
            advertisementCallbacks: 0
        )

        entry.advertisementCallbacks += 1
        entry.rssi = RSSI.intValue
        entry.lastSeen = now
        entry.sample = decoded

        if let prev = lastSeq[moduleID] {
            // (seq - prev) mod 256 == 0 이면 같은 광고 데이터의 재방송 → 새 패킷 아님
            let delta = Int((Int(seq) - Int(prev) + 256) % 256)
            if delta > 0 {
                entry.receivedPackets += 1
                entry.lostPackets += (delta - 1) // 사이에 빠진 만큼이 유실
            }
        } else {
            // 첫 관측: 기준점만 잡고 카운트 시작
            entry.receivedPackets = 1
            entry.firstSeen = now
        }
        lastSeq[moduleID] = seq

        pending[moduleID] = entry
        dirty = true
    }
}
