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

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>    // sdread 방식 3 — POSIX open/read
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"   // 드라이버 전에 남은 인터럽트 켜짐을 끈다 (app_main 첫머리)
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
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
#include "heading_tilt.h"
#include "heading_filter.h"  // OLED·BLE·기록의 상태를 가진 방위 추정기
#include "hlog.h"
#include "mag_calibration.h" // 3축 hard/soft-iron 타원체 보정
#include "rec_control.h"
#include "sdcard.h"

#include "ble.h"
#include "display.h"
#include "gps.h"
#include "imu.h"
#include "lora.h"      // firmware-rak/include/lora.h — 아두이노 흔적 없어 같이 쓴다 (몸통은 main/lora.cpp)
#include "netsrv.h"    // firmware-rak/include/netsrv.h — 같이 쓴다 (몸통은 main/netsrv.cpp)
#include "power.h"     // 7단계 — 깊은잠·저장 버튼·깬 뒤 문턱 (diagnostics.h 도 여기서)

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
static uint8_t gBoatId = 0;   // 로라 배 번호 (PROTOCOL.md §10.11). 0 = 번호 없음. 화면 B-- 에 그린다
static constexpr uint8_t kBoatIdMax = 32;
// firmware-rak 의 gBoatIdSetAt 은 적기만 하고 읽는 곳이 없어 안 옮겼다 (main.cpp:90·5194 뿐)

static void loadSettings() {
    uint8_t damp = 2;
    float deadKn = 0.10f;
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
        gSensorPowerPin = (int)nv::i32(h, "pwr_pin", rak::kSensorPowerA);
        gBoatId         = nv::u8 (h, "boat", 0);
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
//   rawMvOut: 분압 전 핀 mV 평균 (firmware-rak 과 같이 `batt` 가 찍는다). 못 읽으면 0
static float readBatteryVolts(uint32_t* rawMvOut = nullptr) {
    if (rawMvOut) *rawMvOut = 0;
    if (!gAdc || !gCali) return 0.0f;
    uint32_t sum = 0; int ok = 0;
    for (int i = 0; i < 16; ++i) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(gAdc, gCali, gBattCh, &mv) == ESP_OK) { sum += (uint32_t)mv; ++ok; }
        delayMs(2);
    }
    if (!ok) return 0.0f;
    const uint32_t pinMv = sum / (uint32_t)ok;
    if (rawMvOut) *rawMvOut = pinMv;
    return (pinMv / 1000.0f) / rak::kBattDivider * rak::kBattCorrection;
}

// 방전 곡선 표에서 잔량을 찾는다. 표 사이는 직선으로 잇는다 (firmware-rak batteryPercent 그대로)
static float batteryPercent(float volts) {
    if (volts >= rak::kBattCurve[0].volts) return 100.0f;
    const int last = rak::kBattCurveLen - 1;
    if (volts <= rak::kBattCurve[last].volts) return 0.0f;
    for (int i = 0; i < last; i++) {
        const float vHi = rak::kBattCurve[i].volts;
        const float vLo = rak::kBattCurve[i + 1].volts;
        if (volts <= vHi && volts >= vLo) {
            const float pHi = rak::kBattCurve[i].percent;
            const float pLo = rak::kBattCurve[i + 1].percent;
            const float t   = (volts - vLo) / (vHi - vLo);
            return pLo + (pHi - pLo) * t;
        }
    }
    return 0.0f;   // 여기까지 오면 표가 잘못 적힌 것이다
}
static float gBattPct = 0.0f;

// diagnostics.cpp 가 부른다 (firmware-rak 과 같은 이름 — 헤더를 서로 물지 않게 함수로 준다)
void  sailFeedWatchdog() { feedWatchdog(); }
float sailReadBatteryVolts(uint32_t* mv) { return readBatteryVolts(mv); }
float sailBatteryPercent(float volts) { return batteryPercent(volts); }
static constexpr float kBattWarnVolts = 3.0f;   // 저전압 경고 (사용자 결정, firmware-rak main.cpp:65)

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
    // 사용자가 요청한 표시 정책: 운동 가속이어도 값을 숨기지 않는다 (HLG 식 3).
    return hdg::tiltHeadingDeg(acc, v, hdgCfgNow(), false);
}

