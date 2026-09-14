// 자력계(AK8963) 표본 검사. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// ★ 라이브러리 getMagValues() 는 표본을 믿고 그대로 쓴다 [확인: MPU9250_WE 1.2.17
//   MPU9250_WE.cpp#L96-L112, #L194-L205].
//     · 넘침 표식(ST2 HOFL)을 안 본다
//     · I2C 로 한 바이트라도 오면 6바이트를 읽고, 아무것도 안 오면 **초기화 안 된 배열**을 쓴다
//     · 같은 표본을 두 번 읽어도 모른다 — 자력계는 8 Hz(#L128)인데 펌웨어는 10 Hz 로 읽는다
//
// MPU-9250 의 I2C 마스터는 AK8963 의 HXL(0x03)부터 8바이트를 계속 떠 온다 (#L145):
//     [0..5] HXL HXH HYL HYH HZL HZH   [6] ST2   [7] CNTL1
// 우리는 그 거울(EXT_SLV_SENS_DATA_00 = 0x49)에서 8바이트를 한 번에 읽고 여기서 판정한다.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mag {

constexpr uint8_t kSt2Overflow = 0x08;   // REGISTER_VALUE_AK8963_OVF [확인: MPU9250_WE.h#L153]

enum class Check : uint8_t {
    New,        // 새 표본
    Repeat,     // 지난번과 같은 6바이트. ★ 새 측정이 없었다는 뜻이 아니다 — 자기장이
                //   고르면 새 측정도 같은 값일 수 있다. 정상 읽기로 치고, 오래 안 바뀔 때만 의심한다
    ShortRead,  // 8바이트를 다 못 읽었다
    Overflow,   // 자력계가 넘쳤다고 알렸다 (강한 자석 근처)
    Zero,       // 세 축이 모두 0 — 지구 위에서는 나올 수 없다
};

inline int16_t le16(const uint8_t* p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

// got = 실제로 읽은 바이트 수, b = 8바이트, prev = 지난 새 표본의 6바이트
inline Check check(uint8_t got, const uint8_t* b, const uint8_t* prev, bool havePrev) {
    if (got < 8) return Check::ShortRead;
    if (b[6] & kSt2Overflow) return Check::Overflow;
    if (le16(b) == 0 && le16(b + 2) == 0 && le16(b + 4) == 0) return Check::Zero;
    if (havePrev && std::memcmp(b, prev, 6) == 0) return Check::Repeat;
    return Check::New;
}

// ASA (공장 감도 조정값) → 곱할 계수. 라이브러리와 같은 식 [확인: MPU9250_WE.cpp#L170-L177]
inline float asaFactor(uint8_t asa) { return (float)(0.5 * (asa - 128) / 128.0 + 1.0); }

// 원시 → µT. 16비트 출력 기준 (라이브러리와 같은 scaleFactor 4912/32760, #L103)
inline float toMicroTesla(int16_t raw, float corr) {
    return raw * (4912.0f / 32760.0f) * corr;
}

// ── 쓸 만한 표본인가 ─────────────────────────────────────────────────────
//
// ★ 2026-09-14 첫 수정은 "같은 값 = 새 측정 없음" 으로 보고 400 ms 뒤 방위를 무효로 했다.
//   정상적으로 측정이 들어와도 값이 같으면 무효가 될 수 있었다 (사용자 지적). 지금은
//     정상 읽기(새 값·같은 값)가 400 ms 안에 있었다          → 신선
//     세 축 16비트가 5초 동안 한 번도 안 바뀌었다            → 거울 레지스터가 멈춘 것으로 본다
//   5초는 8 Hz 로 40 표본이다. 실제 센서 잡음이면 세 축이 40번 연속 똑같을 일은 없다고 봤다
//   [추측: AK8963 잡음 수준을 이 보드에서 재지 않았다. `hdg` 의 반복 수로 확인할 것].
//   I2C 마스터 상태 레지스터(0x36)의 NACK 비트로 가르는 방법은 비트 뜻을 확인하지 못해 안 썼다.
constexpr uint32_t kStaleMs  = 400;
constexpr uint32_t kFrozenMs = 5000;

struct Freshness {
    uint32_t lastOkMs = 0;       // 마지막 정상 읽기
    uint32_t lastChangeMs = 0;   // 값이 마지막으로 바뀐 때
    void reset() { lastOkMs = lastChangeMs = 0; }
    void update(Check c, uint32_t nowMs) {
        if (c == Check::New) { lastOkMs = nowMs; lastChangeMs = nowMs; }
        else if (c == Check::Repeat) { lastOkMs = nowMs; if (!lastChangeMs) lastChangeMs = nowMs; }
        // ShortRead · Overflow · Zero 는 정상 읽기가 아니다 → 시각을 안 바꾼다
    }
    bool usable(uint32_t nowMs) const {
        return lastOkMs != 0 && nowMs - lastOkMs < kStaleMs && nowMs - lastChangeMs < kFrozenMs;
    }
};

// ── 공장 감도값(ASA) 읽기 ────────────────────────────────────────────────
//
// ★ ASA 레지스터(0x10~0x12)는 **Fuse ROM 접근 모드에서만** 읽힌다. 모드를 바꿀 때는 전원
//   차단 모드를 거친다 [사용자 확인: TDK/AKM AK8963 규격]. 라이브러리도 리셋 직후 Fuse ROM
//   에서 읽고 연속 모드로 넘어간다 [확인: MPU9250_WE.cpp#L122-L128].
//   2026-09-14 첫 수정은 연속 모드가 된 뒤에 읽었다 — 틀린 순서였다.
//
// setMode(m) · readReg(r) 를 받는다. 시험에서는 모드를 흉내 내는 가짜 칩을 넘긴다.
// 0x00·0xFF 는 못 읽은 것으로 보고 거짓 [추측: 공장값이 끝값일 가능성은 낮다고 봤다].
template <class SetMode, class ReadReg>
bool readAsaFuseRom(SetMode&& setMode, ReadReg&& readReg,
                    uint8_t modePowerDown, uint8_t modeFuseRom, uint8_t modeRestore,
                    uint8_t regAsaX, uint8_t out[3]) {
    setMode(modePowerDown);
    setMode(modeFuseRom);
    for (int i = 0; i < 3; ++i) out[i] = readReg((uint8_t)(regAsaX + i));
    setMode(modePowerDown);
    setMode(modeRestore);
    for (int i = 0; i < 3; ++i) if (out[i] == 0x00 || out[i] == 0xFF) return false;
    return true;
}

} // namespace mag
