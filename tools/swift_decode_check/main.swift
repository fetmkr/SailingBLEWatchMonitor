//
//  main.swift
//  펌웨어(C++) 인코더 ↔ 앱(Swift) 디코더 교차 검증기.
//
//  firmware-rak/tools/proto_test.cpp 가 뽑아낸 골든 벡터를
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
    // sog·cog·heel 의 기대값은 "nil" 일 수 있어서 문자열로 들고 있는다.
    // 숫자 하나를 "값 없음" 표시로 쓰면 안 된다 — 힐 -1도 는 실제로 나오는
    // 값이라 -1 을 표시로 쓰면 겹친다. (실제로 겹쳐서 걸렸다)
    guard let battRaw = Int(cols[5]), let seqRaw = Int(cols[6]),
          let uptimeRaw = Int(cols[7]), let moduleRaw = Int(cols[8]) else {
        fail(lineNo, "기대값 파싱 실패")
        continue
    }

    let sample: TelemetrySample?
    switch kind {
    case "gatt":
        guard data.count == SailProtocol.telemetryLength else {
            fail(lineNo, "gatt 길이가 \(SailProtocol.telemetryLength) 이 아님: \(data.count)")
            continue
        }
        sample = TelemetrySample.decodeTelemetryPacket(data)
    case "mfg":
        guard data.count == 2 + SailProtocol.manufacturerPayloadLength else {
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

    // 기대값이 "nil" 이면 값이 없어야 한다는 뜻이다.
    //
    // GPS 가 위성을 놓친 순간을 벡터에 섞어 두고, 앱이 그걸 0 이 아니라
    // **없음(nil)** 으로 읽는지 여기서 확인한다. 0 으로 읽으면 정박 중과
    // 구별이 안 되고, 배에서 그건 위험하다.
    //
    // 물리량은 부동소수 나눗셈을 거치므로 원시값으로 되돌려 정수로 비교한다.
    func checkField(_ name: String, _ actual: Int?, _ expected: String) {
        if expected == "nil" {
            if let got = actual { fail(lineNo, "\(name) 은 값이 없어야 하는데 \(got) 이 나옴") }
            return
        }
        guard let want = Int(expected) else {
            fail(lineNo, "\(name) 기대값 파싱 실패: \(expected)")
            return
        }
        guard let got = actual else {
            fail(lineNo, "\(name) 이 없다고 디코딩됨 (무효 표식이 아닌데)")
            return
        }
        if got != want { fail(lineNo, "\(name) \(got) ≠ \(want)") }
    }

    checkField("sog",  s.sogKnots.map   { Int(($0 * 100).rounded()) }, cols[2])
    checkField("cog",  s.cogDegrees.map { Int(($0 * 10).rounded()) },  cols[3])
    checkField("heel", s.heelDegrees,                                  cols[4])

    if s.batteryPercent != battRaw { fail(lineNo, "batt \(s.batteryPercent) ≠ \(battRaw)") }
    if s.version != SailProtocol.version { fail(lineNo, "ver \(s.version)") }
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

// 값 없음 표식 — 0 이나 지어낸 값이 아니라 nil 로 와야 한다 (PROTOCOL.md §2.1)
do {
    // ver=1, mid=1, uptime=0, sog=FFFF, cog=FFFF, heel=80(-128), batt=50
    let d = Data([0x01, 0x01, 0,0,0,0, 0xFF,0xFF, 0xFF,0xFF, 0x80, 50])
    if let s = TelemetrySample.decodeTelemetryPacket(d) {
        if s.sogKnots != nil    { fail(0, "sog 무효 표식이 nil 이 아님: \(s.sogKnots!)") }
        if s.cogDegrees != nil  { fail(0, "cog 무효 표식이 nil 이 아님: \(s.cogDegrees!)") }
        if s.heelDegrees != nil { fail(0, "heel 무효 표식이 nil 이 아님: \(s.heelDegrees!)") }
        if s.batteryPercent != 50 { fail(0, "무효 표식 옆의 batt 가 깨짐") }
        if s.sogText != "—.—"   { fail(0, "sogText 가 대시가 아님: \(s.sogText)") }
        print("  [ OK ] 값 없음 표식 → nil, 표시 문자열은 대시")
    } else {
        fail(0, "무효 표식이 든 패킷을 통째로 거부함")
    }
}

// 확장 패킷의 배터리 전압 — 뒤에 덧붙인 필드라 있을 수도, 없을 수도 있다.
//
// 골든 벡터는 12바이트와 광고 9바이트만 다룬다. 확장 패킷의 꼬리는 여기서 본다.
// 펌웨어 쪽 proto_test.cpp 가 "3.888 V → [37..38] 에 3888" 을 확인하므로,
// 두 시험을 합치면 전선 위의 약속이 양쪽에서 맞는다.
do {
    // [0..11] 기본 + [12..36] 9축 (값은 0 이어도 상관없다) + [37..38] 전압
    var ext = Data([0x01, 0x01, 0,0,0,0, 0x29,0x02, 0x4E,0x0C, 0xF4, 0x57])
    ext.append(Data(repeating: 0, count: 25))          // → 37바이트
    if let s = TelemetrySample.decodeTelemetryPacket(ext) {
        if s.extra == nil { failures.append("  37바이트인데 확장 필드가 안 읽힘") }
        if s.batteryVolts != nil {
            failures.append("  37바이트인데 전압이 나옴: \(s.batteryVolts!)")
        }
        if s.batteryText != "87%" {
            failures.append("  전압 없을 때 표시가 퍼센트만이 아님: \(s.batteryText)")
        }
        print("  [ OK ] 37바이트 확장 패킷 → 9축은 읽고 전압은 없음")
    } else {
        failures.append("  37바이트 확장 패킷 디코딩이 nil")
    }

    ext.append(contentsOf: [0x30, 0x0F])               // 3888 mV LE → 39바이트
    if let s = TelemetrySample.decodeTelemetryPacket(ext) {
        if s.batteryVolts != 3.888 {
            failures.append("  전압 \(String(describing: s.batteryVolts)) ≠ 3.888")
        }
        if s.batteryText != "87% · 3.89V" {
            failures.append("  배터리 표시가 어긋남: \(s.batteryText)")
        }
        print("  [ OK ] 39바이트 확장 패킷 → 3.888 V, 표시 \"\(s.batteryText)\"")
    } else {
        failures.append("  39바이트 확장 패킷 디코딩이 nil")
    }

    // 0 mV 는 "아직 못 잼" 이다. 0.00 V 로 보여주면 안 된다.
    var noVolt = ext
    noVolt[noVolt.count - 2] = 0
    noVolt[noVolt.count - 1] = 0
    if let s = TelemetrySample.decodeTelemetryPacket(noVolt), s.batteryVolts != nil {
        failures.append("  0 mV 가 값으로 읽힘: \(s.batteryVolts!)")
    }
}

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
if SailProtocol.namePrefix != "SAIL-" {
    failures.append("  namePrefix 가 \"SAIL-\" 가 아님: \(SailProtocol.namePrefix)")
}
if SailProtocol.maxFullNameLength != 16 {
    failures.append("  maxFullNameLength 가 16 이 아님: \(SailProtocol.maxFullNameLength)")
}
let nameCases: [(String, Bool, String)] = [
    ("SAIL-hojun", true,  "hojun"),
    ("SAIL-A3F2",  true,  "A3F2"),
    ("SAIL-",      true,  ""),
    ("AirPods Pro", false, "AirPods Pro"),
    ("sail-lower", false, "sail-lower"),   // 대소문자 구분
]
for (name, expectMatch, expectUser) in nameCases {
    if SailProtocol.isSailName(name) != expectMatch {
        failures.append("  isSailName(\(name)) = \(SailProtocol.isSailName(name)), 기대 \(expectMatch)")
    }
    if SailProtocol.userName(from: name) != expectUser {
        failures.append("  userName(\(name)) = \(SailProtocol.userName(from: name)), 기대 \(expectUser)")
    }
}
// 최대 길이 이름이 scan response 예산(31) 안에 들어가는지
let maxNameAD = 2 + SailProtocol.maxFullNameLength           // [len][type][name]
let mfgAD     = 2 + 2 + SailProtocol.manufacturerPayloadLength // [len][type][company][payload]
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
