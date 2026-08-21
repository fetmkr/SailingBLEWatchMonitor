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
// ★ I2C 핀을 U8g2 생성자에 넘기면 안 된다. 실제로 넘겼다가 보드가 통째로
//   멈췄다.
//
//   U8g2 는 넘겨받은 핀을 초기화할 때 pinMode(핀, OUTPUT) 으로 바꿔 버린다.
//     U8x8lib.cpp  U8X8_MSG_GPIO_AND_DELAY_INIT
//                  → i < U8X8_PIN_OUTPUT_CNT 이면 pinMode(pin, OUTPUT)
//     clib/u8x8.h  U8X8_PIN_I2C_CLOCK = 12, U8X8_PIN_I2C_DATA = 13,
//                  U8X8_PIN_OUTPUT_CNT = 16   → 둘 다 OUTPUT 대상이다
//
//   I2C 는 오픈드레인이어야 하는데 푸시풀 출력이 되면 버스가 죽는다.
//   같은 버스에 매달린 IMU 까지 함께 멈춰서 펌웨어가 통째로 정지했다.
//
// 대신 main 이 먼저 Wire.begin(SDA, SCL, 400000) 을 해 두면, U8g2 가 부르는
// Wire.begin() 은 "이미 초기화됨" 으로 그냥 통과한다.
//     Wire.cpp  if (i2cIsInit(num)) { started = true; goto end; }
// 그래서 핀을 안 넘겨도 우리가 연 GPIO9/40 을 그대로 쓴다.
//
// ※ 따라서 displayBegin() 은 반드시 Wire.begin() 뒤에 불러야 한다.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C gOled(U8G2_R0);

bool gOk = false;

// ── 화면 배치 (128 x 64) ────────────────────────────────────────────────
//
//        0                                      127
//    0   ┌──────────────────────────────────────┐
//    7   │ random()               BLE*     94%  │  5x7
//    9   ├──────────────────────────────────────┤
//   27   │                             ██SIM██  │  6x10 반전 (SIM 일 때만)
//   30   │  6.38 kn                             │  logisoso18 — 제일 크게
//   41   │ COG 045       HDG 344                │  6x10
//   49   │ HEEL  -1.9   PITCH  +2.9             │  5x7
//   56   │ GYR  -0.5  +0.9  +1.1                │  5x7
//   63   │ ACC +0.04 -0.08 -1.00                │  5x7
//        └──────────────────────────────────────┘
//
// 배에서 제일 먼저 눈에 들어와야 하는 건 속도다. 그래서 SOG 만 18px 로 쓰고
// 나머지는 작게 깐다.
//
//   COG 는 배가 실제로 가는 방향 (GPS). 멈춰 있으면 안 나온다.
//   HDG 는 뱃머리가 보는 방향 (자력계). 멈춰 있어도 나온다.
//   요트에서는 이 둘이 다르다. 그래서 나란히 보여준다.
//   자력계 원본 세 축은 HDG 로 대신하고 화면에서는 뺐다 (시리얼에는 나온다).
//
// SOG 는 왼쪽, SIM 표시는 오른쪽이라 세로로 겹쳐도 서로 침범하지 않는다.
constexpr int kW = 128;
constexpr int kH = 64;

constexpr int kTopBaseline = 7;
constexpr int kLineY       = 9;  // 상태줄 아래 가로선
constexpr int kTagBaseline = 27; // 속도 신뢰도 (SIM / H1.2 / FIX)
constexpr int kSogBaseline = 30;
constexpr int kCogBaseline = 41;
constexpr int kAttBaseline = 49; // 힐 / 피치
constexpr int kGyrBaseline = 56;
constexpr int kAccBaseline = 63;

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

