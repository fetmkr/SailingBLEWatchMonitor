// 방위 계산에 쓰는 순수 함수. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// 2026-09-14 헤딩 검토에서 확인된 것들을 여기서 막는다.
//   · while 로 360 을 빼던 정규화가 inf · 1e10 에서 끝나지 않았다 (루프가 멎는다)
//   · hdg off 가 "abc" 를 0 으로, 1e10 을 그대로 저장했다 (재부팅해도 되풀이)
//   · 흔들린 폭을 max − min 으로 쟀다 → 359° 와 1° 사이가 358° 로 나왔다
//   · yawRateNow 가 "수직 둘레 각속도" 를 방위 변화율로 썼다 → 피치가 있으면 힐 변화가 섞였다
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace hdg {

// ── 각도 접기 ────────────────────────────────────────────────────────────
// 반복 없이 fmod 로 한 번에 접는다. 유한하지 않으면 NaN 을 돌려준다 (부르는 쪽이 무효로 본다).
inline float wrap360(float deg) {
    if (!std::isfinite(deg)) return NAN;
    float r = std::fmod(deg, 360.0f);
    if (r < 0.0f) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;          // fmod 반올림으로 360 이 나오는 경우
    return r;
}
inline float wrap180(float deg) {
    if (!std::isfinite(deg)) return NAN;
    float r = wrap360(deg + 180.0f) - 180.0f;
    return r;
}

// 원형 폭: 방위 n 개가 차지하는 가장 짧은 호. 359° 와 1° 는 2° 다.
// 가장 큰 빈 호를 360 에서 뺀다. n ≤ 64 개까지 (넘으면 앞 64 개만 본다).
inline float circularSpread(const float* v, int n) {
    if (n <= 1) return 0.0f;
    if (n > 64) n = 64;
    float s[64];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const float w = wrap360(v[i]);
        if (std::isfinite(w)) s[m++] = w;
    }
    if (m <= 1) return 0.0f;
    for (int i = 1; i < m; ++i) {                   // 삽입 정렬 (64 개 이하)
        const float x = s[i]; int j = i - 1;
        while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; --j; }
        s[j + 1] = x;
    }
    float gap = 360.0f - (s[m - 1] - s[0]);
    for (int i = 1; i < m; ++i) if (s[i] - s[i - 1] > gap) gap = s[i] - s[i - 1];
    return 360.0f - gap;
}

// ── 명령 인자 ────────────────────────────────────────────────────────────
// "-12.5" 같은 숫자만 받는다. 뒤에 글자가 붙거나 inf / nan 이면 거짓.
inline bool parseNumber(const char* s, float* out) {
    if (!s) return false;
    while (*s == ' ') ++s;
    if (!*s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ') ++end;
    if (*end) return false;
    // strtod 는 "inf" "nan" "0x1p3" 도 받는다. 숫자·부호·점·지수만 허용한다.
    for (const char* p = s; p < end; ++p) {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
              c == 'e' || c == 'E' || c == ' ')) return false;
    }
    if (!std::isfinite(v)) return false;
    *out = (float)v;
    return std::isfinite(*out);
}

// NVS 에서 읽은 값이 이상하면 기본값으로. 고쳤으면 true.
inline bool sanitizeFloat(float* v, float lo, float hi, float def) {
    if (std::isfinite(*v) && *v >= lo && *v <= hi) return false;
    *v = def;
    return true;
}

// 방위 축 두 개가 0~2 이고 서로 달라야 한다. 아니면 기본(A=Y, B=X).
inline bool sanitizeAxes(uint8_t* a, uint8_t* b) {
    if (*a <= 2 && *b <= 2 && *a != *b) return false;
    *a = 1; *b = 0;
    return true;
}

// ── 뱃머리 방위 변화율 ───────────────────────────────────────────────────
// 몸통 각속도(앞 p, 오른쪽 q, 아래 r, °/s)와 힐 φ · 피치 θ (라디안) 에서 ZYX 오일러 ψ̇.
//   ψ̇ = (q·sinφ + r·cosφ) / cosθ
// 옛 식(측정 수직 둘레 각속도)은 ψ̇ − φ̇·sinθ 라서 피치가 있으면 힐 변화가 섞였다.
// 피치가 ±85° 를 넘으면 오일러가 풀리지 않으므로 NaN.
inline float eulerYawRate(float p, float q, float r, float rollRad, float pitchRad) {
    (void)p;
    const float c = std::cos(pitchRad);
    if (std::fabs(c) < 0.087f) return NAN;
    return (q * std::sin(rollRad) + r * std::cos(rollRad)) / c;
}

} // namespace hdg
