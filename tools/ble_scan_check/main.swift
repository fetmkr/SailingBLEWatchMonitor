//
//  main.swift
//  맥에서 광고만 스캔해 수신율을 독립 측정하는 진단 도구.
//
//  왜 필요한가:
//  아이폰 스캐너 탭에서 유실이 보일 때, 원인이 (a) 보드가 광고를 제대로 못 쏘는 것인지
//  (b) 아이폰이 같은 보드에 연결까지 하고 있어서 라디오가 경합하는 것인지 구분해야 한다.
//  맥은 연결하지 않고 스캔만 하므로, 여기서 유실이 거의 없으면 원인은 (b) 다.
//
//  빌드/실행:
//    swiftc -O -o /tmp/blescan app/Shared/Protocol.swift tools/ble_scan_check/main.swift
//    /tmp/blescan [측정초]
//
//  처음 실행하면 macOS 가 터미널에 블루투스 권한을 물어본다. 허용해야 동작한다.
//

import CoreBluetooth
import Foundation

let duration = CommandLine.arguments.count > 1
    ? (Double(CommandLine.arguments[1]) ?? 30) : 30

final class ScanProbe: NSObject, CBCentralManagerDelegate {
    private var central: CBCentralManager!

    // 모듈별 통계
    private struct Stats {
        var name = ""
        var callbacks = 0        // allowDuplicates 로 들어온 전체 콜백
        var withMfg = 0          // manufacturer data 가 붙어 있던 콜백
        var uniqueSeq = 0        // seq 가 바뀐 횟수 = 실제로 받은 광고 패킷
        var lost = 0             // seq 점프로 추정한 유실
        var lastSeq: UInt8?
        var firstSeen = Date()
        var lastSeen = Date()
        var rssiSum = 0
        var rssiCount = 0
        var rssiMin = 0
        var rssiMax = -200
    }
    private var stats: [UInt8: Stats] = [:]
    private var noMfgCallbacks = 0
    private var started: Date?

    func run(seconds: Double) {
        central = CBCentralManager(delegate: self, queue: .main)
        let deadline = Date().addingTimeInterval(seconds + 10) // 권한 대기 여유
        while Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
            if let s = started, Date().timeIntervalSince(s) >= seconds { break }
        }
        central.stopScan()
        report(seconds: started.map { Date().timeIntervalSince($0) } ?? 0)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("스캔 시작 — \(Int(duration))초 (연결하지 않고 광고만 관측)\n")
            started = Date()
            central.scanForPeripherals(
                withServices: [SailProtocol.serviceUUID],
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        case .unauthorized:
            print("❌ 블루투스 권한이 거부됐습니다.")
            print("   시스템 설정 → 개인정보 보호 및 보안 → Bluetooth 에서 터미널을 허용하세요.")
            exit(2)
        case .poweredOff:
            print("❌ 블루투스가 꺼져 있습니다."); exit(2)
        case .unsupported:
            print("❌ 이 맥에서 BLE 를 쓸 수 없습니다."); exit(2)
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {

        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
                ?? peripheral.name ?? "?"

        guard let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data,
              let s = SailProtocol.decodeManufacturerDataSample(mfg) else {
            // 광고는 왔는데 scan response(manufacturer data)를 못 받은 경우.
            // 텔레메트리가 scan response 에 실려 있으므로 이게 많으면
            // SCAN_REQ/RSP 왕복이 실패하고 있다는 뜻이다.
            noMfgCallbacks += 1
            return
        }

        var e = stats[s.moduleID] ?? Stats()
        if e.callbacks == 0 { e.firstSeen = Date(); e.rssiMin = RSSI.intValue }
        e.name = name
        e.callbacks += 1
        e.withMfg += 1
        e.lastSeen = Date()
        e.rssiSum += RSSI.intValue
        e.rssiCount += 1
        e.rssiMin = min(e.rssiMin, RSSI.intValue)
        e.rssiMax = max(e.rssiMax, RSSI.intValue)

        if let prev = e.lastSeq {
            let delta = (Int(s.sequence ?? 0) - Int(prev) + 256) % 256
            if delta > 0 {
                e.uniqueSeq += 1
                e.lost += delta - 1
            }
        } else {
            e.uniqueSeq = 1
        }
        e.lastSeq = s.sequence
        stats[s.moduleID] = e
    }

    private func report(seconds: Double) {
        print("\n════════════════════════════════════════════════════════")
        print(String(format: "  측정 %.1f초", seconds))
        print("════════════════════════════════════════════════════════")

        if stats.isEmpty {
            print("  모듈을 하나도 못 찾았습니다.")
            print("  · 보드가 켜져 있고 광고 중인지 확인")
            print("  · manufacturer data 없는 콜백 \(noMfgCallbacks)회")
            return
        }

        for (id, e) in stats.sorted(by: { $0.key < $1.key }) {
            let span = max(e.lastSeen.timeIntervalSince(e.firstSeen), 0.001)
            let total = e.uniqueSeq + e.lost
            let rate = total > 0 ? Double(e.uniqueSeq) / Double(total) * 100 : 0
            print("""

              모듈 \(e.name) (module_id \(id))
                콜백          \(e.callbacks)회  (\(String(format: "%.2f", Double(e.callbacks)/span)) Hz)
                고유 seq      \(e.uniqueSeq)개  (\(String(format: "%.2f", Double(e.uniqueSeq)/span)) Hz  ← 기대 1.00)
                유실 추정     \(e.lost)개
                수신율        \(String(format: "%.1f", rate))%
                RSSI          평균 \(e.rssiCount > 0 ? e.rssiSum/e.rssiCount : 0) / 범위 \(e.rssiMin)…\(e.rssiMax) dBm
            """)
        }

        print("""

              manufacturer data 없는 콜백  \(noMfgCallbacks)회
                (광고는 받았는데 scan response 를 못 받은 경우.
                 텔레메트리가 scan response 에 있으므로 이 값이 크면
                 SCAN_REQ/RSP 왕복이 실패하고 있다는 뜻)
            ════════════════════════════════════════════════════════
            """)
    }
}

ScanProbe().run(seconds: duration)
