// Sailing Monitor 텔레메트리 프로토콜 v1
// 규격 원문: ../../PROTOCOL.md  — 이 파일과 app/Shared/Protocol.swift 는 항상 함께 수정할 것.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace sail {

// ── 식별자 ───────────────────────────────────────────────────────────────
static constexpr const char* kServiceUUID   = "b0a70001-0000-4000-8000-000000000001";
static constexpr const char* kTelemetryUUID = "b0a70002-0000-4000-8000-000000000001";
// 설정 통로. 글자 한 줄을 써 넣으면 한 줄로 답한다 (PROTOCOL.md §9).
// WiFi 이름·비밀번호를 넣고, 받을 때만 WiFi 를 켜는 데 쓴다.
static constexpr const char* kControlUUID   = "b0a70003-0000-4000-8000-000000000001";

// 한 줄의 최대 길이. BLE 한 번에 들어가는 크기(기본 MTU 23 → 20바이트)보다
// 크지만, 요즘 폰·맥은 MTU 를 185 이상으로 올려 잡는다. 넘치면 잘라 보낸다.
static constexpr size_t kControlLineMax = 180;


// 광고 이름은 "SAIL-" + 사용자 지정 이름. 앱은 이 접두사로 우리 모듈을 골라낸다.
//   예) SAIL-hojun
static constexpr const char* kNamePrefix = "SAIL-";

// Scan response 예산: 31 = Manufacturer Data(13) + [len][type] + 이름
//   → 접두사 포함 전체 이름 최대 16바이트
static constexpr size_t kMaxFullNameLen = 16;
static constexpr size_t kMaxUserNameLen = kMaxFullNameLen - 5; // "SAIL-" 제외 → 11

static constexpr uint16_t kCompanyID = 0xFFFF;  // 미할당(테스트용) Company ID
static constexpr uint8_t  kVersion   = 0x01;

// ── 타이밍 ───────────────────────────────────────────────────────────────
static constexpr uint32_t kNotifyPeriodMs = 100;  // 10 Hz (런타임에 `hz` 명령으로 변경 가능)
static constexpr uint32_t kAdvRefreshMs   = 1000; // 1 Hz
static constexpr uint32_t kLogPeriodMs    = 1000; // 1 Hz 시리얼 로그
static constexpr uint16_t kAdvIntervalMs  = 200;  // 광고 인터벌
// NimBLE 광고 인터벌 단위는 0.625 ms
static constexpr uint16_t kAdvIntervalUnits = (uint16_t)(kAdvIntervalMs / 0.625f); // = 320

// ── 페이로드 크기 ────────────────────────────────────────────────────────
static constexpr size_t kTelemetryLen = 12; // GATT characteristic
static constexpr size_t kMfgLen       = 9;  // Company ID 를 제외한 manufacturer 페이로드

// ── module_id ────────────────────────────────────────────────────────────
//
// 이름은 광고(scan response)에만 실리고 GATT 패킷에는 없다.
// 연결한 뒤 "내가 맞는 모듈에 붙었나" 를 매 패킷마다 확인하려면
// 패킷 안에 들어가는 짧은 식별자가 필요하다. 그게 module_id 다.
//
// 이름에서 결정적으로 유도하므로 재부팅해도, 다시 구워도 같은 값이 나온다.
// 0 은 "미지정" 으로 예약.
inline uint8_t moduleIDFromName(const char* name) {
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* p = name; *p; ++p) {
        h ^= (uint8_t)(*p);
        h *= 16777619u;
    }
    uint8_t v = (uint8_t)((h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24)) & 0xFF);
    return v == 0 ? 1 : v;
}

// ── 값 없음 표식 ─────────────────────────────────────────────────────────
//
// GPS 가 위성을 못 잡으면 속도·침로는 "모르는 값" 이다. 0 을 보내면 배가
// 멈춘 것과 구별되지 않고, 지어낸 값을 채우면 실측과 구별되지 않는다.
// 그래서 물리적으로 나올 수 없는 값을 무효 표식으로 예약한다.
//
//   sog  0xFFFF → 655.35 kn. 배가 낼 수 있는 속도가 아니다.
//   cog  0xFFFF → 유효 범위(0…3599) 밖이다.
//   heel 0x80   → -128°. 뒤집힘을 넘어선 각도라 나올 수 없다.
//
// 확장 필드에서 이미 쓰던 방식과 같다 (hdg 0xFFFF, hdop 255).
static constexpr uint16_t kSogInvalid = 0xFFFF;
static constexpr uint16_t kCogInvalid = 0xFFFF;
static constexpr int8_t   kHeelInvalid = -128;

