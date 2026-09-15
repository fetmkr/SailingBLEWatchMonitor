// firmware-idf — 1단계: 기록기 붙이기. PORTING.md 의 단계 표가 원본이다.
//
// 이 판에서 되는 것
//   켤 때 점검(0단계) · NVS 설정 읽기 · 기록기(hlog_idf.cpp) · 기록 제어(원하는 상태 / 실제 상태) · 이어 시작 ·
//   시리얼 명령 `rec …` · NAV 10 Hz · TXT 10초 · 카드 건강 1 Hz · 기록 중 초록 LED
// 아직 안 되는 것 (2단계에서)
//   GPS·IMU — NAV 줄의 위치·속도·침로·자력은 **값 없음 표식**으로 둔다 (0 을 넣지 않는다). IMU 줄은 안 쓴다.
//   머리글 gnssHz·gnssDyn 은 모름(0·255) 으로 둔다 — 모듈에서 실제로 읽은 값만 적는 firmware-rak 규칙.
//
// 옮긴 원본: firmware-rak/src/main.cpp — logStartNow(3500) · recSaveWant/Open · recStartFrom · recWantOn/Off(3574-3636) ·
//   resumeRecordingIfCut(5349) · recGiveUp · recOnResult(5407-5456) · recControlTick(5458) · rec 명령(4600-4756) ·
//   sdFreeFor · loadSettings(380) · reportResetReason(3760) · buildHeadingNote(2765) · setup 의 rec_fail 읽기(5522)

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_rak.h"     // firmware-rak/include — 같이 쓴다
#include "heading_math.h"  // sanitizeFloat · sanitizeAxes · wrap180
#include "hlog.h"
#include "rec_control.h"
#include "sdcard.h"

static inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── NVS 도우미 — 아두이노 Preferences 와 같은 키·형식 ──────────────────────
//   getUChar=u8 · getChar=i8 · getUInt=u32 · getInt=i32 · getFloat=4바이트 blob (Preferences.cpp:255 putBytes)
namespace nv {
static uint8_t  u8 (nvs_handle_t h, const char* k, uint8_t d)  { uint8_t v;  return nvs_get_u8 (h, k, &v) == ESP_OK ? v : d; }
static int8_t   i8 (nvs_handle_t h, const char* k, int8_t d)   { int8_t v;   return nvs_get_i8 (h, k, &v) == ESP_OK ? v : d; }
static uint32_t u32(nvs_handle_t h, const char* k, uint32_t d) { uint32_t v; return nvs_get_u32(h, k, &v) == ESP_OK ? v : d; }
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

// ── 설정 (firmware-rak loadSettings 에서 기록 머리글에 들어가는 것만) ───────
static uint8_t gHeelAxis = 1;   static float gHeelSign = -1.0f;  static float gHeelOffsetDeg = 0.0f;
static uint8_t gPitchAxis = 2;  static float gPitchSign = 1.0f;  static float gPitchOffsetDeg = 0.0f;
static uint8_t gHdgAxisA = 1, gHdgAxisB = 0;
static float   gHdgSignA = 1.0f, gHdgSignB = 1.0f, gHdgOffsetDeg = 0.0f, gHdgDeclDeg = 0.0f;
static float   gMagOff[3] = {0, 0, 0};
static float   gMagRadius = 0.0f, gMagResid = 0.0f;

static void loadSettings() {
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
        gHeelAxis       = nv::u8 (h, "heel_axis", 1);
        gHeelSign       = nv::i8 (h, "heel_sgn", -1) < 0 ? -1.0f : 1.0f;
        gHeelOffsetDeg  = nv::f32(h, "heel_off2", 0.0f);     // heel_off 는 옛 키 — 안 읽는다 (firmware-rak 과 같음)
        gPitchAxis      = nv::u8 (h, "pitch_axis", 2);
        gPitchSign      = nv::i8 (h, "pitch_sgn", 1) < 0 ? -1.0f : 1.0f;
        gPitchOffsetDeg = nv::f32(h, "pitch_off", 0.0f);
        gMagOff[0]      = nv::f32(h, "mag_ox", 0.0f);
        gMagOff[1]      = nv::f32(h, "mag_oy", 0.0f);
        gMagOff[2]      = nv::f32(h, "mag_oz", 0.0f);
        gMagRadius      = nv::f32(h, "mag_r", 0.0f);
        gMagResid       = nv::f32(h, "mag_res", 0.0f);
        gHdgAxisA       = nv::u8 (h, "hdg_a", 1);
        gHdgAxisB       = nv::u8 (h, "hdg_b", 0);
        gHdgSignA       = nv::i8 (h, "hdg_sa", 1) < 0 ? -1.0f : 1.0f;
        gHdgSignB       = nv::i8 (h, "hdg_sb", 1) < 0 ? -1.0f : 1.0f;
        gHdgOffsetDeg   = nv::f32(h, "hdg_off", 0.0f);
        gHdgDeclDeg     = nv::f32(h, "hdg_decl", 0.0f);
        nvs_close(h);
    }
    int fixed = 0;
    fixed += hdg::sanitizeFloat(&gHeelOffsetDeg,  -180.0f, 180.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gPitchOffsetDeg, -180.0f, 180.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gHdgOffsetDeg,   -360.0f, 360.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gHdgDeclDeg,      -30.0f,  30.0f, 0.0f);
    gHdgOffsetDeg = hdg::wrap180(gHdgOffsetDeg);
    bool magBad = false;
    for (int i = 0; i < 3; ++i) magBad |= hdg::sanitizeFloat(&gMagOff[i], -500.0f, 500.0f, 0.0f);
    if (magBad) { gMagOff[0] = gMagOff[1] = gMagOff[2] = 0.0f; gMagRadius = gMagResid = 0.0f; ++fixed; }
    fixed += hdg::sanitizeFloat(&gMagRadius, 0.0f, 500.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&gMagResid,  0.0f, 500.0f, 0.0f);
    fixed += hdg::sanitizeAxes(&gHdgAxisA, &gHdgAxisB);
    if (gHeelAxis > 2)  { gHeelAxis = 1;  ++fixed; }
    if (gPitchAxis > 2) { gPitchAxis = 2; ++fixed; }
    if (fixed) printf("[SET] ★ 보드에 저장된 설정 %d개가 범위 밖이라 기본값으로 씁니다\n", fixed);
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

// ── 배터리 (0단계에서 firmware-rak 과 1 mV 차로 맞춘 방법) — 한 번 만들어 두고 NAV 에 쓴다 ──
static adc_oneshot_unit_handle_t gAdc = nullptr;
static adc_cali_handle_t gCali = nullptr;
static adc_channel_t gBattCh;
static uint16_t gBattMv = 0;      // 배터리 전압 (mV). 0 = 못 읽음

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
static void batteryRead() {
    if (!gAdc || !gCali) { gBattMv = 0; return; }
    uint32_t sum = 0; int ok = 0;
    for (int i = 0; i < 16; ++i) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(gAdc, gCali, gBattCh, &mv) == ESP_OK) { sum += (uint32_t)mv; ++ok; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!ok) { gBattMv = 0; return; }
    const float volts = (sum / (float)ok / 1000.0f) / rak::kBattDivider * rak::kBattCorrection;
    gBattMv = (uint16_t)(volts * 1000.0f + 0.5f);
}

// ── 기록 제어 — 원하는 상태(gWantRec)와 실제 상태(hlog::phase) ──────────────
static constexpr uint8_t  kResumeMax       = 5;
static constexpr uint8_t  kRecRestartMax   = 3;
static constexpr uint32_t kRecRestartGapMs = 3000;
static constexpr uint32_t kRecRestartOkMs  = 600000;

static bool     gWantRec = false;
static bool     gRecGaveUp = false;
static bool     gRecLastSaveBad = false;
static uint8_t  gRecRestarts = 0;
static uint32_t gRecRestartAt = 0;
static uint32_t gRecFailSession = 0;
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

