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
    float battPct        = 100.0f;
    /// 배터리 전압 (V). 0 이하면 아직 못 잰 것이라 안 그린다.
    /// 퍼센트만 보면 3.8~3.9 V 구간에서 잔량이 뚝뚝 떨어지는 것처럼 보인다.
    float battVolts      = 0.0f;

    // ── 항해 값 ──────────────────────────────────────────────────────────
    float sogKn   = 0.0f;
    /// 위치를 직접 차분해서 구한 속도. 음수면 아직 못 구함.
    /// 모듈이 주는 도플러 속도가 저속을 뭉개서, 어느 쪽이 나은지 비교 중이다.
    float sogFromPos = -1.0f;
    float cogDeg  = 0.0f;   // GPS 침로 — 배가 실제로 가는 방향
    float headingDeg = -1.0f; // 자력계 방위 — 뱃머리가 보는 방향. 음수면 없음
    float heelDeg  = 0.0f;
    float pitchDeg = 0.0f;

    // ── 9축 원본 ─────────────────────────────────────────────────────────
    float accX = 0.0f, accY = 0.0f, accZ = 0.0f; // g
    float gyrX = 0.0f, gyrY = 0.0f, gyrZ = 0.0f; // °/s
    float magX = 0.0f, magY = 0.0f, magZ = 0.0f; // µT
    bool  imuOk = false;
    bool  magOk = false;

    // ── 값이 있는가 ──────────────────────────────────────────────────────
    // ★ 없으면 숫자를 그리지 않는다. 지어낸 값도, 0 도 보여주지 않는다.
    bool  sogValid  = false; // GPS 가 위성을 잡았나
    bool  heelValid = false; // IMU 가 살아 있나
    bool  gpsFix     = false;
    int   satellites = 0;
    float hdop       = -1.0f; // 음수면 아직 모름. 작을수록 정확하다.
};

// 0x3C 가 응답하면 화면을 켜고 true. 없으면 false (그래도 정상 동작).
bool displayBegin();

// 화면이 붙어 있나
bool displayPresent();

// 부팅 중에 한 줄 띄우기. BLE 가 늦게 올라와도 보드가 살아있는 걸 눈으로 본다.
void displayBootMessage(const char* line1, const char* line2);

// 한 장면 그리기. 4 Hz 로 부르면 충분하다.
// 화면이 없거나 응답이 끊기면 아무 일도 하지 않고 바로 돌아온다.
void displayUpdate(const DisplayState& s);

// 화면이 아직 붙어 있는지 확인한다. 1 Hz 로 부른다.
// 사라졌으면 그리기를 멈추고, 다시 꽂히면 알아서 붙는다.
// 화면 하나 때문에 배가 계기를 통째로 잃으면 안 된다.
void displayHealthCheck();

} // namespace sail
