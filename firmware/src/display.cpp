#include "display.h"

#if HOHO_HAS_TFT

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>

#include "display_layout.h"

namespace hoho {
namespace {

namespace L = hoho::layout;

// 핀은 보드 variant(pins_arduino.h)가 정의한 값을 그대로 쓴다.
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ── 색 ───────────────────────────────────────────────────────────────────
constexpr uint16_t kBg    = ST77XX_BLACK;
constexpr uint16_t kBarBg = 0x2124; // 어두운 회색
constexpr uint16_t kFg    = ST77XX_WHITE;
constexpr uint16_t kDim   = 0x8410; // 중간 회색
constexpr uint16_t kSogC  = ST77XX_CYAN;
constexpr uint16_t kCogC  = ST77XX_YELLOW;
constexpr uint16_t kOk    = ST77XX_GREEN;
constexpr uint16_t kWarn  = ST77XX_ORANGE;

// 직전에 그린 문자열. 같으면 다시 그리지 않는다(SPI 절약 + 깜빡임 방지).
char     lastName[24]   = {0};
char     lastStatus[16] = {0};
char     lastSog[12]    = {0};
char     lastCog[12]    = {0};
char     lastPoint[8]   = {0};
char     lastBottom[64] = {0};
uint16_t lastStatusColor = 0;

// 고정폭 문자열을 그린다. 길이가 같으므로 배경색이 이전 글자를 덮어 지운다.
void drawFixed(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint16_t bg,
               const char* text) {
    tft.setTextSize(size);
    tft.setTextColor(fg, bg);
    tft.setCursor(x, y);
    tft.print(text);
}

void forgetCache() {
    lastName[0] = lastStatus[0] = lastSog[0] = '\0';
    lastCog[0] = lastPoint[0] = lastBottom[0] = '\0';
    lastStatusColor = 0;
}

} // namespace

// ── 공개 API ─────────────────────────────────────────────────────────────

void displayBegin() {
    // ★ 이 보드는 TFT_I2C_POWER(GPIO21) 를 HIGH 로 올리지 않으면
    //   화면에 전원이 안 들어간다. (STEMMA QT 커넥터 전원도 같은 핀)
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    delay(10);

    tft.init(L::kPanelW, L::kPanelH);
    tft.setRotation(3); // 240x135 가로
    tft.fillScreen(kBg);
    tft.setTextWrap(false);
    tft.cp437(true); // 0xF8 = 도(°) 기호

    // 백라이트는 화면 초기화 후에 켠다 (부팅 시 흰 화면 깜빡임 방지)
    pinMode(TFT_BACKLITE, OUTPUT);
    digitalWrite(TFT_BACKLITE, HIGH);
}

void displayBootMessage(const char* line1, const char* line2) {
    tft.fillScreen(kBg);
    drawFixed(6, 40, 2, kFg, kBg, line1);
    if (line2) drawFixed(6, 68, 1, kDim, kBg, line2);
    forgetCache();
}

void displayBeginMainScreen() {
    tft.fillScreen(kBg);
    tft.fillRect(0, 0, L::kW, L::kTopBarH, kBarBg);
    tft.fillRect(0, L::kBotBarY, L::kW, L::kBotBarH, kBarBg);
    // 고정 라벨은 한 번만
    drawFixed(L::kUnitX, L::kUnitY, L::kUnitSize, kDim, kBg, "kn");
    forgetCache();
}

void displayUpdate(const DisplayState& s) {
    char buf[64];

    // ── 상단바: 이름 ─────────────────────────────────────────────────────
    L::formatName(buf, sizeof(buf), s.name);
    if (strcmp(buf, lastName) != 0) {
        drawFixed(L::kNameX, L::kNameY, L::kNameSize, kFg, kBarBg, buf);
        strcpy(lastName, buf);
    }

    // ── 상단바: 상태 ─────────────────────────────────────────────────────
    const char* status;      // 11글자 고정
    uint16_t    statusColor;
    if (s.connected) {
        status      = s.notifying ? "LINK NOTIFY" : "LINK       ";
        statusColor = kOk;
    } else {
        status      = "ADVERTISING";
        statusColor = kWarn;
    }
    if (strcmp(status, lastStatus) != 0 || statusColor != lastStatusColor) {
        drawFixed(L::kStatusX, L::kStatusY, 1, statusColor, kBarBg, status);
        tft.fillCircle(L::kStatusDotX, L::kStatusDotY, L::kStatusDotR, statusColor);
        strcpy(lastStatus, status);
        lastStatusColor = statusColor;
    }

    // ── SOG (큰 숫자) ────────────────────────────────────────────────────
    L::formatSog(buf, sizeof(buf), s.telemetry.sogKn);
    if (strcmp(buf, lastSog) != 0) {
        drawFixed(L::kSogX, L::kSogY, L::kSogSize, kSogC, kBg, buf);
        strcpy(lastSog, buf);
    }

    // ── COG ──────────────────────────────────────────────────────────────
    L::formatCog(buf, sizeof(buf), s.telemetry.cogDeg);
    if (strcmp(buf, lastCog) != 0) {
        drawFixed(L::kCogX, L::kCogY, L::kCogSize, kCogC, kBg, buf);
        tft.setTextSize(L::kDegSize);
        tft.setTextColor(kCogC, kBg);
        tft.setCursor(L::kDegX, L::kDegY);
        tft.write(0xF8); // °
        strcpy(lastCog, buf);
    }

    const char* point = L::compassPoint(s.telemetry.cogDeg);
    if (strcmp(point, lastPoint) != 0) {
        drawFixed(L::kPointX, L::kPointY, L::kPointSize, kDim, kBg, point);
        strcpy(lastPoint, point);
    }

    // ── 하단바 ───────────────────────────────────────────────────────────
    L::formatBottom(buf, sizeof(buf),
                    (int)encodeHeel(s.telemetry.heelDeg),
                    (int)encodeBatt(s.telemetry.battPct),
                    (unsigned)s.seq,
                    (unsigned)(s.telemetry.uptimeMs / 1000));
    if (strcmp(buf, lastBottom) != 0) {
        drawFixed(L::kBotX, L::kBotTextY, L::kBotSize, kDim, kBarBg, buf);
        strcpy(lastBottom, buf);
    }
}

} // namespace hoho

#endif // HOHO_HAS_TFT
