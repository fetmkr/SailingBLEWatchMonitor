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
//    8   │ random()               ADV      B07   │
//   12   ├──────────────────────────────────────┤
//   23   │ SOG 12.34 kn    ped                   │
//   36   │ COG 045     HDG 344                   │
//   49   │ HEEL -12.4  PIT -2.9                  │
//   62   │ 4.06V                        SAT 12   │
//        └──────────────────────────────────────┘
//
// **글자 크기는 하나다 — 6x10.** 예전에는 속도만 18px 로 키우고 나머지를
// 5x7 로 깔았는데, 자이로·가속도 두 줄을 빼면서 자리가 남았다. 자리가 있는데
// 크기를 섞을 이유가 없다. 다섯 줄을 13px 씩 고르게 벌려 놓았다.
//
//   COG 는 배가 실제로 가는 방향 (GPS). 멈춰 있으면 안 나온다.
//   HDG 는 뱃머리가 보는 방향 (자력계). 멈춰 있어도 나온다.
//   요트에서는 이 둘이 다르다. 그래서 나란히 보여준다.
//   자력계·자이로·가속도 원본은 화면에서 뺐다 (시리얼에는 나온다).
//
// ── 자리를 눈대중으로 잡지 않았다 ────────────────────────────────────────
//
// 실기기에서 getStrWidth() 로 잰 값이다 [확인: `fontw` 명령, 2026-08-28].
// 6x10_tf 는 높이 10 (위 7 · 아래 2), 글자당 정확히 6px 이다.
//
//   "COG 045" 41    "HDG 344" 41    "PITCH -2.9" 59    "PIT -2.9" 47
//   "SAT 12"  35    "4.06V"   29    "B07"        17    "12.34"    29
//
// 가로 — 한 줄에 최대 21글자(126px)가 들어간다. 제일 빡빡한 줄이 힐·피치다.
//   최악  "HEEL -90.0"(59) + 사이 6 + "PIT -90.0"(53) = 118  →  들어간다
//   PITCH 를 PIT 로 줄인 이유가 이것이다. 다 쓰면 130 이라 넘친다.
//
// 세로 — 기준선 8·23·36·49·62. 6x10 은 기준선 위 7 · 아래 2 를 쓰므로
//   1~10 · 16~25 · 29~38 · 42~51 · 55~64. 겹치는 데가 없고 64 를 안 넘는다.
constexpr int kW = 128;
constexpr int kH = 64;

constexpr int kRow1 = 8;  // 이름 / REC · BLE · 배 번호
constexpr int kLineY = 12; // 상태줄 아래 가로선
constexpr int kRow2 = 23; // 속도
constexpr int kRow3 = 36; // 침로와 방위
constexpr int kRow4 = 49; // 힐과 피치
constexpr int kRow5 = 62; // 배터리 · 위성. 가운데는 로라 자리로 비워 둔다

