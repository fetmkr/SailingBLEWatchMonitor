// HOHO TFT 화면 레이아웃 — Arduino 의존성 없는 순수 계산부.
//
// 좌표와 문자열 포맷을 여기 모아둔 이유:
// 화면 글자가 240px 밖으로 밀려나도 컴파일은 통과한다. 실기기에 꽂아봐야
// "숫자가 잘렸네" 를 알게 된다. 그래서 폭 계산을 헤더로 빼서
// tools/sim_test.cpp 가 호스트에서 넘침 여부를 검사하게 했다.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace hoho {
namespace layout {

// ── 패널 ─────────────────────────────────────────────────────────────────
// 물리 패널 135x240 → init(135,240) + setRotation(3) → 240x135 가로
constexpr int16_t kPanelW = 135;
constexpr int16_t kPanelH = 240;
constexpr int16_t kW      = 240; // 회전 후 폭
constexpr int16_t kH      = 135; // 회전 후 높이

// 내장 GFX 폰트는 글자당 6*size (가로) x 8*size (세로)
constexpr int16_t charW(int16_t size) { return 6 * size; }
constexpr int16_t charH(int16_t size) { return 8 * size; }
constexpr int16_t textW(int16_t chars, int16_t size) { return chars * charW(size); }

// ── 영역 ─────────────────────────────────────────────────────────────────
constexpr int16_t kTopBarH = 20;
constexpr int16_t kBotBarH = 14;
constexpr int16_t kBotBarY = kH - kBotBarH; // 121

// 상단바
constexpr int16_t kNameX = 4, kNameY = 3, kNameSize = 2;
constexpr int16_t kNameChars = 11; // 사용자 이름 최대 길이와 동일
constexpr int16_t kStatusChars = 11;
constexpr int16_t kStatusX = kW - textW(kStatusChars, 1) - 4; // 오른쪽 정렬
constexpr int16_t kStatusY = 6;
constexpr int16_t kStatusDotX = kStatusX - 8;
constexpr int16_t kStatusDotY = 9;
constexpr int16_t kStatusDotR = 3;

// SOG — "%5.2f" 라 항상 5글자
constexpr int16_t kSogX = 6, kSogY = 26, kSogSize = 6;
constexpr int16_t kSogChars = 5;
constexpr int16_t kUnitX = kSogX + textW(kSogChars, kSogSize) + 4;
constexpr int16_t kUnitY = 52, kUnitSize = 2;

// COG — "%03d" 라 항상 3글자
constexpr int16_t kCogX = 6, kCogY = 82, kCogSize = 4;
constexpr int16_t kCogChars = 3;
constexpr int16_t kDegX = kCogX + textW(kCogChars, kCogSize) + 4;
constexpr int16_t kDegY = kCogY, kDegSize = 2;
constexpr int16_t kPointX = kDegX + textW(1, kDegSize) + 6;
constexpr int16_t kPointY = kCogY + 16, kPointSize = 2;
constexpr int16_t kPointChars = 3;

// 하단바
constexpr int16_t kBotX = 4, kBotSize = 1;
constexpr int16_t kBotTextY = kBotBarY + 4;
constexpr int16_t kBotChars = 38; // 아래 formatBottom() 결과 길이

// ── 컴파일 타임 검사 ─────────────────────────────────────────────────────
// 좌표를 고치다 화면 밖으로 밀어내면 여기서 빌드가 멈춘다.
static_assert(kNameX + textW(kNameChars, kNameSize) <= kStatusDotX - kStatusDotR,
              "상단바: 이름이 상태 표시와 겹친다");
static_assert(kStatusX + textW(kStatusChars, 1) <= kW, "상단바: 상태 문자열이 화면을 넘는다");
static_assert(kNameY + charH(kNameSize) <= kTopBarH + 1, "상단바: 이름 높이가 바를 넘는다");

static_assert(kSogX + textW(kSogChars, kSogSize) <= kW, "SOG 가 화면 폭을 넘는다");
static_assert(kUnitX + textW(2, kUnitSize) <= kW, "kn 라벨이 화면 폭을 넘는다");
static_assert(kSogY >= kTopBarH, "SOG 가 상단바를 침범한다");
static_assert(kSogY + charH(kSogSize) <= kCogY, "SOG 와 COG 가 겹친다");

static_assert(kCogX + textW(kCogChars, kCogSize) <= kW, "COG 가 화면 폭을 넘는다");
static_assert(kPointX + textW(kPointChars, kPointSize) <= kW, "나침반 방위가 화면 폭을 넘는다");
static_assert(kCogY + charH(kCogSize) <= kBotBarY, "COG 가 하단바를 침범한다");
static_assert(kPointY + charH(kPointSize) <= kBotBarY, "나침반 방위가 하단바를 침범한다");

static_assert(kBotX + textW(kBotChars, kBotSize) <= kW, "하단바 문자열이 화면 폭을 넘는다");
static_assert(kBotTextY + charH(kBotSize) <= kH, "하단바 문자열이 화면 아래로 넘친다");

// ── 문자열 포맷 ──────────────────────────────────────────────────────────
// 전부 고정폭이다. 폭이 흔들리면 이전 글자 픽셀이 남아 화면이 지저분해진다.
// (배경색을 함께 그리는 방식이라 같은 길이여야 깨끗이 덮인다)

inline void formatSog(char* out, size_t n, float knots) {
    snprintf(out, n, "%5.2f", knots); // " 5.53" / "12.34"
}

inline void formatCog(char* out, size_t n, float degrees) {
    float d = fmodf(degrees, 360.0f);
    if (d < 0) d += 360.0f;
    snprintf(out, n, "%03d", (int)lroundf(d) % 360); // "045"
}

inline const char* compassPoint(float degrees) {
    // 3글자 고정폭 (짧은 것은 공백으로 채움)
    static const char* kPoints[16] = {"N  ", "NNE", "NE ", "ENE", "E  ", "ESE", "SE ", "SSE",
                                      "S  ", "SSW", "SW ", "WSW", "W  ", "WNW", "NW ", "NNW"};
    float d = fmodf(degrees, 360.0f);
    if (d < 0) d += 360.0f;
    int idx = (int)lroundf(d / 22.5f) % 16;
    return kPoints[idx];
}

inline void formatBottom(char* out, size_t n, int heelDeg, int battPct,
                         unsigned seq, unsigned uptimeSec) {
    snprintf(out, n, "HEEL%+4d  BATT%4d%%  SEQ%4u  UP%5us",
             heelDeg, battPct, seq, uptimeSec % 100000);
}

inline void formatName(char* out, size_t n, const char* name) {
    snprintf(out, n, "%-11.11s", name);
}

} // namespace layout
} // namespace hoho
