// firmware-idf — 2단계: 기록기 + GPS · IMU. PORTING.md 의 단계 표가 원본이다.
//
// 이 판에서 되는 것
//   켤 때 이유 · NVS 설정 · 센서 전원 · 기록기(hlog_idf.cpp) · 기록 제어(원하는 상태 / 실제 상태) · 이어 시작 ·
//   GPS(gps.cpp) · IMU FIFO 100 Hz · 자력(imu.cpp) · 방위(heading_tilt.h) · NAV 10 Hz · IMU 100 Hz · TXT 10초 ·
//   카드 건강 1 Hz · 센서 끊김·다시 붙이기 · 워치독 · 기록 중 초록 LED
//   시리얼 명령: rec … · imu · fix · gps · gps d · gpscfg (mode/static) · gpshz · sog · navpv (l) · nmea · test imu/gps
// 아직 안 되는 것
//   BLE(3) · 화면(4) · WiFi(5) · LoRa(6) · 끄기·깊은잠·단추(7) · magcal · hdg/heel/pitch/level/smooth/dead 설정 명령
//
// 옮긴 원본: firmware-rak/src/main.cpp (커밋 2b18b17)
//   rawTiltDeg(276) · headingTiltDeg/boatHeadingDeg(2268-2300) · yawRateNow(2308) · imuDrainFifo(2031) ·
//   logWriteImu/buildNav/logWriteNav/logWriteText(3394-3498) · logStartNow(3500) · recSaveWant/Open · settleImuGap ·
//   recStartFrom · recWantOn/Off(3574-3636) · checkSensors(3795) · resumeRecordingIfCut(5349) · recGiveUp ·
//   recOnResult · recControlTick · rec 명령(4600-4756) · loadSettings(380) · applySensorPower(474) ·
//   reportResetReason(3759) · watchdogBegin(3776) · setup(5507) · loop(5658)

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_rak.h"     // firmware-rak/include — 같이 쓴다
#include "heading_math.h"  // sanitizeFloat · sanitizeAxes · wrap180 · eulerYawRate
#include "heading_tilt.h"  // 방위 식 하나 (앱 heading.ts 와 같음)
#include "hlog.h"
#include "rec_control.h"
#include "sdcard.h"

#include "gps.h"
#include "imu.h"

static inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// ── NVS 도우미 — 아두이노 Preferences 와 같은 키·형식 ──────────────────────
//   getUChar=u8 · getChar=i8 · getUInt=u32 · getInt=i32 · getFloat=4바이트 blob (Preferences.cpp putBytes)
namespace nv {
static uint8_t  u8 (nvs_handle_t h, const char* k, uint8_t d)  { uint8_t v;  return nvs_get_u8 (h, k, &v) == ESP_OK ? v : d; }
static int8_t   i8 (nvs_handle_t h, const char* k, int8_t d)   { int8_t v;   return nvs_get_i8 (h, k, &v) == ESP_OK ? v : d; }
static uint32_t u32(nvs_handle_t h, const char* k, uint32_t d) { uint32_t v; return nvs_get_u32(h, k, &v) == ESP_OK ? v : d; }
static int32_t  i32(nvs_handle_t h, const char* k, int32_t d)  { int32_t v;  return nvs_get_i32(h, k, &v) == ESP_OK ? v : d; }
static float    f32(nvs_handle_t h, const char* k, float d) {
    float v; size_t n = sizeof v;
    return (nvs_get_blob(h, k, &v, &n) == ESP_OK && n == sizeof v) ? v : d;
}
static bool has(nvs_handle_t h, const char* k) { return nvs_find_key(h, k, nullptr) == ESP_OK; }
// 쓰기 한 번 = 열기·쓰기·commit·닫기. 실패하면 false (prefs_util.h 의 "말없이 실패하지 않게" 와 같은 뜻)
template <class Fn>
static bool writeWith(Fn&& fn) {
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = fn(h);
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
} // namespace nv

// ── 워치독 (firmware-rak watchdogBegin · feedWatchdog) ─────────────────────
// ★ IDF 6.1 기본 설정은 켤 때 이미 5초 워치독을 건다 (sdkconfig CONFIG_ESP_TASK_WDT_INIT=1, TIMEOUT_S=5).
//   firmware-rak 이 뜻한 30초로 다시 맞추고 이 작업을 등록한다. 쉬는 작업(idle) 감시는 기본값 그대로 두 코어.
static constexpr uint32_t kWatchdogSec = 30;
static bool gWatchdogOn = false;
static void feedWatchdog() { if (gWatchdogOn) esp_task_wdt_reset(); }
static void watchdogBegin() {
    const esp_task_wdt_config_t cfg = {kWatchdogSec * 1000, (1u << portNUM_PROCESSORS) - 1, true};
    const esp_err_t rc = esp_task_wdt_reconfigure(&cfg);
    if (rc == ESP_OK && esp_task_wdt_add(nullptr) == ESP_OK) {
        gWatchdogOn = true;
        printf("[WDT] 워치독 %lu초 — 멈추면 스스로 다시 시작합니다\n", (unsigned long)kWatchdogSec);
    } else {
        printf("[WDT] !! 워치독 등록 실패 (%s) — 멈춤 보호 없이 돕니다\n", esp_err_to_name(rc));
    }
}

// ── 설정 (firmware-rak loadSettings) ───────────────────────────────────────
// 자이로 0점·자력 치우침은 imu::loadSettings 가 읽는다 (한 곳).
static uint8_t gHeelAxis = 1;   static float gHeelSign = -1.0f;  static float gHeelOffsetDeg = 0.0f;
static uint8_t gPitchAxis = 2;  static float gPitchSign = 1.0f;  static float gPitchOffsetDeg = 0.0f;
static uint8_t gHdgAxisA = 1, gHdgAxisB = 0;
static float   gHdgSignA = 1.0f, gHdgSignB = 1.0f, gHdgOffsetDeg = 0.0f, gHdgDeclDeg = 0.0f;
static int     gSensorPowerPin = rak::kSensorPowerA;

static void loadSettings() {
    uint8_t damp = 2;
    float deadKn = 0.10f;
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
        gSensorPowerPin = (int)nv::i32(h, "pwr_pin", rak::kSensorPowerA);
        // 힐 기준각의 키가 heel_off → heel_off2 로 바뀌었다. 옛 키는 안 읽는다 (firmware-rak 과 같음).
        gHeelAxis       = nv::u8 (h, "heel_axis", 1);
        gHeelSign       = nv::i8 (h, "heel_sgn", -1) < 0 ? -1.0f : 1.0f;
        gHeelOffsetDeg  = nv::f32(h, "heel_off2", 0.0f);
        gPitchAxis      = nv::u8 (h, "pitch_axis", 2);
        gPitchSign      = nv::i8 (h, "pitch_sgn", 1) < 0 ? -1.0f : 1.0f;
        gPitchOffsetDeg = nv::f32(h, "pitch_off", 0.0f);
        gHdgAxisA       = nv::u8 (h, "hdg_a", 1);
        gHdgAxisB       = nv::u8 (h, "hdg_b", 0);
        gHdgSignA       = nv::i8 (h, "hdg_sa", 1) < 0 ? -1.0f : 1.0f;
        gHdgSignB       = nv::i8 (h, "hdg_sb", 1) < 0 ? -1.0f : 1.0f;
        gHdgOffsetDeg   = nv::f32(h, "hdg_off", 0.0f);
        gHdgDeclDeg     = nv::f32(h, "hdg_decl", 0.0f);
        damp            = nv::u8 (h, "damp", 2);
        deadKn          = nv::f32(h, "dead_kn", 0.10f);
        nvs_close(h);
    }
    // ★ NVS 에서 읽은 값을 쓰기 전에 검사한다 (firmware-rak 과 같은 범위)
    int fixed = imu::loadSettings();   // 자력 치우침 · 자이로 0점 줄
    fixed += hdg::sanitizeFloat(&gHeelOffsetDeg,  -180.0f, 180.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gPitchOffsetDeg, -180.0f, 180.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gHdgOffsetDeg,   -360.0f, 360.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gHdgDeclDeg,      -30.0f,  30.0f, 0.0f);
    gHdgOffsetDeg = hdg::wrap180(gHdgOffsetDeg);
    fixed += hdg::sanitizeAxes(&gHdgAxisA, &gHdgAxisB);
    if (gHeelAxis > 2)  { gHeelAxis = 1;  ++fixed; }
    if (gPitchAxis > 2) { gPitchAxis = 2; ++fixed; }
    if (damp > 5)       { damp = 2;       ++fixed; }
    fixed += hdg::sanitizeFloat(&deadKn, 0.0f, 2.0f, 0.10f);
    if (fixed) printf("[SET] ★ 보드에 저장된 설정 %d개가 범위 밖이라 기본값으로 씁니다\n", fixed);
    if (gSensorPowerPin != rak::kSensorPowerA && gSensorPowerPin != rak::kSensorPowerB && gSensorPowerPin != 0)
        gSensorPowerPin = rak::kSensorPowerA;
    gps::setDampLevel(damp);
    gps::setDeadbandKn(deadKn);
}

