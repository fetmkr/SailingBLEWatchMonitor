// 배의 방위 — 보드(OLED·BLE·TXT)와 데스크탑·아이패드 앱이 **같은 식**을 쓴다.
//
// ★ 2026-09-15 전에는 식이 셋이었다 (OLED 평평 · 앱 atan2(magY,magX) · 앱 comp). 같은 순간에 값이
//   달랐고 앱 HDG 는 COG 대비 흩어짐 49.5° 였다. 사용자: "과학적인 데이터 분석용 앱인데 맘대로 공식을 적용해?"
//
//   앱 사본   desktop/src/heading.ts
//   같은지    tools/verify.sh 가 heading_vectors.cpp 로 만든 입력을 두 구현에 넣고 맞춰 본다
//   식 번호   HLG 머리글 hdg_formula (hlog.h kOffHdgFormula) — 1 평평 · 2 기울기 보정
//
// 좌표
//   자력계 좌표 (AK8963)   축 설정 A·B 가 가리키는 틀. 방위 = atan2(A·sA, B·sB) 의 두 축
//   앞 = B·sB · 오른쪽 = −A·sA · 아래 = 남은 축, 오른손 법칙으로 부호 (24가지 설정 모두 검산, 2026-09-14)
//   가속→자력 축 (고정)    자력 X = 가속 Y, 자력 Y = 가속 X, 자력 Z = −가속 Z
#pragma once

#include <cmath>
#include <cstdint>

#include "heading_math.h"

namespace hdg {

struct HeadingCfg {
    uint8_t axisA = 1, axisB = 0;
    float   signA = 1.0f, signB = 1.0f;
    float   offDeg  = 0.0f;   // 장착 오프셋
    float   declDeg = 0.0f;   // 자기 편각 (동편 +)
};

// 아래 축의 부호. 설정이 잘못(두 축이 같거나 범위 밖)이면 0.
inline float downSign(const HeadingCfg& c) {
    if (c.axisA > 2 || c.axisB > 2 || c.axisA == c.axisB) return 0.0f;
    const uint8_t f = c.axisB, r = c.axisA;
    const bool even = (f == 0 && r == 1) || (f == 1 && r == 2) || (f == 2 && r == 0);
    return (even ? 1.0f : -1.0f) * c.signB * (-c.signA);
}

// 자력 좌표 벡터 → (앞, 오른쪽, 아래)
inline bool toFRD(const float v[3], const HeadingCfg& c, float* f, float* r, float* d) {
    const float ds = downSign(c);
    if (ds == 0.0f) return false;
    *f =  v[c.axisB] * c.signB;
    *r = -v[c.axisA] * c.signA;
    *d =  v[3 - c.axisA - c.axisB] * ds;
    return true;
}

// 가속(가속도계 좌표, g)으로 힐 φ · 피치 θ (라디안).
// ★ 가속도계가 중력만 잰다고 가정한다. 크기가 1 g 에서 0.15 g 넘게 벗어나면 운동 가속이 섞인 것이라 false.
//   (세션 46: 달리는 중 거절 0.34%, 가장 긴 연속 310 ms)
inline bool gravityRollPitch(const float accAccel[3], const HeadingCfg& c, float* roll, float* pitch) {
    const float a[3] = { accAccel[1], accAccel[0], -accAccel[2] };   // 자력 좌표로
    float f, r, d;
    if (!toFRD(a, c, &f, &r, &d)) return false;
    const float gf = -f, gr = -r, gd = -d;                            // 가속은 위를 가리킨다 → 중력
    const float gn = std::sqrt(gf * gf + gr * gr + gd * gd);
    if (gn < 0.2f || std::fabs(gn - 1.0f) > 0.15f) return false;
    *roll  = std::atan2(gr, gd);
    *pitch = std::atan2(-gf, std::sqrt(gr * gr + gd * gd));
    return true;
}

// 기울기 보정 방위 (도, 0~360). 못 구하면 -1.
//   식은 INSLIB ahrs_mag_detilt (MIT). 72가지 자세 검산 0.5° 미만 (NEXT.md 2026-09-09)
//   mag 는 자력 좌표 µT (HLG 에 적힌 값 — 하드아이언은 이미 빠져 있다)
inline float tiltHeadingDeg(const float accAccel[3], const float mag[3], const HeadingCfg& c) {
    float roll, pitch;
    if (!gravityRollPitch(accAccel, c, &roll, &pitch)) return -1.0f;
    float mf, mr, md;
    if (!toFRD(mag, c, &mf, &mr, &md)) return -1.0f;
    const float cr = std::cos(roll),  sr = std::sin(roll);
    const float ct = std::cos(pitch), st = std::sin(pitch);
    const float ty = cr * mr - sr * md;
    const float tz = sr * mr + cr * md;
    const float hx = ct * mf + st * tz;
    const float hy = ty;
    const float h = wrap360(std::atan2(-hy, hx) * 180.0f / (float)M_PI + c.offDeg + c.declDeg);
    return std::isfinite(h) ? h : -1.0f;
}

// 평평 방위 (진단용, 식 번호 1). 못 구하면 -1.
inline float flatHeadingDeg(const float mag[3], const HeadingCfg& c) {
    if (c.axisA > 2 || c.axisB > 2 || c.axisA == c.axisB) return -1.0f;
    const float h = wrap360(std::atan2(mag[c.axisA] * c.signA, mag[c.axisB] * c.signB) * 180.0f / (float)M_PI +
                            c.offDeg + c.declDeg);
    return std::isfinite(h) ? h : -1.0f;
}

} // namespace hdg