// 배의 방위 — 화면·BLE·TXT·NAV 가 읽는 단 하나. 못 구하면 -1 (평평 식으로 몰래 채우지 않는다)
static heading::Filter gHeading;
static float boatHeadingDeg() {
    return imu::ok() ? gHeading.latest(nowMs()).degrees : -1.0f;
}

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
            const auto& a = imu::acc(); const auto& g = imu::gyr(); const auto& m = imu::mag();
            const float av[3] = {a.x, a.y, a.z}, gv[3] = {g.x, g.y, g.z}, mv[3] = {m.x, m.y, m.z};
            const uint32_t received = nowMs();
            // 같은 원시값이 연속으로 들어와도 정상 8 Hz 측정이다. 마지막 값 변화가 아니라
            // 마지막 정상 읽기 시각으로 표본 나이를 계산한다. 5초 동결 검사는 magFresh가 맡는다.
            const uint32_t magAge = imu::magFresh() ? received - imu::magLastOkMs() : UINT32_MAX;
            gHeading.update(tick, received, imu::headingRevision(), hdgCfgNow(), av, gv, mv, magAge);
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
    // 자력 0.1 µT/LSB — 센서 원본. 보정값과 표시 HDG는 머리글/hdg 칸에 따로 남긴다.
    if (imu::magFresh()) {
        const imu::Vec& m = imu::magRaw();
        a.mag[0] = (int16_t)lroundf(m.x * 10.0f);
        a.mag[1] = (int16_t)lroundf(m.y * 10.0f);
        a.mag[2] = (int16_t)lroundf(m.z * 10.0f);
    }
    const float h = boatHeadingDeg();
    if (h >= 0.0f) {
        long cd = lroundf(h * 100.0f);
        if (cd >= 36000) cd = 0;
        a.hdg = (uint16_t)cd;
        if (gHeading.latest(nowMs()).caution) a.event |= heading::kLogCaution;
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

// TXT에는 실제 표시 식을 적는다. 앱이 옛 평면/기울기 식으로 추측하면 안 된다.
static void buildHeadingNote(char* out, size_t n) {
    static const char* kAx = "XYZ";
    char a[8], b[8];
    snprintf(a, sizeof a, "%c%c", gHdgSignA < 0 ? '-' : '+', kAx[gHdgAxisA < 3 ? gHdgAxisA : 0]);
    snprintf(b, sizeof b, "%c%c", gHdgSignB < 0 ? '-' : '+', kAx[gHdgAxisB < 3 ? gHdgAxisB : 0]);
    const magcal2::Calibration& mc = imu::magCalibration();
    snprintf(out, n,
             "# 방위(화면·BLE·TXT): 3D자력보정+Fusion v1.3.3 (식5), NED 100Hz gain=0.5 reject=10/10deg recovery=5s, 원형 IIR 반감기 1s. 축 atan2(%s,%s), off=%+.2f deg decl=%+.2f deg. ?=초기화·자력거절/공백/복구. COG 미사용.\n"
             "# 자력: HLG mag 는 원본. 보정 v%u 중심 %.2f %.2f %.2f uT, 자기장 %.1f 잔차 %.2f 왜곡 %.2fx 분포 %.2f\n"
             "# 보정행렬: %.5f %.5f %.5f / %.5f %.5f %.5f / %.5f %.5f %.5f\n"
             "# 가속→자력 축: 자력 X=가속 Y, Y=가속 X, Z=−가속 Z\n",
             a, b, gHdgOffsetDeg, gHdgDeclDeg, mc.version, mc.bias[0], mc.bias[1], mc.bias[2],
             mc.fieldUt, mc.rmsUt, mc.condition, mc.coverage,
             mc.matrix[0], mc.matrix[1], mc.matrix[2], mc.matrix[3], mc.matrix[4], mc.matrix[5],
             mc.matrix[6], mc.matrix[7], mc.matrix[8]);
}

static bool logStartNow(uint32_t prevSession) {
    gRecStartErr = nullptr;
    // ★ SD 는 한 번에 한 주인만. WiFi 가 켜져 있으면 파일 서버가 카드를 붙인다.
    //   기록이 WiFi 를 이긴다. 파일을 보내는 중만 아니면 WiFi 를 끄고 시작한다 (firmware-rak main.cpp 3509-3517)
    if (netsrv::mode() != netsrv::Mode::Off) {
        if (netsrv::transferring()) {
            gRecStartErr = "파일을 보내는 중입니다 — 끝나면 다시";
            return false;
        }
        printf("[REC] WiFi 가 켜져 있어 끕니다 — 기록이 먼저다\n");
        netsrv::stop();
    }
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
    h.hdgFormula = heading::kFormula;
    h.hdgAxisA = gHdgAxisA;  h.hdgAxisB = gHdgAxisB;
    h.hdgSignA = gHdgSignA < 0 ? 1 : 0;  h.hdgSignB = gHdgSignB < 0 ? 1 : 0;
    h.hdgOff   = gHdgOffsetDeg;  h.hdgDecl = gHdgDeclDeg;
    const float* mo = imu::magOffset();
    for (int i = 0; i < 3; ++i) h.magHi[i] = mo[i];
    const magcal2::Calibration& mc = imu::magCalibration();
    for (int i = 0; i < 9; ++i) h.magSi[i] = mc.matrix[i];
    h.magCalVersion = mc.version;
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
    if (power::offTick(now)) return;   // 끄기를 기다리는 중 — 닫히면 power 가 잠든다
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

// ── BLE 로 내보낼 값 (firmware-rak buildExtra · buildTelemetry, main.cpp 3834-3882) ──
// ★ 값이 없으면 없다고 보낸다. SOG·COG 는 fix 일 때만, HEEL·9축은 IMU 가 붙어 있을 때만.
static sail::TelemetryExtra buildExtra() {
    const gps::State& gs = gps::state();
    TinyGPSPlus& p = gps::parser();
    sail::TelemetryExtra e;
    e.gpsFix     = gs.fix;
    e.imuOk      = imu::ok();
    e.magOk      = imu::magFresh();
    e.recording  = hlog::recording();
    e.recFailed  = recctl::showFailed(gWantRec, hlog::phase(), gRecGaveUp, gRecLastSaveBad);
    e.satellites = p.satellites.isValid() ? (uint8_t)p.satellites.value() : 0;
    // 위성을 못 잡으면 L76K 가 25.5 같은 값을 채워 보낸다 — 정확도가 아니다
    float hdop = p.hdop.isValid() ? (float)p.hdop.hdop() : -1.0f;
    if (!gs.fix || hdop > 20.0f) hdop = -1.0f;
    e.hdop = hdop;
    e.headingDeg = boatHeadingDeg();
    e.pitchDeg   = currentPitchDeg();
    const imu::Vec& a = imu::acc();
    const imu::Vec& g = imu::gyr();
    const imu::Vec& m = imu::mag();
    e.accX = a.x; e.accY = a.y; e.accZ = a.z;
    e.gyrX = g.x; e.gyrY = g.y; e.gyrZ = g.z;
    e.magX = m.x; e.magY = m.y; e.magZ = m.z;
    e.battVolts = gBattVolts;
    return e;
}

static sail::Telemetry buildTelemetry(uint32_t ms) {
    const gps::State& gs = gps::state();
    sail::Telemetry t;
    t.moduleID = ble::moduleId();
    t.uptimeMs = ms;
    // 품질 거절로 숫자를 숨기지 않는다. 다듬은 값이 없으면 현재 RMC 원본을 표시한다.
    // 원본 자체가 없거나 오래됐으면 gs.fix가 false다.
    t.sogValid = gs.fix;
    t.cogValid = gs.fix && gps::parser().course.isValid() && gps::parser().course.age() < gps::kStaleMs;
    if (gs.fix) {
        t.sogKn  = gs.sogShownOk ? gs.sogShownKn : (float)gps::parser().speed.knots();
        t.cogDeg = (gs.cogDamped >= 0.0f) ? gs.cogDamped : (float)gps::parser().course.deg();
    }
    t.heelValid = imu::ok();
    if (imu::ok()) t.heelDeg = currentHeelDeg();
    t.battPct = gBattPct;
    return t;
}

// ── NVS float (아두이노 Preferences putFloat = 4바이트 blob) ───────────────
static bool nvPutFloat(nvs_handle_t h, const char* k, float v) { return nvs_set_blob(h, k, &v, sizeof v) == ESP_OK; }

// 기록 중에는 방위·자력·힐·피치 설정을 못 바꾼다 (firmware-rak 2777, 2026-09-15 검토 4번).
// HLG 머리글은 시작 때 설정 하나만 담는다. 도중에 바꾸면 보드 화면·워치와 앱 재생이 어긋난다.
static bool settingsLockedWhileRecording(const char* what) {
    if (hlog::phase() == recctl::Phase::Idle) return false;
    printf("[설정] %s — 기록 중에는 못 바꿉니다 (파일 머리글과 어긋남). rec off 뒤에 바꾸세요.\n", what);
    return true;
}

// ── 자력계 3축 보정 — magcal ───────────────────────────────────────────────
// hard-iron 중심을 빼고, soft-iron 타원체를 3x3 행렬로 공에 되돌린다.
// 서로 6 µT 이상 떨어진 점만 128개까지 — 가만히 있으면 안 쌓이고, 골고루 돌려야 찬다.
static constexpr int   kMagCalMax    = 128;
static constexpr float kMagCalMinGap = 6.0f;   // µT
static int16_t  gMagCalPts[kMagCalMax][3];     // 0.1 µT 단위
static int      gMagCalN  = 0;
static bool     gMagCalOn = false;
static uint32_t gMagCalSaidAt = 0;

static void magCalCollect() {
    if (!gMagCalOn || gMagCalN >= kMagCalMax) return;
    const imu::Vec& r = imu::magRaw();   // 빼기 전 원본
    for (int i = 0; i < gMagCalN; i++) {
        const float dx = r.x - gMagCalPts[i][0] * 0.1f;
        const float dy = r.y - gMagCalPts[i][1] * 0.1f;
        const float dz = r.z - gMagCalPts[i][2] * 0.1f;
        if (dx * dx + dy * dy + dz * dz < kMagCalMinGap * kMagCalMinGap) return;
    }
    gMagCalPts[gMagCalN][0] = (int16_t)lroundf(r.x * 10.0f);
    gMagCalPts[gMagCalN][1] = (int16_t)lroundf(r.y * 10.0f);
    gMagCalPts[gMagCalN][2] = (int16_t)lroundf(r.z * 10.0f);
    gMagCalN++;
}

// firmware-rak imuUpdate 는 새 자력 표본이 오면 그 자리에서 magCalCollect 를 불렀다
static void imuUpdateCollect() {
    if (imu::update()) magCalCollect();
}

static bool magCalSave() {
    const magcal2::Calibration& c = imu::magCalibration();
    return nv::writeWith([&](nvs_handle_t h) {
        return nvPutFloat(h, "mag_ox", c.bias[0]) && nvPutFloat(h, "mag_oy", c.bias[1]) &&
               nvPutFloat(h, "mag_oz", c.bias[2]) && nvPutFloat(h, "mag_r", c.fieldUt) &&
               nvPutFloat(h, "mag_res", c.rmsUt) && nvPutFloat(h, "mag_cond", c.condition) &&
               nvPutFloat(h, "mag_cov", c.coverage) &&
               nvs_set_blob(h, "mag_mtx", c.matrix, sizeof c.matrix) == ESP_OK &&
               nvs_set_u8(h, "mag_ver", c.version) == ESP_OK;
    });
}

static void magCalSpread(float out[3]) {
    for (int c = 0; c < 3; c++) {
        int16_t lo = 32767, hi = -32768;
        for (int i = 0; i < gMagCalN; i++) {
            if (gMagCalPts[i][c] < lo) lo = gMagCalPts[i][c];
            if (gMagCalPts[i][c] > hi) hi = gMagCalPts[i][c];
        }
        out[c] = gMagCalN ? (hi - lo) * 0.1f : 0.0f;
    }
}

static void magCalStatus(char* out, size_t n) {
    const magcal2::Calibration& c = imu::magCalibration();
    const float* o = c.bias;
    if (gMagCalOn) {
        float sp[3]; magCalSpread(sp);
        snprintf(out, n, "magcal on %d/%d  퍼짐 %.0f/%.0f/%.0f uT", gMagCalN, kMagCalMax, sp[0], sp[1], sp[2]);
    } else if (c.version == 2) {
        snprintf(out, n, "magcal off v2  중심 %.1f %.1f %.1f uT  자기장 %.1f  잔차 %.2f  왜곡 %.2fx",
                 o[0], o[1], o[2], c.fieldUt, c.rmsUt, c.condition);
    } else if (o[0] != 0.0f || o[1] != 0.0f || o[2] != 0.0f) {
        snprintf(out, n, "magcal off v1  치우침 %.1f %.1f %.1f uT  반지름 %.1f  남은흔들림 %.2f",
                 o[0], o[1], o[2], imu::magRadius(), imu::magResid());
    } else {
        snprintf(out, n, "magcal off  아직 안 잼 (magcal on 으로 시작)");
    }
}

// 명령 처리. 답 한 줄을 out 에 쓴다. 시리얼·BLE 공용.
// 기록 중 거절도 out 에 적는다. 시리얼에만 찍으면 BLE 쪽에는 이유가 전달되지 않는다.
static void magCalCmd(const char* arg, char* out, size_t n) {
    if (n) out[0] = '\0';
    if (!strcmp(arg, "on") || !strcmp(arg, "start")) {
        if (gMagCalOn) {
            // 이미 모으는 중이면 지우지 않는다. 두 번 눌렀다고 날리면 사람은 왜 0 인지 모른다.
            snprintf(out, n, "magcal 이미 모으는 중 %d/%d — 계속 돌리세요 (다시 시작하려면 magcal reset)", gMagCalN, kMagCalMax);
            return;
        }
        gMagCalN = 0; gMagCalOn = true; gMagCalSaidAt = 0;
        snprintf(out, n, "magcal 시작 — 사방으로 천천히 돌리세요. %d점", kMagCalMax);
        return;
    }
    if (!strcmp(arg, "reset")) {
        gMagCalN = 0; gMagCalOn = true; gMagCalSaidAt = 0;
        snprintf(out, n, "magcal 처음부터 다시 — %d점", kMagCalMax);
        return;
    }
    if (!strcmp(arg, "clear")) {
        if (settingsLockedWhileRecording("magcal clear")) {
            snprintf(out, n, "magcal 안 씀 — 기록 종료 후 다시 누르세요. 기존 보정 유지");
            return;
        }
        imu::setMagOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        const bool ok = magCalSave();
        snprintf(out, n, ok ? "magcal 지웠습니다 (치우침 0)" : "magcal 지웠지만 보드에 못 적었습니다");
        return;
    }
    if (!strcmp(arg, "stop")) {
        if (settingsLockedWhileRecording("magcal stop")) {
            snprintf(out, n, "magcal 안 씀 — 기록 종료 후 다시 저장하세요. 모은 점과 기존 보정 유지");
            return;
        }
        if (!gMagCalOn) { snprintf(out, n, "magcal 모으는 중이 아닙니다 — magcal on 부터 누르세요"); return; }
        // 너무 적으면 끄지 않는다. 꺼버리면 다시 처음부터 모아야 한다.
        if (gMagCalN < magcal2::kMinPoints) {
            snprintf(out, n, "magcal 아직 %d점뿐 — 3축 보정은 %d점부터 검사합니다. 계속 돌리세요",
                     gMagCalN, magcal2::kMinPoints);
            return;
        }
        // 후보를 따로 풀고 수량·3차원 분포·자기장·왜곡·잔차를 모두 통과해야만 쓴다.
        magcal2::Calibration fit;
        const magcal2::Verdict v = magcal2::fitAndJudge((const int16_t (*)[3])gMagCalPts, gMagCalN, &fit);
        if (v != magcal2::Verdict::Ok) {
            snprintf(out, n, "magcal 안 씀 — %s | 자기장 %.1f 잔차 %.2f 왜곡 %.2fx 분포 %.2f | 점 %d | 기존 보정 그대로%s",
                     magcal2::verdictText(v), fit.fieldUt, fit.rmsUt, fit.condition, fit.coverage, gMagCalN,
                     gMagCalN >= kMagCalMax ? " — magcal reset 뒤 사방으로 다시" : " — 계속 돌리세요");
            return;
        }
        if (!imu::setMagCalibration(fit)) {
            snprintf(out, n, "magcal 안 씀 — 계산 결과 내부 검사 실패 | 기존 보정 그대로");
            return;
        }
        gMagCalOn = false;
        const bool saved = magCalSave();
        snprintf(out, n, "magcal %s v2 — 중심 %.1f %.1f %.1f uT | 자기장 %.1f±%.2f | 왜곡 %.2fx | 분포 %.2f | 점 %d",
                 saved ? "저장" : "적용(보드에 못 적음)", fit.bias[0], fit.bias[1], fit.bias[2],
                 fit.fieldUt, fit.rmsUt, fit.condition, fit.coverage, gMagCalN);
        return;
    }
    magCalStatus(out, n);
}

// 모으는 동안 1초에 한 번 알려준다. ★ BLE 로도 보낸다 — 워치·폰이 막대로 그린다 (2026-09-10 사용자)
static void magCalTick(uint32_t ms) {
    if (!gMagCalOn) return;
    if (ms - gMagCalSaidAt < 1000) return;
    gMagCalSaidAt = ms;
    float sp[3]; magCalSpread(sp);
    char msg[120];
    snprintf(msg, sizeof msg, "magcal %d/%d  퍼짐 %.0f/%.0f/%.0f uT%s", gMagCalN, kMagCalMax, sp[0], sp[1], sp[2],
             gMagCalN >= kMagCalMax ? "  다 찼습니다" : "");
    ble::controlSay(msg);   // 시리얼과 BLE 양쪽으로
}

// ── level · calib (firmware-rak 2353-2447) ──────────────────────────────────
// 지금 자세를 평형 기준각으로 삼고 NVS 에 남긴다. 운영 설정을 바꾸는 곳은 이 함수 하나다.
static bool setLevelFromNow() {
    imuUpdateCollect();
    gHeelOffsetDeg  = rawTiltDeg(gHeelAxis, gHeelSign);
    gPitchOffsetDeg = rawTiltDeg(gPitchAxis, gPitchSign);
    return nv::writeWith([](nvs_handle_t h) {
        return nvPutFloat(h, "heel_off2", gHeelOffsetDeg) && nvPutFloat(h, "pitch_off", gPitchOffsetDeg);
    });
}

static void doLevel() {
    if (!imu::ok()) { printf("[IMU] 붙어 있지 않습니다.\n"); return; }
    const bool saved = setLevelFromNow();
    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    printf("──────────────────────────────────────────\n");
    printf("  지금 자세를 평형으로 삼았습니다.\n");
    printf("  힐   가속 %s  기준각 %+.1f°  →  지금 %+.1f°\n", hAx.text, gHeelOffsetDeg, currentHeelDeg());
    printf("  피치 가속 %s  기준각 %+.1f°  →  지금 %+.1f°\n", pAx.text, gPitchOffsetDeg, currentPitchDeg());
    printf("%s\n", saved ? "  NVS 에 저장했습니다. 다시 구워도 남습니다." : "  ★ NVS 에 못 적었습니다 — 껐다 켜면 옛 기준각");
    printf("──────────────────────────────────────────\n");
    printf("  ★ 배를 물에 띄우고 평형일 때 다시 한 번 잡으세요.\n");
    printf("    책상에서 잡은 기준은 배 위에서 맞지 않습니다.\n");
}

// 자이로 0점 다시 잡기. 자이로만 만진다 — 배가 기울어 있어도 안전하다. 멈춰 있기만 하면 된다.
static void doCalib() {
    if (!imu::ok()) { printf("[IMU] 붙어 있지 않습니다.\n"); return; }
    printf("──────────────────────────────────────────\n");
    printf("  자이로 0점을 다시 잡습니다. 보드를 움직이지 마세요.\n");
    printf("  (기울어 있어도 괜찮습니다. 멈춰 있기만 하면 됩니다)\n");
    if (imu::calibrateGyro(/*persist=*/true)) printf("  됐습니다. 이제 가만히 두면 자이로가 0 근처로 나옵니다.\n");
    else                                      printf("  다시 해보세요. 손을 떼고 보드가 멈춘 뒤에 치면 됩니다.\n");
    printf("──────────────────────────────────────────\n");
}

// ── hdgtilt — 기울기 보정이 제대로 되는지 손으로 기울여 확인 (firmware-rak 2930-3012) ──
// ★ 이 명령으로 축·부호가 맞다고 확정할 수는 없다. 확인은 아는 방위에서 hdg ref.
static void doHeadingTilt() {
    if (!imu::magOk()) { printf("[HDG] 자력계가 없습니다.\n"); return; }
    const hdg::HeadingCfg cfg = hdgCfgNow();
    const uint8_t d = (uint8_t)(3 - gHdgAxisA - gHdgAxisB);
    const char* nm[3] = {"X", "Y", "Z"};
    printf("──────────────────────────────────────────\n");
    printf("  기울기 보정 방위 확인 — 5초. 뱃머리는 두고 좌우로 기울여 보세요.\n");
    printf("  자력계 축 배정   앞 %s%s   오른쪽 %s%s   아래 %s%s\n",
           gHdgSignB < 0 ? "-" : "+", nm[gHdgAxisB], gHdgSignA > 0 ? "-" : "+", nm[gHdgAxisA],
           hdg::downSign(cfg) < 0 ? "-" : "+", nm[d < 3 ? d : 0]);
    printf("  ※ 힐은 보정에 쓰는 중력 기준입니다 (heel 축 설정이 아님).\n");
    printf("  ──────────────────────────────────────\n");
    printf("      힐     피치     평평     보정     차이    자력크기\n");
    const uint32_t t0 = nowMs();
    float flatS[32], tiltS[32];
    int flatN = 0, tiltN = 0, rows = 0;
    float magMin = 9999, magMax = -9999;
    const float yaw0 = gYawIntDeg;
    float rollMin = 999, rollMax = -999;
    while (nowMs() - t0 < 5000) {
        // ★ FIFO 를 여기서 안 퍼 오면 가속도계가 얼어붙은 채로 자력계만 움직인다 (2026-09-09)
        imuDrain();
        imuUpdateCollect();
        const float flat = flatHeadingDeg(), tilt = headingTiltDeg();
        if (flat >= 0.0f && flatN < 32) flatS[flatN++] = flat;
        if (tilt >= 0.0f && tiltN < 32) tiltS[tiltN++] = tilt;
        float roll = NAN, pitch = NAN;
        const imu::Vec& a = imu::acc();
        const float acc[3] = {a.x, a.y, a.z};
        if (hdg::gravityRollPitch(acc, cfg, &roll, &pitch)) {
            const float rd = roll * 180.0f / (float)M_PI;
            if (rd < rollMin) rollMin = rd;
            if (rd > rollMax) rollMax = rd;
        }
        const imu::Vec& m = imu::mag();
        const float mm = sqrtf(m.x * m.x + m.y * m.y + m.z * m.z);
        if (mm < magMin) magMin = mm;
        if (mm > magMax) magMax = mm;
        printf("  %+6.1f  %+6.1f   %5.1f°   %5.1f°   %+5.1f°   %5.1f\n", roll * 180.0f / (float)M_PI,
               pitch * 180.0f / (float)M_PI, flat, tilt, (flat >= 0.0f && tilt >= 0.0f) ? wrap180(tilt - flat) : NAN, mm);
        ++rows;
        delayMs(250);
        feedWatchdog();
    }
    const float flatSp = hdg::circularSpread(flatS, flatN);
    const float tiltSp = hdg::circularSpread(tiltS, tiltN);
    printf("  ──────────────────────────────────────\n");
    printf("  흔들린 폭(원형)   평평 %.1f° (%d/%d줄)   보정 %.1f° (%d/%d줄)\n", flatSp, flatN, rows, tiltSp, tiltN, rows);
    printf("  기울인 폭   %.0f°  (%.0f° 에서 %.0f° 까지)\n", rollMax - rollMin, rollMin, rollMax);
    if (rollMax - rollMin < 40.0f)       printf("  ※ 기울기가 모자랍니다. 좌우로 40° 넘게 흔들어야 비교할 수 있습니다.\n");
    else if (tiltN < rows / 2)           printf("  ※ 보정 방위가 절반 넘게 무효입니다 — 흔들 때 가속이 1 g 에서 0.15 g 넘게 벗어났습니다. 천천히.\n");
    else if (tiltSp < flatSp * 0.6f)     printf("  보정한 쪽이 덜 흔들립니다 — 기울기 영향은 줄었습니다.\n");
    else                                 printf("  ※ 보정한 쪽이 덜 흔들리지 않습니다 — 축·치우침을 확인하세요.\n");
    printf("  ★ 흔들림 폭으로는 축 부호를 확정할 수 없습니다. 아는 방위 네 곳에서 hdg ref <참 방위>.\n");
    printf("  돌아간 각   %+.1f°  (자이로 100 Hz 적분)\n", gYawIntDeg - yaw0);
    printf("  자력 크기   %.1f ~ %.1f µT   흔들림 %.0f%%\n", magMin, magMax,
           magMax > 0 ? (magMax - magMin) * 100.0f / magMax : 0.0f);
    printf("%s\n", ((magMax - magMin) > magMax * 0.15f) ? "  ★ 크기가 15% 넘게 변합니다 — 하드아이언 보정이 먼저입니다."
                                                        : "  자력 크기는 거의 일정합니다.");
    printf("  ※ 한국의 지구 자기장은 약 50 µT 입니다. 크게 벗어나면 주변 쇠붙이\n");
    printf("    때문입니다. 책상·노트북에서 떨어진 데서 다시 해보세요.\n");
    printf("──────────────────────────────────────────\n");
}

// ── scan — I2C 두 버스 훑기 (firmware-rak 548-641) ─────────────────────────
// I2C1 은 IMU·화면이 쓰는 버스를 그대로 두드린다 (i2c_master_probe). firmware-rak 은 Wire.begin(…, 100 kHz) 로 다시 열었다.
// I2C2(17/18) 는 이 판에서 아무도 안 쓰니 잠깐 만들고 지운다.
static bool gSawDisplay = false;
static bool gSawImu     = false;

static const char* guessI2CDevice(uint8_t addr) {
    switch (addr) {
        case 0x3C: return "★ SSD1306 화면 — RAK1921 (J12 헤더)";
        case 0x68: return "★ MPU-9250 IMU — RAK1905";
        case 0x0C: return "★ AK8963 자력계 — RAK1905 안에 들어있음";
        case 0x3D: return "SSD1306 화면 (주소 점퍼가 반대쪽)";
        case 0x69: return "MPU-9250 (AD0 가 HIGH) 또는 다른 IMU";
        case 0x18:
        case 0x19: return "LIS3DH 가속도 (RAK1904)";
        case 0x1D:
        case 0x53: return "ADXL 계열 가속도";
        case 0x0D: return "자력계 (BMM150 등)";
        case 0x76:
        case 0x77: return "BME280/BMP280/BME680 환경센서";
        case 0x42: return "u-blox GNSS (RAK12500)";
        case 0x51:
        case 0x52: return "RTC";
        case 0x28:
        case 0x29: return "BNO055 자세센서 또는 거리센서";
        default:   return "";
    }
}

static int scanBus(i2c_master_bus_handle_t bus, const char* label, int sda, int scl) {
    printf("  %s (SDA GPIO%d / SCL GPIO%d)\n", label, sda, scl);
    if (!bus) { printf("    (버스를 못 열었습니다)\n"); return 0; }
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            const char* guess = guessI2CDevice(addr);
            printf("    0x%02X  %s\n", addr, guess[0] ? guess : "(알 수 없음)");
            if (addr == rak::kAddrDisplay) gSawDisplay = true;
            if (addr == rak::kAddrImu)     gSawImu     = true;
            found++;
        }
        feedWatchdog();
    }
    if (found == 0) printf("    (응답 없음)\n");
    return found;
}