// firmware-rak buildHeadingNote 와 같은 글자 (TXT 머리)
static void buildHeadingNote(char* out, size_t n) {
    static const char* kAx = "XYZ";
    char a[8], b[8];
    snprintf(a, sizeof a, "%c%c", gHdgSignA < 0 ? '-' : '+', kAx[gHdgAxisA < 3 ? gHdgAxisA : 0]);
    snprintf(b, sizeof b, "%c%c", gHdgSignB < 0 ? '-' : '+', kAx[gHdgAxisB < 3 ? gHdgAxisB : 0]);
    snprintf(out, n,
             "# 방위(화면·BLE·TXT): 기울기 보정(INSLIB ahrs_mag_detilt) — 축 atan2(자력 %s, 자력 %s) 기준, 중력은 그때 가속도, |a| 가 1 g ±0.15 밖이면 방위 없음. + 장착 오프셋 %+.2f° + 자기 편각 %+.2f°\n"
             "# 자력: HLG 의 mag 는 하드아이언을 뺀 값 — 뺀 오프셋 %.2f %.2f %.2f uT (반지름 %.1f, 잔차 %.2f)\n"
             "# 가속→자력 축: 자력 X=가속 Y, Y=가속 X, Z=−가속 Z\n",
             a, b, gHdgOffsetDeg, gHdgDeclDeg, gMagOff[0], gMagOff[1], gMagOff[2], gMagRadius, gMagResid);
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
    h.gnssHz    = 0;                      // ★ 1단계는 GPS 설정을 안 건다 — 모름
    h.sogSrc    = 0;
    h.quatSrc   = 1;
    h.heelAxis  = gHeelAxis;   h.heelSign  = gHeelSign < 0 ? 1 : 0;
    h.pitchAxis = gPitchAxis;  h.pitchSign = gPitchSign < 0 ? 1 : 0;
    h.heelOff   = gHeelOffsetDeg;  h.pitchOff = gPitchOffsetDeg;
    h.hdgFormula = hlog::kHdgFormulaTilt;
    h.hdgAxisA = gHdgAxisA;  h.hdgAxisB = gHdgAxisB;
    h.hdgSignA = gHdgSignA < 0 ? 1 : 0;  h.hdgSignB = gHdgSignB < 0 ? 1 : 0;
    h.hdgOff   = gHdgOffsetDeg;  h.hdgDecl = gHdgDeclDeg;
    for (int i = 0; i < 3; ++i) h.magHi[i] = gMagOff[i];
    h.gnssDyn = 255;                      // ★ 모듈에서 읽은 값만 — 1단계는 모름
    buildHeadingNote(gSessNote, sizeof gSessNote);
    hlog::setSessionNote(gSessNote);
    if (!hlog::start(h)) {
        hlog::Status st; hlog::getStatus(&st);
        gRecStartErr = st.lastError ? st.lastError : "알 수 없음";
        return false;
    }
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
                 gBattMv / 1000.0f, r.bytes / 1048576.0f, (unsigned long)r.lostBytes,
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

// 1단계에는 끄기(깊은잠)가 없다 — 7단계에서 powerOff 를 붙인다.
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

    auto setTry = [](uint8_t v) { nv::writeWith([&](nvs_handle_t w) { return nvs_set_u8(w, "rec_try", v) == ESP_OK; }); };
    switch (recctl::atBoot(p, tries, kResumeMax)) {
    case recctl::Boot::Nothing:
        if (tries) setTry(0);
        return;
    case recctl::Boot::UnclosedOnly:
        printf("[REC] 세션 %u 가 마감 안 된 채 남았습니다 — 사람이 멈춘 뒤라 이어 시작하지 않습니다\n", (unsigned)p.open);
        recSaveOpen(0);
        return;
    case recctl::Boot::TooMany:
        printf("[REC] ★ 이어 시작을 %u번 했는데 계속 끊깁니다 — 멈춥니다.\n", tries);
        printf("      전원선·배터리 접점을 보세요. rec on 으로 직접 걸 수 있습니다.\n");
        recSaveWant(false);
        setTry(0);
        gRecGaveUp = true;
        return;
    case recctl::Boot::Resume:
        break;
    }
    const uint8_t n = (uint8_t)(tries + 1);
    setTry(n);
    const uint32_t prev = p.open ? p.open : lastSess;
    printf("[REC] 지난 세션 %u 가 못 닫히고 끊겼습니다 — 이어서 시작합니다 (%u번째)\n", (unsigned)prev, n);
    gWantRec = true;
    gRecFailSession = prev;
    if (!recStartFrom("켤 때 이어 시작", prev)) gRecRestartAt = nowMs() + kRecRestartGapMs;
}

// ── NAV 줄 (1단계: GPS 없음 — 시각·전압만, 나머지는 값 없음 표식) ─────────────
static void logWriteNav(uint32_t now) {
    hlog::NavSample a;          // 기본값이 전부 "값 없음" 표식이다 (hlog.h NavSample)
    a.localMs = now;
    a.battMv  = gBattMv;
    hlog::writeNav(a);
}

