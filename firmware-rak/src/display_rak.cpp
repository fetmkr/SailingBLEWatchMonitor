#include "display_rak.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <stdio.h>

#include "board_rak.h"

namespace sail {
namespace {

// ── 화면 객체 ────────────────────────────────────────────────────────────
//
// 생성자 인자 순서는 (회전, reset, clock, data) 다.
// ESP32 에서 U8g2 는 이 핀들이 채워져 있으면 Wire.begin(data, clock) 을
// 대신 불러 준다. 비워 두면 보드 기본 I2C 핀으로 가버려서 화면이 안 켜진다.
//   근거: U8g2/src/U8x8lib.cpp 의 U8X8_MSG_BYTE_INIT 처리
//         U8g2lib.h:1695 생성자
U8G2_SSD1306_128X64_NONAME_F_HW_I2C gOled(
    U8G2_R0, U8X8_PIN_NONE, rak::kI2C1_SCL, rak::kI2C1_SDA);

bool gOk = false;

// ── 화면 배치 (128 x 64) ────────────────────────────────────────────────
//
//        0                                      127
//    0   ┌──────────────────────────────────────┐
//        │ 2FB5                     BLE*   94%  │  5x7,  기준선 y=7
//    9   ├──────────────────────────────────────┤
//        │                                 SIM  │  5x7,  기준선 y=22
//        │  6.38                            kn  │  큰숫자, 기준선 y=38
//   42   ├──────────────────────────────────────┤
//        │ COG 045 SIM      HEEL  -0.2          │  6x10, 기준선 y=53
//        │ SAT 0 NOFIX          UP 1234s        │  5x7,  기준선 y=63
//   63   └──────────────────────────────────────┘
//
// 배에서 정작 눈에 들어오는 건 속도 하나다. 그래서 속도만 큰 글씨를 쓰고
// 나머지는 작게 깐다.
constexpr int kW = 128;
constexpr int kH = 64;

constexpr int kTopBaseline = 7;
constexpr int kLine1Y      = 9;  // 상태줄 아래 가로선
constexpr int kSrcBaseline = 22; // 속도 출처(SIM) 표시
constexpr int kSogBaseline = 38;
constexpr int kLine2Y      = 42;
constexpr int kMidBaseline = 53;
constexpr int kBotBaseline = 63;

// 5x7 폰트는 글자당 5px, 6x10 은 6px 이다. 오른쪽 정렬을 이 폭으로 계산한다.
constexpr int kW5 = 5;
constexpr int kW6 = 6;

int rightX5(int chars) { return kW - 1 - chars * kW5; }
int rightX6(int chars) { return kW - 1 - chars * kW6; }

// 화면 밖으로 밀려나면 조용히 잘리기만 해서 알아채기 어렵다.
// 그릴 때 실제 폭을 재서 넘치면 시리얼로 알린다.
void drawChecked(int x, int y, const char* s, const char* what) {
    gOled.drawStr(x, y, s);
    int w = gOled.getStrWidth(s);
    if (x + w > kW) {
        Serial.printf("[OLED] %s 가 %dpx 넘침 (x=%d w=%d): \"%s\"\n",
                      what, x + w - kW, x, w, s);
    }
}

} // namespace

bool displayPresent() {
    Wire.beginTransmission(rak::kAddrDisplay);
    return Wire.endTransmission() == 0;
}

bool displayBegin() {
    if (!displayPresent()) {
        gOk = false;
        return false;
    }
    if (gOk) return true; // 이미 붙어 있으면 다시 초기화하지 않는다 (깜빡임 방지)

    gOled.setBusClock(400000); // begin() 보다 먼저 불러야 먹는다
    gOled.begin();
    gOled.setFontMode(0);
    gOk = true;
    return true;
}

void displayBootMessage(const char* line1, const char* line2) {
    if (!gOk) return;
    gOled.clearBuffer();
    gOled.setFont(u8g2_font_6x10_tf);
    gOled.drawStr(2, 26, line1);
    gOled.setFont(u8g2_font_5x7_tf);
    gOled.drawStr(2, 42, line2);
    gOled.sendBuffer();
}

void displayUpdate(const DisplayState& s) {
    if (!gOk) return;

    char buf[32];
    gOled.clearBuffer();

    // ── 상태줄 ───────────────────────────────────────────────────────────
    gOled.setFont(u8g2_font_5x7_tf);
    drawChecked(2, kTopBaseline, s.userName, "이름");

    // BLE 상태. 별표는 앱이 notify 를 구독 중이라는 뜻.
    const char* ble = s.bleConnected ? (s.bleNotifying ? "BLE*" : "BLE") : "ADV";
    gOled.drawStr(rightX5(9), kTopBaseline, ble);

    snprintf(buf, sizeof(buf), "%3d%%", (int)(s.battPct + 0.5f));
    gOled.drawStr(rightX5(4), kTopBaseline, buf);

    gOled.drawHLine(0, kLine1Y, kW);

    // ── 속도 — 제일 크게 ─────────────────────────────────────────────────
    // 시뮬레이터로 채운 값이면 SIM 을 띄운다. 이게 없으면 지어낸 숫자를
    // 실측으로 오해하게 된다.
    if (!s.sogFromGps) {
        gOled.setFont(u8g2_font_5x7_tf);
        gOled.drawStr(rightX5(3), kSrcBaseline, "SIM");
    }

    snprintf(buf, sizeof(buf), "%.2f", s.sogKn);
    gOled.setFont(u8g2_font_logisoso24_tn); // 숫자와 마침표만 있는 폰트
    gOled.drawStr(2, kSogBaseline, buf);

    gOled.setFont(u8g2_font_6x10_tf);
    gOled.drawStr(rightX6(2), kSogBaseline, "kn");

    gOled.drawHLine(0, kLine2Y, kW);

    // ── 침로와 힐 ────────────────────────────────────────────────────────
    gOled.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "COG %03d", (int)(s.cogDeg + 0.5f) % 360);
    drawChecked(2, kMidBaseline, buf, "COG");

    // 힐은 IMU 가 붙어 있으면 GPS 와 상관없이 늘 실측이다.
    snprintf(buf, sizeof(buf), "HEEL%+5.1f", s.heelDeg);
    gOled.drawStr(rightX6(9), kMidBaseline, buf);

    // ── 아래줄 — 위성과 켜둔 시간 ────────────────────────────────────────
    gOled.setFont(u8g2_font_5x7_tf);
    snprintf(buf, sizeof(buf), "SAT %d %s", s.satellites,
             s.gpsFix ? "FIX" : "NOFIX");
    drawChecked(2, kBotBaseline, buf, "GPS 상태");

    snprintf(buf, sizeof(buf), "UP %lus", (unsigned long)(s.uptimeMs / 1000));
    gOled.drawStr(rightX5(9), kBotBaseline, buf);

    gOled.sendBuffer();
}

} // namespace sail
