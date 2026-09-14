// 자력계 하드아이언 보정 — 공 맞추기와 수락 조건. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// ★ 2026-09-14 검토에서 재현된 것 (float32 로 펌웨어 식을 그대로 돌림)
//     수평으로 뱃머리만 돌린 점 30 개 → 중심 z 가 38 µT 틀렸는데 잔차 0.54, **저장됨**
//   점이 한 평면에 몰리면 같은 원을 지나는 공이 무수히 많다. 그중 하나에 딱 맞으니
//   잔차는 작다. 잔차로는 못 거른다. 그리고 옛 코드는 풀리기만 하면 기존 보정을 덮어썼다.
//
// 지금은 후보를 따로 풀고, 아래를 **모두** 통과해야만 쓴다. 아니면 기존 보정을 그대로 둔다.
//   점 수        20 개 이상
//   풀림         피벗이 작지 않다
//   반지름       25 ~ 70 µT   지구 자기장은 어디서나 약 25~65 µT 다 [추측: WMM 전 세계 범위]
//                             한국은 약 50 µT (HEADING.md 실측 47.5)
//   두께         점 구름의 가장 얇은 방향 표준편차 ≥ 반지름의 10%
//                ★ 첫 수정은 "세 좌표축의 퍼짐" 을 봤다. 비스듬한 평면이면 세 축이 다 넓게
//                  퍼져서 통과했다 — 사용자가 32점으로 중심 19.09 µT 틀린 채 통과시켰다.
//                  공분산의 최소 고유값은 좌표축 방향과 상관없다.
//                  고른 공 전체면 0.58r, 구의 25% 캡이면 약 0.14r, 12% 캡이면 약 0.07r
//                  (HEADING.md 검산: 25% 는 0.5 µT, 12% 는 3.7 µT 틀림) → 문턱 0.10r
//   잔차         반지름의 10% 이하
#pragma once

#include <cmath>
#include <cstdint>

namespace magcal {

struct Fit {
    bool  solved = false;
    float c[3] = {0, 0, 0};
    float r = 0, resid = 0;
    float spread[3] = {0, 0, 0};   // 좌표축별 퍼짐 (보여주기용. 판정에 안 쓴다)
    float thickness = 0;           // 가장 얇은 방향 표준편차 (µT)
};

enum class Verdict : uint8_t { Ok, TooFew, Singular, RadiusOut, Flat, ResidHigh };

inline const char* verdictText(Verdict v) {
    switch (v) {
        case Verdict::Ok:        return "통과";
        case Verdict::TooFew:    return "점이 모자람 (20개 넘게)";
        case Verdict::Singular:  return "풀리지 않음 (한쪽으로만 돌림)";
        case Verdict::RadiusOut: return "반지름이 지구 자기장 범위(25~70 uT) 밖";
        case Verdict::Flat:      return "한 평면에 몰림 (가장 얇은 방향 두께가 반지름의 10% 미만)";
        case Verdict::ResidHigh: return "공에서 많이 벗어남 (주변 쇠붙이·연철)";
    }
    return "?";
}

constexpr int   kMinPoints   = 20;
constexpr float kRadiusLo    = 25.0f;
constexpr float kRadiusHi    = 70.0f;
constexpr float kThicknessRatio = 0.10f; // 가장 얇은 방향 두께 ≥ 반지름 × 이것
constexpr float kResidRatio  = 0.10f;   // 잔차 ≤ 반지름 × 이것

// 대칭 3×3 행렬의 가장 작은 고유값 (삼각함수 닫힌 식)
inline double minEigenSym3(double a00, double a01, double a02, double a11, double a12, double a22) {
    const double p1 = a01 * a01 + a02 * a02 + a12 * a12;
    const double lo = std::fmin(a00, std::fmin(a11, a22));
    if (p1 <= 1e-18) return lo;
    const double q = (a00 + a11 + a22) / 3.0;
    const double p2 = (a00 - q) * (a00 - q) + (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) + 2.0 * p1;
    const double p = std::sqrt(p2 / 6.0);
    const double b00 = (a00 - q) / p, b11 = (a11 - q) / p, b22 = (a22 - q) / p;
    const double b01 = a01 / p, b02 = a02 / p, b12 = a12 / p;
    double r = (b00 * (b11 * b22 - b12 * b12) - b01 * (b01 * b22 - b12 * b02) + b02 * (b01 * b12 - b11 * b02)) / 2.0;
    if (r < -1.0) r = -1.0;
    if (r > 1.0) r = 1.0;
    const double phi = std::acos(r) / 3.0;
    return q + 2.0 * p * std::cos(phi + 2.0 * M_PI / 3.0);
}

// 점 구름의 가장 얇은 방향 표준편차 (µT)
inline float thicknessOf(const int16_t (*pts)[3], int n) {
    if (n < 3) return 0.0f;
    double m[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) m[k] += pts[i][k] * 0.1;
    for (int k = 0; k < 3; ++k) m[k] /= n;
    double c[3][3] = {{0}};
    for (int i = 0; i < n; ++i) {
        const double d[3] = {pts[i][0] * 0.1 - m[0], pts[i][1] * 0.1 - m[1], pts[i][2] * 0.1 - m[2]};
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) c[a][b] += d[a] * d[b];
    }
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) c[a][b] /= n;
    const double lam = minEigenSym3(c[0][0], c[0][1], c[0][2], c[1][1], c[1][2], c[2][2]);
    return lam > 0.0 ? (float)std::sqrt(lam) : 0.0f;
}