// ── 센서 전원 (firmware-rak applySensorPower) ──────────────────────────────
// ★ 저장 버튼 자리(GPIO2 = kAin1)는 절대 건드리지 않는다. 입력으로 바꾸면 풀업이 풀려 "길게 눌림" 으로 먹힌다 (2026-08-27).
static void pinInput(int pin) { gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT); }
static void applySensorPower(int pin, bool cycle) {
    const int btn = rak::kAin1;
    const int other = (pin == rak::kSensorPowerA) ? rak::kSensorPowerB : rak::kSensorPowerA;
    if (other != btn) pinInput(other);
    if (pin == 0) {
        if (rak::kSensorPowerA != btn) pinInput(rak::kSensorPowerA);
        if (rak::kSensorPowerB != btn) pinInput(rak::kSensorPowerB);
        printf("[PWR] 센서 전원 끔 (버튼 자리는 그대로 둡니다)\n");
        return;
    }
    if (pin == btn) {
        printf("[PWR] GPIO%d 은 저장 버튼 자리입니다. 안 건드립니다.\n", pin);
        return;
    }
    const gpio_num_t g = static_cast<gpio_num_t>(pin);
    gpio_set_direction(g, GPIO_MODE_OUTPUT);
    if (cycle) { gpio_set_level(g, 0); delayMs(300); }
    gpio_set_level(g, 1);
    delayMs(300);
    printf("[PWR] 센서 전원 ON — GPIO%d 를 HIGH 로\n", pin);
}

// ── 지난번에 왜 다시 켜졌나 (firmware-rak reportResetReason 과 같은 글자) ──
static const char* gResetWhy = "?";
static void reportResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  gResetWhy = "POWERON 전원이 새로 들어왔다"; break;
        case ESP_RST_BROWNOUT: gResetWhy = "BROWNOUT ★ 전압이 내려앉았다"; break;
        case ESP_RST_PANIC:    gResetWhy = "PANIC ★ 코드가 죽었다"; break;
        case ESP_RST_TASK_WDT: gResetWhy = "TASK_WDT ★ 워치독이 물었다"; break;
        case ESP_RST_INT_WDT:  gResetWhy = "INT_WDT ★ 인터럽트 워치독"; break;
        case ESP_RST_WDT:      gResetWhy = "WDT ★ 워치독"; break;
        case ESP_RST_SW:       gResetWhy = "SW 소프트웨어가 다시 켰다"; break;
        case ESP_RST_DEEPSLEEP:gResetWhy = "DEEPSLEEP 깊은잠에서 깼다"; break;
        case ESP_RST_EXT:      gResetWhy = "EXT 바깥에서 리셋"; break;
        case ESP_RST_USB:      gResetWhy = "USB 포트가 열리며 리셋"; break;   // firmware-rak 목록에 없는 칸 — 거기서는 default(UNKNOWN)로 갔다
        default:               gResetWhy = "UNKNOWN 알 수 없음"; break;
    }
    printf("[BOOT] 지난번 꺼진 이유 — %s\n", gResetWhy);
    hlog::noteBootReason(gResetWhy);
}

// ── 배터리 (0단계에서 firmware-rak 과 1 mV 차로 맞춘 방법) ─────────────────
static adc_oneshot_unit_handle_t gAdc = nullptr;
static adc_cali_handle_t gCali = nullptr;
static adc_channel_t gBattCh;
static float gBattVolts = 0.0f;   // 1초마다 0.8·0.2 로 따라간다 (firmware-rak loop 2). 0 = 못 읽음

static void batteryInit() {
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(rak::kBattAdcPin, &unit, &gBattCh) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = unit;
    if (adc_oneshot_new_unit(&ucfg, &gAdc) != ESP_OK) { gAdc = nullptr; return; }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(gAdc, gBattCh, &ccfg);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {};
    cc.unit_id = unit; cc.chan = gBattCh; cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cc, &gCali) != ESP_OK) gCali = nullptr;
#endif
}

// firmware-rak readBatteryVolts: 16번 · 2 ms · 분압 0.6. 못 읽으면 0 (지어내지 않는다)
static float readBatteryVolts() {
    if (!gAdc || !gCali) return 0.0f;
    uint32_t sum = 0; int ok = 0;
    for (int i = 0; i < 16; ++i) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(gAdc, gCali, gBattCh, &mv) == ESP_OK) { sum += (uint32_t)mv; ++ok; }
        delayMs(2);
    }
    if (!ok) return 0.0f;
    return (sum / (float)ok / 1000.0f) / rak::kBattDivider * rak::kBattCorrection;
}

// ── 자세·방위 (firmware-rak 과 같은 식 · 같은 입력) ────────────────────────
static float wrap180(float deg) { return hdg::wrap180(deg); }

struct AxisName {
    char text[4];
    AxisName(uint8_t axis, float sign) {
        text[0] = (sign < 0.0f) ? '-' : '+';
        text[1] = (axis == 0) ? 'X' : (axis == 1 ? 'Y' : 'Z');
        text[2] = '\0';
    }
};

