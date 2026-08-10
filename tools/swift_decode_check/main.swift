//
//  main.swift
//  펌웨어(C++) 인코더 ↔ 앱(Swift) 디코더 교차 검증기.
//
//  firmware/tools/sim_test.cpp 가 뽑아낸 골든 벡터를
//  app/Shared/Protocol.swift 의 디코더로 해석해 값이 일치하는지 본다.
//  두 구현이 어긋나면 여기서 잡힌다.
//
//  빌드/실행은 tools/verify.sh 가 담당.
//

import Foundation

// MARK: - 입력

guard CommandLine.arguments.count > 1 else {
    FileHandle.standardError.write("usage: swift_decode_check <vectors.tsv>\n".data(using: .utf8)!)
    exit(2)
}

let path = CommandLine.arguments[1]
guard let text = try? String(contentsOfFile: path, encoding: .utf8) else {
    FileHandle.standardError.write("벡터 파일을 읽을 수 없음: \(path)\n".data(using: .utf8)!)
    exit(2)
}

func hexToData(_ hex: String) -> Data? {
    guard hex.count % 2 == 0 else { return nil }
    var out = Data(capacity: hex.count / 2)
    var idx = hex.startIndex
    while idx < hex.endIndex {
        let next = hex.index(idx, offsetBy: 2)
        guard let byte = UInt8(hex[idx..<next], radix: 16) else { return nil }
        out.append(byte)
        idx = next
    }
    return out
}

// MARK: - 검증

var checked = 0
var failures: [String] = []

func fail(_ line: Int, _ message: String) {
    failures.append("  line \(line): \(message)")
}

for (i, raw) in text.split(separator: "\n", omittingEmptySubsequences: true).enumerated() {
    let lineNo = i + 1
    if raw.hasPrefix("#") { continue }

    let cols = raw.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard cols.count == 9 else {
        fail(lineNo, "컬럼 수가 9가 아님 (\(cols.count))")
        continue
    }

    let kind = cols[0]
    guard let data = hexToData(cols[1]) else {
        fail(lineNo, "hex 파싱 실패: \(cols[1])")
        continue
    }
    guard let sogRaw = Int(cols[2]), let cogRaw = Int(cols[3]),
          let heelRaw = Int(cols[4]), let battRaw = Int(cols[5]),
          let seqRaw = Int(cols[6]), let uptimeRaw = Int(cols[7]),
          let moduleRaw = Int(cols[8]) else {
        fail(lineNo, "기대값 파싱 실패")
        continue
    }

    let sample: TelemetrySample?
    switch kind {
    case "gatt":
        guard data.count == HohoProtocol.telemetryLength else {
            fail(lineNo, "gatt 길이가 \(HohoProtocol.telemetryLength) 이 아님: \(data.count)")
            continue
        }
        sample = TelemetrySample.decodeTelemetryPacket(data)
    case "mfg":
        guard data.count == 2 + HohoProtocol.manufacturerPayloadLength else {
            fail(lineNo, "mfg 길이가 11 이 아님: \(data.count)")
            continue
        }
        sample = TelemetrySample.decodeManufacturerData(data)
    default:
        fail(lineNo, "알 수 없는 kind: \(kind)")
        continue
    }

    guard let s = sample else {
        fail(lineNo, "\(kind) 디코딩이 nil 을 반환 — \(cols[1])")
        continue
    }

    checked += 1

    // 물리량은 부동소수 나눗셈을 거치므로 원시값으로 되돌려 정수 비교한다.
    let sogBack = Int((s.sogKnots * 100).rounded())
    let cogBack = Int((s.cogDegrees * 10).rounded())

    if sogBack != sogRaw   { fail(lineNo, "sog \(sogBack) ≠ \(sogRaw)") }
    if cogBack != cogRaw   { fail(lineNo, "cog \(cogBack) ≠ \(cogRaw)") }
    if s.heelDegrees != heelRaw { fail(lineNo, "heel \(s.heelDegrees) ≠ \(heelRaw)") }
    if s.batteryPercent != battRaw { fail(lineNo, "batt \(s.batteryPercent) ≠ \(battRaw)") }
    if s.version != HohoProtocol.version { fail(lineNo, "ver \(s.version)") }
    if s.moduleID != UInt8(moduleRaw) { fail(lineNo, "module_id \(s.moduleID) ≠ \(moduleRaw)") }

    if kind == "gatt" {
        if s.uptimeMs != UInt32(uptimeRaw) {
            fail(lineNo, "uptime \(String(describing: s.uptimeMs)) ≠ \(uptimeRaw)")
        }
        if s.sequence != nil { fail(lineNo, "gatt 인데 seq 가 채워짐") }
    } else {
        if s.sequence != UInt8(seqRaw) {
            fail(lineNo, "seq \(String(describing: s.sequence)) ≠ \(seqRaw)")
        }
        if s.uptimeMs != nil { fail(lineNo, "mfg 인데 uptime 이 채워짐") }
    }
}

// MARK: - 방어적 케이스