constexpr int kColL = 2;  // 왼쪽 값
constexpr int kColR = 68; // 오른쪽 값 (힐 59px 뒤에 6px 띄운 자리)

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

    // 글자 크기는 이 하나뿐이다. 아래에서 다시 setFont 을 부르지 않는다.
    gOled.setFont(u8g2_font_6x10_tf);

    // 한 줄 안에서 왼쪽·오른쪽에 하나씩 놓는다. 오른쪽 것은 오른쪽 정렬이라
    // 자릿수가 바뀌어도 끝이 안 흔들린다.
    auto atRight = [&](int y, const char* t) {
        gOled.drawStr(kW - gOled.getStrWidth(t) - 2, y, t);
    };

    // ── 1줄  이름 · BLE · 배 번호 ────────────────────────────────────────
    // 기록 중이면 이름 대신 REC 와 지난 시간. 배에서 제일 궁금한 게 이거다.
    if (s.recording) {
        char rec[24];
        snprintf(rec, sizeof(rec), "REC %02u:%02u",
                 (unsigned)(s.recSeconds / 60), (unsigned)(s.recSeconds % 60));
        gOled.drawBox(0, kRow1 - 8, gOled.getStrWidth(rec) + 4, 11);
        gOled.setDrawColor(0);
        gOled.drawStr(2, kRow1, rec);
        gOled.setDrawColor(1); // 안 되돌리면 다음 그리기가 다 뒤집힌다
    } else {
        drawChecked(kColL, kRow1, s.userName, "이름");
    }

    // 배 번호가 제일 오른쪽이다. 배를 물에 내리기 전에 뱃머리 번호표와 눈으로
    // 맞춰 보는 값이라, 앱 없이 화면만 훑어도 읽혀야 한다 (PROTOCOL.md §10.11).
    if (s.boatId > 0) snprintf(buf, sizeof(buf), "B%02u", (unsigned)s.boatId);
    else              snprintf(buf, sizeof(buf), "B--");
    atRight(kRow1, buf);

    // 별표는 앱이 notify 를 구독 중이라는 뜻.
    const char* ble = s.bleConnected ? (s.bleNotifying ? "BLE*" : "BLE") : "ADV";
    gOled.drawStr(kW - 2 - 17 - 6 - gOled.getStrWidth(ble), kRow1, ble);

    gOled.drawHLine(0, kLineY, kW);

    // ── 2줄  속도 ────────────────────────────────────────────────────────
    //
    // ★ 값이 없으면 숫자를 아예 안 그린다. 그럴듯한 숫자가 떠 있으면 사람은
    //   그걸 읽는다. 예전에는 시뮬레이터 값에 SIM 을 붙여 띄웠는데 결국
    //   헷갈렸다. 0 도 안 된다 — 정박 중 0.0 kn 과 구별되지 않는다.
    if (s.sogValid) snprintf(buf, sizeof(buf), "SOG %.2f kn", s.sogKn);
    else            snprintf(buf, sizeof(buf), "SOG --- kn");
    drawChecked(kColL, kRow2, buf, "SOG");

    // GPS 움직임 종류. 모드를 번갈아 걸며 견주는 동안(`ab`)만 그린다. 평소에는
    // 어느 모드로 걸어 뒀는지 이미 알고 있어서 자리만 뺏는다. 한 글자로 썼더니
    // 눈에 안 띄어서 낱말로 쓴다. fix 가 없을 때도 그린다 — 실내에서 설정을
    // 바꿔 놓고 화면으로 확인해야 하고, 이건 잰 값이 아니라 우리가 건 설정이라
    // 언제나 확실하다.
    if (s.gnssMode) {
        const char* mw =
            s.gnssMode == 'h' ? "port" : s.gnssMode == 's' ? "stat" :
            s.gnssMode == 'p' ? "ped"  : s.gnssMode == 'c' ? "car"  :
            s.gnssMode == 'b' ? "boat" : "?";
        gOled.drawStr(kColR + 12, kRow2, mw);
    }

    // ── 3줄  침로와 방위 ─────────────────────────────────────────────────
    // COG 는 GPS 가 준 "가는 방향", HDG 는 자력계가 준 "뱃머리 방향".
    if (s.sogValid) snprintf(buf, sizeof(buf), "COG %03d", (int)(s.cogDeg + 0.5f) % 360);
    else            snprintf(buf, sizeof(buf), "COG ---");
    drawChecked(kColL, kRow3, buf, "COG");

    if (s.headingDeg >= 0.0f) snprintf(buf, sizeof(buf), "HDG %03d", (int)(s.headingDeg + 0.5f) % 360);
    else                      snprintf(buf, sizeof(buf), "HDG ---");
    drawChecked(kColR, kRow3, buf, "HDG");

    // ── 4줄  힐과 피치 ───────────────────────────────────────────────────
    // 우현으로 누우면 힐 양수, 뱃머리가 들리면 피치 양수 (PROTOCOL.md §3.1).
    // PITCH 를 다 쓰면 한 줄에 안 들어간다. 위 배치 주석의 계산 참고.
    if (s.heelValid) snprintf(buf, sizeof(buf), "HEEL %.1f", s.heelDeg);
    else             snprintf(buf, sizeof(buf), "HEEL ---");
    drawChecked(kColL, kRow4, buf, "HEEL");

    if (s.heelValid) snprintf(buf, sizeof(buf), "PIT %.1f", s.pitchDeg);
    else             snprintf(buf, sizeof(buf), s.imuOk ? "PIT ---" : "NO IMU");
    drawChecked(kColR, kRow4, buf, "PIT");

    // ── 5줄  배터리와 위성 ───────────────────────────────────────────────
    // 배터리는 전압만. 퍼센트를 안 그리는 이유는 display_rak.h 주석 참고.
    // 가운데는 로라 상태(§10) 자리로 비워 둔다.
    if (s.battVolts > 0.0f) {
        snprintf(buf, sizeof(buf), "%.2fV", s.battVolts);
        drawChecked(kColL, kRow5, buf, "전압");
    }

    // 위성이 몇 개나 보이는지. 밖에서 처음 잡을 때 35초쯤 걸리는데, 그동안
    // 아무 변화가 없으면 고장인지 기다리는 중인지 알 수가 없다. 0 → 1 → 3 → 6
    // 으로 늘어나는 게 보이면 제대로 가고 있다는 뜻이다.
    // 잡고 나면 HDOP 로 바뀐다 — 작을수록 정확하다.
    if (!s.sogValid)         snprintf(buf, sizeof(buf), "SAT %d", s.satellites);
    else if (s.hdop >= 0.0f) snprintf(buf, sizeof(buf), "H%.1f", s.hdop);
    else                     snprintf(buf, sizeof(buf), "FIX");
    atRight(kRow5, buf);

    gOled.sendBuffer();
}

} // namespace sail