// 기준각을 빼기 전의 날각도
static float rawTiltDeg(uint8_t axis, float sign) {
    const imu::Vec& g = imu::acc();
    const float a = (axis == 0) ? g.x : (axis == 1 ? g.y : g.z);
    const float mag = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
    if (mag < 0.2f) return 0.0f;   // 자유낙하 수준이면 중력 방향을 알 수 없다
    float s = sign * a / mag;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return asinf(s) * 180.0f / (float)M_PI;
}
static float currentHeelDeg()  { return wrap180(rawTiltDeg(gHeelAxis, gHeelSign) - gHeelOffsetDeg); }
static float currentPitchDeg() { return wrap180(rawTiltDeg(gPitchAxis, gPitchSign) - gPitchOffsetDeg); }

static hdg::HeadingCfg hdgCfgNow() {
    hdg::HeadingCfg c;
    c.axisA = gHdgAxisA; c.axisB = gHdgAxisB;
    c.signA = gHdgSignA; c.signB = gHdgSignB;
    c.offDeg = gHdgOffsetDeg; c.declDeg = gHdgDeclDeg;
    return c;
}

static float flatHeadingDeg() {
    if (!imu::magFresh()) return -1.0f;
    const imu::Vec& m = imu::mag();
    const float v[3] = {m.x, m.y, m.z};
    return hdg::flatHeadingDeg(v, hdgCfgNow());
}

static float headingTiltDeg() {
    if (!imu::magFresh() || !imu::ok()) return -1.0f;
    const imu::Vec& a = imu::acc();
    const imu::Vec& m = imu::mag();
    const float acc[3] = {a.x, a.y, a.z};
    const float v[3]   = {m.x, m.y, m.z};
    return hdg::tiltHeadingDeg(acc, v, hdgCfgNow());
}

// 배의 방위 — 화면·BLE·TXT·NAV 가 읽는 단 하나. 못 구하면 -1 (평평 식으로 몰래 채우지 않는다)
static float boatHeadingDeg() { return headingTiltDeg(); }

// 뱃머리 방위 변화율 (°/s). ZYX 오일러 ψ̇ = (q·sinφ + r·cosφ) / cosθ
static float yawRateNow() {
    if (!imu::ok()) return 0.0f;
    const imu::Vec& a = imu::acc();
    const float acc[3] = {a.x, a.y, a.z};
    float roll, pitch;
    if (!hdg::gravityRollPitch(acc, hdgCfgNow(), &roll, &pitch)) return 0.0f;
    float g[3];
    imu::gyrInMagFrame(g);
    float p, q, r;
    if (!hdg::toFRD(g, hdgCfgNow(), &p, &q, &r)) return 0.0f;
    const float rate = hdg::eulerYawRate(p, q, r, roll, pitch);
    return std::isfinite(rate) ? rate : 0.0f;
}

// 켠 뒤로 뱃머리가 돈 각의 합 (°). 100 Hz 로 쌓인다. 차이로만 쓴다.
static float gYawIntDeg = 0.0f;

// ── 기록 줄 만들기 ─────────────────────────────────────────────────────────
static void logWriteImu(uint32_t ms) {
    const imu::Vec& a = imu::acc();
    const imu::Vec& g = imu::gyr();
    hlog::ImuSample b;
    b.localMs = ms;
    b.acc[0] = (int16_t)lroundf(a.x * 1000.0f);   // 1 mg/LSB
    b.acc[1] = (int16_t)lroundf(a.y * 1000.0f);
    b.acc[2] = (int16_t)lroundf(a.z * 1000.0f);
    b.gyr[0] = (int16_t)lroundf(g.x * 32.0f);     // 1/32 °/s/LSB
    b.gyr[1] = (int16_t)lroundf(g.y * 32.0f);
    b.gyr[2] = (int16_t)lroundf(g.z * 32.0f);
    hlog::writeImu(b);
}

// firmware-rak imuDrainFifo — 10 ms 마다. 방위 적분과 IMU 줄은 값이 오는 이 자리에서 한다.
static void imuDrain() {
    if (imu::fifoBegin() > 0) {
        uint32_t tick;
        while (imu::fifoNext(&tick)) {
            gYawIntDeg += yawRateNow() * 0.010f;
            if (hlog::recording()) logWriteImu(tick);
        }
    }
    const uint32_t dropped = imu::takeDroppedRows();
    if (dropped) hlog::noteDropped(dropped);
}

// GPS 가 몇 초씩 기다리는 동안 부른다 (firmware-rak casicWait 안의 feedWatchdog + imuDrainFifo)
static void waitHook() {
    feedWatchdog();
    imuDrain();
}

static hlog::NavSample buildNav(uint32_t ms) {
    hlog::NavSample a;
    a.localMs = ms;
    const gps::State& gs = gps::state();
    TinyGPSPlus& p = gps::parser();

    uint16_t wk; uint32_t tow;
    if (gps::weekTow(&wk, &tow)) { a.week = wk; a.itow = tow; }

    if (gs.fix && p.location.isValid()) {
        a.lat = (int32_t)lround(p.location.lat() * 1e7);
        a.lon = (int32_t)lround(p.location.lng() * 1e7);
        a.fix = 1;
    }
    // ★ 다듬기 전 도플러 원본. sogOut() 을 쓰면 안 된다.
    if (gs.fix && p.speed.isValid()) {
        const float kn = (float)p.speed.knots();
        long mmps = lroundf(kn / 1.943844f * 1000.0f);
        if (mmps < 0) mmps = 0;
        if (mmps > 65534) mmps = 65534;
        a.sog = (uint16_t)mmps;
    }
    if (gs.fix && p.course.isValid()) {
        long cd = lroundf((float)p.course.deg() * 100.0f) % 36000;
        if (cd < 0) cd += 36000;
        a.cog = (uint16_t)cd;
    }
    if (p.satellites.isValid()) a.numSv = (uint8_t)p.satellites.value();
    if (gs.pvHAccM >= 0.0f) {
        long cm = lroundf(gs.pvHAccM * 100.0f);
        a.hAcc = (uint16_t)(cm > 65534 ? 65534 : (cm < 0 ? 0 : cm));
    }
    a.battMv = (uint16_t)lroundf(gBattVolts * 1000.0f);
    // 자력 0.1 µT/LSB — 하드아이언을 뺀 값. 새 표본이 없으면 0.
    if (imu::magFresh()) {
        const imu::Vec& m = imu::mag();
        a.mag[0] = (int16_t)lroundf(m.x * 10.0f);
        a.mag[1] = (int16_t)lroundf(m.y * 10.0f);
        a.mag[2] = (int16_t)lroundf(m.z * 10.0f);
    }
    const float h = boatHeadingDeg();
    if (h >= 0.0f) {
        long cd = lroundf(h * 100.0f);
        if (cd >= 36000) cd = 0;
        a.hdg = (uint16_t)cd;
    }
    return a;
}

static void logWriteNav(uint32_t ms) {
    const hlog::NavSample a = buildNav(ms);
    hlog::writeNav(a);
    // 첫 fix 의 UTC — NMEA UTC 로 week/itow 를 만들었으므로 윤초를 더하지 않는다 (머리글 time_ref=1)
    if (a.fix && a.week != hlog::kWeekInvalid && a.itow != hlog::kItowInvalid) {
        const uint32_t sec = 315964800UL + (uint32_t)a.week * 604800UL + a.itow / 1000UL;
        hlog::noteUtcStart(sec, (uint16_t)(a.itow % 1000UL));
    }
}

