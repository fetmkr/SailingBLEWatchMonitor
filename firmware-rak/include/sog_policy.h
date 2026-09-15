// 화면 속도의 판단만 모은 것. 입력 → 품질 → 추정 → 표시 가운데 "품질" 과 "추정" 의 규칙이다.
// 보드 없이 맥에서 시험한다 (tools/fw_logic_test.cpp).
//
// ★ 동작은 2026-09-14 코드 그대로다 (NEXT 5번: 경계만 나눈다). 숫자는 책상 위 몇 분 자료로 정했고
//   물 위에서 다시 맞춘다. 창 길이·판정 규칙은 시각이 찍힌 정지·걷기 자료를 받은 뒤 따로 바꾼다.
#pragma once

#include <cmath>
#include <cstdint>

namespace sog {

struct Policy {
    float    badAccKn    = 1.0f;    // 칩이 밝힌 속도 오차가 이보다 크면 그 표본은 버린다
    float    slowEnterKn = 0.5f;    // 3초 성분 평균이 이 밑이면 느린 모드로 들어간다
    float    slowLeaveKn = 0.8f;    // 이 위면 느린 모드에서 나온다 (사이는 그대로 — 되풀이 막이)
    float    restCutKn   = 0.2f;    // 느린 모드에서 이 밑은 0 (멈춤)
    uint32_t slowWinMs   = 30000;   // 느린 모드 성분 평균 창
    uint32_t shortWinMs  = 3000;    // 모드 판정 창
    uint32_t pvStaleMs   = 2000;    // NAV-PV 가 이만큼 안 오면 RMC 길로 물러선다
};

// 품질 — 표식이 "방금 잰 값" 이고, 칩이 밝힌 오차가 있고, 한도 안인가
inline bool sampleOk(bool velMeasured, float accKn, const Policy& p) {
    return velMeasured && accKn >= 0.0f && accKn <= p.badAccKn;
}

// NAV-PV 가 끊겼나 (한 번도 안 왔거나 오래됨)
inline bool pvStale(uint32_t pvAtMs, uint32_t nowMs, const Policy& p) {
    return pvAtMs == 0 || nowMs - pvAtMs > p.pvStaleMs;
}

// 추정 — 느린 모드 들고 나기. shortKn < 0 은 아직 표본 없음.
inline bool nextSlowMode(bool slow, float shortKn, const Policy& p) {
    if (!slow && shortKn >= 0.0f && shortKn < p.slowEnterKn) return true;
    if (slow && shortKn > p.slowLeaveKn) return false;
    return slow;
}

// 표시 — 느린 모드에서 긴 창 평균을 멈춤 0 으로 자른다
inline float slowShownKn(float longKn, const Policy& p) {
    return (longKn < p.restCutKn) ? 0.0f : longKn;
}

// 성분(북·동) 평균의 크기, 노트. 속도 숫자를 평균하면 늘 0 이상이라 멈춰도 0.23 에 머문다.
//   ring: 가장 최근이 head-1. 창 밖(winMs 넘게 옛것)이나 notBefore 이전을 만나면 멈춘다.
struct Sample { uint32_t ms; float n, e; };
inline float vectorMeanKn(const Sample* ring, int cap, int head, int count,
                          uint32_t nowMs, uint32_t winMs, uint32_t notBefore) {
    float sn = 0.0f, se = 0.0f;
    int k = 0;
    for (int i = 0; i < count; ++i) {
        const Sample& s = ring[(head - 1 - i + cap) % cap];
        if (nowMs - s.ms > winMs || s.ms < notBefore) break;
        sn += s.n; se += s.e; ++k;
    }
    if (k == 0) return -1.0f;
    return std::sqrt(sn * sn + se * se) / (float)k * 1.943844f;
}

} // namespace sog
