// firmware-idf 4단계 — 화면. firmware-rak/src/display_rak.cpp (커밋 2b18b17) 를 **같은 글꼴·좌표·글자** 로 옮겼다.
// 그리는 순서와 값은 한 줄씩 대응한다. 배치 근거(좌표 계산·글자 폭 실측)는 원본 주석에 있고 여기서는 줄였다.
//
// 달라진 점
//   1) U8g2 C++ 겉옷 대신 C 함수를 직접 부른다. 겉옷은 C 함수를 그대로 부르는 인라인이라 결과가 같다 [확인: U8g2lib.h].
//   2) I2C: 아두이노 Wire 대신 i2c_master. START~END 사이 바이트를 모아 한 번에 보낸다 (Wire 도 endTransmission 에 한 번에 보냈다).
//      버스는 imu.cpp 가 만든 것을 나눠 쓴다. 주소 0x3C 장치 하나를 그 버스에 붙인다. 400 kHz (원본 setBusClock(400000)).
//   3) 핀을 U8g2 에 넘기지 않는다 — 원본 주석의 "핀을 넘기면 버스가 죽는다" 는 우리 콜백이 GPIO 를 아예 안 만져서 생길 수 없다.
//   4) I2C 실패 횟수를 센다 (displayI2cErrors). 원본은 endTransmission 결과를 버렸다.
#include "display.h"

#include <cstdio>

#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

#include "board_rak.h"
#include "imu.h"

