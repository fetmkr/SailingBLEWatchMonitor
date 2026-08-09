//
//  BLEManager.swift
//  HOHO-01 텔레메트리 수신기 — iOS / watchOS 공용
//
//  하는 일
//    · Service UUID 로 스캔 → 발견 시 자동 연결 → telemetry characteristic notify 구독
//    · 끊기면 즉시 재연결 (pending connect + 병행 스캔, 타임아웃 없음)
//    · 끊긴 동안 마지막 값을 유지하되 isLive = false 로 표시(뷰에서 회색 처리)
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
    case scanning      // 광고 탐색 중
    case connecting    // 최초 연결 시도 중
    case connected     // 연결 + notify 수신 중
    case reconnecting  // 연결이 끊겨 재연결 대기 중 (pending connect 유지)

    var displayText: String {
        switch self {
        case .idle:         return "대기"
        case .scanning:     return "검색 중…"
        case .connecting:   return "연결 중…"
        case .connected:    return "연결됨"
        case .reconnecting: return "재연결 중…"
        }
    }

    var isUsable: Bool { self == .connected }
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

    // ── 내부 ──────────────────────────────────────────────────────────────
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetryChar: CBCharacteristic?
    private var housekeeping: Timer?
    private var disconnectedAt: Date?
    private var emaInterval: TimeInterval = 0
    private var tickCount = 0

    private let knownPeripheralKey = "hoho.lastPeripheralUUID"

    /// 값이 이 시간 이상 안 들어오면 "살아있지 않음"으로 본다. (4 Hz 기대 → 여유 8배)
    private let stallTimeout: TimeInterval = 2.0

    private override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
        startHousekeeping()
    }

    // MARK: 공개 API

    /// 앱 시작 시 한 번 호출. (블루투스가 아직 안 켜졌으면 poweredOn 콜백에서 이어서 진행)
    func start() {
        appendLog("start() — central.state = \(stateName(central.state))")
        beginConnectAttempt()
    }

    /// 앱이 다시 foreground/active 상태가 될 때 호출.
    /// 연결이 안 되어 있으면 즉시 재시도한다.
    func appBecameActive() {
        appendLog("앱 active — 현재 상태 \(state.rawValue)")
        guard state != .connected else { return }
        beginConnectAttempt()
    }

    /// 디버그용 강제 재연결.
    func forceReconnect() {
        appendLog("수동 재연결 요청")
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        beginConnectAttempt()
    }

    func clearLog() { log.removeAll() }

    // MARK: 연결 절차

    private func beginConnectAttempt() {
        guard central.state == .poweredOn else {
            state = .idle
            return
        }

        // 1) 시스템이 이미 연결해 둔 주변장치가 있으면 그걸 바로 쓴다.
        if peripheral == nil {
            if let already = central.retrieveConnectedPeripherals(
                withServices: [HohoProtocol.serviceUUID]).first {
                appendLog("이미 연결된 주변장치 발견 — \(already.identifier.uuidString.prefix(8))")
                adopt(already)
                if already.state == .connected {
                    handleConnected(already)
                    return
                }
            }
        }

        // 2) 지난번에 붙었던 주변장치를 UUID 로 되살린다.
        //    스캔을 기다리지 않고 바로 connect 를 걸 수 있어 재실행 시 복구가 빠르다.
        if peripheral == nil,
           let saved = UserDefaults.standard.string(forKey: knownPeripheralKey),
           let uuid = UUID(uuidString: saved),
           let restored = central.retrievePeripherals(withIdentifiers: [uuid]).first {
            appendLog("저장된 주변장치 복원 — \(saved.prefix(8))")
            adopt(restored)
        }

        // 3) pending connect (CoreBluetooth 의 connect 는 타임아웃이 없다)
        if let p = peripheral, p.state != .connected {
            state = (disconnectedAt != nil) ? .reconnecting : .connecting
            appendLog("connect() 요청 — pending 유지")
            central.connect(p, options: nil)
        }

        // 4) 병행 스캔. pending connect 와 동시에 돌려도 무방하며,
        //    ESP32 가 리셋되어 주소가 바뀌는 상황까지 커버한다.
        startScanning()
    }

    private func startScanning() {
        guard central.state == .poweredOn else { return }
        guard !central.isScanning else { return }
        if state != .reconnecting && state != .connecting { state = .scanning }
        appendLog("스캔 시작 — service \(HohoProtocol.serviceUUID.uuidString.prefix(8))")
        central.scanForPeripherals(withServices: [HohoProtocol.serviceUUID], options: nil)
    }

    private func adopt(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        UserDefaults.standard.set(p.identifier.uuidString, forKey: knownPeripheralKey)
    }

    private func handleConnected(_ p: CBPeripheral) {
        central.stopScan()
        state = .connected
        if let since = disconnectedAt {
            appendLog(String(format: "재연결 완료 — %.2f초 걸림", Date().timeIntervalSince(since)))
        } else {
            appendLog("연결 완료")
        }
        disconnectedAt = nil
        disconnectedFor = 0
        p.discoverServices([HohoProtocol.serviceUUID])
        p.readRSSI()
    }

    // MARK: 주기 작업 (RSSI 폴링 / 정체 감지 / 끊긴 시간 카운트)

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
        case .poweredOn:   return "poweredOn"
        case .poweredOff:  return "poweredOff"
        case .unauthorized: return "unauthorized"
        case .unsupported: return "unsupported"
        case .resetting:   return "resetting"
        case .unknown:     return "unknown"
        @unknown default:  return "?"
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
            beginConnectAttempt()
        default:
            state = .idle
            isLive = false
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {

        // Service UUID 로 필터링해서 스캔하므로 여기 온 것은 우리 모듈이다.
        // 이름까지 한 번 더 확인해서 다른 구현체를 잘못 잡지 않도록 한다.
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        if let name = advName, name != HohoProtocol.deviceName {
            return
        }

        appendLog("발견 — \(advName ?? peripheral.name ?? "?") RSSI \(RSSI.intValue)")
        rssi = RSSI.intValue

        adopt(peripheral)
        central.stopScan()

        if peripheral.state != .connected {
            state = (disconnectedAt != nil) ? .reconnecting : .connecting
            central.connect(peripheral, options: nil)
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
        startScanning()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        appendLog("didDisconnect — \(error?.localizedDescription ?? "정상 종료") → 즉시 connect 재요청")

        telemetryChar = nil
        isLive = false
        packetRateHz = 0
        emaInterval = 0
        rssi = nil
        state = .reconnecting
        disconnectedAt = Date()
        disconnectedFor = 0

        // ★ 재연결의 핵심: 타임아웃 없는 pending connect 를 즉시 걸어둔다.
        //   ESP32 가 다시 광고를 시작하는 순간 시스템이 알아서 붙여준다.
        central.connect(peripheral, options: nil)
        // 주소가 바뀌는 경우까지 대비해 스캔도 함께 돌린다.
        startScanning()
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            appendLog("서비스 탐색 실패 — \(error.localizedDescription)")
            return
        }
        guard let svc = peripheral.services?.first(where: { $0.uuid == HohoProtocol.serviceUUID })
        else {
            appendLog("서비스 없음 — 연결 해제")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        appendLog("서비스 발견 → characteristic 탐색")
        peripheral.discoverCharacteristics([HohoProtocol.telemetryUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error {
            appendLog("characteristic 탐색 실패 — \(error.localizedDescription)")
            return
        }
        guard let chr = service.characteristics?
            .first(where: { $0.uuid == HohoProtocol.telemetryUUID }) else {
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
