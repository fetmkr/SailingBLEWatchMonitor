//
//  ModulePin.swift
//  "내가 붙을 모듈" 을 고정(pin)해 두는 저장소 — iOS / watchOS 공용
//
//  경기장에는 같은 서비스 UUID 를 광고하는 모듈이 여러 대 있을 수 있다.
//  먼저 발견된 것에 붙어버리면 옆 배 모듈을 보게 되므로,
//  사용자가 한 번 고른 모듈을 저장해 두고 그 모듈에만 붙는다.
//

import Foundation

// MARK: - 고정된 모듈

struct ModulePin: Codable, Equatable {
    /// 광고 이름 전체. 예) `SAIL-hojun`
    var fullName: String
    /// 패킷마다 실려오는 1바이트 식별자. 연결 후 검증에 쓴다.
    var moduleID: UInt8
    /// 이 기기 기준의 주변장치 식별자. 스캔을 건너뛰고 바로 connect 하기 위한 지름길.
    /// (iOS 가 기기별로 부여하므로 아이폰과 워치에서 서로 다른 값이 나온다)
    var peripheralID: UUID?
    var pinnedAt: Date

    /// 화면에 보여줄 짧은 이름. `SAIL-hojun` → `hojun`
    var displayName: String { SailProtocol.userName(from: fullName) }
}

// MARK: - 발견된 모듈 (선택 화면용)

struct DiscoveredModule: Identifiable, Equatable {
    let peripheralID: UUID
    var fullName: String
    /// 광고의 Manufacturer Data 에서 읽은 값. 아직 못 읽었으면 nil.
    var moduleID: UInt8?
    var rssi: Int
    var lastSeen: Date
    /// 광고에 실려온 최신 텔레메트리 (연결 없이도 속도가 보인다)
    var sample: TelemetrySample?

    var id: UUID { peripheralID }
    var displayName: String { SailProtocol.userName(from: fullName) }
    var isStale: Bool { Date().timeIntervalSince(lastSeen) > 5.0 }

    /// 신호 세기 막대 (0…4)
    var signalBars: Int {
        switch rssi {
        case (-55)...:      return 4
        case (-67)...(-56): return 3
        case (-80)...(-68): return 2
        case (-90)...(-81): return 1
        default:            return 0
        }
    }
}

// MARK: - 저장소

enum ModulePinStore {
    private static let key = "sail.pinnedModule"

    static func load() -> ModulePin? {
        guard let data = UserDefaults.standard.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(ModulePin.self, from: data)
    }

    static func save(_ pin: ModulePin) {
        guard let data = try? JSONEncoder().encode(pin) else { return }
        UserDefaults.standard.set(data, forKey: key)
    }

    static func clear() {
        UserDefaults.standard.removeObject(forKey: key)
    }
}