static void logWriteText(uint32_t ms) {
    const gps::State& gs = gps::state();
    hlog::TextSample t;
    t.attOk    = imu::ok();
    t.heelDeg  = currentHeelDeg();
    t.pitchDeg = currentPitchDeg();
    t.hdgDeg   = boatHeadingDeg();
    // ★ 표식이 4 이상일 때만 값을 넣는다. 3 은 옛날 값이라 넣으면 거짓말이 된다.
    t.sogPvKn   = (gs.pvVelValid && gs.pvSpeedKn >= 0) ? gs.pvSpeedKn : -1.0f;
    t.sogPosKn  = gs.sogFromPos;
    t.pvFlag    = (gs.pvAtMs && ms - gs.pvAtMs < 3000) ? gs.pvVelFlag : 255;
    t.cogAccDeg = gs.pvCogAccDeg;
    t.sogAccKn  = (gs.pvAtMs && ms - gs.pvAtMs < 3000 && gs.pvVelValid) ? gs.pvAccKn : -1.0f;
    hlog::writeText(buildNav(ms), t);
}

// ── 기록 제어 — 원하는 상태(gWantRec)와 실제 상태(hlog::phase) ──────────────
static constexpr uint8_t  kResumeMax       = 5;
static constexpr uint32_t kResumeOkMs      = 60000;
static constexpr uint8_t  kRecRestartMax   = 3;
static constexpr uint32_t kRecRestartGapMs = 3000;
static constexpr uint32_t kRecRestartOkMs  = 600000;

static bool     gWantRec = false;
static bool     gRecGaveUp = false;
static bool     gRecLastSaveBad = false;
static uint8_t  gRecRestarts = 0;
static uint32_t gRecRestartAt = 0;
static uint32_t gRecFailSession = 0;
static uint8_t  gResumeTries = 0;   // 0 이면 이어 시작한 게 아니다
static char     gRecFailLine[240] = "";
static const char* gRecStartErr = nullptr;
static char     gSessNote[640] = "";

static void recSaveWant(bool want) {
    if (!nv::writeWith([&](nvs_handle_t h) { return nvs_set_u8(h, "rec_want", want ? 1 : 0) == ESP_OK; }))
        printf("[REC] ★ 이어 시작 의도(rec_want)를 못 적었습니다\n");
}
static void recSaveOpen(uint32_t session) {
    if (!nv::writeWith([&](nvs_handle_t h) { return nvs_set_u32(h, "rec_open", session) == ESP_OK; }))
        printf("[REC] ★ 열린 세션(rec_open)을 못 적었습니다\n");
}

// IMU 가 끊겨 있던 동안의 100 Hz 표본은 기록에 없다. 머리글 dropped 에 어림으로 더한다.
// 다시 붙을 때 · 기록을 멈출 때. 정산한 시각으로 옮겨 두어 같은 구간을 두 번 세지 않는다.
static void settleImuGap(uint32_t untilMs) {
    const uint32_t from = imu::moveLostAt(untilMs);
    if (from && hlog::recording()) hlog::noteDropped(recctl::imuGapRows(from, hlog::recStartedMs(), untilMs));
}

// firmware-rak buildHeadingNote 와 같은 글자 (TXT 머리)
static void buildHeadingNote(char* out, size_t n) {
    static const char* kAx = "XYZ";
    char a[8], b[8];
    snprintf(a, sizeof a, "%c%c", gHdgSignA < 0 ? '-' : '+', kAx[gHdgAxisA < 3 ? gHdgAxisA : 0]);
    snprintf(b, sizeof b, "%c%c", gHdgSignB < 0 ? '-' : '+', kAx[gHdgAxisB < 3 ? gHdgAxisB : 0]);
    const float* mo = imu::magOffset();
    snprintf(out, n,
             "# 방위(화면·BLE·TXT): 기울기 보정(INSLIB ahrs_mag_detilt) — 축 atan2(자력 %s, 자력 %s) 기준, 중력은 그때 가속도, |a| 가 1 g ±0.15 밖이면 방위 없음. + 장착 오프셋 %+.2f° + 자기 편각 %+.2f°\n"
             "# 자력: HLG 의 mag 는 하드아이언을 뺀 값 — 뺀 오프셋 %.2f %.2f %.2f uT (반지름 %.1f, 잔차 %.2f)\n"
             "# 가속→자력 축: 자력 X=가속 Y, Y=가속 X, Z=−가속 Z\n",
             a, b, gHdgOffsetDeg, gHdgDeclDeg, mo[0], mo[1], mo[2], imu::magRadius(), imu::magResid());
}

static bool logStartNow(uint32_t prevSession) {
    gRecStartErr = nullptr;
    hlog::Header h;
    h.prevSession = prevSession;
    esp_read_mac(h.mac, ESP_MAC_WIFI_STA);
    h.fwVersion = 0x0100;
    h.hwRev     = 1;
    h.gnssType  = 0xFF;                   // L76K 는 규격에 없다 — 모름
    h.imuType   = hlog::kImuMPU9250;
    h.timeRef   = 1;
    h.magScale  = 1;
    h.gnssHz    = gps::state().hz;
    h.sogSrc    = 0;
    h.quatSrc   = 1;
    h.heelAxis  = gHeelAxis;   h.heelSign  = gHeelSign < 0 ? 1 : 0;
    h.pitchAxis = gPitchAxis;  h.pitchSign = gPitchSign < 0 ? 1 : 0;
    h.heelOff   = gHeelOffsetDeg;  h.pitchOff = gPitchOffsetDeg;
    h.hdgFormula = hlog::kHdgFormulaTilt;
    h.hdgAxisA = gHdgAxisA;  h.hdgAxisB = gHdgAxisB;
    h.hdgSignA = gHdgSignA < 0 ? 1 : 0;  h.hdgSignB = gHdgSignB < 0 ? 1 : 0;
    h.hdgOff   = gHdgOffsetDeg;  h.hdgDecl = gHdgDeclDeg;
    const float* mo = imu::magOffset();
    for (int i = 0; i < 3; ++i) h.magHi[i] = mo[i];
    h.gnssDyn = gps::state().dyModel;     // 모듈에서 실제로 읽은 값만. 못 읽었으면 255
    buildHeadingNote(gSessNote, sizeof gSessNote);
    hlog::setSessionNote(gSessNote);
    if (!hlog::start(h)) {
        hlog::Status st; hlog::getStatus(&st);
        gRecStartErr = st.lastError ? st.lastError : "알 수 없음";
        return false;
    }
    // 카드를 붙이는 동안 FIFO 에 옛 값이 쌓인다. 비우고 시작한다.
    imu::resetFifoForRecording();
    return true;
}

