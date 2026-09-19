// LoRa 함대 텔레메트리 24바이트의 순수 인코더/디코더.
// 보드 의존 코드가 없어 맥의 fw_logic_test 에서 같은 바이트를 검증한다.
#pragma once

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace lora {

constexpr size_t   kPayloadLen       = 24;
constexpr int32_t  kLatLonInvalid    = (int32_t)0x80000000;
constexpr uint16_t kSogInvalid       = 0xFFFF;
constexpr uint16_t kCogInvalid       = 0xFFFF;
constexpr uint16_t kHdgInvalid       = 0xFFFF;
constexpr int8_t   kAttitudeInvalid  = -128;
constexpr uint32_t kFrameUs          = 1000000;
constexpr uint32_t kSlotUs           = kFrameUs / 32;
constexpr uint8_t  kNormalRateHz     = 1;
constexpr uint8_t  kBenchRateHz      = 5;
constexpr uint32_t kBenchPeriodUs    = kFrameUs / kBenchRateHz;
constexpr uint32_t kPpsHoldoverMs    = 20u * 60u * 1000u;
constexpr uint32_t kHeardFreshMs     = 3000;
constexpr uint8_t  kWireVersion      = 3;
constexpr uint8_t  kBoatMask         = 0x3F;
constexpr uint8_t  kVersionShift     = 6;

constexpr uint8_t kFlagGpsFix      = 1u << 0;
constexpr uint8_t kFlagRecording   = 1u << 1;
constexpr uint8_t kFlagTime        = 1u << 2;
constexpr uint8_t kFlagBoatChanged = 1u << 3;

struct Live {
    uint8_t boat = 0;
    int32_t lat = kLatLonInvalid;
    int32_t lon = kLatLonInvalid;
    bool gpsFix = false;
    bool recording = false;
    bool timeValid = false;
    bool boatChanged = false;
    bool sogValid = false;
    bool cogValid = false;
    bool attitudeValid = false;
    bool headingValid = false;
    float sogKn = 0.0f;
    float cogDeg = 0.0f;
    float heelDeg = 0.0f;
    float pitchDeg = 0.0f;
    float headingDeg = 0.0f; // v3: true north (T), independent of GPS PPS
    uint8_t battPct = 0;
    uint32_t heard = 0;
    uint16_t tie = 0;
};

struct Decoded {
    uint8_t wireVersion = 0;
    uint8_t boat = 0;
    int32_t lat = kLatLonInvalid;
    int32_t lon = kLatLonInvalid;
    uint16_t sog = kSogInvalid;   // 0.01 kn
    uint16_t cog = kCogInvalid;   // 0.1 deg
    int8_t heel = kAttitudeInvalid;
    int8_t pitch = kAttitudeInvalid;
    uint8_t flags = 0;
    uint32_t heard = 0;
    uint16_t tie = 0;
    uint16_t heading = kHdgInvalid; // v3: 0.1 deg true; v2 legacy: magnetic
};

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint16_t get16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
inline uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint16_t encodeSog(float kn) {
    if (!std::isfinite(kn) || kn < 0.0f) return kSogInvalid;
    const float v = roundf(kn * 100.0f);
    return v >= 65535.0f ? 65534 : (uint16_t)v;
}
inline uint16_t encodeCog(float deg) {
    if (!std::isfinite(deg)) return kCogInvalid;
    float d = fmodf(deg, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return (uint16_t)((uint16_t)lroundf(d * 10.0f) % 3600u);
}
inline int8_t encodeAttitude(float deg) {
    if (!std::isfinite(deg)) return kAttitudeInvalid;
    long v = lroundf(deg);
    if (v < -127) v = -127; // -128 은 값 없음 표식
    if (v > 127) v = 127;
    return (int8_t)v;
}
inline uint8_t encodeBatteryNibble(uint8_t pct) {
    if (pct > 100) pct = 100;
    return (uint8_t)lroundf((float)pct * 15.0f / 100.0f);
}

inline bool validLatLon(int32_t lat, int32_t lon) {
    if (lat == kLatLonInvalid && lon == kLatLonInvalid) return true;
    return lat >= -900000000 && lat <= 900000000 &&
           lon >= -1800000000 && lon <= 1800000000;
}

inline void encode(const Live& v, uint8_t out[kPayloadLen]) {
    memset(out, 0, kPayloadLen);
    out[0] = (uint8_t)((kWireVersion << kVersionShift) | (v.boat & kBoatMask));
    const bool pos = v.gpsFix && validLatLon(v.lat, v.lon) && v.lat != kLatLonInvalid;
    put32(out + 1, (uint32_t)(pos ? v.lat : kLatLonInvalid));
    put32(out + 5, (uint32_t)(pos ? v.lon : kLatLonInvalid));
    put16(out + 9,  v.sogValid ? encodeSog(v.sogKn) : kSogInvalid);
    put16(out + 11, v.cogValid ? encodeCog(v.cogDeg) : kCogInvalid);
    out[13] = (uint8_t)(v.attitudeValid ? encodeAttitude(v.heelDeg) : kAttitudeInvalid);
    out[14] = (uint8_t)(v.attitudeValid ? encodeAttitude(v.pitchDeg) : kAttitudeInvalid);
    uint8_t flags = (uint8_t)(encodeBatteryNibble(v.battPct) << 4);
    if (v.gpsFix)      flags |= kFlagGpsFix;
    if (v.recording)   flags |= kFlagRecording;
    if (v.timeValid)   flags |= kFlagTime;
    if (v.boatChanged) flags |= kFlagBoatChanged;
    out[15] = flags;
    put32(out + 16, v.heard);
    put16(out + 20, v.tie);
    put16(out + 22, v.headingValid ? encodeCog(v.headingDeg) : kHdgInvalid);
}

// GPS 시각을 잡기 전의 저빈도 확인 신호. 배 번호·REC·배터리·자세는 살리고,
// 찍힌 시각을 보장할 수 없는 항해값은 반드시 비운다. 수신 측은 kFlagTime이
// 꺼진 것으로 정상 TDMA 자료와 구분한다.
inline void makePresence(Live* v) {
    if (!v) return;
    v->lat = kLatLonInvalid;
    v->lon = kLatLonInvalid;
    v->gpsFix = false;
    v->timeValid = false;
    v->sogValid = false;
    v->cogValid = false;
}

inline bool decode(const uint8_t in[kPayloadLen], Decoded* out) {
    Decoded v;
    v.wireVersion = in[0] >> kVersionShift;
    v.boat = in[0] & kBoatMask;
    v.lat = (int32_t)get32(in + 1);
    v.lon = (int32_t)get32(in + 5);
    v.sog = get16(in + 9);
    v.cog = get16(in + 11);
    v.heel = (int8_t)in[13];
    v.pitch = (int8_t)in[14];
    v.flags = in[15];
    v.heard = get32(in + 16);
    v.tie = get16(in + 20);
    v.heading = get16(in + 22);
    // implicit header 길이도 24바이트로 바뀌었다. 교체 시험을 위해 같은 길이의
    // 옛 버전 값은 읽되, 모르는 미래 버전은 필드 뜻이 다를 수 있으므로 거절한다.
    const bool ok = v.wireVersion <= kWireVersion && v.boat >= 1 && v.boat <= 32 && validLatLon(v.lat, v.lon) &&
                    (v.cog == kCogInvalid || v.cog <= 3599) &&
                    (v.heading == kHdgInvalid || v.heading <= 3599);
    if (ok && out) *out = v;
    return ok;
}

inline uint32_t slotOffsetUs(uint8_t boat) {
    return (boat >= 1 && boat <= 32) ? (uint32_t)(boat - 1) * kSlotUs : 0;
}

inline bool validLiveRateHz(uint8_t hz) {
    return hz == kNormalRateHz || hz == kBenchRateHz;
}

// 5 Hz는 송신기 한 대의 무선 경로를 보는 짧은 시험이라 TDMA 슬롯을 쓰지 않는다.
// 제품 모드 1 Hz에서만 32척 슬롯을 적용한다.
inline uint32_t liveTargetOffsetUs(uint32_t ageUs, uint8_t boat, uint8_t hz) {
    if (hz == kBenchRateHz) {
        return ((ageUs % kFrameUs) / kBenchPeriodUs) * kBenchPeriodUs;
    }
    return slotOffsetUs(boat);
}

// 32-bit micros()는 약 71분에 한 바퀴 돈다. 부호 없는 뺄셈은 한 번의
// wrap을 지나도 경과 시간을 보존한다. holdover는 그보다 짧아야 한다.
inline bool ppsWithinHoldover(bool ever, uint32_t nowUs, uint32_t ppsAtUs,
                             uint32_t holdoverMs) {
    return ever && (uint32_t)(nowUs - ppsAtUs) <= holdoverMs * 1000u;
}

} // namespace lora
