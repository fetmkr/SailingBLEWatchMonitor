//
//  Protocol.swift
//  SAIL-01 BLE 텔레메트리 프로토콜 v1 — iOS / watchOS 공용
//
//  규격 원문: ../../PROTOCOL.md
//  펌웨어 대응 파일: ../../firmware/include/protocol.h
//  ★ 셋 중 하나를 고치면 나머지도 반드시 함께 고칠 것.
//

import CoreBluetooth
import Foundation

// MARK: - 식별자 / 상수

enum SailProtocol {
    /// 모든 모듈의 광고 이름은 이 접두사로 시작한다. 예) `SAIL-hojun`
    /// 앱은 이 접두사로 "우리 모듈"을 골라낸다.
    static let namePrefix = "SAIL-"

    /// 광고에 들어갈 수 있는 전체 이름의 최대 길이 (scan response 예산에서 나온 값)
    static let maxFullNameLength = 16

    static let serviceUUID   = CBUUID(string: "B0A70001-0000-4000-8000-000000000001")
    static let telemetryUUID = CBUUID(string: "B0A70002-0000-4000-8000-000000000001")

    /// 미할당(테스트용) Company ID
    static let companyID: UInt16 = 0xFFFF
    /// 지원하는 프로토콜 버전
    static let version: UInt8 = 0x01

    /// GATT characteristic 페이로드 길이
    static let telemetryLength = 12
    /// Manufacturer Data 중 Company ID(2바이트)를 제외한 페이로드 길이
    static let manufacturerPayloadLength = 9

    /// 펌웨어가 약속한 Notify 주기 (4 Hz)
    static let notifyInterval: TimeInterval = 0.25
    /// 펌웨어가 약속한 광고 데이터 갱신 주기 (1 Hz)
    static let advertisingRefreshInterval: TimeInterval = 1.0

    /// 광고 이름이 우리 모듈의 것인지
    static func isSailName(_ name: String) -> Bool {
        name.hasPrefix(namePrefix)
    }

    /// `SAIL-hojun` → `hojun`. 접두사가 없으면 원문 그대로.
    static func userName(from fullName: String) -> String {
        guard fullName.hasPrefix(namePrefix) else { return fullName }
        return String(fullName.dropFirst(namePrefix.count))
    }
}

// MARK: - 리틀엔디언 리더
//
// Data 는 슬라이스일 때 startIndex 가 0 이 아닐 수 있다.
// 인덱스를 직접 쓰면 조용히 잘못된 값을 읽으므로 항상 이 리더를 통한다.

private struct LEReader {
    private let bytes: [UInt8]
    private var offset = 0

    init(_ data: Data) { self.bytes = [UInt8](data) }

    var remaining: Int { bytes.count - offset }

    mutating func u8() -> UInt8 {
        defer { offset += 1 }
        return bytes[offset]
    }

    mutating func i8() -> Int8 {
        Int8(bitPattern: u8())
    }

    mutating func u16() -> UInt16 {
        let lo = UInt16(bytes[offset])
        let hi = UInt16(bytes[offset + 1])
        offset += 2
        return lo | (hi << 8)
    }

    mutating func u32() -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(bytes[offset + i]) << (8 * i) }
        offset += 4
        return v
    }
}

// MARK: - 텔레메트리 샘플

/// GATT characteristic 또는 광고에서 디코딩한 한 개의 관측값.
struct TelemetrySample: Equatable {
    var version: UInt8
    var moduleID: UInt8
    /// GATT 패킷에만 존재. 광고 경로에서는 nil.
    var uptimeMs: UInt32?
    var sogKnots: Double
    var cogDegrees: Double
    var heelDegrees: Int
    var batteryPercent: Int
    /// 광고 경로에만 존재. GATT 경로에서는 nil.
    var sequence: UInt8?
    /// 앱이 이 값을 수신한 시각
    var receivedAt: Date

    var uptimeSeconds: Double? { uptimeMs.map { Double($0) / 1000.0 } }

    /// 힐 방향 표기 — 좌현(port) 음수 / 우현(starboard) 양수
    var heelSideLabel: String {
        if heelDegrees > 0 { return "STBD" }
        if heelDegrees < 0 { return "PORT" }
        return "—"
    }
}

// MARK: - 디코딩

extension TelemetrySample {

    /// GATT characteristic 12바이트 → 샘플.  PROTOCOL.md §3
    ///
    /// - 길이가 12 미만이면 nil
    /// - `ver` 이 지원 버전이 아니면 nil
    /// - 12 초과는 앞 12바이트만 사용(전방 호환)
    static func decodeTelemetryPacket(_ data: Data, at date: Date = Date()) -> TelemetrySample? {
        guard data.count >= SailProtocol.telemetryLength else { return nil }

        var r = LEReader(data)
        let ver = r.u8()
        guard ver == SailProtocol.version else { return nil }

        let moduleID = r.u8()
        let uptime   = r.u32()
        let sogRaw   = r.u16()
        let cogRaw   = r.u16()
        let heelRaw  = r.i8()
        let battRaw  = r.u8()

        return TelemetrySample(
            version: ver,
            moduleID: moduleID,
            uptimeMs: uptime,
            sogKnots: Double(sogRaw) / 100.0,
            cogDegrees: Double(cogRaw) / 10.0,
            heelDegrees: Int(heelRaw),
            batteryPercent: Int(battRaw),
            sequence: nil,
            receivedAt: date
        )
    }

    /// Manufacturer Specific Data → 샘플.  PROTOCOL.md §4.3
    ///
    /// CoreBluetooth 가 주는 `CBAdvertisementDataManufacturerDataKey` 는
    /// **Company ID 2바이트를 포함한** 전체 바이트열이다. 따라서 총 11바이트를 기대한다.
    static func decodeManufacturerData(_ data: Data, at date: Date = Date()) -> TelemetrySample? {
        let expected = 2 + SailProtocol.manufacturerPayloadLength
        guard data.count >= expected else { return nil }

        var r = LEReader(data)
        let company = r.u16()
        guard company == SailProtocol.companyID else { return nil }

        let ver = r.u8()
        guard ver == SailProtocol.version else { return nil }

        let moduleID = r.u8()
        let sogRaw   = r.u16()
        let cogRaw   = r.u16()
        let heelRaw  = r.i8()
        let battRaw  = r.u8()
        let seq      = r.u8()

        return TelemetrySample(
            version: ver,
            moduleID: moduleID,
            uptimeMs: nil,
            sogKnots: Double(sogRaw) / 100.0,
            cogDegrees: Double(cogRaw) / 10.0,
            heelDegrees: Int(heelRaw),
            batteryPercent: Int(battRaw),
            sequence: seq,
            receivedAt: date
        )
    }
}

// MARK: - 표시용 포매팅

extension TelemetrySample {
    var sogText: String  { String(format: "%.2f", sogKnots) }
    var cogText: String  { String(format: "%.1f°", cogDegrees) }
    var heelText: String { String(format: "%+d°", heelDegrees) }
}

/// 침로를 나침반 방위로. 스캐너/라이브 탭 보조 표시용.
func compassPoint(_ degrees: Double) -> String {
    let points = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
    let normalized = degrees.truncatingRemainder(dividingBy: 360)
    let positive = normalized < 0 ? normalized + 360 : normalized
    let index = Int((positive / 22.5).rounded()) % 16
    return points[index]
}