static bool recStartFrom(const char* who, uint32_t prev = 0) {
    const uint32_t t0 = nowMs();
    const bool ok = logStartNow(prev);
    if (ok) {
        hlog::Status st; hlog::getStatus(&st);
        printf("[REC] 시작 (%s) — %s  %lums\n", who, st.path, (unsigned long)(nowMs() - t0));
        recSaveOpen(st.session);
        gRecLastSaveBad = false;
    } else {
        printf("[REC] ★ 시작 못 함 (%s) — %s\n", who, gRecStartErr ? gRecStartErr : "알 수 없음");
    }
    return ok;
}

static bool recWantOn(const char* who) {
    gRecGaveUp = false; gRecRestarts = 0; gRecRestartAt = 0;
    gWantRec = true;
    recSaveWant(true);
    const recctl::Phase ph = hlog::phase();
    if (ph == recctl::Phase::Recording) return true;
    if (ph == recctl::Phase::Closing) {
        printf("[REC] 앞 기록을 닫는 중 — 닫히면 시작합니다 (%s)\n", who);
        return true;
    }
    const bool ok = recStartFrom(who);
    if (!ok) { gWantRec = false; recSaveWant(false); }
    return ok;
}

static void recWantOff(const char* who) {
    gWantRec = false; gRecGaveUp = false; gRecRestartAt = 0; gRecRestarts = 0;
    recSaveWant(false);
    if (hlog::phase() != recctl::Phase::Recording) return;
    settleImuGap(nowMs());
    hlog::requestStop();
    printf("[REC] 멈춤 요청 (%s) — 닫는 중\n", who);
}

static void recGiveUp(const char* why) {
    gRecGaveUp = true; gWantRec = false; gRecRestartAt = 0;
    recSaveWant(false);
    printf("[REC] ★ %s — 멈춘 채로 둡니다 (REC FAIL)\n", why);
}

static void recOnResult(const recctl::SessionResult& r, uint32_t now) {
    gRecFailSession = r.session;
    recSaveOpen(0);
    const bool clean = r.drained && r.headerOk && r.firstKind == recctl::kErrNone;
    gRecLastSaveBad = !clean;
    printf("[REC] 세션 %u 닫힘 — %s  %u분 %u초%s\n", (unsigned)r.session,
           clean ? "정상 종료" : "★ 정상 종료 아님",
           (unsigned)(r.durS / 60), (unsigned)(r.durS % 60),
           r.lostBytes ? "  · 못 쓴 바이트 있음" : "");
    if (r.firstKind != recctl::kErrNone) {
        static const char* kKind[] = {"?", "쓰기 실패", "카드 빠짐", "닫으면서 다 못 씀", "?", "머리글 못 고침"};
        const char* kind = (r.firstKind <= 5) ? kKind[r.firstKind] : "?";
        snprintf(gRecFailLine, sizeof gRecFailLine,
                 "세션 %u %u분%u초째 %s%s | 쓸것 %u 쓴것 %u | errno %d %s | 다시쓰기 %u번 | 카드 %s | %.2fV | 누적 %.1fMB | 못 쓰고 버림 %lu바이트 | 머리글 %s",
                 (unsigned)r.session, (unsigned)(r.recSec / 60), (unsigned)(r.recSec % 60),
                 kind, r.fake ? "(시험)" : "",
                 (unsigned)r.want, (unsigned)r.wrote,
                 r.err, r.err ? strerror(r.err) : "(이유 안 줌)",
                 (unsigned)r.tries, r.card ? "있음" : "없음",
                 gBattVolts, r.bytes / 1048576.0f, (unsigned long)r.lostBytes,
                 r.headerOk ? "고침" : "못 고침");
        printf("[REC] ★ 멈춘 이유 — %s\n", gRecFailLine);
        nv::writeWith([&](nvs_handle_t h) {
            uint32_t n = 0; nvs_get_u32(h, "rec_fail_n", &n);
            return nvs_set_str(h, "rec_fail", gRecFailLine) == ESP_OK && nvs_set_u32(h, "rec_fail_n", n + 1) == ESP_OK;
        });
        hlog::noteLastFail(gRecFailLine);
    }
    switch (recctl::afterSession(gWantRec, r.firstKind, gRecRestarts, kRecRestartMax)) {
    case recctl::Next::Nothing: break;
    case recctl::Next::RestartSoon:
        gRecRestartAt = now + kRecRestartGapMs;
        printf("[REC] %u초 뒤 새 파일로 다시 겁니다\n", (unsigned)(kRecRestartGapMs / 1000));
        break;
    case recctl::Next::GiveUp:
        recGiveUp("다시 걸기를 다 썼습니다");
        break;
    }
}

// 끄기(깊은잠)는 7단계에서 붙인다.
static void recControlTick(uint32_t now) {
    recctl::SessionResult r;
    if (hlog::poll(&r)) recOnResult(r, now);
    const recctl::Phase ph = hlog::phase();
    if (ph == recctl::Phase::Recording) {
        if (gRecRestarts && now - hlog::recStartedMs() >= kRecRestartOkMs) gRecRestarts = 0;
        return;
    }
    if (!gWantRec || ph != recctl::Phase::Idle) return;
    if (gRecRestartAt == 0) {
        if (!recStartFrom("닫힌 뒤 시작")) { gWantRec = false; recSaveWant(false); }
        return;
    }
    if ((int32_t)(now - gRecRestartAt) < 0) return;
    ++gRecRestarts;
    printf("[REC] 새 파일로 다시 겁니다 (%u번째, 앞 세션 %u)\n", gRecRestarts, (unsigned)gRecFailSession);
    if (recStartFrom("쓰기 실패 뒤 다시 걸기", gRecFailSession)) { gRecRestartAt = 0; return; }
    if (recctl::afterRestartFailed(gRecRestarts, kRecRestartMax) == recctl::Next::RestartSoon)
        gRecRestartAt = now + kRecRestartGapMs;
    else
        recGiveUp("다시 걸기를 다 썼습니다");
}

static void setRecTry(uint8_t v) {
    nv::writeWith([&](nvs_handle_t w) { return nvs_set_u8(w, "rec_try", v) == ESP_OK; });
}

