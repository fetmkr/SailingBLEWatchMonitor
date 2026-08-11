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
static constexpr uint32_t kNotifyPeriodMs = 250;  // 4 Hz
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
};

// GATT characteristic 12바이트 인코딩. PROTOCOL.md §3
inline void encodeTelemetryPacket(const Telemetry& t, uint8_t out[kTelemetryLen]) {
    out[0] = kVersion;
    out[1] = t.moduleID;
    putU32LE(&out[2], t.uptimeMs);
    putU16LE(&out[6], encodeSog(t.sogKn));
    putU16LE(&out[8], encodeCog(t.cogDeg));
    out[10] = (uint8_t)encodeHeel(t.heelDeg);
    out[11] = encodeBatt(t.battPct);
}

// Manufacturer Specific Data. Company ID(2) + 페이로드(9) = 11바이트. PROTOCOL.md §4.3
inline void encodeManufacturerData(const Telemetry& t, uint8_t seq, uint8_t out[2 + kMfgLen]) {
    putU16LE(&out[0], kCompanyID);
    out[2] = kVersion;
    out[3] = t.moduleID;
    putU16LE(&out[4], encodeSog(t.sogKn));
    putU16LE(&out[6], encodeCog(t.cogDeg));
    out[8]  = (uint8_t)encodeHeel(t.heelDeg);
    out[9]  = encodeBatt(t.battPct);
    out[10] = seq;
}

} // namespace sail
