// Sailing Monitor — Adafruit ESP32-S3 TFT Feather 내장 화면 (240x135 ST7789)
//
// 보드가 지금 만들어 내보내고 있는 값을 그대로 보여준다.
// (모듈이 송신 측이므로 화면에는 "수신" 개념이 없다 — 광고/연결 상태만 표시)
//
//   ┌────────────────────────────────────────┐
//   │ hojun                        ● LINK    │  상단바: 이름 + 연결 상태
//   │                                        │
//   │   5.53 kn                              │  SOG (큰 숫자)
//   │                                        │
//   │   045° NE                              │  COG
//   │                                        │
//   │ HEEL +12  BATT 100%  SEQ 42  UP 123s   │  하단바
//   └────────────────────────────────────────┘
//
// 핀은 보드 variant(pins_arduino.h)가 정의한 값을 그대로 쓴다.
//   TFT_I2C_POWER 21 · TFT_CS 7 · TFT_DC 39 · TFT_RST 40 · TFT_BACKLITE 45
#pragma once

#include "protocol.h"

#if SAIL_HAS_TFT

namespace sail {

// 화면에 그릴 상태 묶음
struct DisplayState {
    const char* name      = "";    // 사용자 이름 부분 (접두사 제외)
    uint8_t     moduleID  = 0;
    bool        connected = false;
    bool        notifying = false;
    uint8_t     seq       = 0;
    Telemetry   telemetry;
};

// 패널 전원·초기화·백라이트. setup() 맨 앞에서 한 번.
void displayBegin();

// 부팅 직후 한 줄 메시지 (BLE 초기화 전에 화면이 켜졌는지 눈으로 확인용)
void displayBootMessage(const char* line1, const char* line2);

// 정규 화면의 정적 요소(바, 고정 라벨)를 그린다. BLE 준비가 끝난 뒤 한 번.
void displayBeginMainScreen();

// 바뀐 부분만 다시 그린다. loop() 에서 주기적으로 호출(4 Hz 로 충분).
void displayUpdate(const DisplayState& s);

} // namespace sail

#endif // SAIL_HAS_TFT