static void logWriteText(uint32_t now) {
    hlog::NavSample a;
    a.localMs = now;
    a.battMv  = gBattMv;
    hlog::TextSample t;         // attOk=false · hdg=-1 · 속도 셋 -1 · pvFlag=255 — 없는 값
    hlog::writeText(a, t);
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
    printf("  IMU FIFO      2단계 전 — 아직 없음\n");
    printf("──────────────────────────────────────────\n");
    printf("  rec on / rec off / rec mark\n");
    printf("  rec ls          카드에 있는 파일 목록\n");
    printf("  rec check [번호]  보드가 직접 되읽어 검사 (기본: 마지막 세션)\n");
    printf("  rec tail [번호] [줄수]  TXT 사본 끝 몇 줄 (rec head 는 앞부분)\n");
    printf("  rec rm <번호>     그 세션의 HLG·TXT 를 지운다 (못 되돌린다)\n");
}

static void handleLine(char* line) {
    // 앞뒤 빈칸 없애고 소문자로 (firmware-rak 은 rec 인자를 toLowerCase 했다)
    while (*line == ' ') ++line;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ')) line[--n] = '\0';
    if (!n) return;
    if (!strcmp(line, "rec")) { cmdRec(""); return; }
    if (!strncmp(line, "rec ", 4)) {
        char* arg = line + 4;
        while (*arg == ' ') ++arg;
        for (char* p = arg; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        cmdRec(arg);
        return;
    }
    printf("[CMD] 모르는 명령: %s  (1단계에서 되는 것은 rec … 뿐)\n", line);
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

// ── 켤 때 점검 (0단계) — 짧게 ──────────────────────────────────────────────
static void logBoot() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    printf("═══════════════════════════════════════════\n");
    printf("  firmware-idf 1단계 (기록기) · ESP-IDF %s\n", esp_get_idf_version());
    printf("  MAC %02X:%02X:%02X:%02X:%02X:%02X · 플래시 %" PRIu32 " MB · PSRAM %u 바이트\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], flash / (1024 * 1024), (unsigned)esp_psram_get_size());
    printf("═══════════════════════════════════════════\n");
}

static void ledsInit() {
    static const int kLeds[] = {rak::kLedGreen, rak::kLedBlue};
    for (int p : kLeds) {
        gpio_set_direction(static_cast<gpio_num_t>(p), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(p), 0);
    }
}

extern "C" void app_main(void) {
    // USB 입력을 읽으려면 드라이버를 깔고, printf 도 그 드라이버로 보낸다 (usb_serial_jtag_vfs.h)
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 1024;
    ucfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&ucfg);
    usb_serial_jtag_vfs_use_driver();

    ledsInit();
    logBoot();

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
    loadSettings();

    gpio_set_direction(static_cast<gpio_num_t>(rak::kSensorPowerA), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(rak::kSensorPowerA), 1);   // GPS 전원 (2단계에서 켤 때 절차를 붙인다)

    batteryInit();
    batteryRead();
    printf("[BATT] %u mV\n", (unsigned)gBattMv);

    hlog::begin();
    resumeRecordingIfCut();
    printf("  rec 로 기록 상태 · rec on / rec off\n");

    uint32_t lastNav = nowMs(), lastText = nowMs(), lastHealth = nowMs(), lastBatt = nowMs();
    bool ledOn = false;
    for (;;) {
        const uint32_t now = nowMs();
        pollSerial();
        recControlTick(now);

        // NAV 10 Hz — 칸을 더해 나간다 (firmware-rak: lastNav += navMs, 많이 밀리면 지금부터)
        const uint32_t navMs = 1000u / hlog::kRateNav;
        if (now - lastNav >= navMs) {
            lastNav += navMs;
            if (now - lastNav >= navMs * 5) lastNav = now;
            if (hlog::recording()) logWriteNav(now);
        }
        if (hlog::recording() && now - lastText >= 10000) { lastText = now; logWriteText(now); }
        if (now - lastHealth >= 1000) { lastHealth = now; hlog::healthCheck(); }
        if (now - lastBatt >= 1000) { lastBatt = now; batteryRead(); }

        // 초록 LED — 기록 중이면 1초에 80 ms (firmware-rak loop 1d)
        const bool want = hlog::recording() && (now % 1000) < 80;
        if (want != ledOn) { ledOn = want; gpio_set_level(static_cast<gpio_num_t>(rak::kLedGreen), want); }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