void displayHealthCheck() {
    const bool now = displayPresent();
    if (gOk && !now) {
        gOk = false;
        Serial.println("[OLED] 응답이 끊겼습니다 — 그리기를 멈춥니다 (나머지는 계속 돕니다)");
    } else if (!gOk && now) {
        Serial.println("[OLED] 다시 보입니다 — 붙입니다");
        displayBegin();
    }
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

    char buf[40];
    gOled.clearBuffer();

    // ── 상태줄 ───────────────────────────────────────────────────────────
    gOled.setFont(u8g2_font_5x7_tf);
    drawChecked(2, kTopBaseline, s.userName, "이름");

    // 별표는 앱이 notify 를 구독 중이라는 뜻.
    const char* ble = s.bleConnected ? (s.bleNotifying ? "BLE*" : "BLE") : "ADV";
    gOled.drawStr(rightX5(9), kTopBaseline, ble);

    snprintf(buf, sizeof(buf), "%3d%%", (int)(s.battPct + 0.5f));
    gOled.drawStr(rightX5(4), kTopBaseline, buf);

    gOled.drawHLine(0, kLineY, kW);

    // ── 속도 — 제일 크게 ─────────────────────────────────────────────────
    //
    // ★ 값이 없으면 숫자를 아예 안 그린다. 그럴듯한 숫자가 떠 있으면 사람은
    //   그걸 읽는다. 예전에는 시뮬레이터 값에 SIM 을 붙여 띄웠는데 결국
    //   헷갈렸다. 0 도 안 된다 — 정박 중 0.0 kn 과 구별되지 않는다.
    if (s.sogValid) {
        snprintf(buf, sizeof(buf), "%.2f", s.sogKn);
        gOled.setFont(u8g2_font_logisoso18_tn); // 숫자와 마침표만 있는 폰트
        drawChecked(2, kSogBaseline, buf, "SOG");

        gOled.setFont(u8g2_font_6x10_tf);
        gOled.drawStr(64, kSogBaseline, "kn");

        // 위치 차분 속도를 작게 옆에 붙인다 (비교 중).
        // 모듈이 주는 도플러 속도는 걷는 속도를 0 으로 뭉개고, 움직여도
        // 3초쯤 지나야 값이 올라온다. 어느 쪽을 쓸지 밖에서 보고 정한다.
    } else {
        // 큰 폰트는 숫자 전용(tn)이라 글자를 못 그린다. 작은 폰트로 바꾼다.
        gOled.setFont(u8g2_font_6x10_tf);
        gOled.drawStr(2, kSogBaseline, "NO GPS FIX");
    }

    // 위성이 몇 개나 보이는지. 밖에서 처음 잡을 때 35초쯤 걸리는데,
    // 그동안 아무 변화가 없으면 고장인지 기다리는 중인지 알 수가 없다.
    // 0 → 1 → 3 → 6 으로 늘어나는 게 보이면 제대로 가고 있다는 뜻이다.
    gOled.setFont(u8g2_font_6x10_tf);
    if (!s.sogValid) {
        char tag[16];
        snprintf(tag, sizeof(tag), "SAT %d", s.satellites);
        const int tw = gOled.getStrWidth(tag);
        const int bx = kW - tw - 6;
        gOled.drawBox(bx, kTagBaseline - 9, tw + 5, 12);
        gOled.setDrawColor(0); // 박스 위에는 검은 글씨
        gOled.drawStr(bx + 2, kTagBaseline, tag);
        gOled.setDrawColor(1); // 원래대로 돌려놓지 않으면 다음 그리기가 다 뒤집힌다
    } else {
        if (s.hdop >= 0.0f) snprintf(buf, sizeof(buf), "H%.1f", s.hdop);
        else                snprintf(buf, sizeof(buf), "FIX");
        gOled.drawStr(kW - gOled.getStrWidth(buf) - 2, kTagBaseline, buf);
    }

    // ── 침로와 방위 ──────────────────────────────────────────────────────
    // COG 는 GPS 가 준 "가는 방향", HDG 는 자력계가 준 "뱃머리 방향".
    if (s.sogValid) snprintf(buf, sizeof(buf), "COG %03d", (int)(s.cogDeg + 0.5f) % 360);
    else            snprintf(buf, sizeof(buf), "COG ---");
    drawChecked(2, kCogBaseline, buf, "COG");

    if (s.headingDeg >= 0.0f) {
        snprintf(buf, sizeof(buf), "HDG %03d", (int)(s.headingDeg + 0.5f) % 360);
    } else {
        snprintf(buf, sizeof(buf), "HDG ---");
    }
    gOled.drawStr(66, kCogBaseline, buf);

    // ── 자세 ─────────────────────────────────────────────────────────────
    gOled.setFont(u8g2_font_5x7_tf);
    if (s.heelValid) {
        snprintf(buf, sizeof(buf), "HEEL%+6.1f  PITCH%+6.1f", s.heelDeg, s.pitchDeg);
    } else {
        snprintf(buf, sizeof(buf), "HEEL  ---   NO IMU");
    }
    drawChecked(2, kAttBaseline, buf, "자세");

    // ── 9축 원본 ─────────────────────────────────────────────────────────
    if (s.imuOk) {
        snprintf(buf, sizeof(buf), "GYR%+6.1f%+6.1f%+6.1f", s.gyrX, s.gyrY, s.gyrZ);
        drawChecked(2, kGyrBaseline, buf, "자이로");

        snprintf(buf, sizeof(buf), "ACC%+6.2f%+6.2f%+6.2f", s.accX, s.accY, s.accZ);
        drawChecked(2, kAccBaseline, buf, "가속도");
    } else {
        gOled.drawStr(2, kGyrBaseline, "IMU NOT RESPONDING");
    }

    gOled.sendBuffer();
}

} // namespace sail
