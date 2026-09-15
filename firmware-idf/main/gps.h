// firmware-idf 2단계 — GPS (RAK12501 / Quectel L76K, 슬롯 A, UART1)
//
// firmware-rak src/main.cpp 의 GPS 부분을 **뜻·단위·무효 표식 그대로** 옮겼다.
// 루프가 부르는 모양이다. 콜백·ISR 에서 일하지 않는다.
//
//   켤 때      gps::begin()          UART1 설치(핀 한 번) → 9600·115200 두 속도로 PCAS → 10 Hz → NAV-PV → 선박 모드
//   루프마다    gps::poll()           UART 바이트를 CASIC 파서와 TinyGPS++ 에 먹인다. 화면 속도 갱신
//   10 Hz      gps::updateFix()      fix 판정 · 시계 맞추기 · 다듬기 · 위치차분
//
// ── firmware-rak → 여기 ──────────────────────────────────────────────────
//
//   firmware-rak (main.cpp)          여기
//   ─────────────────────────────    ────────────────────────────────────────
//   gGps (TinyGPSPlus)               gps::parser()
//   kGpsBaud                         gps::kBaud
//   kGpsBoatMode · gGpsDynWant       gps::kBoatMode
//   kGpsStaleMs                      gps::kStaleMs
//   kPvVelMeasured                   gps::kPvVelMeasured
//   kDampTau[6]                      gps::kDampTau
//   gGpsHz                           gps::state().hz
//   gGpsDyModel                      gps::state().dyModel          (255 = 아직 못 물어봄)
//   gCasicIdFirst                    gps::state().casicIdFirst
//   gGpsFix · gEverHadFix            gps::state().fix · .everHadFix
//   gClockSet                        gps::state().clockSet
//   gPvSpeedKn · gPvCogDeg           gps::state().pvSpeedKn · .pvCogDeg          (-1 = 없음)
//   gPvAccKn · gPvHAccM · gPvCogAccDeg  gps::state().pvAccKn · .pvHAccM · .pvCogAccDeg (-1 = 없음)
//   gPvVelFlag · gPvVelValid         gps::state().pvVelFlag · .pvVelValid
//   gPvAtMs · gPvVelN · gPvVelE      gps::state().pvAtMs · .pvVelN · .pvVelE     (pvAtMs 0 = 한 번도 안 옴)
//   gCasicRejected                   gps::state().casicRejected
//   gLastFixRmc · gLastFixAtMs       gps::state().lastFixRmc · .lastFixAtMs
//   gMaxSogKn · gMaxSogFromPos · gMaxSogPv  gps::state().maxSogKn · .maxSogFromPos · .maxSogPv
//   gFixSeenCount                    gps::state().fixSeenCount
//   gSogFromPos                      gps::state().sogFromPos       (-1 = 못 구함)
//   gSogDamped · gCogDamped          gps::state().sogDamped · .cogDamped (-1 = 없음)
//   gSlowMode                        gps::state().slowMode
//   gSogShownOk · gSogShownKn        gps::state().sogShownOk · .sogShownKn
//   gSogShortKn · gSogLongKn         gps::state().sogShortKn · .sogLongKn (-1 = 없음)
//   gSogBadCount                     gps::state().sogBadCount
//   gDampLevel · gDeadbandKn         gps::setDampLevel() · gps::setDeadbandKn()  / state().dampLevel · .deadbandKn
//   gGpsPauseUntil (test gps <초>)   gps::testPause(sec)
//   gpsBegin()                       gps::begin()   (+ setup 의 "[GPS] UART1 ..." 한 줄)
//   gpsPoll()                        gps::poll()
//   gpsUpdateFix() · clockFromGps()  gps::updateFix()
//   gpsWeekTow()                     gps::weekTow()
//   sogOut()                         gps::sogOut()
//   gpsSend()                        gps::send()
//   gpsSetRate()                     gps::setRate()
//   gpsApplyNavPv() · gpsApplyBoatMode()  gps::applyNavPv() · gps::applyBoatMode()
//   casicSend/Wait/WaitAck/SetAcked/Query  gps::casicSend/casicWait/casicWaitAck/casicSetAcked/casicQuery
//   dyModelName()                    gps::dyModelName()
//   printGpsLine()                   gps::printLine()
//   doFix()                          gps::printFix()          (fix 명령)
//   sogStatusPrint()                 gps::printSogStatus()    (sog 명령)
//   gpsCfgDump()                     gps::cfgDump()           (gpscfg)
//   gpsNavPv(n)                      gps::navPvPrint(n)       (navpv · navpv l)
//   gpsCfgSetNavx()                  gps::cfgSetNavx()        (gpscfg mode · gpscfg static)
//   gpsSendAndWatch()                gps::sendAndWatch()      (nmea — 본문 다듬기는 명령 쪽에 남는다)
//   peekGps(s, slotD)                gps::peek(s, slotD)      (gps · gps d)
//   gpshz 명령 몸통                   gps::rateCommand(hz)
//   feedWatchdog()+imuDrainFifo()    gps::setWaitHook(fn)     — 몇 초 기다리는 동안 부른다 (아래)
//
// ── firmware-rak 과 달라진 점 ─────────────────────────────────────────────
//
//   1) UART 는 핀을 한 번만 묶고 속도는 uart_set_baudrate 로 바꾼다. Serial1.end() 자리는
//      uart_flush_input (받아 둔 바이트 버리기). 이미 누가 UART1 을 설치했으면 다시 설치하지 않는다.
//   2) Serial1.flush() (끝없이 기다림) 자리는 uart_wait_tx_done 1초.
//   3) 기다리는 곳(casicWait·casicWaitAck·applyNavPv·navPvPrint·sendAndWatch·peek·rateCommand)에서
//      waitHook 을 부른다. firmware-rak 은 casicWait·casicWaitAck 에서만 imuDrainFifo 를 불렀고
//      나머지는 feedWatchdog 만(또는 아무것도 안) 불렀다. navpv 의 900 ms 쉬기는 10 ms 씩 끊어 훅을 부른다.
//   4) 출력은 Serial.printf 대신 printf (USB 콘솔).
//   5) TinyGPS++ 의 age() 기준 시각은 esp_timer (부팅 뒤 ms). Arduino millis() 와 뜻이 같다.
//   6) 다듬기 단계·잡음 바닥은 전역 대신 setDampLevel()·setDeadbandKn(). NVS 읽기·범위 검사는 부르는 쪽.
#pragma once

