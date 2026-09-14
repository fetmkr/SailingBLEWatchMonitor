// CASIC 바이너리 프레임 (L76K). 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// 틀 [확인: CASIC Protocol Spec §2.2 — scratchpad casic.txt 1206-1213]
//
//     BA CE | len(U2) | class(U1) | id(U1) | payload(len) | ckSum(U4)
//
//     ckSum = (class << 24) + (id << 16) + len  +  payload 를 4바이트씩(리틀엔디안) 더한 것
//     payload 길이는 늘 4 의 배수다
//
// ★ 옛 코드는 체크섬 4바이트를 **세기만 하고** 버렸다. 깨진 NAV-PV 가 정상 속도로
//   들어왔다. 선언 길이가 버퍼보다 크면 남은 바이트를 안 먹어서 다음 프레임 경계도 잃었다.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace casic {

constexpr uint16_t kMaxPayload     = 128;   // 우리가 받는 것 중 제일 큰 것이 NAV-PV 80
//
// ★ 시간 초과는 없다. 처음엔 250 ms 를 뒀는데 그 시각이 바이트 도착이 아니라 feed 호출
//   기준이라, 루프가 프레임 도중 멎으면(SD 마운트 0.7초 등) 버퍼에 멀쩡히 있던 프레임을
//   버리고 남은 페이로드를 새 입력으로 훑었다 (2026-09-14 리뷰). 대신 선언 길이가
//   kMaxPayload 를 넘으면 그 자리에서 버린다 — 잘린 프레임이 삼키는 바이트가 len+4 로 묶인다.

inline uint32_t checksum(uint8_t cls, uint8_t id, uint16_t len, const uint8_t* p,
                         bool idFirst = false) {
    const uint8_t hi = idFirst ? id : cls;
    const uint8_t lo = idFirst ? cls : id;
    uint32_t ck = ((uint32_t)hi << 24) + ((uint32_t)lo << 16) + len;
    for (uint16_t i = 0; i + 3 < len; i += 4) {
        ck += (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) |
              ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
    }
    return ck;
}

struct Frame {
    uint8_t  cls = 0, id = 0;
    uint16_t len = 0;
    uint8_t  payload[kMaxPayload] = {0};
};

enum class Result : uint8_t {
    None,         // 아직 한 개가 안 끝났다
    Frame,        // 체크섬까지 맞는 한 개
    BadChecksum,  // 끝까지 받았는데 체크섬이 틀림 → 버림
    Oversize,     // 선언 길이가 버퍼보다 큼 → 그 자리에서 버리고 다시 머리를 찾는다
    BadLength,    // 길이가 4 의 배수가 아님 → 그 자리에서 버림
};

class Parser {
public:
    // 한 바이트를 먹인다. 프레임 밖의 바이트(NMEA)는 passthrough 가 참이 된다.
    // nowMs 는 지금 안 쓴다 (시간 초과를 없앴다). 부르는 쪽 모양을 유지하려고 남겨 둔다.
    Result feed(uint8_t b, uint32_t nowMs, bool* passthrough) {
        (void)nowMs;
        if (passthrough) *passthrough = false;
        switch (st_) {
            case 0:
                if (b == 0xBA) { st_ = 1; }
                else if (passthrough) *passthrough = true;
                return Result::None;
            case 1:
                if (b == 0xCE) st_ = 2;
                else if (b != 0xBA) st_ = 0;          // BA BA CE 도 받는다
                return Result::None;
            case 2: len_ = b; st_ = 3; return Result::None;
            case 3: len_ |= (uint16_t)b << 8; st_ = 4; return Result::None;
            case 4: f_.cls = b; st_ = 5; return Result::None;
            case 5:
                f_.id = b;
                f_.len = len_;
                n_ = 0;
                ck_ = 0;
                st_ = 0;
                if (len_ > kMaxPayload) return Result::Oversize;
                if ((len_ % 4) != 0)    return Result::BadLength;
                st_ = (len_ > 0) ? 6 : 7;
                return Result::None;
            case 6:
                f_.payload[n_] = b;
                if (++n_ >= len_) { n_ = 0; st_ = 7; }
                return Result::None;
            case 7:
                ck_ |= (uint32_t)b << (8 * n_);
                if (++n_ < 4) return Result::None;
                st_ = 0;
                if (ck_ != checksum(f_.cls, f_.id, f_.len, f_.payload, false) &&
                    ck_ != checksum(f_.cls, f_.id, f_.len, f_.payload, true)) {
                    return Result::BadChecksum;
                }
                return Result::Frame;
        }
        st_ = 0;
        return Result::None;
    }

