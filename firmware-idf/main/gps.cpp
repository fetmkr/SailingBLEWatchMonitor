// firmware-idf 2단계 — GPS. firmware-rak src/main.cpp 에서 옮겼다 (커밋 2b18b17 기준 줄 번호).
//   gpsSend 690 · gpsSetRate 704 · gpsApplyBoatMode 726 · gpsBegin 767 · 상태 818-903 · gpsApplyNavPv 915
//   gpsPoll 934 · 다듬기·화면 속도 1024-1181 · clockFromGps 1201 · gpsUpdateFix 1234 · printGpsLine 1257
//   doFix 1271 · CASIC 1360-1478 · gpsCfgDump 1484 · gpsNavPv 1579 · gpsCfgSetNavx 1634
//   gpsSendAndWatch 1692 · peekGps 1741 · gpsWeekTow 3371 · test gps 4371 · gpshz 4949
// 주석은 원본에서 필요한 것만 가져왔다. 긴 사연은 원본을 본다. 달라진 점은 gps.h 머리.
#include "gps.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_rak.h"
#include "sog_policy.h"

namespace gps {
namespace {

constexpr uart_port_t kPort       = UART_NUM_1;
constexpr size_t      kRxBufBytes = 4096;   // firmware-rak setRxBufferSize(4096) 과 같다
constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kRadToDeg = 57.29577951308232f;

uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000); }
void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

State       s;
TinyGPSPlus sGps;
WaitHook    sHook       = nullptr;
uint32_t    sPauseUntil = 0;   // test gps: 이때까지 GPS 바이트를 읽어서 버린다

void idle() { if (sHook) sHook(); }

// ms 동안 10 ms 씩 끊어 쉬며 훅을 부른다
void waitWithHook(uint32_t ms) {
    const uint32_t t0 = millis32();
    while (millis32() - t0 < ms) { idle(); delayMs(10); }
}

// ── Serial1.available() / read() 자리 ─────────────────────────────────────
// 조금씩 퍼 와서 여기 두고 한 바이트씩 꺼낸다. 다 안 꺼낸 바이트는 다음에 부르는 쪽이 이어 받는다
// (casicWait 가 프레임을 찾고 돌아가도 뒤 바이트가 안 사라진다 — Serial1 과 같다).
uint8_t sRx[256];              // ★ 스택에 안 올린다
int     sRxN = 0, sRxPos = 0;

bool rxAvailable() {
    if (sRxPos < sRxN) return true;
    const int n = uart_read_bytes(kPort, sRx, sizeof sRx, 0);
    sRxPos = 0;
    sRxN   = (n > 0) ? n : 0;
    return sRxN > 0;
}
uint8_t rxRead() { return sRx[sRxPos++]; }   // rxAvailable() 이 참일 때만

void rxDiscard() {             // Serial1.end() 가 받아 둔 것을 버리던 자리
    sRxN = sRxPos = 0;
    uart_flush_input(kPort);
}

void txWrite(const uint8_t* p, size_t n) { if (n) uart_write_bytes(kPort, p, n); }
void txFlush() { uart_wait_tx_done(kPort, pdMS_TO_TICKS(1000)); }

// ── 위치차분 ──────────────────────────────────────────────────────────────
double   sPrevLat = 0, sPrevLon = 0;
uint32_t sPrevPosMs = 0;

// ── 다듬기 ────────────────────────────────────────────────────────────────
float    sCogCos = 0.0f, sCogSin = 0.0f;
uint32_t sDampAtMs = 0;

// ── 화면 속도 ─────────────────────────────────────────────────────────────
const sog::Policy kSog{};
constexpr int     kPvRing = 320;              // 10 Hz × 30초 + 여유
sog::Sample       sPvRing[kPvRing];           // static — 스택에 안 올린다
int      sPvHead = 0, sPvCount = 0;
uint32_t sPvSeenAt = 0;
uint32_t sSlowSinceMs = 0;

bool uartInit() {
    if (uart_is_driver_installed(kPort)) {
        printf("[GPS] UART1 이 이미 설치돼 있습니다 — 핀·받는 버퍼는 설치한 쪽 값을 씁니다\n");
        return true;
    }
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)kBaud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t e = uart_param_config(kPort, &cfg);
    if (e == ESP_OK) e = uart_set_pin(kPort, rak::kUART1_TX, rak::kUART1_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e == ESP_OK) e = uart_driver_install(kPort, (int)kRxBufBytes, 0, 0, nullptr, 0);
    if (e != ESP_OK) {
        printf("[GPS] ★ UART1 설치 실패 %s\n", esp_err_to_name(e));
        return false;
    }
    printf("[GPS] 받는 버퍼 %u 바이트\n", (unsigned)kRxBufBytes);
    return true;
}

// 위치가 갱신될 때마다 부른다. 1초 간격으로만 계산한다.
void updatePositionSpeed() {
    if (!sGps.location.isValid()) return;
    const uint32_t now = millis32();
    const double lat = sGps.location.lat();
    const double lon = sGps.location.lng();
    if (sPrevPosMs == 0) {
        sPrevLat = lat; sPrevLon = lon; sPrevPosMs = now;
        return;
    }
    const double dt = (now - sPrevPosMs) / 1000.0;
    if (dt < 1.0) return;
    const double meters = TinyGPSPlus::distanceBetween(sPrevLat, sPrevLon, lat, lon);
    const double mps    = meters / dt;
    s.sogFromPos = (float)(mps * 1.943844);   // m/s → knot
    sPrevLat = lat; sPrevLon = lon; sPrevPosMs = now;
}

void dampingReset() {
    s.sogDamped = -1.0f;
    s.cogDamped = -1.0f;
    sDampAtMs   = 0;
}

