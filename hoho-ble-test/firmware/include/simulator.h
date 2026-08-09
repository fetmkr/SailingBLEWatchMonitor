// HOHO-01 가상 텔레메트리 생성기.
// Arduino 의존성이 없는 순수 C++ 헤더라서 호스트(macOS)에서도 그대로 컴파일해
// 값을 실측 검증할 수 있다. → tools/sim_test.cpp
#pragma once

#include <math.h>
#include <stdint.h>

#include "protocol.h"

namespace hoho {
namespace sim {

// ── 시뮬레이션 파라미터 ──────────────────────────────────────────────────
static constexpr float kSogBase     = 5.5f;  // kn
static constexpr float kSogAmp      = 1.5f;  // kn
static constexpr float kWavePeriodS = 40.0f; // sog/heel 공통 사인 주기
static constexpr float kSogNoiseKn  = 0.2f;

static constexpr float kTackPeriodS  = 60.0f;  // 60초마다 택
static constexpr float kTackRampS    = 5.0f;   // 5초에 걸쳐 부드럽게 전환
static constexpr float kCogStarboard = 45.0f;  // 우현 택 침로
static constexpr float kCogPort      = 315.0f; // 좌현 택 침로

static constexpr float kHeelBase     = 12.0f; // deg
static constexpr float kHeelAmp      = 6.0f;  // deg
static constexpr float kHeelNoiseDeg = 0.5f;

static constexpr float kBattStartPct = 100.0f;
static constexpr float kBattStepS    = 600.0f; // 10분마다 1 % 씩 계단식 감소

// ── 수학 유틸 ────────────────────────────────────────────────────────────

// 0→1 구간에서 양끝 기울기가 0 이 되는 부드러운 보간 계수
inline float smoothstep(float p) {
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    return p * p * (3.0f - 2.0f * p);
}

// 최단 회전 방향으로 각도 보간. 315° → 45° 는 북쪽(0°)을 통과하는 +90°.
inline float lerpAngle(float a, float b, float p) {
    float delta = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    return fmodf(a + delta * p + 360.0f, 360.0f);
}

// ── 난수 공급자 ──────────────────────────────────────────────────────────
// [0,1) 을 돌려주는 함수 포인터. 펌웨어는 Arduino random(), 테스트는 고정 시드 LCG.
using Rand01 = float (*)();

inline float noNoise() { return 0.5f; } // 항상 노이즈 0 (0.5 → 중앙값)

// ── 본체 ─────────────────────────────────────────────────────────────────
// nowMs: 부팅 후 경과 ms. rng: [0,1) 난수 공급자.
inline Telemetry simulate(uint32_t nowMs, Rand01 rng) {
    const float t = nowMs / 1000.0f;

    // rng() 를 [-amp, +amp] 로 변환
    auto noise = [&](float amp) { return (rng() * 2.0f - 1.0f) * amp; };

    Telemetry tm;
    tm.uptimeMs = nowMs;

    // 공통 파도 사인파
    const float wave = sinf(2.0f * (float)M_PI * t / kWavePeriodS);

    // SOG
    tm.sogKn = kSogBase + kSogAmp * wave + noise(kSogNoiseKn);
    if (tm.sogKn < 0.0f) tm.sogKn = 0.0f;

    // 택 진행 상태.
    //  tackIndex 짝수 → 우현 택(45°), 홀수 → 좌현 택(315°)
    //  각 택의 처음 kTackRampS 초 동안 이전 침로에서 새 침로로 부드럽게 전환.
    const long  tackIndex = (long)floorf(t / kTackPeriodS);
    const float phase     = t - (float)tackIndex * kTackPeriodS;
    const float ramp      = (tackIndex == 0) ? 1.0f // 부팅 직후에는 전환 없이 바로 안착
                                             : smoothstep(phase / kTackRampS);

    const bool  starboard = (tackIndex % 2 == 0);
    const float cogNow    = starboard ? kCogStarboard : kCogPort;
    const float cogPrev   = starboard ? kCogPort : kCogStarboard;
    tm.cogDeg             = lerpAngle(cogPrev, cogNow, ramp);

    // 힐: 택 전환과 함께 부호가 부드럽게 반전 (우현 택 = 양수)
    const float signNow  = starboard ? 1.0f : -1.0f;
    const float heelSign = -signNow + (signNow - (-signNow)) * ramp;
    tm.heelDeg           = heelSign * (kHeelBase + kHeelAmp * wave) + noise(kHeelNoiseDeg);

    // 배터리: 10분당 1 % 계단식 감소.
    // (연속 감소 + 반올림으로 하면 부팅 5분 만에 99% 로 떨어져 스펙과 어긋난다.)
    tm.battPct = kBattStartPct - floorf(t / kBattStepS);
    if (tm.battPct < 0.0f) tm.battPct = 0.0f;

    return tm;
}

} // namespace sim
} // namespace hoho