#include <cstdint>

#include "TinyGPS++.h"   // components/tinygpsplus — main 의 REQUIRES 에 tinygpsplus 가 있어야 한다
#include "casic.h"       // firmware-rak/include

namespace gps {

constexpr uint32_t kBaud          = 115200;
constexpr uint8_t  kBoatMode      = 4;      // CFG-NAVX dyModel 4 = Nautical. 하드코딩 (CLAUDE.md)
constexpr uint32_t kStaleMs       = 3000;   // GPS 가 준 값이 이보다 오래되면 낡은 것
constexpr uint8_t  kPvVelMeasured = 4;      // NAV-PV velValid 가 이보다 작으면 잰 값이 아니다
constexpr float    kDampTau[6]    = {0.0f, 0.3f, 0.6f, 1.2f, 2.5f, 5.0f};   // 초

struct State {
    // 켜기·설정
    uint8_t  hz            = 10;     // 실제로 건 갱신율
    uint8_t  dyModel       = 255;    // 모듈에 실제로 걸린 움직임 종류. 255 = 아직 못 물어봄
    bool     casicIdFirst  = true;   // 체크섬 공식. true = L76K 문서 방식
    uint8_t  dampLevel     = 2;      // 0~5
    float    deadbandKn    = 0.10f;  // 이보다 작은 속도는 0 으로 보인다

    // fix
    bool     fix           = false;  // 지금 GPS 값을 믿을 수 있나
    bool     everHadFix    = false;
    bool     clockSet      = false;  // GPS 로 보드 시계를 맞췄나

    // NAV-PV (NMEA 거치기 전)
    float    pvSpeedKn     = -1.0f;
    float    pvCogDeg      = -1.0f;
    float    pvAccKn       = -1.0f;
    float    pvHAccM       = -1.0f;
    float    pvCogAccDeg   = -1.0f;
    uint8_t  pvVelFlag     = 0;
    bool     pvVelValid    = false;  // pvVelFlag >= 4 일 때만 참
    uint32_t pvAtMs        = 0;
    float    pvVelN        = 0.0f;   // m/s
    float    pvVelE        = 0.0f;   // m/s
    uint32_t casicRejected = 0;      // 체크섬·길이·숫자 범위에서 버린 프레임

    // 밖에 나갔다 와서 볼 기록
    char     lastFixRmc[100] = {0};
    uint32_t lastFixAtMs   = 0;
    float    maxSogKn      = 0.0f;
    float    maxSogFromPos = 0.0f;
    float    maxSogPv      = 0.0f;
    uint32_t fixSeenCount  = 0;

    // 위치차분 · 다듬기 · 화면 속도
    float    sogFromPos    = -1.0f;
    float    sogDamped     = -1.0f;
    float    cogDamped     = -1.0f;
    bool     slowMode      = false;
    bool     sogShownOk    = false;
    float    sogShownKn    = 0.0f;
    float    sogShortKn    = -1.0f;
    float    sogLongKn     = -1.0f;
    uint32_t sogBadCount   = 0;
};

const State& state();
TinyGPSPlus& parser();

// 몇 초씩 기다리는 동안 부를 함수. 메인이 "IMU FIFO 퍼 와서 기록 + 워치독" 을 넣는다.
// ★ 콜백이 아니다 — gps 함수를 부른 그 작업(루프) 안에서 그대로 불린다.
using WaitHook = void (*)();
void setWaitHook(WaitHook hook);

bool begin();                                   // false = UART 설치 실패
void poll();
void updateFix();
bool weekTow(uint16_t* week, uint32_t* tow);
float sogOut();
void setDampLevel(uint8_t level);
void setDeadbandKn(float kn);
void testPause(long sec);                       // test gps <초> (0~300)

void send(const char* body);                    // $body*CK\r\n
void setRate(uint8_t hz);
void applyNavPv();
void applyBoatMode();

void casicSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
bool casicWait(uint32_t waitMs, uint8_t wantCls, uint8_t wantId,
               uint8_t* out, uint16_t outCap, uint16_t* outLen, uint8_t* gotId);
casic::Ack casicWaitAck(uint32_t waitMs, uint8_t reqCls, uint8_t reqId);
bool casicSetAcked(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len, const char* what);
bool casicQuery(uint8_t cls, uint8_t id, uint8_t* out, uint16_t cap, uint16_t* len);
const char* dyModelName(uint8_t m);

// 진단 (몇 초씩 걸린다 — 부르는 쪽이 blockingDiagOk 로 거른다)
void printLine();
void printFix();
void printSogStatus();
void cfgDump();
void navPvPrint(int samples);
void cfgSetNavx(bool setModel, uint8_t model, bool setStatic, float staticTh);
void sendAndWatch(const char* body);
void peek(uint32_t seconds, bool slotD);
void rateCommand(long hz);

} // namespace gps
