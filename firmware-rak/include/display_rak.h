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
    /// 배터리 전압 (V). 0 이하면 아직 못 잰 것이라 안 그린다.
    ///
    /// **퍼센트는 안 그린다.** 리튬폴리머는 3.8~3.9 V 에서 방전 곡선이 거의
    /// 평평해서, 전압이 0.05 V 떨어지면 퍼센트가 20 씩 내려앉는다. 퍼센트만
    /// 보면 배터리가 갑자기 닳는 것처럼 보인다. 전압은 그런 거짓말을 안 한다.
    /// 퍼센트는 BLE 로는 그대로 나간다 (PROTOCOL.md §3).
    float battVolts      = 0.0f;
    /// 로라 배 번호 (PROTOCOL.md §10.11). 0 이면 번호 없음 — `B--` 로 그린다.
    uint8_t boatId       = 0;
    /// 기록 중인가. 참이면 이름 자리에 REC 와 지난 시간을 보여준다.
    bool  recording      = false;
    uint32_t recSeconds  = 0;

    // ── 항해 값 ──────────────────────────────────────────────────────────
    float sogKn   = 0.0f;
    /// 위치를 직접 차분해서 구한 속도. 음수면 아직 못 구함.
    /// 모듈이 주는 도플러 속도가 저속을 뭉개서, 어느 쪽이 나은지 비교 중이다.
    float sogFromPos = -1.0f;
    float cogDeg  = 0.0f;   // GPS 침로 — 배가 실제로 가는 방향
    float headingDeg = -1.0f; // 자력계 방위 — 뱃머리가 보는 방향. 음수면 없음
    float heelDeg  = 0.0f;
    float pitchDeg = 0.0f;
/// GPS 움직임 종류. 속도 옆에 낱말로 그린다.
    ///
    /// **0 이면 안 그린다.** 정해 둔 모드(NVS 의 gps_dyn)가 실제로 걸려
    /// 있으면 자리를 뺏을 이유가 없다. **다를 때만** 띄운다.
    ///
    /// 모듈은 전원이 오르내리면 기본값 0(휴대)으로 돌아가는데, 그 모드는
    /// 저속을 통째로 0 으로 뭉갠다. 2026-08-30 에 29분짜리 세션의 속도를
    /// 그렇게 통째로 잃었다. 물 위에서는 화면 말고 알아챌 방법이 없다.
    ///
    /// h 휴대 · s 정지 · p 보행 · c 자동차 · b 선박 · ? 모름
    char gnssMode = 0;

    // ── 센서가 살아 있나 ─────────────────────────────────────────────────
    // 9축 원본 세 축은 화면에서 뺐으므로 여기에도 안 둔다. 화면이 안 쓰는
    // 값을 이 구조체에 남겨 두면 "화면에 나오는 값" 으로 오해하게 된다.
    // 원본은 BLE 확장 페이로드(PROTOCOL.md §3.1)와 시리얼로 나간다.
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