// pts: 0.1 µT 단위 점 n 개. 기존 펌웨어 magCalSolve 와 같은 식이다 (선형 최소제곱).
inline Verdict fitAndJudge(const int16_t (*pts)[3], int n, Fit* out) {
    Fit f;
    for (int ax = 0; ax < 3; ++ax) {
        int16_t lo = 32767, hi = -32768;
        for (int i = 0; i < n; ++i) { if (pts[i][ax] < lo) lo = pts[i][ax]; if (pts[i][ax] > hi) hi = pts[i][ax]; }
        f.spread[ax] = n ? (hi - lo) * 0.1f : 0.0f;
    }
    f.thickness = thicknessOf(pts, n);
    if (out) *out = f;
    if (n < kMinPoints) return Verdict::TooFew;

    float A[4][4] = {{0}}, b[4] = {0};
    for (int i = 0; i < n; ++i) {
        const float x = pts[i][0] * 0.1f, y = pts[i][1] * 0.1f, z = pts[i][2] * 0.1f;
        const float row[4] = {2 * x, 2 * y, 2 * z, 1.0f};
        const float rhs = x * x + y * y + z * z;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) A[r][c] += row[r] * row[c];
            b[r] += row[r] * rhs;
        }
    }
    float sol[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
        if (std::fabs(A[piv][c]) < 1e-9f) return Verdict::Singular;
        if (piv != c) {
            for (int k = 0; k < 4; ++k) { const float t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
            const float t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < 4; ++r) {
            const float g = A[r][c] / A[c][c];
            for (int k = c; k < 4; ++k) A[r][k] -= g * A[c][k];
            b[r] -= g * b[c];
        }
    }
    for (int r = 3; r >= 0; --r) {
        float v = b[r];
        for (int k = r + 1; k < 4; ++k) v -= A[r][k] * sol[k];
        sol[r] = v / A[r][r];
    }
    const float r2 = sol[3] + sol[0] * sol[0] + sol[1] * sol[1] + sol[2] * sol[2];
    if (!(r2 > 1.0f) || !std::isfinite(r2)) return Verdict::Singular;
    f.solved = true;
    f.c[0] = sol[0]; f.c[1] = sol[1]; f.c[2] = sol[2];
    f.r = std::sqrt(r2);
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float x = pts[i][0] * 0.1f - f.c[0], y = pts[i][1] * 0.1f - f.c[1], z = pts[i][2] * 0.1f - f.c[2];
        const float d = std::sqrt(x * x + y * y + z * z) - f.r;
        sum += d * d;
    }
    f.resid = std::sqrt(sum / n);
    if (out) *out = f;

    if (f.r < kRadiusLo || f.r > kRadiusHi) return Verdict::RadiusOut;
    if (f.thickness < f.r * kThicknessRatio) return Verdict::Flat;
    if (f.resid > f.r * kResidRatio) return Verdict::ResidHigh;
    return Verdict::Ok;
}

} // namespace magcal
