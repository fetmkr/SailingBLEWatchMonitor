// firmware-idf 7단계 — 끄기(깊은잠) · 깬 뒤 5초 문턱 · 저장 단추 · 잠자기 기록 · 켤 때 배터리 곡선
//
// firmware-rak src/main.cpp (커밋 2b18b17) 를 **뜻·글자·순서 그대로** 옮겼다. 자세한 경위는 POWER.md.
//
//   firmware-rak (main.cpp)                     여기
//   ──────────────────────────────────────────  ─────────────────────────────────────────────
//   wakeGate() 2253                             power::wakeGate()          app_main 맨 앞 (NVS·워치독 전)
//   gWokeFromSleep                              power::wokeFromSleep()
//   armButtonWake() 3044                        (power.cpp 안)
//   goToSleep(testSec, leaveCard) 3091          power::goToSleep(testSec, leaveCard)
//   requestPowerOff(force) 3640                 power::requestPowerOff(force)
//   recControlTick 의 gOffRequestedAt 칸 5463    power::offTick(now) — 참이면 recControlTick 이 거기서 return
//   gOffRequestedAt · gOffForceArmed            (power.cpp 안)
//   buttonBegin() 2588 · buttonPoll() 2614      power::buttonBegin() · power::buttonPoll(now)
//   gBtnOwnsScreen                              power::buttonOwnsScreen()   — 그리기 전에 본다
//   gRtc* (RTC_DATA_ATTR) 2560-2568             (power.cpp 안)
//   sleepStatsNow() 3730                        power::sleepStats()
//   diag::sleepReport (diagnostics.cpp)         power::sleepReport()       — `sleepstat`
//   sleepLogToCard() 2735                       (power::reportAfterBoot 안)
//   setup 의 "깼으면 기록부터" 5603             power::reportAfterBoot()   — hlog::begin 뒤
//   gBattBoot · loop 1f 5719 · battboot 4344    power::battBootTick(now) · power::battBootReport(battVoltsNow)
//
// ★ 메인의 기록 제어 상태(gWantRec 등)는 여기서 안 만진다. 메인이 Hooks 로 넘긴 함수를 부른다.
#pragma once

#include <cstdint>

#include "diagnostics.h"   // firmware-rak/include — diag::SleepStats (아두이노 흔적 없음)

namespace power {

// 메인이 준다. 모두 루프 작업에서 불린다 (콜백이 아니다).
struct Hooks {
    float       (*readBatteryVolts)() = nullptr;        // 16번 평균. 못 읽으면 0
    void        (*feedWatchdog)()     = nullptr;
    void        (*pollRecResult)()    = nullptr;        // { SessionResult r; if (hlog::poll(&r)) recOnResult(r, now); }
    void        (*recWantOff)(const char* who) = nullptr;
    bool        (*recWantOn)(const char* who)  = nullptr;
    // requestPowerOff 의 "기록 중 아닌데 원하는 상태가 남았으면 지운다" 칸:
    //   if (gWantRec || gRecGaveUp) { gWantRec = false; gRecGaveUp = false; gRecRestartAt = 0; recSaveWant(false); }
    void        (*dropWantIfSet)()    = nullptr;
    // 강제 끄기: gWantRec = false (NVS rec_want·rec_forced·rec_fail 은 power 가 적는다)
    void        (*clearWantFlag)()    = nullptr;
    const char* (*recStartErrShort)() = nullptr;        // 화면 한 줄용. 없으면 nullptr
    int         sensorPowerPin        = 14;             // gSensorPowerPin (rak::kSensorPowerA)
};
void setHooks(const Hooks& h);

void wakeGate();
bool wokeFromSleep();

void buttonBegin();
void buttonPoll(uint32_t nowMs);
bool buttonOwnsScreen();

void requestPowerOff(bool force);
bool offTick(uint32_t nowMs);          // 끄기를 기다리는 중이면 참 (recControlTick 이 return)
void goToSleep(uint32_t testWakeSec = 0, bool leaveCard = false);   // 버튼이 안 떨어지면 돌아온다

diag::SleepStats sleepStats();
void sleepReport();                    // `sleepstat` — diag::sleepReport 와 같은 글자
void reportAfterBoot();                // 깊은잠에서 깼으면 sleepReport + /sd/SLEEP.TXT 한 줄

void battBootTick(uint32_t nowMs);     // 루프 매 바퀴. 켠 뒤 20초 동안 1초마다 담는다
void battBootReport(float battVoltsNow);   // `battboot`

} // namespace power