static void resumeRecordingIfCut() {
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READWRITE, &h) != ESP_OK) return;
    const bool haveNew = nv::has(h, "rec_want");
    recctl::Persist cur;
    cur.want = nv::u8(h, "rec_want", 0);
    cur.open = nv::u32(h, "rec_open", 0);
    const bool haveOld = nv::has(h, "rec_on");
    const uint32_t lastSess = nv::u32(h, "sess_n", 0);
    const recctl::Persist p = recctl::migrate(haveNew, cur, haveOld, haveOld ? nv::u8(h, "rec_on", 0) : 0, lastSess);
    if (!haveNew) { nvs_set_u8(h, "rec_want", p.want); nvs_set_u32(h, "rec_open", p.open); }
    if (haveOld) nvs_erase_key(h, "rec_on");
    const uint8_t tries = nv::u8(h, "rec_try", 0);
    const uint32_t forced = nv::u32(h, "rec_forced", 0);
    if (forced) nvs_erase_key(h, "rec_forced");
    nvs_commit(h);
    nvs_close(h);
    if (haveOld) printf("[REC] 옛 표시 rec_on 을 rec_want=%u · rec_open=%u 로 옮겼습니다\n", p.want, (unsigned)p.open);
    if (forced) printf("[REC] ★ 지난번 세션 %u 는 닫기 전에 사람이 강제로 껐습니다 — 끝이 잘렸을 수 있습니다\n", (unsigned)forced);

    switch (recctl::atBoot(p, tries, kResumeMax)) {
    case recctl::Boot::Nothing:
        if (tries) setRecTry(0);
        return;
    case recctl::Boot::UnclosedOnly:
        printf("[REC] 세션 %u 가 마감 안 된 채 남았습니다 — 사람이 멈춘 뒤라 이어 시작하지 않습니다\n", (unsigned)p.open);
        recSaveOpen(0);
        return;
    case recctl::Boot::TooMany:
        printf("[REC] ★ 이어 시작을 %u번 했는데 계속 끊깁니다 — 멈춥니다.\n", tries);
        printf("      전원선·배터리 접점을 보세요. rec on 으로 직접 걸 수 있습니다.\n");
        recSaveWant(false);
        setRecTry(0);
        gRecGaveUp = true;
        return;
    case recctl::Boot::Resume:
        break;
    }
    const uint8_t n = (uint8_t)(tries + 1);
    setRecTry(n);
    const uint32_t prev = p.open ? p.open : lastSess;
    printf("[REC] 지난 세션 %u 가 못 닫히고 끊겼습니다 — 이어서 시작합니다 (%u번째)\n", (unsigned)prev, n);
    gWantRec = true;
    gRecFailSession = prev;
    if (recStartFrom("켤 때 이어 시작", prev)) gResumeTries = n;
    else gRecRestartAt = nowMs() + kRecRestartGapMs;
}

// ── 센서 끊김·다시 붙이기 (firmware-rak checkSensors 의 IMU 부분) ───────────
static void checkSensors() {
    uint32_t gapFrom = 0;
    if (imu::checkPresence(&gapFrom) == imu::Presence::Reattached && gapFrom && hlog::recording())
        hlog::noteDropped(recctl::imuGapRows(gapFrom, hlog::recStartedMs(), nowMs()));
}

// ── SD 사용권 (firmware-rak sdFreeFor) ─────────────────────────────────────
static bool sdFreeFor(const char* what) {
    const sdcard::Owner o = sdcard::owner();
    if (o == sdcard::Owner::None) return true;
    printf("[SD] %s — 카드를 지금 쓰는 곳: %s.%s\n", what, sdcard::ownerName(o),
           o == sdcard::Owner::Recorder ? " rec off 먼저." :
           o == sdcard::Owner::Download ? " wifi off 먼저." : "");
    return false;
}

// 몇 초씩 루프를 붙잡는 진단. 기록 중에는 막는다 — 그동안 NAV·텍스트가 밀린다.
static bool blockingDiagOk(const char* what) {
    if (!hlog::busy()) return true;
    printf("[진단] %s — 기록 중에는 몇 초씩 루프를 붙잡는 진단을 막습니다. rec off 먼저.\n", what);
    return false;
}

// ── 진단: imu (firmware-rak doImu · printImuLine) ──────────────────────────
static void printImuLine() {
    if (!imu::printRaw()) return;
    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    printf(" | 힐 %+6.1f° (가속 %s)  피치 %+6.1f° (가속 %s)\n",
           currentHeelDeg(), hAx.text, currentPitchDeg(), pAx.text);
    if (imu::magFresh()) {
        const float flat = flatHeadingDeg(), tilt = headingTiltDeg();
        if (tilt >= 0.0f)
            printf("   방위 비교  평평 %5.1f°  |  기울기보정 %5.1f°  |  차이 %+5.1f°\n", flat, tilt, wrap180(tilt - flat));
        else
            printf("   방위 비교  평평 %5.1f°  |  기울기보정 ---  (가속이 1 g 에서 벗어남)\n", flat);
    } else if (imu::magOk()) {
        printf("   방위 비교  --- (자력 새 표본 없음)\n");
    }
}

static void doImu() {
    if (!imu::diagBegin()) return;
    // ★ FIFO 를 계속 비운다. update() 만 부르면 FIFO 가 켜져 있을 때 값이 멈춰 보이고 FIFO 는 넘친다.
    const uint32_t start = nowMs();
    while (nowMs() - start < 5000) {
        const uint32_t t1 = nowMs();
        while (nowMs() - t1 < 400) {
            imuDrain();
            gps::poll();
            feedWatchdog();
            delayMs(10);
        }
        imu::update();
        printImuLine();
    }
    imu::diagEnd();
}

