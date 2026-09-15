// firmware-idf 4단계 — 화면 (RAK1921 SSD1306 128x64, I2C 0x3C)
//
// ★ API 는 firmware-rak/include/display_rak.h 를 **그대로 같이 쓴다** (아두이노 흔적 0, PORTING.md 규칙 4).
//   구현만 main/display.cpp 에 새로 짰다: U8g2 C 원본(components/u8g2, 2.36.18) + i2c_master 콜백.
//
//   firmware-rak                        여기
//   ──────────────────────────────      ─────────────────────────────────────
//   U8G2_SSD1306_128X64_NONAME_F_HW_I2C  u8g2_Setup_ssd1306_i2c_128x64_noname_f + 우리 I2C·지연 콜백
//   Wire (main 이 먼저 begin)            imu::bus() 를 나눠 쓴다. 없으면 imu::begin() 이 버스를 만든다
//   gOled.xxx()                         u8g2_Xxx(&u8g2, …)  (U8g2lib.h 의 겉옷이 부르는 C 함수 그대로)
//   diag::oledWidths()  (diagnostics)    diag::oledWidths()  (여기 display.cpp)
//   Serial.printf                        printf
#pragma once

#include "display_rak.h"   // sail::DisplayState · displayBegin · displayUpdate · …

namespace diag {
// `oledw` 명령 — 한글 안내 줄이 128px 안에 들어가나 (firmware-rak diagnostics.cpp 와 같은 글자)
void oledWidths();
}

namespace sail {
// I2C 로 보내다 실패한 횟수 (이 부팅). firmware-rak 에는 없던 진단 — Wire.endTransmission 결과를 버렸다.
uint32_t displayI2cErrors();
}