void dampingUpdate(float rawSog, float rawCog, uint32_t nowMs) {
    const float tau = kDampTau[s.dampLevel <= 5 ? s.dampLevel : 2];
    if (tau <= 0.0f || sDampAtMs == 0 || s.sogDamped < 0.0f) {
        s.sogDamped = rawSog;
        s.cogDamped = rawCog;
        sCogCos     = cosf(rawCog * kDegToRad);
        sCogSin     = sinf(rawCog * kDegToRad);
        sDampAtMs   = nowMs;
        return;
    }
    const float dt = (nowMs - sDampAtMs) / 1000.0f;
    if (dt <= 0.0f) return;
    sDampAtMs = nowMs;
    if (dt > 2.0f) {   // 오래 끊겼다가 돌아온 값은 새로 시작한다
        s.sogDamped = rawSog;
        sCogCos     = cosf(rawCog * kDegToRad);
        sCogSin     = sinf(rawCog * kDegToRad);
        s.cogDamped = rawCog;
        return;
    }
    const float a = dt / (tau + dt);
    s.sogDamped += (rawSog - s.sogDamped) * a;
    sCogCos += (cosf(rawCog * kDegToRad) - sCogCos) * a;
    sCogSin += (sinf(rawCog * kDegToRad) - sCogSin) * a;
    float deg = atan2f(sCogSin, sCogCos) * kRadToDeg;
    if (deg < 0.0f) deg += 360.0f;
    s.cogDamped = deg;
}

float pvMeanKn(uint32_t nowMs, uint32_t winMs, uint32_t notBefore) {
    return sog::vectorMeanKn(sPvRing, kPvRing, sPvHead, sPvCount, nowMs, winMs, notBefore);
}

// 경계: 입력(poll 이 pv* 에 넣음) → 품질(sog::sampleOk) → 추정(느린 모드·성분 평균) → 표시(sogShown*).
void sogShownUpdate(uint32_t nowMs) {
    if (sog::pvStale(s.pvAtMs, nowMs, kSog)) {   // NAV-PV 가 끊겼으면 원래 길로
        s.slowMode   = false;
        s.sogShownOk = s.fix;
        s.sogShownKn = sogOut();
        return;
    }
    if (s.pvAtMs == sPvSeenAt) {                 // 새 표본 없음
        if (!s.slowMode) { s.sogShownKn = sogOut(); }
        return;
    }
    sPvSeenAt = s.pvAtMs;

    if (!sog::sampleOk(s.pvVelValid, s.pvAccKn, kSog)) {
        ++s.sogBadCount;
        dampingReset();                          // 튄 값이 다듬기에 남지 않게
        if (!(s.sogShownOk && s.sogShownKn == 0.0f)) s.sogShownOk = false;
        return;
    }

    sPvRing[sPvHead] = sog::Sample{s.pvAtMs, s.pvVelN, s.pvVelE};
    sPvHead = (sPvHead + 1) % kPvRing;
    if (sPvCount < kPvRing) ++sPvCount;

    s.sogShortKn = pvMeanKn(nowMs, kSog.shortWinMs, 0);
    const bool slow = sog::nextSlowMode(s.slowMode, s.sogShortKn, kSog);
    if (slow && !s.slowMode) sSlowSinceMs = s.pvAtMs;
    s.slowMode = slow;

    if (s.slowMode) {
        s.sogLongKn  = pvMeanKn(nowMs, kSog.slowWinMs, sSlowSinceMs);
        s.sogShownKn = sog::slowShownKn(s.sogLongKn, kSog);
    } else {
        s.sogLongKn  = -1.0f;
        s.sogShownKn = sogOut();
    }
    s.sogShownOk = s.fix;
}