static void doScan() {
    gSawDisplay = false; gSawImu = false;
    printf("──────────────────────────────────────────\n");
    printf("  I2C 스캔 — 센서 전원 %s\n", gSensorPowerPin ? "ON" : "OFF (power 명령으로 켜세요)");
    if (!imu::bus()) imu::begin();
    const int a = scanBus(imu::bus(), "I2C1 — 센서 슬롯 A~D + J12 헤더", rak::kI2C1_SDA, rak::kI2C1_SCL);
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = -1;                      // 빈 포트를 알아서 고른다
    cfg.sda_io_num = static_cast<gpio_num_t>(rak::kI2C2_SDA);
    cfg.scl_io_num = static_cast<gpio_num_t>(rak::kI2C2_SCL);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus2 = nullptr;
    if (i2c_new_master_bus(&cfg, &bus2) != ESP_OK) bus2 = nullptr;
    const int b = scanBus(bus2, "I2C2 — 코어 커넥터에서 끝나는 버스", rak::kI2C2_SDA, rak::kI2C2_SCL);
    if (bus2) i2c_del_master_bus(bus2);

    printf("──────────────────────────────────────────\n");
    printf("  화면 RAK1921 (0x%02X)  %s\n", rak::kAddrDisplay, gSawDisplay ? "보임" : "안 보임");
    printf("  IMU  RAK1905 (0x%02X)  %s\n", rak::kAddrImu, gSawImu ? "보임" : "안 보임");
    printf("  GPS(UART)와 SD(SPI)는 I2C 가 아니라 여기 안 나옵니다.\n");
    printf("──────────────────────────────────────────\n");
    printf("  ※ IMU 는 항상 켜져 있는 VDD 를 쓴다. 전원 스위치와 무관하다.\n");
    printf("    전원 핀 판정은 scan 이 아니라 gps 명령으로 한다.\n");
    printf("──────────────────────────────────────────\n");
    if (a + b == 0) {
        printf("  아무것도 안 잡혔습니다. 모듈이 덜 꽂혔는지 보세요.\n");
        printf("  (IMU 조차 안 보이면 I2C 배선 자체를 의심할 상황입니다)\n");
    } else if (!gSawDisplay) {
        printf("  화면이 안 보입니다. J12 헤더에 꽂혀 있는지 보세요.\n");
        printf("  (센서 슬롯이 아니라 2.54mm I2C 핀헤더입니다)\n");
    }
    if (gSawDisplay && sail::displayBegin()) printf("  화면을 붙였습니다 — 값이 바로 나옵니다.\n");
}

// ── help (firmware-rak printHelp 4239-4306 글자 그대로) ─────────────────────
static void printHelp() {
    printf("──────────────────────────────────────────\n");
    printf("  name <이름>   보드 이름 설정 (최대 11자, 영숫자/-/_)\n");
    printf("                예) name hojun  →  SAIL-hojun\n");
    printf("  hz <1~100>    notify 주기 설정. 예) hz 20  (기본 10)\n");
    printf("  boat <0~32>   로라 배 번호. 0 은 번호 없음. 예) boat 7\n");
    printf("  lora          로라 상태 (주파수·전파시간·받은 개수)\n");
    printf("  lora on       로라 켜기\n");
    printf("  lora regs     칩 버그 세 개가 실제로 걸렸는지 레지스터로 확인\n");
    printf("  lora rssi     이 주파수의 바닥 잡음. 보드 한 대로 하는 확인\n");
    printf("  lora tx       시험 삼아 하나 보내기\n");
    printf("  lora watch    받을 때마다 한 줄씩 뱉기 (두 대로 시험할 때)\n");
    printf("  info          현재 설정 출력\n");
    printf("\n");
    printf("  ── 보드 진단 ──\n");
    printf("  power %-2d      센서 전원을 GPIO%d 로 (datasheet 쪽)\n", rak::kSensorPowerA, rak::kSensorPowerA);
    printf("  power %-2d      센서 전원을 GPIO%d 로 (pins_arduino.h 쪽)\n", rak::kSensorPowerB, rak::kSensorPowerB);
    printf("  power off     센서 전원 끄기\n");
    printf("  check         ★ 아래를 한 번에 — 처음엔 이것부터\n");
    printf("  fix           GPS 파싱 상태 (위성 수, 위치, 속도, 침로)\n");
    printf("  imu           9축 값 5초 출력 — 기울여 보세요\n");
    printf("  scan          I2C — 화면 RAK1921(0x3C) / IMU RAK1905(0x68)\n");
    printf("  oledw         화면에 쓸 한글 줄의 폭을 잰다 (128px 안에 드나)\n");
    printf("  hdgtilt       기울기 보정 방위 확인 — 손으로 기울이며 5초 본다\n");
    printf("  magcal        자력계 치우침 재기. on / stop / clear (BLE 로도 됨)\n");
    printf("  off           보드를 끈다 (깊은잠). 버튼 5초 누르면 켜진다\n");
    printf("  sleepstat     지난번에 정말 잤나 — 헛깬 횟수·잔 시간·3V3_S\n");
    printf("  sd            SD카드 마운트 + 쓰기 시험\n");
    printf("  sdbench [줄수] SD 쓰기 속도·최대 멈춤 실측 (기본 3600줄)\n");
    printf("  rec           ★ 기록 상태. rec on / rec off / rec mark\n");
    printf("  rec ls / rec check [번호]   파일 목록 / 되읽어 검사\n");
    printf("  rec tail [번호] [줄수]      TXT 사본 끝줄 (전압·멈춤·버퍼)\n");
    printf("  rec rm <번호>               그 세션 파일을 지운다 (못 되돌린다)\n");
    printf("  pin <번호>    그 GPIO 를 5초 지켜본다 (버튼 달 자리 찾기)\n");
    printf("  wifi          ★ 기록 파일을 WiFi 로 내보내기. wifi ap / join / off\n");
    printf("  oled          화면을 나중에 꽂았을 때 다시 붙이기\n");
    printf("  gps           UART1 원시 NMEA 5초 (GPS 가 슬롯 A)\n");
    printf("  gps d         같은 것 (GPS 가 슬롯 D — IO6 로 리셋 해제)\n");
    printf("  gpshz <1|2|5|10> GPS 갱신율 (기본 5). 실제로 걸렸는지 세어 줍니다\n");
    printf("  gpscfg        모듈에 실제로 걸린 설정을 되물어봅니다\n");
    printf("  smooth <0~5>  속도·침로 다듬기 세기 (0 원본, 기본 2)\n");
    printf("  dead <kn>     잡음 바닥. 이보다 작은 속도는 0 (기본 0.10)\n");
    printf("  navpv         NMEA 거치기 전 속도를 RMC 와 나란히 (navpv l 은 길게)\n");
    printf("  gpscfg mode <0~7>  움직임 종류 (0휴대 1정지 2보행 3자동차 4선박)\n");
    printf("  gpscfg static <m/s> 정지로 볼 속도 문턱값\n");
    printf("  nmea <본문>   NMEA 명령을 보내고 응답을 봅니다 (체크섬 자동)\n");
    printf("  batt          배터리 전압 실측\n");
    printf("  tz [분]       파일 이름에 쓸 시각 기울기 (기본 540 = 한국)\n");
    printf("  sess [번호]   세션 번호 보기·고치기 (NVS 에 남는다)\n");
    printf("  usbbench [KB] USB 시리얼 속도 실측 (기본 512 KB)\n");
    printf("  level         ★ 지금 자세를 힐·피치 0° 로 삼기 (배가 평형일 때)\n");
    printf("  heel [x|y|z]  힐을 어느 가속도 축에서 볼지 (앞에 - 로 뒤집기)\n");
    printf("  hdg <A> <B>   방위를 만들 자력계 두 축. 예) hdg -z x\n");
    printf("  hdg off <도>  방위 0점 보정 (자기 편각 + 보드 어긋남)\n");
    printf("  pitch [x|y|z] 피치를 어느 가속도 축에서 볼지\n");
    printf("  calib         자이로 0점 다시 잡기 (기울어 있어도 OK)\n");
    printf("  status        한 줄 상태 / loopstat  루프가 어디에 시간을 쓰나\n");
    printf("  battboot      켠 뒤 1초마다 담은 배터리 값\n");
    printf("  wifi ssid/pass <값>   망 이름·비밀번호를 NVS 에 넣는다\n");
    printf("  wifi scan / status / idle <초> / off\n");
    printf("  help          이 도움말  (전체 목록은 저장소 COMMANDS.md)\n");
    printf("──────────────────────────────────────────\n");
    printf("  붙어 있는 것:  GPS 슬롯A · IMU 슬롯C · 화면 J12 · SD IO슬롯\n");
    printf("  값이 없으면 --- 로 나옵니다. 지어낸 값(시뮬레이터)은 없습니다.\n");
    printf("──────────────────────────────────────────\n");
}