// ── 물리량 → 원시값 인코딩 ───────────────────────────────────────────────
inline uint16_t encodeSog(float knots) {
    if (knots < 0.0f) knots = 0.0f;
    float raw = roundf(knots * 100.0f);
    if (raw > 65535.0f) raw = 65535.0f;
    return (uint16_t)raw;
}

inline uint16_t encodeCog(float degrees) {
    float d = fmodf(degrees, 360.0f);
    if (d < 0.0f) d += 360.0f;
    uint16_t raw = (uint16_t)lroundf(d * 10.0f);
    if (raw > 3599) raw = (uint16_t)(raw % 3600); // 3600 → 0 wrap
    return raw;
}

inline int8_t encodeHeel(float degrees) {
    long v = lroundf(degrees);
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

inline uint8_t encodeBatt(float percent) {
    long v = lroundf(percent);
    if (v > 100) v = 100;
    if (v < 0) v = 0;
    return (uint8_t)v;
}

// ── 리틀엔디언 write 헬퍼 ────────────────────────────────────────────────
inline void putU16LE(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

inline void putU32LE(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// ── 텔레메트리 스냅샷 ────────────────────────────────────────────────────
struct Telemetry {
    uint8_t  moduleID = 1;
    uint32_t uptimeMs = 0;
    float    sogKn    = 0.0f;
    float    cogDeg   = 0.0f;
    float    heelDeg  = 0.0f;
    float    battPct  = 100.0f;

    // 값이 있는가. false 면 위 숫자는 의미가 없고 무효 표식이 나간다.
    //   sog·cog  GPS 가 위성을 잡았을 때만 참
    //   heel     IMU 가 살아 있을 때만 참
    bool sogValid  = true;
    bool cogValid  = true;
    bool heelValid = true;
};

// GATT characteristic 12바이트 인코딩. PROTOCOL.md §3
inline void encodeTelemetryPacket(const Telemetry& t, uint8_t out[kTelemetryLen]) {
    out[0] = kVersion;
    out[1] = t.moduleID;
    putU32LE(&out[2], t.uptimeMs);
    putU16LE(&out[6], t.sogValid ? encodeSog(t.sogKn) : kSogInvalid);
    putU16LE(&out[8], t.cogValid ? encodeCog(t.cogDeg) : kCogInvalid);
    out[10] = (uint8_t)(t.heelValid ? encodeHeel(t.heelDeg) : kHeelInvalid);
    out[11] = encodeBatt(t.battPct);
}

// ── 확장 페이로드 (37 바이트) ────────────────────────────────────────────
//
// 앞 12바이트는 위 encodeTelemetryPacket() 과 완전히 같다. 그 뒤에 9축과
// GPS 상태를 덧붙인다.
//
// ★ 버전을 올리지 않는다. PROTOCOL.md §7 이 "길면 앞부분만 파싱" 을 규정하고
//   있어서, 옛 수신 측은 이 패킷을 받아도 앞 12바이트만 읽고 그대로 돈다.
//   그래서 앱을 안 고쳐도 깨지지 않는다.
//
// ※ firmware/ (Feather TFT) 는 9축이 없으므로 12바이트만 보낸다. 그것도
//   규격에 맞다. 두 보드가 서로 다른 길이를 보내는 게 정상이다.
//
//  offset  size  type     name       단위
//  ------  ----  -------  ---------  --------------------------------------
//  [0..11]  12            (기존)     PROTOCOL.md §3 그대로
//  [12]      1   u8       flags      bit0 GPS fix / bit1 IMU / bit2 자력계
//                                    bit3 예약 (항상 0)
//  [13]      1   u8       sats       위성 수
//  [14]      1   u8       hdop       HDOP × 10.  255 = 모름
//  [15..16]  2   u16le    hdg        자력계 방위 deg × 10 (0…3599)
//                                    0xFFFF = 자력계 없음
//  [17..18]  2   i16le    pitch      deg × 10
//  [19..24]  6   i16le×3  acc XYZ    g × 1000
//  [25..30]  6   i16le×3  gyr XYZ    °/s × 10
//  [31..36]  6   i16le×3  mag XYZ    µT × 10
//  [37..38]  2   u16le    batt mV    배터리 전압 (mV). 0 = 아직 못 잼
//  ------  ----  -------  ---------  --------------------------------------
//  total    39
//
// ※ [37..38] 은 나중에 덧붙였다. 앞 37바이트는 한 글자도 안 바뀌었으므로
//   옛 앱은 그대로 돈다 (PROTOCOL.md §7 "길면 앞부분만 파싱").
//   그래서 앱 쪽은 "37 이상이면 9축, 39 이상이면 전압까지" 로 읽는다.
static constexpr size_t kTelemetryExtBaseLen = 37; // 9축까지
static constexpr size_t kTelemetryExtLen     = 39; // + 배터리 전압

struct TelemetryExtra {
    bool    gpsFix       = false;
    bool    imuOk        = false;
    bool    magOk        = false;
    uint8_t satellites   = 0;
    float   hdop         = -1.0f; // 음수면 모름
    float   headingDeg   = -1.0f; // 음수면 자력계 없음
    float   pitchDeg     = 0.0f;
    float   accX = 0, accY = 0, accZ = 0; // g
    float   gyrX = 0, gyrY = 0, gyrZ = 0; // °/s
    float   magX = 0, magY = 0, magZ = 0; // µT

    /// 배터리 전압 (V). 0 이하면 아직 못 잰 것으로 보낸다.
    ///
    /// 퍼센트만으로는 배터리를 판단할 수 없다. 리튬폴리머는 3.8~3.9 V 에서
    /// 곡선이 거의 평평해서, 전압이 0.05 V 떨어지면 퍼센트가 20 씩 내려간다.
    /// 그래서 둘을 나란히 보여준다.
    float   battVolts = 0.0f;
};

// 실수를 int16 칸에 넣는다. 범위를 벗어나면 자른다.
// 자르는 편이 조용히 뒤집히는 것보다 낫다 (예: 32768 이 -32768 이 되는 일).
inline int16_t clampToI16(float v) {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)lroundf(v);
}

inline void putI16LE(uint8_t* p, int16_t v) { putU16LE(p, (uint16_t)v); }

inline void encodeTelemetryExt(const Telemetry& t, const TelemetryExtra& e,
                               uint8_t out[kTelemetryExtLen]) {
    encodeTelemetryPacket(t, out); // 앞 12바이트는 기존과 한 글자도 다르지 않다

    uint8_t flags = 0;
    if (e.gpsFix) flags |= 0x01;
    if (e.imuOk) flags |= 0x02;
    if (e.magOk) flags |= 0x04;
    // bit3 은 예약이다. 예전에 "시뮬레이터 값" 을 뜻했지만 시뮬레이터를
    // 걷어냈다. 규격(PROTOCOL.md §3.1)이 항상 0 이라고 못 박고 있다.
    out[12] = flags;

    out[13] = e.satellites;

    if (e.hdop < 0.0f) {
        out[14] = 255; // 모름
    } else {
        long h  = lroundf(e.hdop * 10.0f);
        out[14] = (uint8_t)(h > 254 ? 254 : (h < 0 ? 0 : h));
    }

    uint16_t hdg = 0xFFFF;
    if (e.headingDeg >= 0.0f) {
        long d = lroundf(e.headingDeg * 10.0f) % 3600;
        if (d < 0) d += 3600;
        hdg = (uint16_t)d;
    }
    putU16LE(&out[15], hdg);

    putI16LE(&out[17], clampToI16(e.pitchDeg * 10.0f));

    putI16LE(&out[19], clampToI16(e.accX * 1000.0f));
    putI16LE(&out[21], clampToI16(e.accY * 1000.0f));
    putI16LE(&out[23], clampToI16(e.accZ * 1000.0f));

    putI16LE(&out[25], clampToI16(e.gyrX * 10.0f));
    putI16LE(&out[27], clampToI16(e.gyrY * 10.0f));
    putI16LE(&out[29], clampToI16(e.gyrZ * 10.0f));

    putI16LE(&out[31], clampToI16(e.magX * 10.0f));
    putI16LE(&out[33], clampToI16(e.magY * 10.0f));
    putI16LE(&out[35], clampToI16(e.magZ * 10.0f));

    long mv = lroundf(e.battVolts * 1000.0f);
    if (mv < 0) mv = 0;
    if (mv > 65535) mv = 65535;
    putU16LE(&out[37], (uint16_t)mv);
}

// Manufacturer Specific Data. Company ID(2) + 페이로드(9) = 11바이트. PROTOCOL.md §4.3
inline void encodeManufacturerData(const Telemetry& t, uint8_t seq, uint8_t out[2 + kMfgLen]) {
    putU16LE(&out[0], kCompanyID);
    out[2] = kVersion;
    out[3] = t.moduleID;
    putU16LE(&out[4], t.sogValid ? encodeSog(t.sogKn) : kSogInvalid);
    putU16LE(&out[6], t.cogValid ? encodeCog(t.cogDeg) : kCogInvalid);
    out[8]  = (uint8_t)(t.heelValid ? encodeHeel(t.heelDeg) : kHeelInvalid);
    out[9]  = encodeBatt(t.battPct);
    out[10] = seq;
}

} // namespace sail