// ── 시리얼 명령 ────────────────────────────────────────────────────────────
static void cmdRec(const char* arg) {
    if (!strcmp(arg, "on") || !strcmp(arg, "start")) { recWantOn("rec on"); return; }
    if (!strcmp(arg, "off") || !strcmp(arg, "stop")) {
        if (hlog::phase() != recctl::Phase::Recording) printf("[REC] 기록 중이 아닙니다\n");
        recWantOff("rec off");
        return;
    }
    if (!strncmp(arg, "slow ", 5)) {
        long ms = strtol(arg + 5, nullptr, 10);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        hlog::testSlowClose((uint32_t)ms);
        printf("[REC] 시험 — 다음 닫기 직전에 %ld ms 쉽니다\n", ms);
        return;
    }
    if (!strcmp(arg, "mark")) { hlog::mark(); return; }
    if (!strcmp(arg, "fail clear")) {
        nv::writeWith([](nvs_handle_t h) {
            nvs_erase_key(h, "rec_fail"); nvs_erase_key(h, "rec_fail_n"); nvs_erase_key(h, "rec_forced");
            return true;
        });
        gRecFailLine[0] = '\0';
        hlog::noteLastFail(nullptr);
        gRecGaveUp = false; gRecRestarts = 0; gRecLastSaveBad = false;
        hlog::testFailWrites(0);
        printf("[REC] 멈춤 기록을 지웠습니다\n");
        return;
    }
    if (!strncmp(arg, "fail ", 5)) {
        long n = strtol(arg + 5, nullptr, 10);
        hlog::testFailWrites((uint8_t)(n < 0 ? 0 : (n > 200 ? 200 : n)));
        printf("[REC] 시험 — 다음 %ld번 쓰기를 실패로 흉내 냅니다\n", n);
        return;
    }
    if (!strcmp(arg, "ls") || !strcmp(arg, "list")) { if (sdFreeFor("rec ls")) hlog::listFiles(); return; }
    if (!strncmp(arg, "rm ", 3)) {
        long n = strtol(arg + 3, nullptr, 10);
        if (sdFreeFor("rec rm")) hlog::removeSession((uint32_t)(n < 0 ? 0 : n));
        return;
    }
    if (!strncmp(arg, "tail", 4) || !strncmp(arg, "head", 4)) {
        const bool head = !strncmp(arg, "head", 4);
        long sess = 0, n = 20;
        sscanf(arg + 4, "%ld %ld", &sess, &n);
        if (n < 1) n = 1;
        if (n > 400) n = 400;
        if (sdFreeFor("rec tail")) hlog::tail((uint32_t)(sess < 0 ? 0 : sess), (uint16_t)n, head);
        return;
    }
    if (!strncmp(arg, "dump ", 5)) {
        char kind[8] = {0};
        long sess = 0, off = 0, len = 0;
        if (sscanf(arg + 5, "%ld %7s %ld %ld", &sess, kind, &off, &len) != 4 || sess <= 0 || off < 0 || len <= 0 ||
            (strcmp(kind, "hlg") != 0 && strcmp(kind, "txt") != 0)) {
            printf("@DUMP X 형식: rec dump <번호> <hlg|txt> <시작> <바이트>\n");
            return;
        }
        if (sdFreeFor("rec dump")) hlog::dump((uint32_t)sess, kind[0] == 'h', (uint32_t)off, (uint32_t)len);
        return;
    }
    if (!strncmp(arg, "hash ", 5)) {
        char kind[8] = {0};
        long sess = 0;
        if (sscanf(arg + 5, "%ld %7s", &sess, kind) != 2 || sess <= 0 ||
            (strcmp(kind, "hlg") != 0 && strcmp(kind, "txt") != 0)) {
            printf("@HASH X 형식: rec hash <번호> <hlg|txt>\n");
            return;
        }
        if (sdFreeFor("rec hash")) hlog::hashFile((uint32_t)sess, kind[0] == 'h');
        return;
    }
    if (!strncmp(arg, "check", 5)) {
        long n = strtol(arg + 5, nullptr, 10);
        if (sdFreeFor("rec check")) hlog::verify((uint32_t)(n < 0 ? 0 : n));
        return;
    }

    hlog::Status st; hlog::getStatus(&st);
    printf("──────────────────────────────────────────\n");
    printf("  카드          %s\n", st.cardPresent ? "있음" : "없음");
    if (!st.recording) {
        printf("  기록          멈춰 있음   (rec on 으로 시작)\n");
        if (st.lastError) printf("  지난 오류     %s\n", st.lastError);
        if (st.session)   printf("  지난 세션     %s  %u+%u줄\n", st.path, (unsigned)st.navRows, (unsigned)st.imuRows);
    } else {
        const uint32_t sec = (nowMs() - st.startedMs) / 1000;
        printf("  기록 중       %s\n", st.path);
        printf("  지난 시간     %u분 %u초\n", (unsigned)(sec / 60), (unsigned)(sec % 60));
        printf("  NAV 줄        %u   (10 Hz 면 %u 쯤이어야 정상)\n", (unsigned)st.navRows, (unsigned)(sec * 10));
        printf("  IMU 줄        %u   (100 Hz 면 %u 쯤)\n", (unsigned)st.imuRows, (unsigned)(sec * 100));
        printf("  쓴 양         %.2f MB\n", st.bytes / 1048576.0);
    }
    printf("  ─── 건강 상태 ───\n");
    printf("  다시 써서 살림 %u번 (이 부팅)\n", (unsigned)hlog::writeRetries());
    {
        nvs_handle_t h;
        char last[240] = "";
        uint32_t cnt = 0;
        if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof last;
            if (nvs_get_str(h, "rec_fail", last, &len) != ESP_OK) last[0] = '\0';
            cnt = nv::u32(h, "rec_fail_n", 0);
            nvs_close(h);
        }
        if (last[0]) printf("  지난 멈춤     모두 %u번. 마지막: %s\n", (unsigned)cnt, last);
        else         printf("  지난 멈춤     없음\n");
    }
    printf("  버린 줄       %u   %s\n", (unsigned)st.dropped, st.dropped ? "★ 구멍이 났습니다" : "(0 이어야 정상)");
    printf("  기다린 횟수   %u   (버퍼가 찰 뻔한 횟수)\n", (unsigned)st.waited);
    printf("  최대 멈춤     %u ms  (카드가 제일 오래 안 놓아준 시간)\n", (unsigned)st.maxStallMs);
    printf("  버퍼 최고     %u %%\n", (unsigned)st.maxFillPct);
    printf("  IMU FIFO      %s,  넘칠 뻔 %u번 %s\n",
           imu::fifoOn() ? "켜짐(칩이 100Hz 로 뜸)" : "꺼짐",
           (unsigned)imu::fifoOverrun(),
           imu::fifoOverrun() ? "★ 그때 값이 비었습니다" : "(0 이어야 정상)");
    printf("──────────────────────────────────────────\n");
    printf("  rec on / rec off / rec mark\n");
    printf("  rec ls          카드에 있는 파일 목록\n");
    printf("  rec check [번호]  보드가 직접 되읽어 검사 (기본: 마지막 세션)\n");
    printf("  rec tail [번호] [줄수]  TXT 사본 끝 몇 줄 (rec head 는 앞부분)\n");
    printf("  rec rm <번호>     그 세션의 HLG·TXT 를 지운다 (못 되돌린다)\n");
}

static void handleLine(char* line) {
    while (*line == ' ') ++line;
    size_t n = strlen(line);
    while (n && line[n - 1] == ' ') line[--n] = '\0';
    if (!n) return;

    if (!strcmp(line, "rec")) { cmdRec(""); return; }
    if (!strncmp(line, "rec ", 4)) {
        char* arg = line + 4;
        while (*arg == ' ') ++arg;
        for (char* p = arg; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        cmdRec(arg);
        return;
    }
    if (!strncmp(line, "test imu ", 9)) { imu::testPause(strtol(line + 9, nullptr, 10)); return; }
    if (!strncmp(line, "test gps ", 9)) { gps::testPause(strtol(line + 9, nullptr, 10)); return; }
    if (!strcmp(line, "imu"))     { if (blockingDiagOk("imu")) doImu(); return; }
    if (!strcmp(line, "fix"))     { gps::printFix(); return; }
    if (!strncmp(line, "gpshz ", 6)) {
        if (!blockingDiagOk("gpshz")) return;
        gps::rateCommand(strtol(line + 6, nullptr, 10));
        return;
    }
    if (!strcmp(line, "gps"))     { if (blockingDiagOk("gps")) gps::peek(5, false); return; }
    if (!strcmp(line, "gps d") || !strcmp(line, "gps D")) { if (blockingDiagOk("gps d")) gps::peek(5, true); return; }
    if (!strcmp(line, "gpscfg"))  { if (blockingDiagOk("gpscfg")) gps::cfgDump(); return; }
    if (!strcmp(line, "sog"))     { gps::printSogStatus(); return; }
    if (!strcmp(line, "navpv"))   { if (blockingDiagOk("navpv")) gps::navPvPrint(8);  return; }
    if (!strcmp(line, "navpv l")) { if (blockingDiagOk("navpv l")) gps::navPvPrint(40); return; }
    if (!strncmp(line, "gpscfg mode ", 12)) {
        if (!blockingDiagOk("gpscfg mode")) return;
        const long m = strtol(line + 12, nullptr, 10);
        if (m < 0 || m > 7) { printf("  0~7 중에서 고르세요\n"); return; }
        gps::cfgSetNavx(true, (uint8_t)m, false, 0.0f);
        return;
    }
    if (!strncmp(line, "gpscfg static ", 14)) {
        gps::cfgSetNavx(false, 0, true, strtof(line + 14, nullptr));
        return;
    }
    // nmea <본문>  — 체크섬은 알아서 붙인다. 예: nmea PMTK386,0
    if (!strncmp(line, "nmea ", 5)) {
        char* body = line + 5;
        while (*body == ' ') ++body;
        if (*body == '$') ++body;
        if (char* star = strchr(body, '*')) *star = '\0';
        if (!*body) { printf("  보낼 내용이 없습니다\n"); return; }
        gps::sendAndWatch(body);
        return;
    }
    printf("[CMD] 모르는 명령: %s  (2단계에서 되는 것: rec … · imu · fix · gps · gpscfg · gpshz · sog · navpv · nmea · test imu/gps)\n", line);
}

// USB 로 온 글자를 한 줄씩 모은다 (firmware-rak pollSerial — 64자까지)
static void pollSerial() {
    static char buf[65];
    static size_t n = 0;
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == '\n' || c == '\r') {
            if (n) { buf[n] = '\0'; handleLine(buf); n = 0; }
        } else if (n < sizeof buf - 1) {
            buf[n++] = (char)c;
        }
    }
}