// ── 한 바퀴에 얼마나 걸리나 — loopstat (firmware-rak 3676-3702) ─────────────
static inline uint32_t microsNow() { return (uint32_t)esp_timer_get_time(); }
static bool     gLoopStat = false;
static uint32_t gLoopCount = 0;
static uint32_t gLoopMaxUs = 0;
struct SecStat { uint32_t maxUs = 0; uint32_t sumUs = 0; };
static SecStat gStGps, gStImu, gStNotify, gStDraw, gStLog;
static inline void secDone(SecStat& st, uint32_t us) {
    if (us > st.maxUs) st.maxUs = us;
    st.sumUs += us;
}
static void loopStatPrint(uint32_t elapsedMs) {
    if (!gLoopStat) return;
    const float sec = elapsedMs / 1000.0f;
    printf("   루프  %.0f바퀴/초  한 바퀴 최대 %.1fms\n", gLoopCount / sec, gLoopMaxUs / 1000.0f);
    printf("   ├ GPS읽기 최대 %5.1fms  합 %5.1fms/초\n", gStGps.maxUs / 1000.0f, gStGps.sumUs / 1000.0f / sec);
    printf("   ├ IMU읽기 최대 %5.1fms  합 %5.1fms/초\n", gStImu.maxUs / 1000.0f, gStImu.sumUs / 1000.0f / sec);
    printf("   ├ notify  최대 %5.1fms  합 %5.1fms/초\n", gStNotify.maxUs / 1000.0f, gStNotify.sumUs / 1000.0f / sec);
    printf("   ├ 화면    최대 %5.1fms  합 %5.1fms/초\n", gStDraw.maxUs / 1000.0f, gStDraw.sumUs / 1000.0f / sec);
    printf("   └ 기록    최대 %5.1fms  합 %5.1fms/초\n", gStLog.maxUs / 1000.0f, gStLog.sumUs / 1000.0f / sec);
    gLoopCount = 0; gLoopMaxUs = 0;
    gStGps = gStImu = gStNotify = gStDraw = gStLog = SecStat();
}

// ── BLE 제어 특성으로 들어온 줄 (firmware-rak controlLine, main.cpp 4015-4150) ──
// ★ 콜백이 아니라 루프가 부른다. ble.cpp 콜백은 줄을 베껴 두기만 한다.
// 아직 없는 것: magcal — firmware-rak 이 모르는 줄에 하던 대로 "err unknown" 으로 답한다.
// WiFi 켜고 끄기는 여기서 표식만 세우고 루프가 한다 — 답이 폰에 닿은 뒤 BLE 가 내려간다.
static volatile uint8_t gWifiWant = 0;   // 0 없음, 1 join, 2 ap, 3 off

// 앞에서 n 글자 뒤의 나머지를 앞뒤 빈칸 없이 out 에 (Arduino substring + trim)
static void restTrim(const char* line, size_t from, char* out, size_t cap) {
    const char* s = line + (strlen(line) > from ? from : strlen(line));
    while (*s == ' ' || *s == '\t') ++s;
    snprintf(out, cap, "%s", s);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r')) out[--n] = '\0';
}

static void controlLine(const char* raw) {
    while (*raw == ' ' || *raw == '\t') ++raw;
    char line[192];
    snprintf(line, sizeof line, "%s", raw);
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) line[--n] = '\0';
    if (!n) return;
    // WiFi 비밀번호는 로그에 안 남긴다.
    if (!strncmp(line, "wifi pass ", 10)) printf("[CTL] ← wifi pass ****\n");
    else printf("[CTL] ← %s\n", line);

    char out[240];
    char v[192];

    // wifi ssid <이름>
    if (!strncmp(line, "wifi ssid ", 10)) {
        restTrim(line, 10, v, sizeof v);
        netsrv::setCreds(v, nullptr);
        snprintf(out, sizeof out, "ok wifi ssid %s", v);
        ble::controlSay(out);
        return;
    }
    // wifi pass <비밀번호> — 뒤 공백도 비밀번호일 수 있다 (firmware-rak 은 substring 만, trim 안 함)
    if (!strncmp(line, "wifi pass ", 10)) {
        // ★ line 은 위에서 뒤 빈칸을 지웠다. firmware-rak 은 controlLine 첫 줄에서 trim 했으므로 같다
        netsrv::setCreds(netsrv::staSsid(), line + 10);
        // ★ 비밀번호는 되읽어 주지 않는다. 길이만 알린다.
        snprintf(out, sizeof out, "ok wifi pass %u자", (unsigned)strlen(line + 10));
        ble::controlSay(out);
        return;
    }
    // wifi scan — BLE 를 안 내리고 할 수 있다
    if (!strcmp(line, "wifi scan")) {
        // ★ 기록 중에는 안 한다 (09-15 외부 검토 R3). 스캔은 끝날 때까지 기다리는 방식(block)이라 채널마다
        //   100~300 ms 씩 루프가 멈추고, 그동안 IMU FIFO(38벌 넘으면 비움)·NAV 줄·버튼이 밀린다. firmware-rak 은 막지 않았다.
        if (hlog::busy()) { ble::controlSay("err wifi recording"); return; }
        static netsrv::ScanEntry list[20];   // 스택에 KB 를 안 올린다 (memory: esp32-stack-and-leak-rules)
        const int n2 = netsrv::scan(list, 20);
        snprintf(out, sizeof out, "scan begin %d", n2);
        ble::controlSay(out);
        for (int i = 0; i < n2; i++) {
            snprintf(out, sizeof out, "scan %d %d %s %s", i, (int)list[i].rssi, list[i].locked ? "lock" : "open", list[i].ssid);
            ble::controlSay(out);
        }
        ble::controlSay("scan end");
        return;
    }
    // wifi on / wifi join [앱번호] — 답을 먼저 보내고 켠다
    if (!strcmp(line, "wifi on") || !strcmp(line, "wifi join") ||
        !strncmp(line, "wifi on ", 8) || !strncmp(line, "wifi join ", 10)) {
        if (hlog::busy()) { ble::controlSay("err wifi recording"); return; }
        char appId[64] = "";
        {
            const char* sp = strchr(line + 5, ' ');   // "wifi on" 의 두 번째 빈칸
            if (sp) restTrim(sp, 1, appId, sizeof appId);
        }
        // 이미 붙어 있으면 다시 붙지 않는다. 받고 있는 다른 기기가 끊긴다.
        if (netsrv::mode() == netsrv::Mode::Join) {
            const int others = netsrv::othersThan(appId);
            const char* oip = netsrv::otherIpText(appId);
            snprintf(out, sizeof out, "ok wifi joining %s mdns %s.local last %s users %d by %s",
                     netsrv::staSsid(), netsrv::mdnsHost(), netsrv::ipText(), others, (others && *oip) ? oip : "-");
            ble::controlSay(out);
            return;
        }
        const char* ss = netsrv::staSsid();
        if (!ss || !*ss) { ble::controlSay("err wifi no-ssid"); return; }
        const char* last = netsrv::lastIp();
        snprintf(out, sizeof out, "ok wifi joining %s mdns %s.local last %s users 0 by -",
                 ss, netsrv::mdnsHost(), (last && *last) ? last : "-");
        ble::controlSay(out);
        gWifiWant = 1;
        return;
    }
    if (!strcmp(line, "wifi ap") || !strncmp(line, "wifi ap ", 8)) {
        if (hlog::busy()) { ble::controlSay("err wifi recording"); return; }
        char apId[64] = "";
        restTrim(line, 8, apId, sizeof apId);
        const bool apUp = netsrv::mode() == netsrv::Mode::AP;
        const int others = apUp ? netsrv::othersThan(apId) : 0;
        const char* oip = netsrv::otherIpText(apId);
        snprintf(out, sizeof out, "ok wifi ap %s pass %s ip 192.168.4.1 users %d by %s",
                 ble::fullName(), netsrv::apPass(), others, (others && *oip) ? oip : "-");
        ble::controlSay(out);
        // 이미 열어 뒀으면 다시 열지 않는다. 붙어 있던 기기가 떨어진다.
        if (netsrv::mode() != netsrv::Mode::AP) gWifiWant = 2;
        return;
    }
    if (!strcmp(line, "wifi off")) { ble::controlSay("ok wifi off"); gWifiWant = 3; return; }
    // wifi idle <초> — 아무도 안 쓰면 저절로 끄기까지의 시간. 0 이면 안 끈다
    if (!strncmp(line, "wifi idle ", 10)) {
        const uint32_t sec = (uint32_t)strtol(line + 10, nullptr, 10);
        netsrv::setIdleOff(sec);
        snprintf(out, sizeof out, "ok wifi idle %lu", (unsigned long)sec);
        ble::controlSay(out);
        return;
    }
    if (!strcmp(line, "wifi status") || !strcmp(line, "status")) {
        snprintf(out, sizeof out, "status name %s mode %s ip %s rec %s idle %lus left %lus",
                 ble::fullName(),
                 netsrv::mode() == netsrv::Mode::Off ? "off" : netsrv::mode() == netsrv::Mode::AP ? "ap" : "join",
                 netsrv::ipText(), hlog::recording() ? "on" : "off",
                 (unsigned long)netsrv::idleOffSec(), (unsigned long)(netsrv::idleLeftMs() / 1000));
        ble::controlSay(out);
        return;
    }
    // 자력계 치우침. **시리얼과 같은 함수를 부른다** — 말이 어긋날 수가 없다.
    if (!strcmp(line, "magcal") || !strncmp(line, "magcal ", 7)) {
        char msg[200];
        magCalCmd(strlen(line) > 7 ? line + 7 : "", msg, sizeof msg);
        if (msg[0]) ble::controlSay(msg);
        return;
    }
    if (!strcmp(line, "help")) {
        ble::controlSay("cmds: wifi ssid|pass|scan|on|ap|off|status | magcal on|stop|clear");
        return;
    }
    snprintf(out, sizeof out, "err unknown %s", line);
    ble::controlSay(out);
}

// netsrv.cpp 가 부른다 — WiFi 로 파일을 보내는 동안만 BLE 를 내린다 (firmware-rak main.cpp 5226-5303 의 뜻)
const char* sailFullName() { return ble::fullName(); }
void sailBleStart() { ble::start(buildTelemetry(nowMs()), buildExtra()); }
void sailBleStop()  { ble::stop(); }

