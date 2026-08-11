//
//  BLEManager.swift
//  Sailing Monitor 텔레메트리 수신기 — iOS / watchOS 공용
//
//  두 가지 모드로 동작한다.
//
//   · discovering — 고정된 모듈이 없을 때. 스캔만 하고 연결하지 않는다.
//                   발견한 모듈 목록을 설정 화면에 넘겨 사용자가 고르게 한다.
//   · bound       — 고정된 모듈이 있을 때. 그 모듈에만 붙고 notify 를 받는다.
//                   끊기면 즉시 재연결(pending connect, 타임아웃 없음).
//
//  잘못된 배에 붙지 않기 위한 3단 방어
//   ① 스캔 자체를 서비스 UUID 로 필터 → 다른 BLE 기기는 콜백조차 안 온다
//   ② 연결 전, 광고 이름이 고정한 이름과 같은지 확인
//   ③ 연결 후, 첫 패킷의 module_id 가 다르면 즉시 끊고 다시 찾는다
//
//  CBCentralManager 를 메인 큐로 만들었으므로 모든 델리게이트 콜백과
//  @Published 갱신은 메인 스레드에서 일어난다.
//

import Combine
import CoreBluetooth
import Foundation

// MARK: - 연결 상태 머신

enum BLEConnectionState: String {
    case idle          // 아직 시작 전 / 블루투스 꺼짐
    case choosing      // 모듈 선택 대기 (고정된 모듈 없음)
    case scanning      // 고정한 모듈을 찾는 중
    case connecting    // 최초 연결 시도 중
    case connected     // 연결 + notify 수신 중
    case reconnecting  // 연결이 끊겨 재연결 대기 중 (pending connect 유지)

    var displayText: String {
        switch self {
        case .idle:         return "대기"
        case .choosing:     return "모듈 선택 필요"
        case .scanning:     return "검색 중…"
        case .connecting:   return "연결 중…"
        case .connected:    return "연결됨"
        case .reconnecting: return "재연결 중…"
        }
    }
}

// MARK: - 디버그 로그 한 줄

struct BLELogLine: Identifiable {
    let id = UUID()
    let at: Date
    let text: String

    var timeText: String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f.string(from: at)
    }
}

// MARK: - BLEManager

final class BLEManager: NSObject, ObservableObject {

    /// iOS 라이브 탭 / watchOS 화면 / 워크아웃 세션이 모두 같은 연결을 공유한다.
    static let shared = BLEManager()

    // ── 외부에 노출되는 상태 ──────────────────────────────────────────────
    @Published private(set) var state: BLEConnectionState = .idle
    /// 마지막으로 수신한 값. 연결이 끊겨도 지우지 않는다(회색으로 계속 표시).
    @Published private(set) var sample: TelemetrySample?
    /// 지금 값이 살아있는지. false 면 뷰에서 회색 처리.
    @Published private(set) var isLive = false
    @Published private(set) var rssi: Int?
    /// 실측 notify 수신율 (Hz). 펌웨어 기대치는 4 Hz.
    @Published private(set) var packetRateHz: Double = 0
    @Published private(set) var lastPacketAt: Date?
    @Published private(set) var bluetoothPoweredOn = false
    @Published private(set) var log: [BLELogLine] = []

    /// 연결이 끊긴 뒤 지금까지 경과 시간(초). 재연결 소요시간 측정용.
    @Published private(set) var disconnectedFor: TimeInterval = 0

    /// 고정된 모듈. nil 이면 설정 화면에서 골라야 한다.
    @Published private(set) var pinnedModule: ModulePin?
    /// 선택 화면에 보여줄, 지금 주변에서 광고 중인 모듈들.
    @Published private(set) var discoveredModules: [DiscoveredModule] = []

    var hasPinnedModule: Bool { pinnedModule != nil }