static void logBoot() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    printf("═══════════════════════════════════════════\n");
    printf("  firmware-idf 2단계 (기록기 + GPS·IMU) · ESP-IDF %s\n", esp_get_idf_version());
    printf("  MAC %02X:%02X:%02X:%02X:%02X:%02X · 플래시 %" PRIu32 " MB · PSRAM %u 바이트\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], flash / (1024 * 1024), (unsigned)esp_psram_get_size());
    printf("═══════════════════════════════════════════\n");
}

extern "C" void app_main(void) {
    // USB 입력을 읽으려면 드라이버를 깔고, printf 도 그 드라이버로 보낸다 (usb_serial_jtag_vfs.h)
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 1024;
    ucfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&ucfg);
    usb_serial_jtag_vfs_use_driver();
    // 맥이 포트를 열어 리셋한 경우만, 맥이 다시 붙을 때까지 2초 안에서 기다린다.
    // 안 기다리면 켤 때 몇 초치 로그가 사라졌다 (1·2단계 시험). 배에서 켤 때(POWERON)는 안 기다린다.
    if (esp_reset_reason() == ESP_RST_USB) {
        const uint32_t t0 = nowMs();
        while (!usb_serial_jtag_is_connected() && nowMs() - t0 < 2000) delayMs(10);
        delayMs(100);
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) printf("[BOOT] ★ NVS 초기화 실패 %s — 지우지 않는다\n", esp_err_to_name(err));

    reportResetReason();
    {
        nvs_handle_t h;
        if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof gRecFailLine;
            if (nvs_get_str(h, "rec_fail", gRecFailLine, &len) == ESP_OK && gRecFailLine[0]) {
                hlog::noteLastFail(gRecFailLine);
                printf("[REC] 지난 기록 실패 (모두 %u번) — %s\n", (unsigned)nv::u32(h, "rec_fail_n", 0), gRecFailLine);
            } else {
                gRecFailLine[0] = '\0';
            }
            nvs_close(h);
        }
    }
    watchdogBegin();

    static const int kLeds[] = {rak::kLedGreen, rak::kLedBlue};
    for (int p : kLeds) {
        gpio_set_direction(static_cast<gpio_num_t>(p), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(p), 0);
    }

    loadSettings();
    logBoot();

    // 센서 전원부터. 없으면 GPS 가 통째로 죽어 있다 (IMU 는 늘 켜진 VDD)
    applySensorPower(gSensorPowerPin, /*cycle=*/true);

    batteryInit();
    gBattVolts = readBatteryVolts();

    nv::writeWith([](nvs_handle_t h) {
        uint32_t n = 0; nvs_get_u32(h, "boot_n", &n);
        return nvs_set_u32(h, "boot_n", n + 1) == ESP_OK;
    });
    hlog::begin();

    gps::setWaitHook(waitHook);
    gps::begin();

    if (imu::gyrNeedSave()) {
        const bool ok = imu::saveGyrOffsets();
        printf("[IMU] 저장된 자이로 0점을 새 단위(±250 원시)로 옮겼습니다%s\n",
               ok ? "" : " — ★ 다시 적기 실패 (다음 부팅에 또 옮깁니다)");
    }
    if (imu::attach("부팅")) {
        imu::calibrateGyro(/*persist=*/false);   // 이번 부팅만. 저장값과 크게 다르면 안 쓴다
    } else {
        printf("[IMU] !! 응답 없음 — 힐·9축은 무효로 보냅니다. 꽂히면 저절로 붙입니다\n");
    }

    printf("[SRC] SOG/COG 는 GPS 가 위성을 잡았을 때만 값이 있습니다 (못 잡으면 무효)\n");
    printf("      HEEL·9축은 IMU 가 붙어 있을 때만 값이 있습니다\n");

    // ★ 제일 마지막에 본다. 센서·카드가 다 올라온 뒤여야 한다.
    resumeRecordingIfCut();

    uint32_t lastImu = 0, lastImuFast = 0, lastBatt = 0, lastText = 0, lastNav = 0;
    bool ledOn = false;
    for (;;) {
        const uint32_t now = nowMs();
        pollSerial();

        // GPS 는 쉬지 않고 읽는다. UART 버퍼가 넘치면 문장 중간이 잘린다.
        gps::poll();

        // 10 ms 마다 FIFO 를 퍼 온다. 값 사이 간격은 칩이 만든 10 ms 그대로다.
        if (now - lastImuFast >= 10) { lastImuFast = now; imuDrain(); }
        // 10 Hz — 자력계까지
        if (now - lastImu >= 100) { lastImu = now; imu::update(); }

        // 이어 시작한 뒤 1분 넘게 멀쩡히 돌면 세던 것을 지운다
        if (gResumeTries && hlog::recording() && now - hlog::recStartedMs() >= kResumeOkMs) {
            gResumeTries = 0;
            setRecTry(0);
            printf("[REC] 이어 시작한 기록이 1분 넘게 멀쩡합니다 — 되풀이 세기를 지웠습니다\n");
        }

        if (now - lastBatt >= 1000) {
            lastBatt = now;
            const float freshV = readBatteryVolts();
            if (freshV > 0.0f) gBattVolts = (gBattVolts > 0.0f) ? gBattVolts * 0.8f + freshV * 0.2f : freshV;
            checkSensors();
            hlog::healthCheck();
        }
        recControlTick(now);

        // 초록 LED — 기록 중이면 1초에 80 ms
        const bool want = hlog::recording() && (now % 1000) < 80;
        if (want != ledOn) { ledOn = want; gpio_set_level(static_cast<gpio_num_t>(rak::kLedGreen), want); }

        if (hlog::recording() && now - lastText >= 10000) { lastText = now; logWriteText(now); }

        // 항법 10 Hz — 칸을 더해 나간다. 많이 밀렸으면 지금부터.
        const uint32_t navMs = 1000u / hlog::kRateNav;
        if (now - lastNav >= navMs) {
            lastNav += navMs;
            if (now - lastNav >= navMs * 5) lastNav = now;
            gps::updateFix();
            if (hlog::recording()) logWriteNav(now);
        }

        feedWatchdog();   // 여기까지 왔으면 살아 있다
        delayMs(1);
    }
}