static void printIdentity() {
    char mac[20];
    ble::formatMac(mac, sizeof mac);
    printf("[ID ] 이름 %s | module_id %u (0x%02X) | MAC %s\n", ble::fullName(), ble::moduleId(), ble::moduleId(), mac);
    printf("[ID ] notify %.1f Hz (%ums) | adv %.1f Hz\n", 1000.0f / ble::notifyPeriodMs(),
           (unsigned)ble::notifyPeriodMs(), 1000.0f / sail::kAdvRefreshMs);
    if (gSensorPowerPin) printf("[PWR] 센서 전원 GPIO%d (ON)\n", gSensorPowerPin);
    else                 printf("[PWR] 센서 전원 꺼짐\n");
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
    const auto estimate = gHeading.latest(nowMs());
    if (estimate.degrees >= 0) printf(" | 표시 HDG %.1f%s", estimate.degrees, estimate.caution ? " ?" : "");
    else printf(" | 표시 HDG ---");
    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    printf(" | 힐 %+6.1f° (가속 %s)  피치 %+6.1f° (가속 %s)\n",
           currentHeelDeg(), hAx.text, currentPitchDeg(), pAx.text);
    if (imu::magFresh()) {
        const float flat = flatHeadingDeg(), tilt = headingTiltDeg();
        if (tilt >= 0.0f)
            printf("   방위 비교  평평 %5.1f°  |  기울기보정 %5.1f°  |  차이 %+5.1f°\n", flat, tilt, wrap180(tilt - flat));
        else
            printf("   방위 비교  평평 %5.1f°  |  기울기보정 ---  (유효한 센서 입력 없음)\n", flat);
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
        imuUpdateCollect();
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
        hlog::testFailFlush(0);   // 남은 흉내 횟수까지 지운다 (CLAUDE.md "지우기 명령은 남은 흉내 상태까지")
        printf("[REC] 멈춤 기록을 지웠습니다\n");
        return;
    }
    // 시험: 다음 n 번의 카드 반영(flush)을 실패로 흉내 (R1 — 저장 마무리 실패가 정상 종료로 보이지 않나)
    if (!strncmp(arg, "failflush ", 10)) {
        long n = strtol(arg + 10, nullptr, 10);
        hlog::testFailFlush((uint8_t)(n < 0 ? 0 : (n > 200 ? 200 : n)));
        printf("[REC] 시험 — 다음 %ld번 카드 반영(flush)을 실패로 흉내 냅니다\n", n);
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

// ── 7단계 power 가 부를 메인 쪽 함수 (power.h Hooks) ────────────────────────
// 센서 전원 핀이 바뀌면(power 명령) 다시 부른다.
static void installPowerHooks() {
    power::Hooks ph;
    ph.readBatteryVolts = []() { return readBatteryVolts(); };
    ph.feedWatchdog     = feedWatchdog;
    ph.pollRecResult    = []() { recctl::SessionResult r; if (hlog::poll(&r)) recOnResult(r, nowMs()); };
    ph.recWantOff       = recWantOff;
    ph.recWantOn        = recWantOn;
    ph.dropWantIfSet    = []() {
        if (gWantRec || gRecGaveUp) { gWantRec = false; gRecGaveUp = false; gRecRestartAt = 0; recSaveWant(false); }
    };
    ph.clearWantFlag    = []() { gWantRec = false; };
    ph.recStartErrShort = []() -> const char* { hlog::Status st; hlog::getStatus(&st); return st.lastErrorShort; };
    ph.sensorPowerPin   = gSensorPowerPin;
    power::setHooks(ph);
}

// ── 설정·진단 명령 몸통 (firmware-rak handleCommand 에서 그대로) ────────────

// hdg — 방위를 만드는 두 축 · 장착 오프셋 · 편각 · 아는 방위와 대조 (firmware-rak 4783-4888)
static bool parseAxisTok(const char* t, uint8_t* axis, float* sign) {
    *sign = 1.0f;
    if (*t == '-') { *sign = -1.0f; ++t; }
    else if (*t == '+') { ++t; }
    if (!strcmp(t, "x")) { *axis = 0; return true; }
    if (!strcmp(t, "y")) { *axis = 1; return true; }
    if (!strcmp(t, "z")) { *axis = 2; return true; }
    return false;
}

static void cmdHdg(const char* rest) {
    char arg[64];
    restTrim(rest, 0, arg, sizeof arg);
    for (char* p = arg; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

    if (!strncmp(arg, "off", 3) || !strncmp(arg, "decl", 4)) {
        // ★ 숫자만 받고 범위를 본다. 옛 코드는 1e10 을 저장해 방위 정규화가 끝나지 않아 루프가 멎었다.
        const bool isDecl = !strncmp(arg, "decl", 4);
        char v[32];
        restTrim(arg, isDecl ? 4 : 3, v, sizeof v);
        float deg = 0.0f;
        const float lim = isDecl ? 30.0f : 360.0f;
        if (!hdg::parseNumber(v, &deg) || deg < -lim || deg > lim) {
            if (isDecl) printf("  hdg decl <도>   자기 편각, 동편 +·서편 − (-30~30). 예) hdg decl -8.5\n");
            else        printf("  hdg off <도>    장착 오프셋 (-360~360). 예) hdg off 12.5\n");
            return;
        }
        if (settingsLockedWhileRecording(isDecl ? "hdg decl" : "hdg off")) return;
        if (!isDecl) deg = hdg::wrap180(deg);
        (isDecl ? gHdgDeclDeg : gHdgOffsetDeg) = deg;
        gHeading.reset();
        const bool ok = nv::writeWith([&](nvs_handle_t h) { return nvPutFloat(h, isDecl ? "hdg_decl" : "hdg_off", deg); });
        printf("[IMU] %s %+.2f°%s\n", isDecl ? "자기 편각" : "장착 오프셋", deg, ok ? "" : " — ★ 보드에 못 적었습니다");
    } else if (!strncmp(arg, "ref", 3)) {
        // 아는 방위(참 방위)에 대고 두 방위의 오차를 본다. 네 방향에서 해야 뜻이 있다.
        char v[32];
        restTrim(arg, 3, v, sizeof v);
        float ref = 0.0f;
        if (!hdg::parseNumber(v, &ref) || ref < 0.0f || ref >= 360.0f) {
            printf("  hdg ref <참 방위 0~359>   예) 부두 방향이 045° 면 hdg ref 45\n");
            return;
        }
        imuDrain();
        imuUpdateCollect();
        const float flat = flatHeadingDeg(), tilt = headingTiltDeg();
        printf("──────────────────────────────────────────\n");
        if (flat >= 0.0f) printf("  기준 %.1f°   평평 %.1f° (오차 %+.1f°)\n", ref, flat, wrap180(flat - ref));
        else              printf("  기준 %.1f°   평평 --- (자력 새 표본 없음)\n", ref);
        if (tilt >= 0.0f) printf("               순간 기울기 보정 %.1f° (오차 %+.1f°) · 진단용\n", tilt, wrap180(tilt - ref));
        else              printf("               보정 --- (유효한 센서 입력 없음)\n");
        const float fused = boatHeadingDeg();
        if (fused >= 0.0f) printf("               센서 융합 %.1f° (오차 %+.1f°)%s ← 화면·BLE·기록\n", fused, wrap180(fused-ref), gHeading.latest(nowMs()).caution ? " ?" : "");
        else printf("               센서 융합 --- (유효한 센서 입력 없음)\n");
        printf("  수평으로 두고 0°·90°·180°·270° 네 방향에서 해 보세요.\n");
        printf("    오차가 네 방향 모두 비슷하다   → 장착 기준·자북/진북 기준부터 확인한다\n");
        printf("    방향마다 다르다 (특히 부호가 바뀐다) → 축·부호·자력 치우침 문제다\n");
        printf("──────────────────────────────────────────\n");
        return;
    } else if (arg[0]) {
        char* sp = strchr(arg, ' ');
        if (!sp || sp == arg) {
            printf("  hdg <A> <B>   예) hdg -z x   (앞에 - 를 붙이면 뒤집기)\n");
            printf("  hdg off <도>  0점 보정\n");
            return;
        }
        *sp = '\0';
        char ta[16], tb[16];
        restTrim(arg, 0, ta, sizeof ta);
        restTrim(sp + 1, 0, tb, sizeof tb);
        uint8_t a = 0, b = 0; float sa = 1.0f, sb = 1.0f;
        if (!parseAxisTok(ta, &a, &sa) || !parseAxisTok(tb, &b, &sb)) {
            printf("  축은 x y z 중에서 고르세요. 예) hdg -z x\n");
            return;
        }
        if (a == b) { printf("  두 축이 같으면 방위가 안 나옵니다. 서로 다른 축이어야 합니다.\n"); return; }
        if (settingsLockedWhileRecording("hdg 축")) return;
        gHdgAxisA = a; gHdgAxisB = b; gHdgSignA = sa; gHdgSignB = sb;
        gHeading.reset();
        const bool ok = nv::writeWith([&](nvs_handle_t h) {
            return nvs_set_u8(h, "hdg_a", a) == ESP_OK && nvs_set_u8(h, "hdg_b", b) == ESP_OK &&
                   nvs_set_i8(h, "hdg_sa", sa < 0 ? -1 : 1) == ESP_OK && nvs_set_i8(h, "hdg_sb", sb < 0 ? -1 : 1) == ESP_OK;
        });
        if (!ok) printf("  ★ 보드에 못 적었습니다 — 껐다 켜면 옛 축\n");
    }

    imuUpdateCollect();
    imuDrain();  // 설정 변경 뒤 진단도 새 추정 상태를 읽는다
    const AxisName aAx(gHdgAxisA, gHdgSignA), bAx(gHdgAxisB, gHdgSignB);
    const imu::Vec& m = imu::mag();
    printf("──────────────────────────────────────────\n");
    printf("  지금 자력  %+.1f %+.1f %+.1f µT\n", m.x, m.y, m.z);
    const float hNow = flatHeadingDeg(), hBoat = boatHeadingDeg();
    const auto estimate = gHeading.latest(nowMs());
    printf("  방위 식 5 3D자력보정+Fusion 1.3.3 · 가속 거절 %d (%.1f°) · 자력 거절 %d (%.1f°) · 복구 %d\n",
           estimate.accelIgnored, estimate.accelError,
           estimate.magIgnored, estimate.magError, estimate.recovering);
    if (hNow >= 0.0f)
        printf("  평평  atan2(자력 %s, 자력 %s) + 오프셋 %+.1f° + 편각 %+.1f°  →  %.1f°  (진단용)\n",
               aAx.text, bAx.text, gHdgOffsetDeg, gHdgDeclDeg, hNow);
    else
        printf("  평평  atan2(자력 %s, 자력 %s) + 오프셋 %+.1f° + 편각 %+.1f°  →  --- (자력 새 표본 없음)\n",
               aAx.text, bAx.text, gHdgOffsetDeg, gHdgDeclDeg);
    if (hBoat >= 0.0f) printf("  방위  센서 융합 (화면·BLE·기록)  →  %.1f°\n", hBoat);
    else               printf("  방위  센서 융합 (화면·BLE·기록)  →  --- (유효한 센서 입력 없음)\n");
    char ago[40];
    const uint32_t lc = imu::magLastChangeMs();
    if (lc) snprintf(ago, sizeof ago, "%lu ms 전 바뀜", (unsigned long)(nowMs() - lc));
    else    snprintf(ago, sizeof ago, "없음");
    printf("  자력 표본  새 %lu · 반복 %lu · 짧음 %lu · 넘침 %lu · 0벡터 %lu · 마지막 새 표본 %s\n",
           (unsigned long)imu::magCount(0), (unsigned long)imu::magCount(1), (unsigned long)imu::magCount(2),
           (unsigned long)imu::magCount(3), (unsigned long)imu::magCount(4), ago);
    printf("  공장 감도값 ASA %02X %02X %02X  →  계수 %.4f %.4f %.4f\n",
           imu::asaRaw(0), imu::asaRaw(1), imu::asaRaw(2), imu::asa(0), imu::asa(1), imu::asa(2));
    printf("──────────────────────────────────────────\n");
    printf("  케이스를 평평하게 두고 제자리에서 한 바퀴 돌려 보세요.\n");
    printf("  수평인 두 축은 크게 오르내리고, 위아래 축은 거의 그대로입니다.\n");
}

// heel · pitch — 힐·피치를 어느 가속도 축에서 볼지 (firmware-rak 4890-4946)
static void cmdHeelPitch(const char* line) {
    const bool isHeel = !strncmp(line, "heel", 4);
    // 값만 보는 heel · pitch 는 된다. 축·부호를 바꾸는 heel -y 같은 것만 막는다.
    if ((!strncmp(line, "heel ", 5) || !strncmp(line, "pitch ", 6)) &&
        settingsLockedWhileRecording(isHeel ? "heel 축" : "pitch 축")) return;
    const char* what = isHeel ? "힐" : "피치";
    uint8_t& axisRef = isHeel ? gHeelAxis : gPitchAxis;
    float&   signRef = isHeel ? gHeelSign : gPitchSign;
    float&   offRef  = isHeel ? gHeelOffsetDeg : gPitchOffsetDeg;

    char arg[16];
    restTrim(line, isHeel ? 4 : 5, arg, sizeof arg);
    if (arg[0]) {
        float sign = 1.0f;
        char* t = arg;
        if (*t == '-') { sign = -1.0f; ++t; }
        else if (*t == '+') { ++t; }
        for (char* p = t; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        int axis = -1;
        if (!strcmp(t, "x")) axis = 0;
        else if (!strcmp(t, "y")) axis = 1;
        else if (!strcmp(t, "z")) axis = 2;
        if (axis < 0) {
            const char* w = isHeel ? "heel" : "pitch";
            printf("[IMU] %s x | %s y | %s z (앞에 - 를 붙이면 뒤집기)\n", w, w, w);
            return;
        }
        axisRef = (uint8_t)axis;
        signRef = sign;
        offRef  = 0.0f;   // 축을 바꾸면 옛 기준각은 다른 축에서 잡은 값이라 뜻이 없다
        nv::writeWith([&](nvs_handle_t h) {
            return nvs_set_u8(h, isHeel ? "heel_axis" : "pitch_axis", axisRef) == ESP_OK &&
                   nvs_set_i8(h, isHeel ? "heel_sgn" : "pitch_sgn", sign < 0.0f ? -1 : 1) == ESP_OK &&
                   nvPutFloat(h, isHeel ? "heel_off2" : "pitch_off", 0.0f);
        });
        printf("  기준각은 0 으로 되돌렸습니다. 평형일 때 level 을 다시 치세요.\n");
    }

    imuUpdateCollect();
    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    const imu::Vec& a = imu::acc();
    printf("──────────────────────────────────────────\n");
    printf("  지금 가속   %+.2f %+.2f %+.2f g\n", a.x, a.y, a.z);
    printf("  힐    가속 %s  기준각 %+.1f°  →  %+.1f° (기준각 빼기 전 %+.1f°)\n",
           hAx.text, gHeelOffsetDeg, currentHeelDeg(), rawTiltDeg(gHeelAxis, gHeelSign));
    printf("  피치  가속 %s  기준각 %+.1f°  →  %+.1f° (기준각 빼기 전 %+.1f°)\n",
           pAx.text, gPitchOffsetDeg, currentPitchDeg(), rawTiltDeg(gPitchAxis, gPitchSign));
    printf("──────────────────────────────────────────\n");
    printf("  배가 평형일 때 %s 축이 0 g 에 가까워야 맞는 축입니다.\n", what);
    printf("  남은 한 축이 위아래를 향하는 축이라 ±1 g 를 읽습니다.\n");
}

// dead <kn> — 잡음 바닥 (firmware-rak 5013-5037)
static void cmdDead(const char* line) {
    char a[32];
    restTrim(line, 4, a, sizeof a);
    const gps::State& gs = gps::state();
    if (a[0]) {
        const bool numeric = isdigit((unsigned char)a[0]) || a[0] == '.';   // toFloat 은 글자여도 0 을 준다
        const float v = strtof(a, nullptr);
        if (!numeric || !std::isfinite(v) || v < 0.0f || v > 2.0f) { printf("  0 ~ 2.0 kn 사이 숫자로 주세요\n"); return; }
        gps::setDeadbandKn(v);
        if (!nv::writeWith([v](nvs_handle_t h) { return nvPutFloat(h, "dead_kn", v); }))
            printf("  ★ 보드에 못 적었습니다 — 껐다 켜면 옛 값으로 돌아갑니다\n");
    }
    printf("──────────────────────────────────────────\n");
    printf("  잡음 바닥  %.2f kn — 이보다 작으면 0 으로 보여줍니다\n", gs.deadbandKn);
    printf("  도플러의 이론 잡음이 초당 몇 cm(=0.1 kn 언저리)라서 기본값이 0.10 입니다.\n");
    printf("  Velocitek ProStart V2 가 파는 물건의 사양도 ±0.1 kn 입니다.\n");
    if (gs.fix) printf("  지금  다듬은 값 %.2f  →  보여주는 값 %.2f kn\n", gs.sogDamped, gps::sogOut());
    printf("──────────────────────────────────────────\n");
}

// smooth <0~5> — 다듬기 세기 (firmware-rak 5040-5064)
static void cmdSmooth(const char* line) {
    char a[16];
    restTrim(line, 6, a, sizeof a);
    const gps::State& gs = gps::state();
    if (a[0]) {
        if (strlen(a) != 1 || !isdigit((unsigned char)a[0])) { printf("  0~5 중에서 고르세요\n"); return; }
        const int lv = a[0] - '0';
        if (lv < 0 || lv > 5) { printf("  0~5 중에서 고르세요\n"); return; }
        gps::setDampLevel((uint8_t)lv);
        gps::resetDamping();
        if (!nv::writeWith([lv](nvs_handle_t h) { return nvs_set_u8(h, "damp", (uint8_t)lv) == ESP_OK; }))
            printf("  ★ 보드에 못 적었습니다 — 껐다 켜면 옛 값으로 돌아갑니다\n");
    }
    printf("──────────────────────────────────────────\n");
    printf("  다듬기 세기  %u단계  (시상수 %.1f초)\n", gs.dampLevel, gps::kDampTau[gs.dampLevel <= 5 ? gs.dampLevel : 2]);
    printf("  0 없음 / 1 0.3초 / 2 0.6초 / 3 1.2초 / 4 2.5초 / 5 5초\n");
    printf("  잔잔하면 낮게, 물결이 거칠면 높게. 요트 계기들이 쓰는 방식이다.\n");
    if (gs.fix)
        printf("  지금  원본 %.2f kn %5.1f°  →  다듬은 값 %.2f kn %5.1f°\n",
               gps::parser().speed.knots(), gps::parser().course.deg(), gs.sogDamped, gs.cogDamped);
    printf("──────────────────────────────────────────\n");
}

// sess [번호] — 세션 번호 보기·고치기 (firmware-rak 4396-4409)
static void cmdSess(const char* line) {
    nv::writeWith([&](nvs_handle_t h) {
        if (strlen(line) > 5) {
            const uint32_t n = (uint32_t)strtol(line + 5, nullptr, 10);
            nvs_set_u32(h, "sess_n", n);
            printf("[SESS] 다음 세션은 %u 번부터 (카드에 더 큰 번호가 있으면 그 다음으로 올라갑니다)\n", (unsigned)(n + 1));
        }
        printf("[SESS] 마지막으로 쓴 번호 %u\n", (unsigned)nv::u32(h, "sess_n", 0));
        return true;
    });
}

// tz [분] — 파일 이름에 쓸 시각 기울기 (firmware-rak 4415-4443)
static void cmdTz(const char* line) {
    int32_t cur = 540;
    nv::writeWith([&](nvs_handle_t h) {
        if (strlen(line) > 3) {
            const int32_t m = (int32_t)strtol(line + 3, nullptr, 10);
            if (m < -720 || m > 840) printf("[TZ] -720 ~ 840 분 사이여야 합니다.\n");
            else {
                nvs_set_i32(h, "tz_min", m);
                printf("[TZ] %+d분 (%+.1f시간) 으로 두었습니다.\n", (int)m, m / 60.0f);
            }
        }
        cur = nv::i32(h, "tz_min", 540);
        return true;
    });
    printf("[TZ] 지금 %+d분 (%+.1f시간). 파일 이름에만 쓰입니다.\n", (int)cur, cur / 60.0f);
    const time_t tnow = time(nullptr);
    if (tnow > 1600000000) {
        struct tm t; gmtime_r(&tnow, &t);
        printf("[TZ] 보드 시계 %04d-%02d-%02d %02d:%02d:%02d UTC\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        printf("[TZ] 보드 시계가 아직 안 맞았습니다 — 위성을 잡으면 맞습니다.\n");
    }
}

// pin <번호> — 그 GPIO 를 5초 지켜본다 (firmware-rak 4448-4498)
static void cmdPin(const char* line) {
    const int g = (int)strtol(line + 4, nullptr, 10);
    if (!GPIO_IS_VALID_GPIO(g)) { printf("[PIN] GPIO%d 는 없는 번호입니다\n", g); return; }
    const gpio_num_t gn = static_cast<gpio_num_t>(g);
    gpio_set_direction(gn, GPIO_MODE_INPUT);
    // 풀업 HIGH · 풀다운 LOW 여야 비어 있다. 풀업만 보면 아무것도 안 물린 핀과 못 가른다.
    gpio_set_pull_mode(gn, GPIO_PULLUP_ONLY);   delayMs(20); const int up   = gpio_get_level(gn);
    gpio_set_pull_mode(gn, GPIO_PULLDOWN_ONLY); delayMs(20); const int down = gpio_get_level(gn);
    gpio_set_pull_mode(gn, GPIO_PULLUP_ONLY);   delayMs(20);
    printf("[PIN] GPIO%d  풀업 %s · 풀다운 %s  → %s\n", g, up ? "HIGH" : "LOW", down ? "HIGH" : "LOW",
           (up && !down) ? "비어 있음 (버튼 달 수 있다)" : (!up && !down) ? "무언가 LOW 로 잡고 있다"
           : (up && down) ? "무언가 HIGH 로 잡고 있다" : "이상한 값");
    printf("[PIN] GPIO%d 를 5초 봅니다 (내부 풀업). 눌러 보세요.\n", g);
    int last = gpio_get_level(gn);
    uint32_t changes = 0, lowMs = 0;
    const uint32_t t0 = nowMs();
    uint32_t lastT = t0;
    while (nowMs() - t0 < 5000) {
        const int v = gpio_get_level(gn);
        if (v != last) {
            const uint32_t now2 = nowMs();
            if (!last) lowMs += now2 - lastT;
            lastT = now2;
            last = v;
            ++changes;
        }
        esp_rom_delay_us(200);
        if ((nowMs() - t0) % 1000 == 0) feedWatchdog();
    }
    printf("[PIN] 바뀐 횟수 %u,  LOW 로 있던 시간 %u ms,  지금 %s\n", (unsigned)changes, (unsigned)lowMs, last ? "HIGH" : "LOW");
    if (changes == 0 && !last) {
        printf("      ★ 풀업을 걸었는데 LOW 입니다. 누가 이 선을 끌어내리고\n");
        printf("        있습니다. 버튼을 달면 눌린 것과 구별이 안 됩니다.\n");
    } else if (changes == 0) {
        printf("      아무도 안 건드립니다 — 버튼 달아도 됩니다.\n");
    } else if (changes >= 8) {
        printf("      ★ 누가 이 선을 흔들고 있습니다 (PPS 같은 것). 다른 핀을 쓰세요.\n");
    } else {
        printf("      몇 번 바뀌었습니다. 누른 게 아니라면 다른 핀을 쓰세요.\n");
    }
}

// usbbench [KB] — USB 시리얼 속도 실측 (firmware-rak 4508-4526)
static void cmdUsbBench(const char* line) {
    uint32_t kb = 512;
    if (strlen(line) > 9) kb = (uint32_t)strtol(line + 9, nullptr, 10);
    if (kb < 1) kb = 1;
    if (kb > 8192) kb = 8192;
    static uint8_t buf[1024];
    memset(buf, '.', sizeof buf);
    printf("BENCH START %lu\n", (unsigned long)kb);
    fflush(stdout);
    const uint32_t t0 = nowMs();
    for (uint32_t i = 0; i < kb; i++) { fwrite(buf, 1, sizeof buf, stdout); if ((i & 63) == 0) feedWatchdog(); }
    fflush(stdout);
    const uint32_t dt = nowMs() - t0;
    printf("\nBENCH END %lu KB  %.2f초  %.0f KB/초\n", (unsigned long)kb, dt / 1000.0f, dt ? kb * 1000.0f / dt : 0.0f);
}

// sdread <파일이름> <MB> <방식> — WiFi 없이 SD 파일 읽기만 잰다 (09-15 새로 만듦, firmware-rak 에 없음)
//   파일 받기가 느린 게 카드·SPI 한계인지, 읽는 코드의 손실인지 가르려고. 같은 파일·같은 양으로 방식만 바꾼다.
//   방식 0 fopen 기본 버퍼 + fread 4 KB (setvbuf 없이 — 고치기 전 파일 보내기와 같음)
//        1 setvbuf 4 KB + fread 4 KB · 2 setvbuf 16 KB + fread 4 KB · 3 POSIX open/read 16 KB (stdio 안 거침)
static void cmdSdRead(const char* line) {
    char name[64] = "";
    long mb = 2, how = 0;
    if (sscanf(line + 7, "%63s %ld %ld", name, &mb, &how) < 1 || strchr(name, '/') || strstr(name, "..")) {
        printf("[SDREAD] 형식: sdread <LOGS 안 파일이름> <MB> <방식 0~3>\n");
        return;
    }
    if (mb < 1) mb = 1;
    if (mb > 64) mb = 64;
    if (how < 0 || how > 3) how = 0;
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) { printf("[SDREAD] 카드를 못 잡았습니다\n"); return; }
    char path[96];
    snprintf(path, sizeof path, "/sd/LOGS/%s", name);
    static uint8_t* buf = nullptr;
    static char* vbuf = nullptr;
    if (!buf) buf = (uint8_t*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!vbuf) vbuf = (char*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t want = (uint32_t)mb * 1048576u;
    uint64_t got = 0;
    const int64_t t0 = esp_timer_get_time();
    uint32_t calls = 0;
    bool ok = buf && vbuf;
    if (ok && how == 3) {
        const int fd = open(path, O_RDONLY);
        if (fd < 0) ok = false;
        while (ok && got < want) {
            const int n = read(fd, buf, 16384);
            if (n <= 0) break;
            got += (uint64_t)n; ++calls;
            if ((calls & 63) == 0) feedWatchdog();
        }
        if (fd >= 0) close(fd);
    } else if (ok) {
        FILE* f = fopen(path, "rb");
        if (!f) ok = false;
        if (f && how == 1) setvbuf(f, vbuf, _IOFBF, 4096);
        if (f && how == 2) setvbuf(f, vbuf, _IOFBF, 16384);
        while (ok && got < want) {
            const size_t n = fread(buf, 1, 4096, f);
            if (n == 0) break;
            got += n; ++calls;
            if ((calls & 255) == 0) feedWatchdog();
        }
        if (f) fclose(f);
    }
    const double sec = (esp_timer_get_time() - t0) / 1e6;
    const int realKhz = sdcard::cardFreqKhz();   // 놓기 전에 — 카드가 실제로 붙은 주파수
    sdcard::release(sdcard::Owner::Diagnostic);
    printf("[SDREAD] 카드 주파수 %d kHz\n", realKhz);
    if (!ok) { printf("[SDREAD] %s 를 못 열었습니다 (또는 버퍼 못 잡음)\n", name); return; }
    static const char* kHow[] = {"fopen 기본 버퍼 + fread 4KB", "setvbuf 4KB + fread 4KB", "setvbuf 16KB + fread 4KB", "read() 16KB"};
    printf("[SDREAD] 방식 %ld (%s) · %llu 바이트 · %.2f초 · %.0f KB/초 · 부름 %lu번\n",
           how, kHow[how], (unsigned long long)got, sec, sec > 0 ? got / 1024.0 / sec : 0.0, (unsigned long)calls);
}

// power [14|2|off] — 센서 전원 핀 (firmware-rak 5112-5149)
static void cmdPower(const char* line) {
    char arg[16];
    restTrim(line, 5, arg, sizeof arg);
    if (!arg[0]) { printIdentity(); return; }
    int pin;
    if (!strcmp(arg, "off")) {
        pin = 0;
    } else {
        pin = (int)strtol(arg, nullptr, 10);
        if (pin != rak::kSensorPowerA && pin != rak::kSensorPowerB) {
            printf("[PWR] %d 은 후보가 아닙니다. %d, %d, off 중에서 고르세요.\n", pin, rak::kSensorPowerA, rak::kSensorPowerB);
            return;
        }
        // 후보 B(GPIO2)는 저장 버튼과 같은 핀이다. 막는다.
        if (pin == rak::kAin1) { printf("[PWR] GPIO%d 은 저장 버튼 자리입니다. 못 씁니다.\n", pin); return; }
    }
    gSensorPowerPin = pin;
    installPowerHooks();   // 잠들 때 끌 핀이 바뀌었다
    applySensorPower(pin, /*cycle=*/true);
    // 전원을 껐다 켰으니 GPS 가 기본값으로 돌아갔다 — 휴대(0), NAV-PV 끔. 켤 때와 같은 절차로 다시 건다.
    if (pin) { gps::applyNavPv(); gps::applyBoatMode(); }
    nv::writeWith([pin](nvs_handle_t h) { return nvs_set_i32(h, "pwr_pin", pin) == ESP_OK; });
    if (pin) printf("[PWR] 이어서 scan 을 쳐서 모듈이 보이는지 확인하세요.\n");
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
        if (!blockingDiagOk("gpscfg static")) return;
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
        if (!blockingDiagOk("nmea")) return;   // 3초 응답을 기다린다 (R3)
        gps::sendAndWatch(body);
        return;
    }
    if (!strcmp(line, "info")) { printIdentity(); return; }
    if (!strcmp(line, "help") || !strcmp(line, "?")) { printHelp(); return; }
    if (!strcmp(line, "batt"))      { diag::batteryReport(); return; }
    if (!strcmp(line, "sleepstat")) { diag::sleepReport(power::sleepStats()); return; }
    if (!strcmp(line, "battboot"))  { power::battBootReport(gBattVolts); return; }
    if (!strcmp(line, "off"))       { power::requestPowerOff(false); return; }
    // 닫기가 15초 넘게 안 끝났을 때 사람이 고르는 길. 끝이 잘린다 (단추로는 5초 한 번 더)
    if (!strcmp(line, "off force")) { power::requestPowerOff(true); return; }
    if (!strncmp(line, "off ", 4)) {   // `off 120` — 120초 뒤 스스로 깬다 (시험용)
        if (hlog::phase() != recctl::Phase::Idle) { printf("[SLEEP] 기록 중에는 시험 잠자기를 못 합니다. rec off 먼저.\n"); return; }
        power::goToSleep((uint32_t)strtol(line + 4, nullptr, 10));
        return;
    }
    if (!strcmp(line, "sd"))        { if (sdFreeFor("sd")) diag::sdCheck(); return; }
    if (!strncmp(line, "sdread ", 7)) { if (sdFreeFor("sdread") && blockingDiagOk("sdread")) cmdSdRead(line); return; }
    // sdhz <kHz> — 시험: 다음 마운트부터 SD SPI 주파수 (0 = 기본 20000). 전원을 끄면 기본으로 돌아간다 (NVS 에 안 적는다)
    if (!strcmp(line, "sdhz") || !strncmp(line, "sdhz ", 5)) {
        if (strlen(line) > 5) {
            if (!sdFreeFor("sdhz")) return;
            const long k = strtol(line + 5, nullptr, 10);
            // ★ 20000 kHz 가 상한. 09-15 에 40000 을 걸자 카드 초기화가 실패하고(send_csd 0x108), 보드 리셋으로도 안 풀렸다
            //   (카드는 VDD 라 전원을 못 끊는다 — POWER.md). 카드를 뽑았다 꽂아야 했다. 다시는 못 걸게 막는다.
            //   SDMMC 1비트는 풀업이 약해서 쓰지 않기로 했다 (09-15 사용자 결정, CHECKLIST 5장)
            if (k > 20000) { printf("[SDHZ] 20000 kHz 넘게는 못 겁니다 — 40 MHz 에서 카드가 멈췄습니다 (CHECKLIST 5장)\n"); return; }
            sdcard::setTestFreqKhz((int)(k < 0 ? 0 : k));
        }
        printf("[SDHZ] 다음 마운트부터 %s (지금 붙은 카드 %d kHz)\n",
               strlen(line) > 5 && strtol(line + 5, nullptr, 10) > 0 ? "시험 주파수" : "기본 20000 kHz", sdcard::cardFreqKhz());
        return;
    }
    // SD 쓰기 속도 실측. 기본 3600줄 = 10 Hz 로 6분치.
    if (!strcmp(line, "sdbench") || !strncmp(line, "sdbench ", 8)) {
        long n = (strlen(line) > 8) ? strtol(line + 8, nullptr, 10) : 3600;
        if (n < 100) n = 100;
        if (n > 2000000) n = 2000000;
        if (sdFreeFor("sdbench")) diag::sdBench((uint32_t)n);
        return;
    }
    // 붙어 있는 것을 한 번에 훑는다. 보드를 처음 구웠을 때 이것부터 친다.
    if (!strcmp(line, "check")) {
        if (!sdFreeFor("check") || !blockingDiagOk("check")) return;
        printf("\n");
        printf("════ 모듈 전체 점검 ════\n");
        printIdentity();
        doScan();
        doImu();
        diag::sdCheck();
        gps::printFix();
        printf("════ 점검 끝 ════\n");
        printf("  안 잡힌 게 있으면 power 값을 바꿔 다시 check 하세요.\n");
        return;
    }
    // 몇 초씩 루프를 붙잡는 진단은 기록 중 막는다 (R3). firmware-rak 은 scan·pin·nmea·gpscfg static 을 막지 않았다.
    if (!strcmp(line, "scan"))     { if (blockingDiagOk("scan")) doScan(); return; }
    if (!strcmp(line, "hdgtilt"))  { if (blockingDiagOk("hdgtilt")) doHeadingTilt(); return; }
    if (!strcmp(line, "magcal") || !strncmp(line, "magcal ", 7)) {
        char msg[200];
        magCalCmd(strlen(line) > 7 ? line + 7 : "", msg, sizeof msg);
        if (msg[0]) printf("  %s\n", msg);
        return;
    }
    if (!strcmp(line, "sess") || !strncmp(line, "sess ", 5)) { cmdSess(line); return; }
    if (!strcmp(line, "tz") || !strncmp(line, "tz ", 3))     { cmdTz(line); return; }
    if (!strncmp(line, "pin ", 4))                            { if (blockingDiagOk("pin")) cmdPin(line); return; }
    if (!strcmp(line, "usbbench") || !strncmp(line, "usbbench ", 9)) { cmdUsbBench(line); return; }
    if (!strcmp(line, "loopstat")) {
        gLoopStat = !gLoopStat;
        printf("[STAT] 루프 시간 출력 %s\n", gLoopStat ? "켬" : "끔");
        return;
    }
    if (!strcmp(line, "calib"))    { doCalib(); return; }
    if (!strcmp(line, "level"))    { if (!settingsLockedWhileRecording("level")) doLevel(); return; }
    if (!strcmp(line, "hdg") || !strncmp(line, "hdg ", 4)) { cmdHdg(line + 3); return; }
    if (!strcmp(line, "heel") || !strncmp(line, "heel ", 5) || !strcmp(line, "pitch") || !strncmp(line, "pitch ", 6)) {
        cmdHeelPitch(line);
        return;
    }
    if (!strncmp(line, "dead", 4))   { cmdDead(line); return; }
    if (!strncmp(line, "smooth", 6)) { cmdSmooth(line); return; }
    if (!strncmp(line, "power", 5))  { cmdPower(line); return; }
    // WiFi 로 기록 파일 내보내기 (firmware-rak main.cpp 4529-4590). 자세히는 netsrv.h
    if (!strcmp(line, "wifi") || !strncmp(line, "wifi ", 5)) {
        char orig[64] = "";
        restTrim(line, 5, orig, sizeof orig);   // 이름·비밀번호는 원문 대소문자가 필요하다
        char arg[64];
        snprintf(arg, sizeof arg, "%s", orig);
        for (char* p = arg; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

        if (!strcmp(arg, "join")) {
            if (hlog::busy()) { printf("[NET] 기록 중입니다. rec off 먼저 하세요.\n"); return; }
            netsrv::startJoin();
            return;
        }
        if (!strcmp(arg, "off")) {
            netsrv::stop();
            printf("[NET] WiFi 껐습니다.\n");
            return;
        }
        // 아래는 BLE 설정 통로와 **같은 말**을 쓴다. 앱 없이 여기서 시험한다.
        if (!strncmp(arg, "ssid ", 5) || !strncmp(arg, "pass ", 5) || !strcmp(arg, "scan") || !strcmp(arg, "status") ||
            !strcmp(arg, "on") || !strcmp(arg, "ap") || !strncmp(arg, "on ", 3) || !strncmp(arg, "ap ", 3) ||
            !strncmp(arg, "idle ", 5)) {
            char full[80];
            snprintf(full, sizeof full, "wifi %s", orig);
            controlLine(full);
            return;
        }
        printf("──────────────────────────────────────────\n");
        switch (netsrv::mode()) {
            case netsrv::Mode::Off:
                printf("  WiFi          꺼져 있음\n");
                break;
            case netsrv::Mode::AP:
                printf("  WiFi          내가 만든 망\n");
                printf("  이름          %s\n", netsrv::ssidText());
                printf("  주소          http://%s/\n", netsrv::ipText());
                break;
            case netsrv::Mode::Join:
                printf("  WiFi          %s 에 붙어 있음\n", netsrv::ssidText());
                printf("  주소          http://%s/\n", netsrv::ipText());
                break;
        }
        printf("  보낸 파일     %u개  %.2f MB\n", (unsigned)netsrv::servedFiles(), netsrv::servedBytes() / 1048576.0);
        // WiFi 를 여러 번 켜고 끌 때 새는지 보려고 (체크리스트 W06). PSRAM 이 섞이지 않게 내부 메모리만.
        printf("  내부 메모리   %u 바이트 남음 (가장 작았을 때 %u)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        printf("──────────────────────────────────────────\n");
        printf("  wifi ap    보드가 스스로 WiFi 를 만든다 (바닷가용)\n");
        printf("  wifi join  저장된 WiFi 에 붙는다\n");
        printf("  wifi off   끈다\n");
        printf("  ─ 아래는 BLE 설정 통로와 같은 말이다 ─\n");
        printf("  wifi ssid <이름>   붙을 WiFi 이름 (전원 빼도 남는다)\n");
        printf("  wifi pass <비번>   비밀번호\n");
        printf("  wifi scan          주변 WiFi 훑기 (BLE 안 내리고 된다)\n");
        printf("  wifi idle <초>     아무도 안 쓰면 끄기까지 (0 이면 안 끔)\n");
        printf("  wifi status        지금 상태\n");
        return;
    }
    if (!strcmp(line, "lora"))       { lora::report();      return; }
    if (!strcmp(line, "lora regs"))  { lora::reportRegs();  return; }
    if (!strcmp(line, "lora tx"))    { lora::txTest();      return; }
    if (!strcmp(line, "lora rssi"))  { lora::reportNoise(); return; }
    if (!strcmp(line, "lora watch")) { lora::watchToggle(); return; }
    if (!strcmp(line, "lora on"))    { lora::begin();       return; }
    // 로라 배 번호. PROTOCOL.md §10.11
    if (!strcmp(line, "boat") || !strncmp(line, "boat ", 5)) {
        if (!strcmp(line, "boat")) {
            if (gBoatId == 0) printf("[BOAT] 번호 없음 — 로라로 안 보낸다\n");
            else printf("[BOAT] %u번 (차례 %u)\n", gBoatId, gBoatId - 1);
            return;
        }
        // ★ 달리는 중에는 안 바꾼다. 번호가 바뀌면 말할 차례가 옮겨 가 남의 차례에 떨어질 수 있다.
        if (hlog::recording()) { printf("[BOAT] 기록 중에는 못 바꿉니다. 먼저 stop 하세요\n"); return; }
        char* arg = line + 5;
        while (*arg == ' ') ++arg;
        char* end = nullptr;
        const long n = strtol(arg, &end, 10);
        // firmware-rak: 빈 칸 · 숫자 아님(toInt 0 인데 "0" 아님) · 범위 밖을 거절
        if (!*arg || end == arg || n < 0 || n > kBoatIdMax) {
            printf("[BOAT] 0~%u 로 입력하세요. 0 은 번호 없음. 예) boat 7\n", kBoatIdMax);
            return;
        }
        gBoatId = (uint8_t)n;
        if (!nv::writeWith([](nvs_handle_t h) { return nvs_set_u8(h, "boat", gBoatId) == ESP_OK; }))
            printf("[BOAT] ★ 보드에 못 적었습니다 — 껐다 켜면 옛 번호로 돌아갑니다\n");
        if (gBoatId == 0) printf("[BOAT] 번호 없음 — 로라로 안 보낸다\n");
        else printf("[BOAT] %u번 (차례 %u). 뱃머리 번호표와 같은지 보세요\n", gBoatId, gBoatId - 1);
        return;
    }
    if (!strcmp(line, "oledw")) { diag::oledWidths(); return; }
    // 화면을 나중에 꽂았을 때 다시 붙인다. 재부팅할 필요 없다.
    if (!strcmp(line, "oled")) {
        if (sail::displayBegin()) {
            printf("[OLED] 붙었습니다 — 화면에 값이 나옵니다.\n");
            sail::displayBootMessage(ble::fullName(), "hello");
        } else {
            printf("[OLED] 0x3C 응답 없음. J12 헤더(2.54mm I2C)에 꽂혀 있나요?\n");
            printf("       센서 슬롯 A~D 가 아닙니다.\n");
        }
        return;
    }
    if (!strncmp(line, "hz ", 3)) {
        const long hz = strtol(line + 3, nullptr, 10);
        if (hz < 1 || hz > 100) { printf("[ID ] 1~100 Hz 범위로 입력하세요. 예) hz 20\n"); return; }
        if (!ble::setNotifyPeriodMs((uint32_t)(1000.0f / hz + 0.5f)))
            printf("[ID ] ★ 보드에 못 적었습니다 — 껐다 켜면 옛 값으로 돌아갑니다\n");
        printf("[ID ] notify 주기 → %.1f Hz (%ums)\n", 1000.0f / ble::notifyPeriodMs(), (unsigned)ble::notifyPeriodMs());
        return;
    }
    if (!strncmp(line, "name ", 5)) {
        char* arg = line + 5;
        while (*arg == ' ') ++arg;
        if (!*arg) { printf("[ID ] 이름이 비어 있습니다. 예) name hojun\n"); return; }
        if (strlen(arg) > sail::kMaxUserNameLen)
            printf("[ID ] 이름이 너무 깁니다 (최대 %u자). 잘라서 저장합니다.\n", (unsigned)sail::kMaxUserNameLen);
        if (!ble::saveIdentity(arg)) printf("[ID ] ★ 보드에 못 적었습니다 — 껐다 켜면 옛 이름으로 돌아갑니다\n");
        printIdentity();
        ble::requestAdvApply();   // 광고 이름이 바뀌었으니 다시 올린다
        printf("[ID ] 저장 완료 — 앱에서 모듈을 다시 선택해야 합니다.\n");
        return;
    }
    printf("[CMD] 모르는 명령: %s  (3단계에서 되는 것: rec … · imu · fix · gps · gpscfg · gpshz · sog · navpv · nmea · test imu/gps · info · hz · name)\n", line);
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
    printf("  %s — firmware-idf 3단계 (기록기 + GPS·IMU + BLE) · ESP-IDF %s\n", ble::fullName(), esp_get_idf_version());
    printf("  module_id %u (0x%02X) · notify %.1fHz / adv refresh %.1fHz\n", ble::moduleId(), ble::moduleId(),
           1000.0f / ble::notifyPeriodMs(), 1000.0f / sail::kAdvRefreshMs);
    printf("  MAC %02X:%02X:%02X:%02X:%02X:%02X · 플래시 %" PRIu32 " MB · PSRAM %u 바이트\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], flash / (1024 * 1024), (unsigned)esp_psram_get_size());
    printf("═══════════════════════════════════════════\n");
}

extern "C" void app_main(void) {
    // USB 입력을 읽으려면 드라이버를 깔고, printf 도 그 드라이버로 보낸다 (usb_serial_jtag_vfs.h)
    // ★ 진단 (2026-09-15): 다시 꽂은 뒤 드라이버를 까는 순간 USB 인터럽트가 안 멈춰 인터럽트 워치독으로 되풀이 재시작했다
    //   (PORTING.md). 까기 직전에 인터럽트 켜짐·걸림 레지스터를 날것으로 찍는다 — 어느 비트인지 짐작하지 않고 본다.
    //   드라이버 전이라 이 줄은 드라이버 없는 콘솔 길로 나간다.
    printf("[USB] 드라이버 전  int_ena 0x%08" PRIx32 "  int_st 0x%08" PRIx32 "  int_raw 0x%08" PRIx32 "\n",
           USB_SERIAL_JTAG.int_ena.val, USB_SERIAL_JTAG.int_st.val, USB_SERIAL_JTAG.int_raw.val);
    // ★ 고침: IDF 드라이버의 인터럽트 처리기는 IN_EMPTY 와 OUT_RECV_PKT 만 지운다 (usb_serial_jtag.c:55-154).
    //   아두이노 HWCDC(firmware-rak) 는 BUS_RESET 도 켰다 (HWCDC.cpp:344-345). 이 칸은 코어 리셋으로 안 지워지고
    //   배터리가 붙어 있으면 USB 를 뽑아도 칩 전원이 안 끊겨 그대로 남는다 [추측 — 위 줄이 찍는 int_ena 로 확인].
    //   다시 꽂으면 버스 리셋이 걸리고, 아무도 안 지워 처리기가 끝없이 다시 불린다.
    //   드라이버가 다루는 두 칸 말고는 켜짐을 끄고, 남은 버스 리셋 걸림을 지운 뒤 깐다. 드라이버가 필요한 걸림(IN_EMPTY)은 안 건드린다.
    usb_serial_jtag_ll_disable_intr_mask(~(uint32_t)(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT));
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_BUS_RESET);
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 1024;
    ucfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&ucfg);
    usb_serial_jtag_vfs_use_driver();

    // ★ 제일 먼저 한다. 깊은잠에서 깼으면 5초를 채웠는지 여기서 가른다. 못 채웠으면 이 안에서 도로 잠든다.
    //   플래시에 쓰기 전, 워치독을 걸기 전이어야 한다 (firmware-rak setup 5517 wakeGate).
    installPowerHooks();
    power::wakeGate();

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
    installPowerHooks();   // NVS 의 센서 전원 핀을 읽었다
    ble::loadIdentity();   // NVS name · notify_ms
    logBoot();

    // 센서 전원부터. 없으면 GPS 가 통째로 죽어 있다 (IMU 는 늘 켜진 VDD)
    applySensorPower(gSensorPowerPin, /*cycle=*/true);

    batteryInit();
    // 깬 뒤에는 분압 마디 콘덴서가 비어 전압이 낮게 나온다. 1초면 찬다 (firmware-rak 5588, 2026-09-08 battboot 실측)
    if (power::wokeFromSleep()) { delayMs(1000); feedWatchdog(); }
    gBattVolts = readBatteryVolts();
    gBattPct   = batteryPercent(gBattVolts);

    nv::writeWith([](nvs_handle_t h) {
        uint32_t n = 0; nvs_get_u32(h, "boot_n", &n);
        return nvs_set_u32(h, "boot_n", n + 1) == ESP_OK;
    });
    hlog::begin();
    // 깊은잠에서 깼으면 그 기록부터 남긴다. 사람이 명령을 안 쳐도 남게.
    power::reportAfterBoot();

    gps::setWaitHook(waitHook);
    gps::begin();
    power::buttonBegin();

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

    // 화면은 J12 헤더에 꽂는다. 없어도 그냥 지나간다. I2C 버스는 IMU 와 나눠 쓴다 (imu::bus)
    if (sail::displayBegin()) {
        printf("[OLED] RAK1921 붙음 (128x64)\n");
        sail::displayBootMessage(ble::fullName(), "starting...");
    } else {
        printf("[OLED] 없음 — J12 헤더에 꽂으면 자동으로 잡힙니다\n");
    }

    // 무전기는 켤 때 올린다. 배에서 명령을 칠 수가 없다. 번호가 있든 없든 늘 받는다 (PROTOCOL.md §10.11).
    // 없거나 실패해도 보드는 그대로 돈다.
    lora::begin();

    printf("[SRC] SOG/COG 는 GPS 가 위성을 잡았을 때만 값이 있습니다 (못 잡으면 무효)\n");
    printf("      HEEL·9축은 IMU 가 붙어 있을 때만 값이 있습니다\n");

    ble::start(buildTelemetry(nowMs()), buildExtra());

    // ★ 제일 마지막에 본다. 센서·카드가 다 올라온 뒤여야 한다.
    resumeRecordingIfCut();

    uint32_t lastImu = 0, lastImuFast = 0, lastBatt = 0, lastText = 0, lastNav = 0;
    bool ledOn = false;
    for (;;) {
        const uint32_t now = nowMs();
        const uint32_t loopT0 = microsNow();
        // 코어 0 받기 일꾼이 링버퍼에 넣어 둔 것을 꺼낸다. 안 꺼내면 64개 뒤로 버린다
        lora::pump();
        pollSerial();
        netsrv::poll();

        // GPS 는 쉬지 않고 읽는다. UART 버퍼가 넘치면 문장 중간이 잘린다.
        { const uint32_t t = microsNow(); gps::poll(); secDone(gStGps, microsNow() - t); }

        // 10 ms 마다 FIFO 를 퍼 온다. 값 사이 간격은 칩이 만든 10 ms 그대로다.
        if (now - lastImuFast >= 10) {
            const uint32_t t = microsNow();
            lastImuFast = now;
            imuDrain();
            secDone(gStImu, microsNow() - t);
        }
        // 10 Hz — 자력계까지
        if (now - lastImu >= 100) { lastImu = now; imuUpdateCollect(); }

        // 이어 시작한 뒤 1분 넘게 멀쩡히 돌면 세던 것을 지운다
        if (gResumeTries && hlog::recording() && now - hlog::recStartedMs() >= kResumeOkMs) {
            gResumeTries = 0;
            setRecTry(0);
            printf("[REC] 이어 시작한 기록이 1분 넘게 멀쩡합니다 — 되풀이 세기를 지웠습니다\n");
        }

        if (now - lastBatt >= 1000) {
            lastBatt = now;
            const float freshV = readBatteryVolts();
            if (freshV > 0.0f) {
                gBattVolts = (gBattVolts > 0.0f) ? gBattVolts * 0.8f + freshV * 0.2f : freshV;
                gBattPct   = gBattPct * 0.8f + batteryPercent(freshV) * 0.2f;
            }
            checkSensors();
            sail::displayHealthCheck();   // firmware-rak checkSensors 끝에서 불렀다
            hlog::healthCheck();
        }
        recControlTick(now);

        // 초록 LED — 기록 중이면 1초에 80 ms
        const bool want = hlog::recording() && (now % 1000) < 80;
        if (want != ledOn) { ledOn = want; gpio_set_level(static_cast<gpio_num_t>(rak::kLedGreen), want); }

        if (hlog::recording() && now - lastText >= 10000) {
            lastText = now;
            const uint32_t t = microsNow();
            logWriteText(now);
            secDone(gStLog, microsNow() - t);
        }

        // 자력계 치우침을 재는 중이면 1초에 한 번 진행을 알려준다
        magCalTick(now);
        // 켠 뒤 20초 동안 배터리 값을 1초마다 담는다 (battboot) · 저장 버튼
        power::battBootTick(now);
        power::buttonPoll(now);

        // 항법 10 Hz — 칸을 더해 나간다. 많이 밀렸으면 지금부터.
        const uint32_t navMs = 1000u / hlog::kRateNav;
        if (now - lastNav >= navMs) {
            lastNav += navMs;
            if (now - lastNav >= navMs * 5) lastNav = now;
            gps::updateFix();
            if (hlog::recording()) logWriteNav(now);
        }

        // 화면. 기록 중에는 1 Hz — 한 장에 I2C 가 묶여 IMU 를 못 읽는다 (firmware-rak 실측 33.6 ms, 4 Hz 면 IMU 78 Hz)
        {
            static uint32_t lastDraw = 0;
            const uint32_t drawPeriod = hlog::recording() ? 1000 : 250;
            if (now - lastDraw >= drawPeriod) {
                lastDraw = now;
                const gps::State& gs = gps::state();
                const sail::Telemetry lt = buildTelemetry(now);
                TinyGPSPlus& p = gps::parser();
                auto modeChar = [](uint8_t m) -> char {
                    return m == 0 ? 'h' : m == 1 ? 's' : m == 2 ? 'p' : m == 3 ? 'c' : m == 4 ? 'b' : '?';
                };
                sail::DisplayState ds;
                ds.userName     = ble::userName();
                ds.bleConnected = ble::connected();
                ds.bleNotifying = ble::subscribed();
                ds.battVolts    = gBattVolts;
                ds.boatId       = gBoatId;
                ds.recording    = hlog::recording();
                ds.recFailed    = recctl::showFailed(gWantRec, hlog::phase(), gRecGaveUp, gRecLastSaveBad);
                ds.recClosing   = hlog::phase() == recctl::Phase::Closing;
                ds.battLow      = kBattWarnVolts > 0.0f && gBattVolts > 0.0f && gBattVolts < kBattWarnVolts;
                ds.recSeconds   = ds.recording ? (now - hlog::recStartedMs()) / 1000 : 0;
                ds.sogKn        = lt.sogKn;   // 정상은 다듬은 값, 품질 거절 시 원본 + ?
                // ★ 정해 둔 모드(선박 4)와 다를 때만 속도 줄에 띄운다. 전압 옆 칸은 늘.
                ds.gnssMode     = (gs.dyModel == gps::kBoatMode) ? 0 : modeChar(gs.dyModel);
                ds.gnssModeNow  = modeChar(gs.dyModel);
                ds.cogDeg       = lt.cogDeg;
                ds.headingDeg   = boatHeadingDeg();
                ds.heelDeg      = lt.heelDeg;
                ds.pitchDeg     = currentPitchDeg();
                ds.imuOk        = imu::ok();
                ds.magOk        = imu::magOk();
                ds.sogValid     = lt.sogValid;
                ds.cogValid     = lt.cogValid;
                ds.sogCaution   = lt.sogValid && !gs.sogShownOk;
                ds.headingCaution = ds.headingDeg >= 0.0f && gHeading.latest(nowMs()).caution;
                ds.heelValid    = lt.heelValid;
                ds.gpsFix       = gs.fix;
                ds.satellites   = p.satellites.isValid() ? (int)p.satellites.value() : 0;
                ds.hdop         = p.hdop.isValid() ? (float)p.hdop.hdop() : -1.0f;
                // 버튼이 막대를 그리고 있으면 평소 계기 화면은 건너뛴다. 안 그러면 4 Hz 로 막대를 지운다.
                if (!power::buttonOwnsScreen()) {
                    const uint32_t tD = microsNow();
                    sail::displayUpdate(ds);
                    secDone(gStDraw, microsNow() - tD);
                }
            }
        }

        // BLE — 제어 줄은 루프에서 처리 · 광고 다시 걸기 · notify 주기 (칸을 더해 나간다) · 1 Hz 광고 갱신
        {
            char ctl[192];
            while (ble::takeControlLine(ctl, sizeof ctl)) controlLine(ctl);
            ble::pump();
            static uint32_t lastNotify = 0, lastAdv = 0;
            const uint32_t period = ble::notifyPeriodMs();
            if (now - lastNotify >= period) {
                lastNotify += period;
                if (now - lastNotify >= period * 5) lastNotify = now;
                const uint32_t tN = microsNow();
                ble::publish(buildTelemetry(now), buildExtra());
                secDone(gStNotify, microsNow() - tN);
            }
            if (now - lastAdv >= sail::kAdvRefreshMs) { lastAdv = now; ble::refreshAdvPayload(); }
        }

        // BLE 로 시킨 WiFi 켜고 끄기. ★ 콜백 안에서 하면 안 된다 — 표식만 세우고 여기서 한다.
        if (gWifiWant) {
            const uint8_t want = gWifiWant;
            gWifiWant = 0;
            delayMs(150);   // 답이 폰에 닿을 시간. 이 뒤로 BLE 가 내려갈 수 있다
            if (want == 1) netsrv::startJoin();
            else if (want == 2) netsrv::startAP();
            else netsrv::stop();
        }

        // 1 Hz — 시리얼 로그 (firmware-rak 5882-5923). 값 옆에 그 값이 어디서 왔는지 붙인다.
        {
            static uint32_t lastLog = 0;
            if (now - lastLog >= sail::kLogPeriodMs) {
                lastLog = now;
                const sail::Telemetry& lt = ble::latest();
                char sogTxt[16], cogTxt[16], heelTxt[16];
                if (lt.sogValid)  snprintf(sogTxt, sizeof sogTxt, "%5.2f", lt.sogKn);
                else              snprintf(sogTxt, sizeof sogTxt, "%5s", "--.--");
                if (lt.cogValid)  snprintf(cogTxt, sizeof cogTxt, "%5.1f", lt.cogDeg);
                else              snprintf(cogTxt, sizeof cogTxt, "%5s", "---");
                if (lt.heelValid) snprintf(heelTxt, sizeof heelTxt, "%+6.1f", lt.heelDeg);
                else              snprintf(heelTxt, sizeof heelTxt, "%6s", "---");
                // 움직임 종류 한 글자 — 늘 찍는다. h 휴대 · s 정지 · p 보행 · c 자동차 · b 선박 · ? 모름
                const uint8_t dm = gps::state().dyModel;
                const char modeCh = dm == 0 ? 'h' : dm == 1 ? 's' : dm == 2 ? 'p' : dm == 3 ? 'c' : dm == 4 ? 'b' : '?';
                printf("[%7.1fs] %s | SOG %c %s kn | COG %s° | HEEL %s° | BATT %3d%% %.2fV | seq %3u | %s%s\n",
                       now / 1000.0f, ble::fullName(), modeCh, sogTxt, cogTxt, heelTxt,
                       (int)sail::encodeBatt(lt.battPct), gBattVolts, (unsigned)ble::seq(),
                       ble::connected() ? "CONNECTED" : "ADVERTISING",
                       ble::connected() ? (ble::subscribed() ? " (notify ON)" : " (notify OFF)") : "");
                gps::printLine();
                if (gps::state().fix) {
                    char pos[16];
                    if (gps::state().sogFromPos >= 0) snprintf(pos, sizeof pos, "%.2f", gps::state().sogFromPos);
                    else                              snprintf(pos, sizeof pos, " --- ");
                    printf("   속도 비교  도플러 %5.2f kn  |  위치차분 %s kn\n", gps::parser().speed.knots(), pos);
                }
                printImuLine();
                loopStatPrint(sail::kLogPeriodMs);
            }
        }

        // 한 바퀴에 얼마나 걸렸나
        {
            const uint32_t dt = microsNow() - loopT0;
            if (dt > gLoopMaxUs) gLoopMaxUs = dt;
            ++gLoopCount;
        }

        feedWatchdog();   // 여기까지 왔으면 살아 있다
        delayMs(1);
    }
}