    // ── 내부 ──────────────────────────────────────────────────────────────
    private enum Mode { case discovering, bound }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetryChar: CBCharacteristic?
    private var housekeeping: Timer?
    private var disconnectedAt: Date?
    private var emaInterval: TimeInterval = 0
    private var tickCount = 0
    private var mode: Mode = .discovering
    private var discovered: [UUID: DiscoveredModule] = [:]

    /// 값이 이 시간 이상 안 들어오면 "살아있지 않음"으로 본다. (4 Hz 기대 → 여유 8배)
    private let stallTimeout: TimeInterval = 2.0

    private override init() {
        super.init()
        pinnedModule = ModulePinStore.load()
        mode = pinnedModule == nil ? .discovering : .bound
        central = CBCentralManager(delegate: self, queue: .main)
        startHousekeeping()
    }

    // MARK: 공개 API — 수명주기

    /// 앱 시작 시 한 번 호출. (블루투스가 아직 안 켜졌으면 poweredOn 콜백에서 이어서 진행)
    func start() {
        if let pin = pinnedModule {
            appendLog("start() — 고정 모듈 \(pin.fullName) (module_id \(pin.moduleID))")
        } else {
            appendLog("start() — 고정된 모듈 없음, 탐색 모드")
        }
        resume()
    }

    /// 앱이 다시 foreground/active 상태가 될 때 호출.
    func appBecameActive() {
        appendLog("앱 active — 상태 \(state.rawValue)")
        guard state != .connected else { return }
        resume()
    }

    /// 디버그용 강제 재연결.
    func forceReconnect() {
        appendLog("수동 재연결 요청")
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        resume()
    }

    func clearLog() { log.removeAll() }

    // MARK: 공개 API — 모듈 선택

    /// 설정 화면에서 모듈을 골랐을 때.
    func selectModule(_ module: DiscoveredModule) {
        let pin = ModulePin(fullName: module.fullName,
                            moduleID: module.moduleID ?? 0,
                            peripheralID: module.peripheralID,
                            pinnedAt: Date())
        ModulePinStore.save(pin)
        pinnedModule = pin
        appendLog("모듈 고정 — \(pin.fullName) (module_id \(pin.moduleID))")

        stopScanIfNeeded()
        discovered.removeAll()
        discoveredModules = []
        mode = .bound
        resume()
    }

    /// "다른 모듈에 붙기" — 고정을 풀고 다시 탐색 모드로.
    func unpinModule() {
        appendLog("모듈 고정 해제 — 탐색 모드로 복귀")
        ModulePinStore.clear()
        pinnedModule = nil

        if let p = peripheral, p.state == .connected || p.state == .connecting {
            central.cancelPeripheralConnection(p)
        }
        peripheral = nil
        telemetryChar = nil
        sample = nil
        isLive = false
        rssi = nil
        packetRateHz = 0
        emaInterval = 0
        lastPacketAt = nil
        disconnectedAt = nil
        disconnectedFor = 0

        mode = .discovering
        resume()
    }

    /// 선택 화면을 열 때 — 탐색 스캔을 (다시) 돌린다.
    func refreshDiscovery() {
        guard mode == .discovering else { return }
        discovered.removeAll()
        discoveredModules = []
        stopScanIfNeeded()
        startScan()
    }

    // MARK: 연결 절차