namespace sail {
namespace {

u8g2_t gOled;
bool gSetup = false;   // u8g2_Setup 을 불렀나 (한 번만)
bool gOk = false;
i2c_master_dev_handle_t gDev = nullptr;
uint32_t gI2cErrors = 0;

constexpr int kI2cTimeoutMs = 50;   // imu.cpp 와 같은 한도 (firmware-rak Wire.setTimeOut(50))

// ── U8g2 콜백 ────────────────────────────────────────────────────────────
// START_TRANSFER 부터 END_TRANSFER 까지 모은다. SSD1306 i2c 경로는 한 번에 24+1 바이트 이하로 보낸다
// [확인: u8x8_cad.c u8x8_cad_ssd13xx_fast_i2c "only 24 bytes will be sent"]. 초기화 줄도 짧다. 넉넉히 128.
uint8_t gTx[128];
size_t gTxN = 0;
bool gTxOver = false;

uint8_t byteCb(u8x8_t* u8x8, uint8_t msg, uint8_t argInt, void* argPtr) {
    (void)u8x8;
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_SET_DC:
        return 1;
    case U8X8_MSG_BYTE_START_TRANSFER:
        gTxN = 0;
        gTxOver = false;
        return 1;
    case U8X8_MSG_BYTE_SEND: {
        const uint8_t* p = static_cast<const uint8_t*>(argPtr);
        for (uint8_t i = 0; i < argInt; ++i) {
            if (gTxN < sizeof gTx) gTx[gTxN++] = p[i];
            else gTxOver = true;
        }
        return 1;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (gTxOver) {
            ++gI2cErrors;
            printf("[OLED] ★ 한 번에 보낼 바이트가 %u 를 넘었습니다 — 이 조각을 버립니다\n", (unsigned)sizeof gTx);
            return 1;
        }
        if (gDev && gTxN && i2c_master_transmit(gDev, gTx, gTxN, kI2cTimeoutMs) != ESP_OK) ++gI2cErrors;
        return 1;
    default:
        return 0;
    }
}

// 원본 u8x8_gpio_and_delay_arduino 의 지연 부분. 핀은 안 쓴다 (리셋·I2C 핀 모두 U8X8_PIN_NONE).
uint8_t gpioDelayCb(u8x8_t* u8x8, uint8_t msg, uint8_t argInt, void* argPtr) {
    (void)u8x8; (void)argPtr;
    switch (msg) {
    case U8X8_MSG_DELAY_MILLI:   vTaskDelay(pdMS_TO_TICKS(argInt ? argInt : 1)); break;
    case U8X8_MSG_DELAY_10MICRO: esp_rom_delay_us(10u * argInt); break;
    case U8X8_MSG_DELAY_100NANO: esp_rom_delay_us(1); break;
    default: break;
    }
    return 1;
}

// ── 화면 배치 (128 x 64) — 원본과 같은 값 ───────────────────────────────
constexpr int kW = 128;
constexpr int kRow1 = 8;
constexpr int kLineY = 12;
constexpr int kRow2 = 23;
constexpr int kRow3 = 36;
constexpr int kRow4 = 49;
constexpr int kRow5 = 62;
constexpr int kColL = 2;
constexpr int kColR = 68;

void drawChecked(int x, int y, const char* s, const char* what) {
    u8g2_DrawStr(&gOled, x, y, s);
    int w = u8g2_GetStrWidth(&gOled, s);
    if (x + w > kW) {
        printf("[OLED] %s 가 %dpx 넘침 (x=%d w=%d): \"%s\"\n", what, x + w - kW, x, w, s);
    }
}

bool busReady() {
    if (!imu::bus()) imu::begin();   // IMU 가 아직 버스를 안 만들었으면 만든다 (IMU 가 없어도 버스는 남는다)
    return imu::bus() != nullptr;
}

} // namespace

uint32_t displayI2cErrors() { return gI2cErrors; }

// 0x3C 가 대답하나 (원본 Wire.beginTransmission + endTransmission == 0)
bool displayPresent() {
    if (!busReady()) return false;
    return i2c_master_probe(imu::bus(), rak::kAddrDisplay, kI2cTimeoutMs) == ESP_OK;
}

bool displayBegin() {
    if (!displayPresent()) {
        gOk = false;
        return false;
    }
    if (gOk) return true;   // 이미 붙어 있으면 다시 초기화하지 않는다 (깜빡임 방지)

    if (!gDev) {
        i2c_device_config_t dc = {};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address  = rak::kAddrDisplay;
        dc.scl_speed_hz    = 400000;   // 원본 setBusClock(400000)
        if (i2c_master_bus_add_device(imu::bus(), &dc, &gDev) != ESP_OK) {
            gDev = nullptr;
            printf("[OLED] ★ I2C 장치 0x%02X 못 붙임\n", rak::kAddrDisplay);
            return false;
        }
    }
    if (!gSetup) {
        u8g2_Setup_ssd1306_i2c_128x64_noname_f(&gOled, U8G2_R0, byteCb, gpioDelayCb);
        gSetup = true;
    }
    // 원본 gOled.begin() = initDisplay · clearDisplay · setPowerSave(0)
    u8g2_InitDisplay(&gOled);
    u8g2_ClearDisplay(&gOled);
    u8g2_SetPowerSave(&gOled, 0);
    u8g2_SetFontMode(&gOled, 0);
    gOk = true;
    return true;
}

void displayHealthCheck() {
    const bool now = displayPresent();
    if (gOk && !now) {
        gOk = false;
        printf("[OLED] 응답이 끊겼습니다 — 그리기를 멈춥니다 (나머지는 계속 돕니다)\n");
    } else if (!gOk && now) {
        printf("[OLED] 다시 보입니다 — 붙입니다\n");
        displayBegin();
    }
}

void displayBootMessage(const char* line1, const char* line2) {
    if (!gOk) return;
    u8g2_ClearBuffer(&gOled);
    u8g2_SetFont(&gOled, u8g2_font_6x10_tf);
    u8g2_DrawStr(&gOled, 2, 26, line1);
    u8g2_SetFont(&gOled, u8g2_font_5x7_tf);
    u8g2_DrawStr(&gOled, 2, 42, line2);
    u8g2_SendBuffer(&gOled);
}

void displayHoldBar(int pct, const char* title, const char* hint) {
    if (!gOk) return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    u8g2_ClearBuffer(&gOled);
    u8g2_SetFont(&gOled, u8g2_font_unifont_t_korean2);

    if (title) {
        const int w = u8g2_GetUTF8Width(&gOled, title);
        u8g2_DrawUTF8(&gOled, (kW - w) / 2, 26, title);
    }
    constexpr int kBarX = 10, kBarY = 32, kBarW = 108, kBarH = 12;
    u8g2_DrawFrame(&gOled, kBarX, kBarY, kBarW, kBarH);
    const int fill = (kBarW - 4) * pct / 100;
    if (fill > 0) u8g2_DrawBox(&gOled, kBarX + 2, kBarY + 2, fill, kBarH - 4);

    if (hint) {
        const int w = u8g2_GetUTF8Width(&gOled, hint);
        u8g2_DrawUTF8(&gOled, (kW - w) / 2, 60, hint);
    }
    u8g2_SendBuffer(&gOled);
}

void displayNotice(const char* line1, const char* line2) {
    if (!gOk) return;
    u8g2_ClearBuffer(&gOled);
    u8g2_SetFont(&gOled, u8g2_font_unifont_t_korean2);
    if (line1) {
        const int w = u8g2_GetUTF8Width(&gOled, line1);
        u8g2_DrawUTF8(&gOled, (kW - w) / 2, 28, line1);
    }
    if (line2) {
        const int w = u8g2_GetUTF8Width(&gOled, line2);
        u8g2_DrawUTF8(&gOled, (kW - w) / 2, 52, line2);
    }
    u8g2_SendBuffer(&gOled);
}

int displayTextWidth(const char* utf8) {
    if (!gOk) return -1;
    u8g2_SetFont(&gOled, u8g2_font_unifont_t_korean2);
    return u8g2_GetUTF8Width(&gOled, utf8);
}

void displayOff() {
    if (!gOk) return;
    u8g2_SetPowerSave(&gOled, 1);
}

void displayOn() {
    if (!gOk) return;
    u8g2_SetPowerSave(&gOled, 0);
}

void displayUpdate(const DisplayState& s) {
    if (!gOk) return;

    char buf[40];
    u8g2_ClearBuffer(&gOled);
    u8g2_SetFont(&gOled, u8g2_font_6x10_tf);

    auto atRight = [&](int y, const char* t) {
        u8g2_DrawStr(&gOled, kW - u8g2_GetStrWidth(&gOled, t) - 2, y, t);
    };
    auto inverted = [&](const char* t) {
        u8g2_DrawBox(&gOled, 0, kRow1 - 8, u8g2_GetStrWidth(&gOled, t) + 4, 11);
        u8g2_SetDrawColor(&gOled, 0);
        u8g2_DrawStr(&gOled, 2, kRow1, t);
        u8g2_SetDrawColor(&gOled, 1);   // 안 되돌리면 다음 그리기가 다 뒤집힌다
    };

    // ── 1줄  이름 · BLE · 배 번호 ────────────────────────────────────────
    if (s.recording) {
        char rec[24];
        snprintf(rec, sizeof(rec), "REC %02u:%02u",
                 (unsigned)(s.recSeconds / 60), (unsigned)(s.recSeconds % 60));
        inverted(rec);
    } else if (s.recClosing) {
        inverted("SAVING");
    } else if (s.recFailed) {
        inverted("REC FAIL");
    } else {
        drawChecked(kColL, kRow1, s.userName, "이름");
    }

    if (s.boatId > 0) snprintf(buf, sizeof(buf), "B%02u", (unsigned)s.boatId);
    else              snprintf(buf, sizeof(buf), "B--");
    atRight(kRow1, buf);

    const char* ble = s.bleConnected ? (s.bleNotifying ? "BLE*" : "BLE") : "ADV";
    u8g2_DrawStr(&gOled, kW - 2 - 17 - 6 - u8g2_GetStrWidth(&gOled, ble), kRow1, ble);

    u8g2_DrawHLine(&gOled, 0, kLineY, kW);

    // ── 2줄  속도 — 값이 없으면 숫자를 안 그린다 ─────────────────────────
    if (s.sogValid) snprintf(buf, sizeof(buf), "SOG %.2f kn", s.sogKn);
    else            snprintf(buf, sizeof(buf), "SOG --- kn");
    drawChecked(kColL, kRow2, buf, "SOG");

    if (s.gnssMode) {
        const char* mw =
            s.gnssMode == 'h' ? "port" : s.gnssMode == 's' ? "stat" :
            s.gnssMode == 'p' ? "ped"  : s.gnssMode == 'c' ? "car"  :
            s.gnssMode == 'b' ? "boat" : "?";
        u8g2_DrawStr(&gOled, kColR + 12, kRow2, mw);
    }

    // ── 3줄  침로와 방위 ─────────────────────────────────────────────────
    if (s.sogValid) snprintf(buf, sizeof(buf), "COG %03d", (int)(s.cogDeg + 0.5f) % 360);
    else            snprintf(buf, sizeof(buf), "COG ---");
    drawChecked(kColL, kRow3, buf, "COG");

    if (s.headingDeg >= 0.0f) snprintf(buf, sizeof(buf), "HDG %03d", (int)(s.headingDeg + 0.5f) % 360);
    else                      snprintf(buf, sizeof(buf), "HDG ---");
    drawChecked(kColR, kRow3, buf, "HDG");

    // ── 4줄  힐과 피치 ───────────────────────────────────────────────────
    if (s.heelValid) snprintf(buf, sizeof(buf), "HEEL %.1f", s.heelDeg);
    else             snprintf(buf, sizeof(buf), "HEEL ---");
    drawChecked(kColL, kRow4, buf, "HEEL");

    if (s.heelValid) snprintf(buf, sizeof(buf), "PIT %.1f", s.pitchDeg);
    else             snprintf(buf, sizeof(buf), s.imuOk ? "PIT ---" : "NO IMU");
    drawChecked(kColR, kRow4, buf, "PIT");

    // ── 5줄  배터리 · 모드 · 위성 ────────────────────────────────────────
    if (s.battVolts > 0.0f) {
        snprintf(buf, sizeof(buf), "%.2fV", s.battVolts);
        drawChecked(kColL, kRow5, buf, "전압");
        if (s.battLow) u8g2_DrawStr(&gOled, kColL + 62, kRow5, "LOW");
    }
    {
        const char* mw =
            s.gnssModeNow == 'h' ? "port" : s.gnssModeNow == 's' ? "stat" :
            s.gnssModeNow == 'p' ? "ped"  : s.gnssModeNow == 'c' ? "car"  :
            s.gnssModeNow == 'b' ? "boat" : "?";
        u8g2_DrawStr(&gOled, kColL + 36, kRow5, mw);
    }
    if (!s.sogValid)         snprintf(buf, sizeof(buf), "SAT %d", s.satellites);
    else if (s.hdop >= 0.0f) snprintf(buf, sizeof(buf), "H%.1f", s.hdop);
    else                     snprintf(buf, sizeof(buf), "FIX");
    atRight(kRow5, buf);

    u8g2_SendBuffer(&gOled);
}

} // namespace sail

namespace diag {
void oledWidths() {
    static const char* kLines[] = {
        "켜는 중", "놓으면 취소", "켜집니다",
        "누르면 꺼짐", "놓으면 기록시작", "기록 멈춤",
        "끄는 중", "기록 저장 중", "기록 종료", "저장 중",
        "기록 시작", "꺼졌습니다", "5초 눌러 켜기",
        "버튼이 눌린 채", "끄지 않습니다", "손을 떼세요",
    };
    printf("──────────────────────────────────────────\n");
    printf("  화면 글자 폭 (128px 안에 들어가야 한다)\n");
    printf("  한글 16px · 빈칸/숫자 8px. 0 이 섞이면 글꼴에 없는 글자다.\n");
    for (const char* t : kLines) {
        const int w = sail::displayTextWidth(t);
        printf("  %-20s %4d px  %s\n", t, w, w < 0 ? "화면 없음" : (w <= 128 ? "OK" : "★ 넘침"));
    }
    printf("──────────────────────────────────────────\n");
}
} // namespace diag