func expectNil(_ label: String, _ value: TelemetrySample?) {
    if value != nil { failures.append("  \(label): nil 이어야 하는데 값이 나옴") }
}
func expectSome(_ label: String, _ value: TelemetrySample?) {
    if value == nil { failures.append("  \(label): 값이 나와야 하는데 nil") }
}

print("\n── 잘못된 입력 방어 ──")

expectNil("짧은 gatt(11바이트)", TelemetrySample.decodeTelemetryPacket(Data(repeating: 1, count: 11)))
expectNil("버전 불일치 gatt",
          TelemetrySample.decodeTelemetryPacket(Data([0x02, 0x01, 0,0,0,0, 0,0, 0,0, 0, 0])))
expectSome("긴 gatt(14바이트, 전방호환)",
           TelemetrySample.decodeTelemetryPacket(Data([0x01, 0x01, 0,0,0,0, 0x29,0x02, 0x4E,0x0C, 0xF4, 0x57, 0xAA, 0xBB])))
expectNil("Company ID 불일치 mfg",
          TelemetrySample.decodeManufacturerData(Data([0xAB,0xCD, 0x01,0x01, 0,0, 0,0, 0, 0, 0])))
expectNil("짧은 mfg(10바이트)", TelemetrySample.decodeManufacturerData(Data(repeating: 0xFF, count: 10)))

// Data 슬라이스(startIndex != 0)에서도 올바르게 읽히는지 — 흔한 함정
let padded = Data([0xDE, 0xAD, 0xBE, 0xEF])
    + Data([0x01, 0x01, 0x40, 0xE2, 0x01, 0x00, 0x29, 0x02, 0x4E, 0x0C, 0xF4, 0x57])
let slice = padded[4...]
if let s = TelemetrySample.decodeTelemetryPacket(slice) {
    if s.sogKnots != 5.53 || s.cogDegrees != 315.0 || s.heelDegrees != -12 || s.batteryPercent != 87 {
        failures.append("  Data 슬라이스 디코딩 값 불일치: \(s)")
    } else {
        print("  [ OK ] Data 슬라이스(startIndex=4) 정상 디코딩")
    }
} else {
    failures.append("  Data 슬라이스 디코딩이 nil")
}

// compassPoint
let compassCases: [(Double, String)] = [(0, "N"), (45, "NE"), (90, "E"), (180, "S"),
                                        (315, "NW"), (359, "N"), (-10, "N")]
for (deg, expect) in compassCases where compassPoint(deg) != expect {
    failures.append("  compassPoint(\(deg)) = \(compassPoint(deg)), 기대 \(expect)")
}

// 모듈 이름 규칙 — 펌웨어의 kNamePrefix / kMaxFullNameLen 과 맞아야 한다
print("\n── 모듈 이름 규칙 ──")
if HohoProtocol.namePrefix != "HOHO-" {
    failures.append("  namePrefix 가 \"HOHO-\" 가 아님: \(HohoProtocol.namePrefix)")
}
if HohoProtocol.maxFullNameLength != 16 {
    failures.append("  maxFullNameLength 가 16 이 아님: \(HohoProtocol.maxFullNameLength)")
}
let nameCases: [(String, Bool, String)] = [
    ("HOHO-hojun", true,  "hojun"),
    ("HOHO-A3F2",  true,  "A3F2"),
    ("HOHO-",      true,  ""),
    ("AirPods Pro", false, "AirPods Pro"),
    ("hoho-lower", false, "hoho-lower"),   // 대소문자 구분
]
for (name, expectMatch, expectUser) in nameCases {
    if HohoProtocol.isHohoName(name) != expectMatch {
        failures.append("  isHohoName(\(name)) = \(HohoProtocol.isHohoName(name)), 기대 \(expectMatch)")
    }
    if HohoProtocol.userName(from: name) != expectUser {
        failures.append("  userName(\(name)) = \(HohoProtocol.userName(from: name)), 기대 \(expectUser)")
    }
}
// 최대 길이 이름이 scan response 예산(31) 안에 들어가는지
let maxNameAD = 2 + HohoProtocol.maxFullNameLength           // [len][type][name]
let mfgAD     = 2 + 2 + HohoProtocol.manufacturerPayloadLength // [len][type][company][payload]
print("  최대 이름 시 scan response = \(mfgAD + maxNameAD) 바이트 (한도 31)")
if mfgAD + maxNameAD > 31 {
    failures.append("  최대 길이 이름에서 scan response 가 31바이트를 넘음")
} else {
    print("  [ OK ] scan response 예산 통과")
}

// MARK: - 결과

print("")
print("════════════════════════════════════════════════════════")
print("  Swift 디코더 교차 검증 — 벡터 \(checked)개 검사")
if failures.isEmpty {
    print("  ✅ 펌웨어 인코더와 앱 디코더가 완전히 일치")
    print("════════════════════════════════════════════════════════")
    exit(0)
} else {
    print("  ❌ 불일치 \(failures.count)건")
    for f in failures.prefix(30) { print(f) }
    print("════════════════════════════════════════════════════════")
    exit(1)
}