// GPS 로 보드 시계를 맞춘다. 한 번만. NMEA 시각은 UTC 다.
void clockFromGps() {
    if (s.clockSet) return;
    if (!sGps.date.isValid() || !sGps.time.isValid()) return;
    if (sGps.date.year() < 2020) return;

    struct tm tmv = {};
    tmv.tm_year = sGps.date.year() - 1900;
    tmv.tm_mon  = sGps.date.month() - 1;
    tmv.tm_mday = sGps.date.day();
    tmv.tm_hour = sGps.time.hour();
    tmv.tm_min  = sGps.time.minute();
    tmv.tm_sec  = sGps.time.second();
    static const int mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    const int y = sGps.date.year();
    long days = (y - 1970) * 365L + (y - 1969) / 4;
    days += mdays[tmv.tm_mon];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (leap && tmv.tm_mon > 1) days += 1;
    days += tmv.tm_mday - 1;
    const time_t utc = (time_t)(days * 86400L +
                                tmv.tm_hour * 3600L + tmv.tm_min * 60L + tmv.tm_sec);
    if (utc <= 0) return;

    struct timeval tv = {};
    tv.tv_sec  = utc;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    s.clockSet = true;
    printf("[시계] GPS 로 맞췄습니다 — %04d-%02d-%02d %02d:%02d:%02d UTC\n",
           sGps.date.year(), sGps.date.month(), sGps.date.day(),
           sGps.time.hour(), sGps.time.minute(), sGps.time.second());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

const State& state() { return s; }
TinyGPSPlus& parser() { return sGps; }
void setWaitHook(WaitHook hook) { sHook = hook; }

void setDampLevel(uint8_t level) { s.dampLevel = level; }
void setDeadbandKn(float kn) { s.deadbandKn = kn; }

// 잡음 바닥 아래는 0 으로 보여준다. 다듬은 값이 없으면 RMC 속도.
float sogOut() {
    const float v = (s.sogDamped >= 0.0f) ? s.sogDamped : (float)sGps.speed.knots();
    return (v < s.deadbandKn) ? 0.0f : v;
}

void testPause(long sec) {
    if (sec < 0) sec = 0;
    if (sec > 300) sec = 300;
    sPauseUntil = millis32() + (uint32_t)sec * 1000;
    printf("[TEST] GPS 입력을 %ld초 동안 버립니다\n", sec);
}

// 체크섬을 붙여 한 줄 보낸다. ($ 와 * 사이 문자들의 XOR)
void send(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; ++p) ck ^= (uint8_t)*p;
    char line[160];
    const int n = snprintf(line, sizeof line, "$%s*%02X\r\n", body, ck);
    if (n > 0) txWrite((const uint8_t*)line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    txFlush();
    delayMs(60);   // flush 뒤에도 실제로 나갈 시간을 준다. 이걸 아끼면 명령이 씹힌다.
}

// 문서(§2.3.2)가 적어 놓은 값은 1000/500/200 뿐. 100(10 Hz)은 제품 사양의 "up to 10 Hz".
void setRate(uint8_t hz) {
    int ms = 1000;
    if (hz >= 10)     ms = 100;
    else if (hz >= 5) ms = 200;
    else if (hz >= 2) ms = 500;
    char body[24];
    snprintf(body, sizeof(body), "PCAS02,%d", ms);
    send(body);
    s.hz = (ms == 100) ? 10 : (ms == 200 ? 5 : (ms == 500 ? 2 : 1));
}

// GPS 를 선박 모드(4)로 건다. 되물어서 실제로 걸린 값을 dyModel 에 둔다.
// ★ 루프에서 주기적으로 부르지 않는다. 한 번 물을 때 최대 3초 붙잡는다.
void applyBoatMode() {
    uint8_t  p[44];
    uint16_t len = 0;
    if (!casicQuery(0x06, 0x07, p, sizeof(p), &len) || len < 44) {
        s.dyModel = 255;
        printf("[GPS] ★ 움직임 종류를 못 물어봤습니다 — 화면에 ? 로 뜹니다\n");
        return;
    }
    s.dyModel = p[4];
    if (p[4] == kBoatMode) {
        printf("[GPS] 움직임 종류 %u (%s) — 이미 선박\n", p[4], dyModelName(p[4]));
        return;
    }
    printf("[GPS] 움직임 종류 %u (%s) — 선박으로 겁니다\n", p[4], dyModelName(p[4]));

    for (int attempt = 1; attempt <= 3; ++attempt) {
        const uint32_t mask = (1UL << 0);          // dyModel 만 바꾼다
        memcpy(p + 0, &mask, 4);
        p[4] = kBoatMode;
        casicSetAcked(0x06, 0x07, p, 44, "CFG-NAVX dyModel 4");

        if (casicQuery(0x06, 0x07, p, sizeof(p), &len) && len >= 44) {
            s.dyModel = p[4];
            if (p[4] == kBoatMode) {
                printf("[GPS] ✓ 되물으니 %u (%s)  (%d번째)\n", p[4], dyModelName(p[4]), attempt);
                return;
            }
            printf("[GPS] ★ 되물으니 아직 %u (%s)  (%d번째)\n", p[4], dyModelName(p[4]), attempt);
        } else {
            s.dyModel = 255;
            printf("[GPS] ★ 건 뒤 되물어보기 실패 (%d번째)\n", attempt);
        }
    }
    printf("[GPS] ★★ 선박 모드를 못 걸었습니다 — 저속이 0 으로 뭉개집니다\n");
}

bool begin() {
    if (!uartInit()) return false;

    // 지금 모듈이 어느 속도로 말하는지 모른다. 두 속도로 각각 보낸다. 못 알아듣는 쪽은 버려진다.
    static constexpr uint32_t kTryBauds[2] = {9600u, kBaud};
    for (uint32_t baud : kTryBauds) {
        uart_set_baudrate(kPort, baud);
        rxDiscard();
        delayMs(150);
        send("PCAS04,7");                              // GPS+BeiDou+GLONASS
        send("PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0,0");     // GGA, RMC 만
        send("PCAS01,5");                              // 115200
        delayMs(150);
        rxDiscard();                                   // Serial1.end()
        delayMs(50);
    }

    uart_set_baudrate(kPort, kBaud);
    rxDiscard();
    delayMs(200);

    setRate(10);
    applyNavPv();
    applyBoatMode();

    printf("[GPS] UART1 %lubps / %u Hz (RX GPIO%d / TX GPIO%d)\n",
           (unsigned long)kBaud, s.hz, rak::kUART1_RX, rak::kUART1_TX);
    return true;
}

// NAV-PV 를 측위마다(10 Hz) 내보내게 하고, 실제로 프레임이 오는지 본다.
void applyNavPv() {
    uint8_t on[4] = {0x01, 0x03, 1, 0};
    casicSetAcked(0x06, 0x01, on, 4, "CFG-MSG NAV-PV 10Hz");
    const uint32_t t0 = millis32();
    while (millis32() - t0 < 1500) {
        poll();
        if (s.pvAtMs && (int32_t)(s.pvAtMs - t0) >= 0) break;
        idle();
        delayMs(10);
    }
    if (s.pvAtMs && (int32_t)(s.pvAtMs - t0) >= 0)
        printf("[GPS] ✓ NAV-PV 받는 중 (%lums 만에 첫 프레임)\n", (unsigned long)(s.pvAtMs - t0));
    else
        printf("[GPS] ★ NAV-PV 가 1.5초 안에 안 옵니다 — 화면 속도는 RMC 로 물러섭니다\n");
}

void poll() {
    if ((int32_t)(millis32() - sPauseUntil) < 0) {
        while (rxAvailable()) rxRead();
        return;
    }
    static char   line[100];
    static size_t n = 0;
    // NMEA 는 전부 아스키라 0xBA 가 나올 수 없다. 체크섬·길이·NaN 까지 casic.h 가 본다.
    static casic::Parser ps;

    while (rxAvailable()) {
        const uint8_t u = rxRead();
        const char    c = (char)u;

        bool pass = false;
        const casic::Result fr = ps.feed(u, millis32(), &pass);
        if (fr == casic::Result::Frame) {
            const casic::Frame& f = ps.frame();
            if (f.cls == 0x01 && f.id == 0x03) {
                casic::NavPv pv;
                if (casic::parseNavPv(f, &pv)) {
                    s.pvVelN      = pv.velN;
                    s.pvVelE      = pv.velE;
                    s.pvHAccM     = pv.hAccM;
                    s.pvCogAccDeg = pv.cogAccDeg;
                    s.pvVelFlag   = pv.velValid;
                    s.pvVelValid  = (s.pvVelFlag >= kPvVelMeasured);
                    s.pvSpeedKn   = pv.speed2D * 1.943844f;
                    s.pvCogDeg    = pv.heading;
                    s.pvAccKn     = (pv.sAccMs > 0.0f) ? pv.sAccMs * 1.943844f : -1.0f;
                    s.pvAtMs      = millis32();
                    if (s.pvVelValid && s.pvSpeedKn > s.maxSogPv) s.maxSogPv = s.pvSpeedKn;
                } else {
                    ++s.casicRejected;
                }
            }
        } else if (fr != casic::Result::None) {
            ++s.casicRejected;
        }
        if (!pass) continue;       // 바이너리 프레임 안의 바이트는 NMEA 파서에 안 먹인다

        sGps.encode(c);

        // 원문도 한 줄씩 모은다. RMC 를 다 읽은 시점이라 파서 상태가 갱신되어 있다.
        if (c == '\n' || c == '\r') {
            if (n > 6) {
                line[n] = '\0';
                if (strstr(line, "RMC") != nullptr && sGps.location.isValid()) {
                    strncpy(s.lastFixRmc, line, sizeof(s.lastFixRmc) - 1);
                    s.lastFixRmc[sizeof(s.lastFixRmc) - 1] = '\0';
                    s.lastFixAtMs = millis32();
                }
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
    sogShownUpdate(millis32());
}

// ★ age() 검사가 꼭 필요하다. 라이브러리는 놓친 뒤에도 마지막 값을 들고 있다.
void updateFix() {
    clockFromGps();
    const bool ok = sGps.location.isValid() && sGps.location.age() < kStaleMs &&
                    sGps.speed.isValid() && sGps.speed.age() < kStaleMs;
    if (ok) {
        s.everHadFix = true;
        s.fixSeenCount++;
        const float kn = (float)sGps.speed.knots();
        if (kn > s.maxSogKn) s.maxSogKn = kn;
        updatePositionSpeed();
        if (s.sogFromPos > s.maxSogFromPos) s.maxSogFromPos = s.sogFromPos;
        dampingUpdate(kn, (float)sGps.course.deg(), millis32());
    } else {
        dampingReset();
        // fix 를 놓쳤으면 이전 위치를 버린다. 다시 잡았을 때 그동안 움직인 거리가 한 번의 속도로 튄다.
        sPrevPosMs   = 0;
        s.sogFromPos = -1.0f;
    }
    s.fix = ok;
}

// NMEA 시각(UTC) 을 GPS 주·주중 ms 로. 헤더 time_ref 1(UTC 환산).
bool weekTow(uint16_t* week, uint32_t* tow) {
    if (!sGps.date.isValid() || !sGps.time.isValid()) return false;
    const int y = sGps.date.year(), m = sGps.date.month(), d = sGps.date.day();
    if (y < 2000) return false;

    int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468; // 1970-01-01 기준
    const long gpsDays = days - 3657;                          // 1980-01-06 까지 3657일
    if (gpsDays < 0) return false;

    *week = (uint16_t)(gpsDays / 7);
    const uint32_t dow = (uint32_t)(gpsDays % 7);
    *tow = ((dow * 86400u) + sGps.time.hour() * 3600u +
            sGps.time.minute() * 60u + sGps.time.second()) * 1000u +
           sGps.time.centisecond() * 10u;
    return true;
}

// ── CASIC 바이너리 ────────────────────────────────────────────────────────
//   BA CE | len(U2) | class | id | payload | ckSum(U4)
// ★ 문서 두 개가 체크섬 공식을 다르게 적었다. 답이 오는 쪽이 정답이다 (casicQuery 가 바꿔 본다).

void casicSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    const uint8_t hdr[6] = {0xBA, 0xCE,
                            (uint8_t)(len & 0xFF), (uint8_t)(len >> 8),
                            cls, id};
    const uint32_t ck = casic::checksum(cls, id, len, payload, s.casicIdFirst);
    txWrite(hdr, 6);
    if (len > 0) txWrite(payload, len);
    const uint8_t ckb[4] = {(uint8_t)(ck), (uint8_t)(ck >> 8),
                            (uint8_t)(ck >> 16), (uint8_t)(ck >> 24)};
    txWrite(ckb, 4);
    txFlush();
}

// 바이너리 응답 한 개를 기다린다. wantId 가 0xFF 면 그 class 의 아무거나.
// 지나가는 NMEA 는 파서에 먹인다 — 기다리는 동안 위치가 멎으면 안 된다.
bool casicWait(uint32_t waitMs, uint8_t wantCls, uint8_t wantId,
               uint8_t* out, uint16_t outCap, uint16_t* outLen, uint8_t* gotId) {
    const uint32_t start = millis32();
    casic::Parser ps;
    while (millis32() - start < waitMs) {
        while (rxAvailable()) {
            const uint8_t c = rxRead();
            bool pass = false;
            const casic::Result r = ps.feed(c, millis32(), &pass);
            if (pass) { sGps.encode((char)c); continue; }
            if (r == casic::Result::None) continue;
            if (r != casic::Result::Frame) { ++s.casicRejected; continue; }
            const casic::Frame& f = ps.frame();
            if (f.cls == wantCls && (wantId == 0xFF || f.id == wantId)) {
                if (gotId)  *gotId  = f.id;
                const uint16_t cp = (f.len > outCap) ? outCap : f.len;
                if (out && cp) memcpy(out, f.payload, cp);
                if (outLen) *outLen = cp;
                return true;
            }
        }
        idle();      // firmware-rak: feedWatchdog() + imuDrainFifo()
        delayMs(2);
    }
    return false;
}

// 요청(cls, id)에 대한 ACK/NACK. 다른 요청의 ACK 는 우리 것으로 안 본다.
casic::Ack casicWaitAck(uint32_t waitMs, uint8_t reqCls, uint8_t reqId) {
    const uint32_t start = millis32();
    casic::Parser ps;
    while (millis32() - start < waitMs) {
        while (rxAvailable()) {
            const uint8_t c = rxRead();
            bool pass = false;
            const casic::Result r = ps.feed(c, millis32(), &pass);
            if (pass) { sGps.encode((char)c); continue; }
            if (r == casic::Result::None) continue;
            if (r != casic::Result::Frame) { ++s.casicRejected; continue; }
            const casic::Ack a = casic::ackFor(ps.frame(), reqCls, reqId);
            if (a != casic::Ack::NotMine) return a;
        }
        idle();
        delayMs(2);
    }
    return casic::Ack::NotMine;
}

// 설정을 보내고 ACK 를 확인한다. 짐작하지 않는다.
bool casicSetAcked(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len, const char* what) {
    while (rxAvailable()) sGps.encode((char)rxRead());   // 밀린 것 비우기
    casicSend(cls, id, payload, len);
    const casic::Ack ack = casicWaitAck(2000, cls, id);
    if (ack == casic::Ack::NotMine) {
        printf("  %s — 응답 없음. 이 칩이 모르는 설정입니다\n", what);
        return false;
    }
    if (ack == casic::Ack::Ack) {
        printf("  %s — ACK. 받아들였습니다\n", what);
        return true;
    }
    printf("  %s — NACK. 거절당했습니다\n", what);
    return false;
}

// 조회는 길이 0 으로 보낸다 (문서 §2.11). 안 오면 반대 체크섬 공식으로 한 번 더.
bool casicQuery(uint8_t cls, uint8_t id, uint8_t* out, uint16_t cap, uint16_t* len) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        while (rxAvailable()) sGps.encode((char)rxRead());
        casicSend(cls, id, nullptr, 0);
        if (casicWait(1500, cls, id, out, cap, len, nullptr)) return true;
        s.casicIdFirst = !s.casicIdFirst;
    }
    return false;
}

// CFG-NAVX 의 dyModel 값 이름 (문서 §2.11.8 Remark[2])
const char* dyModelName(uint8_t m) {
    switch (m) {
        case 0: return "Portable 일반 휴대";
        case 1: return "Static   정지";
        case 2: return "Walking  보행";
        case 3: return "Car      자동차";
        case 4: return "Nautical 선박";
        case 5: return "Flight   항공 <1g";
        case 6: return "Flight   항공 <2g";
        case 7: return "Flight   항공 <4g";
        default: return "알 수 없는 값";
    }
}

// ── 진단 ─────────────────────────────────────────────────────────────────

void printLine() {
    const int sats = sGps.satellites.isValid() ? (int)sGps.satellites.value() : 0;
    if (s.fix) {
        printf("   GPS  위성 %d | %.6f, %.6f | HDOP %.1f\n",
               sats, sGps.location.lat(), sGps.location.lng(),
               sGps.hdop.isValid() ? sGps.hdop.hdop() : 99.9);
    } else {
        printf("   GPS  위성 %d | 아직 못 잡음%s\n",
               sats, s.everHadFix ? " (한 번 잡았다가 놓침)" : "");
    }
}

void printFix() {
    updateFix();
    const int sats = sGps.satellites.isValid() ? (int)sGps.satellites.value() : 0;

    printf("──────────────────────────────────────────\n");
    printf("  GPS 파싱 상태 (RAK12501 / L76K)\n");
    printf("  받은 글자     %lu\n", (unsigned long)sGps.charsProcessed());
    printf("  체크섬        통과 %lu / 실패 %lu\n",
           (unsigned long)sGps.passedChecksum(), (unsigned long)sGps.failedChecksum());
    printf("  위성 수       %d\n", sats);
    printf("  갱신율        %u Hz (%lubps)\n", s.hz, (unsigned long)kBaud);
    printf("  fix           %s\n", s.fix ? "있음" : "없음");

    if (s.fix) {
        printf("  위치          %.6f, %.6f\n", sGps.location.lat(), sGps.location.lng());
        printf("  속도(SOG)     %.2f kn\n", sGps.speed.knots());
        printf("  침로(COG)     %.1f°\n", sGps.course.deg());
        printf("  HDOP          %.1f (작을수록 정확)\n",
               sGps.hdop.isValid() ? sGps.hdop.hdop() : 99.9);
    }
    printf("──────────────────────────────────────────\n");
    printf("  fix 판정 횟수  %lu\n", (unsigned long)s.fixSeenCount);
    printf("  본 최고 속도   RMC %.2f kn / NAV-PV %.2f kn / 위치차분 %.2f kn\n",
           s.maxSogKn, s.maxSogPv, s.maxSogFromPos);
    if (s.pvAtMs != 0) {
        printf("  속도 표식      %u %s\n", s.pvVelFlag,
               s.pvVelFlag >= kPvVelMeasured ? "(방금 잰 값)" :
               s.pvVelFlag == 3 ? "★ 옛날 값을 들고 있음" :
               s.pvVelFlag == 2 ? "★ 대충 어림잡은 값" : "★ 속도를 모름");
        if (s.pvCogAccDeg >= 0) printf("  침로 오차      ±%.1f도\n", s.pvCogAccDeg);
        printf("  지금 NAV-PV    %.2f kn (오차 ±%.2f)  침로 %.1f  유효 %s  %lu초 전\n",
               s.pvSpeedKn, s.pvAccKn, s.pvCogDeg,
               s.pvVelValid ? "예" : "아니오",
               (unsigned long)((millis32() - s.pvAtMs) / 1000));
    }
    if (s.lastFixRmc[0] != '\0') {
        printf("  마지막 fix RMC (%.0f초 전)\n", (millis32() - s.lastFixAtMs) / 1000.0f);
        printf("    %s\n", s.lastFixRmc);
        printf("    필드 순서: 시각,상태,위도,N/S,경도,E/W,속도,침로,날짜\n");
        printf("                                          ↑ 7번째가 속도(kn)\n");
    } else {
        printf("  아직 fix 된 적이 없어 기억해 둔 RMC 가 없습니다\n");
    }
    printf("──────────────────────────────────────────\n");

    if (sGps.charsProcessed() == 0) {
        printf("  한 글자도 안 들어왔습니다.\n");
        printf("    → power %d 로 센서 전원이 켜져 있는지 보세요.\n", rak::kSensorPowerA);
    } else if (sGps.failedChecksum() > sGps.passedChecksum() / 4) {
        printf("  체크섬 실패가 많습니다. 통신 속도(9600)를 의심하세요.\n");
    } else if (!s.fix) {
        printf("  문장은 잘 들어옵니다. 위성만 아직 못 잡았습니다.\n");
        printf("  실내에서는 정상입니다. 창가나 밖으로 나가면 잡힙니다.\n");
        printf("  차가운 시작은 35초쯤 걸립니다.\n");
    }
}

void printSogStatus() {
    printf("[SOG] %s | 화면 %s %.2f kn | 3초 %.2f | 30초 %.2f | 원본 %.2f ±%.2f f%u | 버린 표본 %lu\n",
           s.slowMode ? "느린" : "빠른", s.sogShownOk ? "" : "(--.--)", s.sogShownKn,
           s.sogShortKn, s.sogLongKn, s.pvSpeedKn, s.pvAccKn, s.pvVelFlag,
           (unsigned long)s.sogBadCount);
    printf("[GPS] CASIC 프레임 거름 %lu (체크섬·길이·숫자 범위)\n", (unsigned long)s.casicRejected);
}

// 지금 걸려 있는 설정을 전부 되물어서 보여준다. 모듈이 "이렇게 되어 있다" 고 답한 값이다.
void cfgDump() {
    uint8_t  buf[64];
    uint16_t len = 0;

    printf("──────────────────────────────────────────\n");
    printf("  모듈이 답한 실제 설정 (CASIC 바이너리 조회)\n");
    printf("──────────────────────────────────────────\n");

    if (casicQuery(0x06, 0x00, buf, sizeof(buf), &len) && len >= 8) {
        uint32_t baud; memcpy(&baud, buf + 4, 4);
        printf("  통신 속도     %lu bps", (unsigned long)baud);
        printf("   %s\n", baud == kBaud ? "— 우리가 건 값과 같습니다" : "★ 우리가 건 값과 다릅니다");
        printf("  포트 %u  프로토콜마스크 0x%02X (B1 텍스트입력 B5 텍스트출력)\n", buf[0], buf[1]);
    } else {
        printf("  통신 속도     조회 실패 (CFG-PRT 무응답)\n");
    }

    if (casicQuery(0x06, 0x04, buf, sizeof(buf), &len) && len >= 4) {
        uint16_t interval; memcpy(&interval, buf + 0, 2);
        printf("  측위 간격     %u ms", (unsigned)interval);
        if (interval > 0) printf(" (= %.1f Hz)", 1000.0f / interval);
        printf("   %s\n", interval == 100 ? "— 10 Hz 로 걸렸습니다" : "★ 10 Hz 가 아닙니다");
    } else {
        printf("  측위 간격     조회 실패 (CFG-RATE 무응답)\n");
    }

    if (casicQuery(0x06, 0x07, buf, sizeof(buf), &len) && len >= 44) {
        float    staticTh; memcpy(&staticTh, buf + 40, 4);
        uint32_t mask;     memcpy(&mask,     buf + 0,  4);
        const uint8_t nav = buf[13];
        s.dyModel = buf[4];   // 실제로 걸려 있는 값을 기억해 둔다
        printf("  움직임 종류   %u  %s\n", buf[4], dyModelName(buf[4]));
        printf("  정지 문턱값   %.2f m/s (= %.2f kn)%s\n",
               staticTh, staticTh * 1.943844f,
               staticTh > 0.0f ? "   ★ 이 아래 속도는 0 으로 뭉갠다" : "   — 꺼져 있다");
        printf("  쓰는 위성     %s%s%s  (GPS/BDS/GLONASS 중)\n",
               (nav & 1) ? "GPS " : "", (nav & 2) ? "BeiDou " : "", (nav & 4) ? "GLONASS" : "");
        printf("  최소 신호     %u dB-Hz   위성 %u~%u개   최소 고도각 %d도\n",
               buf[8], buf[6], buf[7], (int8_t)buf[11]);
        printf("  mask          0x%08lX\n", (unsigned long)mask);
        // 해석이 맞는지 눈으로 확인할 수 있게 원본을 그대로 찍는다.
        printf("  원본 44바이트 ");
        for (uint16_t i = 0; i < len; i++) {
            if (i && i % 16 == 0) printf("\n                ");
            printf("%02X ", buf[i]);
        }
        printf("\n");
    } else {
        printf("  항법 엔진     조회 실패 (CFG-NAVX 무응답)\n");
    }
    printf("  (체크섬 공식은 %s 문서 방식이 먹혔습니다)\n", s.casicIdFirst ? "L76K" : "CASIC");
    printf("──────────────────────────────────────────\n");
}

// NMEA 로 만들어지기 전의 속도를 직접 본다 (NAV-PV, 문서 §2.7.4).
void navPvPrint(int samples) {
    printf("──────────────────────────────────────────\n");
    printf("  RMC 속도 vs NAV-PV 속도 (NMEA 거치기 전)\n");
    printf("──────────────────────────────────────────\n");

    for (int i = 0; i < samples; i++) {
        uint8_t  p[80];
        uint16_t len = 0;
        // NAV 메시지는 길이 0 조회로는 안 나온다 (실측). CFG-MSG 갱신율 0xFFFF = 즉시 한 번 (§2.11.2)
        const uint8_t poll[4] = {0x01, 0x03, 0xFF, 0xFF};
        while (rxAvailable()) sGps.encode((char)rxRead());
        casicSend(0x06, 0x01, poll, 4);

        if (!casicWait(2000, 0x01, 0x03, p, sizeof(p), &len, nullptr) || len < 80) {
            printf("  NAV-PV 응답 없음 (%u 바이트)\n", (unsigned)len);
            break;
        }

        float velN, velE, velU, speed2D, heading, sAcc, pDop;
        memcpy(&pDop,    p + 12, 4);
        memcpy(&velN,    p + 48, 4);
        memcpy(&velE,    p + 52, 4);
        memcpy(&velU,    p + 56, 4);
        memcpy(&speed2D, p + 64, 4);
        memcpy(&heading, p + 68, 4);
        memcpy(&sAcc,    p + 72, 4);

        const float rmcKn  = sGps.speed.isValid() ? (float)sGps.speed.knots() : -1.0f;
        const float pvKn   = speed2D * 1.943844f;
        const float sAccMs = (sAcc > 0.0f) ? sqrtf(sAcc) : 0.0f;
        char rmcTxt[16];
        if (rmcKn >= 0) snprintf(rmcTxt, sizeof rmcTxt, "%.2f", rmcKn);   // String(rmcKn, 2)
        else            snprintf(rmcTxt, sizeof rmcTxt, " --- ");

        printf("  RMC %6s kn | NAV-PV %6.2f kn | N%+6.2f E%+6.2f U%+6.2f m/s"
               " | 침로 %5.1f | 오차 ±%.2f kn | 위성 %u | pDop %.1f | 유효 위치%u 속도%u\n",
               rmcTxt, pvKn, velN, velE, velU, heading,
               sAccMs * 1.943844f, p[7], pDop, p[4], p[5]);

        waitWithHook(900);
    }
    printf("──────────────────────────────────────────\n");
}

// CFG-NAVX 에서 항목 몇 개만 바꾼다. mask 에 세운 항목만 적용된다 (§2.11.8 Remark[1]).
// ★ ACK 가 왔다고 믿지 않는다. 되읽어서 값이 바뀌었는지 확인한다.
void cfgSetNavx(bool setModel, uint8_t model, bool setStatic, float staticTh) {
    uint8_t  p[44];
    uint16_t len = 0;

    printf("──────────────────────────────────────────\n");
    if (!casicQuery(0x06, 0x07, p, sizeof(p), &len) || len < 44) {
        printf("  지금 값을 못 읽었습니다. 아무것도 바꾸지 않습니다.\n");
        printf("──────────────────────────────────────────\n");
        return;
    }

    float beforeTh; memcpy(&beforeTh, p + 40, 4);
    printf("  바꾸기 전   dyModel %u (%s)   staticHoldTh %.2f m/s\n", p[4], dyModelName(p[4]), beforeTh);

    uint32_t mask = 0;
    if (setModel)  { mask |= (1UL << 0);  p[4] = model; }
    if (setStatic) { mask |= (1UL << 13); memcpy(p + 40, &staticTh, 4); }
    memcpy(p + 0, &mask, 4);

    if (!casicSetAcked(0x06, 0x07, p, 44, "CFG-NAVX")) {
        printf("──────────────────────────────────────────\n");
        return;
    }

    if (casicQuery(0x06, 0x07, p, sizeof(p), &len) && len >= 44) {
        float afterTh; memcpy(&afterTh, p + 40, 4);
        printf("  바꾼 뒤     dyModel %u (%s)   staticHoldTh %.2f m/s\n", p[4], dyModelName(p[4]), afterTh);
        const bool okModel  = !setModel  || p[4] == model;
        const bool okStatic = !setStatic || afterTh == staticTh;
        if (okModel && okStatic) {
            printf("  ✓ 값이 실제로 바뀌었습니다\n");
            if (setModel) {
                // 시험용으로 지금만 바꾼다. 부팅하면 늘 선박(4) — kBoatMode 하드코딩.
                s.dyModel = model;
                printf("  지금만 바꿨습니다. 껐다 켜면 선박(4)으로 돌아갑니다.\n");
            }
        } else {
            printf("  ★ ACK 는 왔는데 값이 안 바뀌었습니다 — 이 칩은 이 항목을 안 받습니다\n");
        }
    }
    printf("──────────────────────────────────────────\n");
}

// NMEA 명령을 하나 보내고 3초 동안 응답을 본다. 늘 오는 위치 문장은 걸러낸다.
void sendAndWatch(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; ++p) ck ^= (uint8_t)*p;

    printf("──────────────────────────────────────────\n");
    printf("  보냅니다   $%s*%02X\n", body, ck);
    printf("  3초 동안 응답을 봅니다 (위치 문장은 걸러냅니다)\n");
    printf("──────────────────────────────────────────\n");

    send(body);

    char     line[140];
    size_t   n     = 0;
    int      shown = 0;
    const uint32_t start = millis32();

    while (millis32() - start < 3000) {
        while (rxAvailable()) {
            const char c = (char)rxRead();
            sGps.encode(c);
            if (c == '\n' || c == '\r') {
                if (n > 0) {
                    line[n] = 0;
                    const bool routine =
                        line[0] == '$' &&
                        (strstr(line, "GGA") || strstr(line, "RMC") ||
                         strstr(line, "GSV") || strstr(line, "GSA") ||
                         strstr(line, "VTG") || strstr(line, "GLL"));
                    if (!routine) {
                        printf("  <<  %s\n", line);
                        shown++;
                    }
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
        }
        idle();
        delayMs(2);
    }

    if (shown == 0) {
        printf("  응답 없음 — 모듈이 이 명령을 모릅니다.\n");
        printf("  (문장은 계속 들어오고 있으니 통신 자체는 멀쩡합니다)\n");
    }
    printf("──────────────────────────────────────────\n");
}

// 파싱하지 않은 원시 바이트를 그대로 보여준다.
void peek(uint32_t seconds, bool slotD) {
    printf("──────────────────────────────────────────\n");
    printf("  UART1 (RX GPIO%d / TX GPIO%d) %lubps — %us 동안 원시 데이터\n",
           rak::kUART1_RX, rak::kUART1_TX, (unsigned long)kBaud, (unsigned)seconds);
    printf("  슬롯 %s 로 가정합니다.\n", slotD ? "D" : "A");

    // GPS 모듈 RESET 핀이 슬롯마다 다르다. 슬롯 A → IO2(센서 전원과 같은 선), 슬롯 D → GPIO39
    if (slotD) {
        printf("  슬롯 D 이므로 GPIO%d(IO6)을 HIGH 로 올려 리셋을 풉니다.\n", rak::kGpsResetSlotD);
        const gpio_num_t pin = static_cast<gpio_num_t>(rak::kGpsResetSlotD);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        delayMs(200);
        gpio_set_level(pin, 1);
        delayMs(500);
    }
    printf("──────────────────────────────────────────\n");

    const uint32_t start = millis32();
    uint32_t bytes = 0;
    while (millis32() - start < seconds * 1000UL) {
        while (rxAvailable()) {
            const char c = (char)rxRead();
            putchar(c);
            sGps.encode(c);
            bytes++;
        }
        idle();
        delayMs(5);
    }

    printf("\n");
    printf("──────────────────────────────────────────\n");
    if (bytes == 0) {
        printf("  한 바이트도 안 들어왔습니다. 순서대로 의심하세요:\n");
        printf("    1) 센서 전원이 꺼져 있다 (power 명령)\n");
        printf("    2) GPS 가 슬롯 B 나 C 에 꽂혀 있다 → A 나 D 로 옮기기\n");
        if (!slotD) printf("    3) 슬롯 D 에 꽂혀 있다 → gps d 로 다시 해보기\n");
        else        printf("    3) 슬롯 A 에 꽂혀 있다 → gps 로 다시 해보기\n");
    } else {
        printf("  %u 바이트 수신 — GPS 가 말하고 있습니다.\n", (unsigned)bytes);
        printf("  쉼표 사이가 비어 있으면 아직 위성을 못 잡은 것입니다.\n");
        printf("  자세한 상태는 fix 명령으로 보세요.\n");
    }
}

// gpshz 명령. 말로만 바뀌었는지 모르니 실제로 들어오는 문장을 센다.
void rateCommand(long hz) {
    if (hz != 1 && hz != 2 && hz != 5 && hz != 10) {
        printf("[GPS] 1, 2, 5, 10 중에서 고르세요. 예) gpshz 5\n");
        printf("      1/2/5 는 문서에 있는 값, 10 은 확인 안 된 값입니다.\n");
        return;
    }
    setRate((uint8_t)hz);
    printf("[GPS] 갱신율 → %u Hz 로 요청했습니다. 실제로 걸렸는지 셉니다...\n", s.hz);

    const uint32_t before = sGps.passedChecksum();
    const uint32_t bad0   = sGps.failedChecksum();
    const uint32_t t0     = millis32();
    while (millis32() - t0 < 5000) {
        poll();
        idle();
        delayMs(2);
    }
    const uint32_t got  = sGps.passedChecksum() - before;
    const uint32_t bad  = sGps.failedChecksum() - bad0;
    const float perSec  = got / 5.0f;

    printf("──────────────────────────────────────────\n");
    printf("  5초 동안 문장 %lu개 (초당 %.1f개)\n", (unsigned long)got, perSec);
    printf("  GGA+RMC 두 종류이므로 → 초당 %.1f 번 갱신\n", perSec / 2.0f);
    printf("  체크섬 실패 %lu\n", (unsigned long)bad);
    printf("──────────────────────────────────────────\n");
    if (bad > got / 10) {
        printf("  ★ 깨진 문장이 많습니다. 대역폭이 모자랍니다.\n");
        printf("    gpshz 5 로 되돌리세요.\n");
    } else if (perSec / 2.0f < s.hz * 0.7f) {
        printf("  ★ 요청한 %u Hz 만큼 안 옵니다. 모듈이 무시한 것입니다.\n", s.hz);
    } else {
        printf("  요청한 만큼 들어옵니다.\n");
    }
}

// smooth 명령이 세기를 바꾼 뒤 부른다 (firmware-rak smooth → dampingReset)
void resetDamping() { dampingReset(); }

} // namespace gps