    bool inFrame() const { return st_ != 0; }
    const Frame& frame() const { return f_; }

private:
    uint8_t  st_ = 0;
    uint16_t len_ = 0, n_ = 0;
    uint32_t ck_ = 0;
    Frame    f_;
};

inline float rdF32(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }

// ── NAV-PV (0x01 0x03), 80바이트 ─────────────────────────────────────────
//
// 자리는 기존 펌웨어가 쓰던 것을 그대로 옮겼다 (main.cpp gpsPoll·gpsNavPv).
struct NavPv {
    uint8_t posValid = 0, velValid = 0, numSv = 0;
    float hAccM = -1, velN = 0, velE = 0, velU = 0;
    float speed2D = 0, heading = 0, sAccMs = -1, cogAccDeg = -1, pDop = 0;
};

// 숫자 범위까지 본다. NaN·무한대, 말도 안 되는 속도는 버린다.
// 딩기·자동차 시험까지 넉넉히 100 m/s(194 kn) 로 막는다.
inline bool parseNavPv(const Frame& f, NavPv* out) {
    if (f.cls != 0x01 || f.id != 0x03 || f.len != 80) return false;
    const uint8_t* p = f.payload;
    const float pDop = rdF32(p + 12);
    const float ha   = rdF32(p + 40);   // 수평 위치 분산 (m²)
    const float vn   = rdF32(p + 48);
    const float ve   = rdF32(p + 52);
    const float vu   = rdF32(p + 56);
    const float sp   = rdF32(p + 64);
    const float hd   = rdF32(p + 68);
    const float sa   = rdF32(p + 72);   // 속도 분산 (m/s)²
    const float ca   = rdF32(p + 76);   // 침로 분산 (도²)
    const float all[] = {pDop, ha, vn, ve, vu, sp, hd, sa, ca};
    for (float v : all) if (!std::isfinite(v)) return false;
    constexpr float kMaxMs = 100.0f;
    if (std::fabs(vn) > kMaxMs || std::fabs(ve) > kMaxMs || std::fabs(vu) > kMaxMs) return false;
    if (sp < 0.0f || sp > kMaxMs) return false;
    if (hd < -360.0f || hd > 720.0f) return false;
    if (ha < 0.0f || sa < 0.0f || ca < 0.0f || pDop < 0.0f) return false;
    if (out) {
        out->posValid = p[4];
        out->velValid = p[5];
        out->numSv    = p[7];
        out->pDop     = pDop;
        out->hAccM    = ha > 0.0f ? std::sqrt(ha) : -1.0f;
        out->velN = vn; out->velE = ve; out->velU = vu;
        out->speed2D  = sp;
        out->heading  = hd;
        out->sAccMs   = sa > 0.0f ? std::sqrt(sa) : -1.0f;
        out->cogAccDeg= ca > 0.0f ? std::sqrt(ca) : -1.0f;
    }
    return true;
}

// ── ACK (0x05 0x01) / NACK (0x05 0x00) ───────────────────────────────────
//
// 페이로드 앞 두 바이트가 **받은 요청의** class · id 다
// [확인: casic.txt 2060-2061 — clsID, msgID]. 다른 요청의 ACK 를 우리 것으로 읽지 않는다.
enum class Ack : uint8_t { NotMine, Ack, Nack };
inline Ack ackFor(const Frame& f, uint8_t reqCls, uint8_t reqId) {
    if (f.cls != 0x05 || (f.id != 0x00 && f.id != 0x01) || f.len < 4) return Ack::NotMine;
    if (f.payload[0] != reqCls || f.payload[1] != reqId) return Ack::NotMine;
    return f.id == 0x01 ? Ack::Ack : Ack::Nack;
}

} // namespace casic