    private func resume() {
        guard central.state == .poweredOn else {
            state = .idle
            return
        }

        switch mode {
        case .discovering:
            state = .choosing
            startScan()

        case .bound:
            guard let pin = pinnedModule else { mode = .discovering; resume(); return }

            // 1) 시스템이 이미 연결해 둔 주변장치가 있으면 그걸 바로 쓴다.
            if peripheral == nil,
               let already = central.retrieveConnectedPeripherals(
                   withServices: [SailProtocol.serviceUUID]).first {
                appendLog("이미 연결된 주변장치 발견 — \(already.identifier.uuidString.prefix(8))")
                adopt(already)
                if already.state == .connected {
                    handleConnected(already)
                    return
                }
            }

            // 2) 고정해 둔 주변장치를 UUID 로 되살린다.
            //    스캔을 기다리지 않고 바로 connect 를 걸 수 있어 재실행 시 복구가 빠르다.
            if peripheral == nil, let saved = pin.peripheralID,
               let restored = central.retrievePeripherals(withIdentifiers: [saved]).first {
                appendLog("고정 모듈 복원 — \(saved.uuidString.prefix(8))")
                adopt(restored)
            }

            // 3) pending connect (CoreBluetooth 의 connect 는 타임아웃이 없다)
            if let p = peripheral, p.state != .connected {
                state = (disconnectedAt != nil) ? .reconnecting : .connecting
                appendLog("connect() 요청 — pending 유지")
                central.connect(p, options: nil)
            }

            // 4) 병행 스캔. 저장된 UUID 가 더 이상 유효하지 않은 경우까지 커버한다.
            startScan()
        }
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        guard !central.isScanning else { return }

        if mode == .bound, state != .reconnecting, state != .connecting {
            state = .scanning
        }

        // 두 모드 모두 duplicates 를 켠다.
        //  · discovering: RSSI 와 속도를 실시간으로 갱신해 보여주려고
        //  · bound: 첫 콜백에 이름이 아직 안 붙어 있을 수 있어 재시도 기회가 필요해서
        appendLog("스캔 시작 (\(mode == .bound ? "고정 모듈 탐색" : "모듈 목록"))")
        central.scanForPeripherals(
            withServices: [SailProtocol.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    private func stopScanIfNeeded() {
        if central.isScanning { central.stopScan() }
    }

    private func adopt(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
    }

    private func handleConnected(_ p: CBPeripheral) {
        stopScanIfNeeded()
        state = .connected
        if let since = disconnectedAt {
            appendLog(String(format: "재연결 완료 — %.2f초 걸림", Date().timeIntervalSince(since)))
        } else {
            appendLog("연결 완료")
        }
        disconnectedAt = nil
        disconnectedFor = 0

        // 지름길로 붙었을 수도 있으니 식별자를 갱신해 둔다.
        if var pin = pinnedModule, pin.peripheralID != p.identifier {
            pin.peripheralID = p.identifier
            pinnedModule = pin
            ModulePinStore.save(pin)
        }

        p.discoverServices([SailProtocol.serviceUUID])
        p.readRSSI()
    }

    /// 엉뚱한 모듈에 붙었을 때 — 끊고 지름길을 버린 뒤 다시 찾는다.
    private func rejectWrongModule(_ p: CBPeripheral, got: UInt8, want: UInt8) {
        appendLog("⚠︎ 다른 모듈에 붙음 (module_id \(got), 기대 \(want)) — 끊고 재탐색")

        if var pin = pinnedModule {
            pin.peripheralID = nil // 잘못된 지름길 폐기
            pinnedModule = pin
            ModulePinStore.save(pin)
        }
        telemetryChar = nil
        isLive = false
        peripheral = nil
        central.cancelPeripheralConnection(p)
        state = .scanning
        startScan()
    }

    /// 광고가 고정한 모듈의 것인지. 판단할 근거가 아직 없으면 nil.
    private func matchesPin(name: String?, moduleID: UInt8?) -> Bool? {
        guard let pin = pinnedModule else { return nil }
        if let name { return name == pin.fullName }
        if let moduleID, pin.moduleID != 0 { return moduleID == pin.moduleID }
        return nil // 이름도 manufacturer data 도 아직 없음 → 다음 콜백을 기다린다
    }

    // MARK: 주기 작업 (RSSI 폴링 / 정체 감지 / 목록 정리)

    private func startHousekeeping() {
        housekeeping?.invalidate()
        let t = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.tick()
        }
        RunLoop.main.add(t, forMode: .common)
        housekeeping = t
    }

    private func tick() {
        tickCount &+= 1

        // 타이머는 0.5초마다 돌지만 RSSI 는 1초에 한 번만 읽는다.
        if tickCount % 2 == 0,
           state == .connected, let p = peripheral, p.state == .connected {
            p.readRSSI()
        }

        // 값이 멈췄으면 라이브 해제
        if isLive, let last = lastPacketAt, Date().timeIntervalSince(last) > stallTimeout {
            isLive = false
            packetRateHz = 0
            appendLog("데이터 정체 — \(String(format: "%.1f", stallTimeout))초 동안 수신 없음")
        }

        if let since = disconnectedAt {
            disconnectedFor = Date().timeIntervalSince(since)
        }

        // 탐색 모드에서는 목록을 주기적으로 갱신(오래된 항목 정리 + RSSI 반영)
        if mode == .discovering {
            publishDiscovered()
        }
    }

    private func publishDiscovered() {
        let cutoff = Date().addingTimeInterval(-10)
        discovered = discovered.filter { $0.value.lastSeen > cutoff }
        discoveredModules = discovered.values.sorted { $0.rssi > $1.rssi } // 가까운 순
    }

    // MARK: 로그

    private func appendLog(_ text: String) {
        log.append(BLELogLine(at: Date(), text: text))
        if log.count > 300 { log.removeFirst(log.count - 300) }
        #if DEBUG
        print("[BLE] \(text)")
        #endif
    }

    private func stateName(_ s: CBManagerState) -> String {
        switch s {
        case .poweredOn:    return "poweredOn"
        case .poweredOff:   return "poweredOff"
        case .unauthorized: return "unauthorized"
        case .unsupported:  return "unsupported"
        case .resetting:    return "resetting"
        case .unknown:      return "unknown"
        @unknown default:   return "?"
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        bluetoothPoweredOn = central.state == .poweredOn
        appendLog("central 상태 → \(stateName(central.state))")

        switch central.state {
        case .poweredOn:
            resume()
        default:
            state = .idle
            isLive = false
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {

        // 서비스 UUID 로 필터링해서 스캔하므로 오디오 기기 등은 여기까지 오지 않는다.
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
                   ?? peripheral.name
        if let name = advName, !SailProtocol.isSailName(name) { return }

        // 광고에 실린 텔레메트리(있으면). 연결 없이도 module_id 와 속도를 알 수 있다.
        var advSample: TelemetrySample?
        if let mfg = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
            advSample = TelemetrySample.decodeManufacturerData(mfg)
        }

        switch mode {
        case .discovering:
            // 이름을 모르면 목록에 올릴 수 없다(사용자가 고를 수가 없으므로).
            guard let name = advName else { return }
            var entry = discovered[peripheral.identifier]
                ?? DiscoveredModule(peripheralID: peripheral.identifier,
                                    fullName: name,
                                    moduleID: nil,
                                    rssi: RSSI.intValue,
                                    lastSeen: Date(),
                                    sample: nil)
            entry.fullName = name
            entry.rssi = RSSI.intValue
            entry.lastSeen = Date()
            if let s = advSample {
                entry.moduleID = s.moduleID
                entry.sample = s
            }
            discovered[peripheral.identifier] = entry

        case .bound:
            switch matchesPin(name: advName, moduleID: advSample?.moduleID) {
            case .some(false):
                return // 다른 배의 모듈 — 무시
            case .none:
                return // 아직 판단 불가 — duplicates 로 곧 다시 온다
            case .some(true):
                break
            }

            appendLog("고정 모듈 발견 — \(advName ?? "?") RSSI \(RSSI.intValue)")
            rssi = RSSI.intValue
            adopt(peripheral)
            stopScanIfNeeded()

            if peripheral.state != .connected {
                state = (disconnectedAt != nil) ? .reconnecting : .connecting
                central.connect(peripheral, options: nil)
            }
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        appendLog("didConnect")
        adopt(peripheral)
        handleConnected(peripheral)
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        appendLog("didFailToConnect — \(error?.localizedDescription ?? "이유 없음") → 즉시 재시도")
        state = .reconnecting
        if disconnectedAt == nil { disconnectedAt = Date() }
        central.connect(peripheral, options: nil) // pending 유지
        startScan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        appendLog("didDisconnect — \(error?.localizedDescription ?? "정상 종료")")

        telemetryChar = nil
        isLive = false
        packetRateHz = 0
        emaInterval = 0
        rssi = nil

        // 고정 해제로 인한 끊김이면 재연결하지 않는다.
        guard mode == .bound, pinnedModule != nil else { return }

        state = .reconnecting
        disconnectedAt = Date()
        disconnectedFor = 0

        // ★ 재연결의 핵심: 타임아웃 없는 pending connect 를 즉시 걸어둔다.
        //   ESP32 가 다시 광고를 시작하는 순간 시스템이 알아서 붙여준다.
        central.connect(peripheral, options: nil)
        // 저장된 식별자가 무효해진 경우까지 대비해 스캔도 함께 돌린다.
        startScan()
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            appendLog("서비스 탐색 실패 — \(error.localizedDescription)")
            return
        }
        guard let svc = peripheral.services?.first(where: { $0.uuid == SailProtocol.serviceUUID })
        else {
            appendLog("서비스 없음 — 연결 해제")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        appendLog("서비스 발견 → characteristic 탐색")
        peripheral.discoverCharacteristics([SailProtocol.telemetryUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error {
            appendLog("characteristic 탐색 실패 — \(error.localizedDescription)")
            return
        }
        guard let chr = service.characteristics?
            .first(where: { $0.uuid == SailProtocol.telemetryUUID }) else {
            appendLog("telemetry characteristic 없음")
            return
        }
        telemetryChar = chr
        appendLog("notify 구독 요청")
        peripheral.setNotifyValue(true, for: chr)
        peripheral.readValue(for: chr) // 첫 값을 곧바로 채운다
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            appendLog("notify 설정 실패 — \(error.localizedDescription)")
        } else {
            appendLog("notify \(characteristic.isNotifying ? "ON" : "OFF")")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil, let data = characteristic.value else { return }

        guard let decoded = TelemetrySample.decodeTelemetryPacket(data) else {
            appendLog("디코딩 실패 — \(data.count)바이트: \(data.map { String(format: "%02X", $0) }.joined(separator: " "))")
            return
        }

        // ③ 최종 방어선 — 매 패킷마다 "내 모듈이 맞나" 확인한다.
        //    지름길(peripheralID)로 붙었을 때 엉뚱한 기기일 가능성을 여기서 차단한다.
        if let pin = pinnedModule, pin.moduleID != 0, decoded.moduleID != pin.moduleID {
            rejectWrongModule(peripheral, got: decoded.moduleID, want: pin.moduleID)
            return
        }

        // 고정 당시 module_id 를 몰랐다면(광고를 못 읽은 경우) 지금 채워 넣는다.
        if var pin = pinnedModule, pin.moduleID == 0 {
            pin.moduleID = decoded.moduleID
            pinnedModule = pin
            ModulePinStore.save(pin)
            appendLog("module_id 확정 — \(decoded.moduleID)")
        }

        let now = decoded.receivedAt
        if let last = lastPacketAt {
            let dt = now.timeIntervalSince(last)
            if dt > 0.001 && dt < 5 {
                emaInterval = emaInterval == 0 ? dt : (emaInterval * 0.8 + dt * 0.2)
                packetRateHz = 1.0 / emaInterval
            }
        }
        lastPacketAt = now
        sample = decoded
        isLive = true
        if state != .connected { state = .connected }
    }

    func peripheral(_ peripheral: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        guard error == nil else { return }
        rssi = RSSI.intValue
    }
}
