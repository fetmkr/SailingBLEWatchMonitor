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
    /// 실제로 스캔을 듣고 있던 시간(초). 일시정지·백그라운드 구간은 빠진다.
    var listeningSeconds: Double = 0

    /// 수신율 (%)
    var receptionRate: Double {
        let total = receivedPackets + lostPackets
        guard total > 0 else { return 100 }
        return Double(receivedPackets) / Double(total) * 100
    }

    /// 실측 광고 갱신율 (Hz). 펌웨어 기대치 1 Hz.
    /// 분모는 벽시계 시간이 아니라 "실제로 듣고 있던 시간" 이다.
    /// 백그라운드로 나가 있던 시간을 넣으면 갱신율이 실제보다 낮게 나온다.
    var updateRateHz: Double {
        guard listeningSeconds > 0.5, receivedPackets > 1 else { return 0 }
        return Double(receivedPackets - 1) / listeningSeconds
    }

    /// 콜백 자체가 들어오는 비율 (Hz). 광고 인터벌 200ms → 이론상 5 Hz.
    var callbackRateHz: Double {
        guard listeningSeconds > 0.5 else { return 0 }
        return Double(advertisementCallbacks) / listeningSeconds
    }

    var isStale: Bool { Date().timeIntervalSince(lastSeen) > 3.0 }
}

// MARK: - ScannerManager

final class ScannerManager: NSObject, ObservableObject {

    @Published private(set) var modules: [ScannedModule] = []
    @Published private(set) var isScanning = false
    @Published private(set) var bluetoothPoweredOn = false
    /// 우리 서비스 UUID 를 광고하지만 manufacturer data 를 못 읽은 횟수.
    /// 텔레메트리가 scan response 에 실려 있으므로, 이 값이 크면
    /// SCAN_REQ/RSP 왕복이 실패하고 있다는 뜻이다.
    @Published private(set) var undecodableCount = 0
    /// 스캔을 멈췄다 재개해서 기준점을 다시 잡은 횟수 (유실 통계에서 제외된 구간)
    @Published private(set) var pausedResyncs = 0

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

        // ★ 기준점을 버린다.
        //   보드의 seq 는 1초마다 계속 올라간다. 스캔을 멈춘 동안 벌어진 간격을
        //   그대로 두면, 재개했을 때 그 공백이 전부 "유실" 로 계산된다.
        //   (앱을 1분 백그라운드에 뒀다 오면 유실 60개가 찍힌다)
        //   우리가 안 듣고 있던 시간은 패킷 유실이 아니므로 다음 관측에서 재기준을 잡는다.
        lastSeq.removeAll()
        pausedResyncs += 1
    }

    func reset() {
        pending.removeAll()
        lastSeq.removeAll()
        modules.removeAll()
        undecodableCount = 0
        pausedResyncs = 0
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
            advertisementCallbacks: 0,
            listeningSeconds: 0
        )

        // 듣고 있던 시간만 누적한다. 콜백은 광고 인터벌(200ms)마다 오므로
        // 간격이 1초를 넘으면 그건 스캔이 멈춰 있었다는 뜻이라 빼야 한다.
        let gap = now.timeIntervalSince(entry.lastSeen)
        if entry.advertisementCallbacks > 0 && gap > 0 && gap < 1.0 {
            entry.listeningSeconds += gap
        }

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
