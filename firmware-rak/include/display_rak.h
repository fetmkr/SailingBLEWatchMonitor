// RAK1921 OLED (SSD1306 128x64, I2C 0x3C) 표시.
//
// ★ 이 화면은 센서 슬롯이 아니라 RAK19007 의 J12 헤더(2.54 mm I2C 핀헤더)에
//   꽂는다. 그래서 I2C1 버스에 IMU 와 나란히 매달린다. 주소가 0x3C 와 0x68 로
//   달라서 부딪히지 않는다.
//
// 화면이 안 꽂혀 있어도 펌웨어는 그대로 돌아야 한다. displayBegin() 이
// 0x3C 응답을 먼저 확인하고, 없으면 조용히 꺼진 채로 지나간다.
#pragma once

#include <stdint.h>

namespace sail {

// 화면에 그릴 한 장면. main.cpp 의 전역 상태를 여기에 담아 넘긴다.
struct DisplayState {
    const char* userName = "";    // "2FB5" — 접두사 뺀 이름
    bool  bleConnected   = false;
    bool  bleNotifying   = false;

    float sogKn   = 0.0f;
    float cogDeg  = 0.0f;
    float heelDeg = 0.0f;
    float battPct = 100.0f;

    // 값의 출처. false 면 시뮬레이터로 채운 값이라 화면에 SIM 을 띄운다.
    bool sogFromGps  = false;
    bool heelFromImu = false;

    int  satellites = 0;
    bool gpsFix     = false;

    uint32_t uptimeMs = 0;
};

// 0x3C 가 응답하면 화면을 켜고 true. 없으면 false (그래도 정상 동작).
bool displayBegin();

// 화면이 붙어 있나
bool displayPresent();

// 부팅 중에 한 줄 띄우기. BLE 가 늦게 올라와도 보드가 살아있는 걸 눈으로 본다.
void displayBootMessage(const char* line1, const char* line2);

// 한 장면 그리기. 4 Hz 로 부르면 충분하다.
void displayUpdate(const DisplayState& s);

} // namespace sail
