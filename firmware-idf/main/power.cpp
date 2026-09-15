// firmware-idf 7단계 — 끄기(깊은잠) · 깬 뒤 5초 문턱 · 저장 단추 · 잠자기 기록. 이름 표는 power.h.
//
// ── 아두이노 2.0.17 (IDF 4.4) 과 달라진 API ──────────────────────────────────
//   pinMode(INPUT_PULLUP)·digitalRead/Write  →  gpio_config · gpio_get_level · gpio_set_level
//   esp_sleep_enable_ext1_wakeup             →  esp_sleep_enable_ext1_wakeup_io   (6.1 헤더: 옛 이름은 "v6.0 에서 deprecated 예정")
//   esp_sleep_get_wakeup_cause() == EXT1     →  esp_sleep_get_wakeup_causes() & BIT(EXT1)   (옛 이름에 deprecated 속성, sleep_modes.c:2576)
//   Serial1.begin/available/read/end         →  uart_driver_install · uart_read_bytes · uart_driver_delete (UART1, 핀 43/44)
//   Wire.begin + sail::displayBegin          →  sail::displayBegin (버스가 없으면 imu::begin 이 만든다 — display.cpp)
//   gImu.sleep(true)                         →  imu::sleep(true)
//   SD.open("/SLEEP.TXT", FILE_APPEND)       →  fopen("/sd/SLEEP.TXT", "a")  (sdcard::acquire 가 "/sd" 에 붙인다)
//   Serial.flush()                           →  fflush(stdout) + usb_serial_jtag_wait_tx_done (드라이버가 깔렸을 때)
//   millis()/delay()                         →  esp_timer · vTaskDelay
//   RTC_DATA_ATTR · rtc_gpio_* · esp_sleep_pd_config · esp_deep_sleep_start · gettimeofday — 이름 같음 (6.1 헤더 확인)

#include "power.h"

#include <cstdio>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "board_rak.h"
#include "display.h"      // sail::display* (display_rak.h 를 같이 쓴다)
#include "hlog.h"
#include "imu.h"
#include "lora.h"
#include "rec_control.h"
#include "sdcard.h"

namespace power {
namespace {

inline uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

Hooks sH;
void feed() { if (sH.feedWatchdog) sH.feedWatchdog(); }

// ── 저장 단추 (firmware-rak 2499-2535) ─────────────────────────────────────
//   짧게(0.05~1초) 마킹 · 2초 이상 기록 시작/종료 · 5초 이상 끄기 (깊은잠)
constexpr int      kButtonPin   = rak::kAin1;   // GPIO2, J11 1번
constexpr uint32_t kBtnLongMs   = 2000;         // 기록 시작 / 종료
constexpr uint32_t kBtnOffMs    = 5000;         // 끄기
constexpr uint32_t kBtnHintMs   = 800;          // 뗀 뒤 결과를 보여주는 시간
constexpr uint32_t kBtnStuckMs  = 10000;        // 이만큼 안 떨어지면 안 끈다
constexpr uint32_t kBtnStableMs = 30;           // 값이 바뀐 순간부터 이만큼 안 흔들려야 진짜 바뀐 것

bool     gBtnRaw      = false;
uint32_t gBtnRawAt    = 0;
bool     gBtnDown     = false;
uint32_t gBtnDownAt   = 0;
bool     gBtnLongDone = false;
bool     gBtnIgnoreUntilUp = false;   // 켜자마자 손을 안 뗐으면 뗄 때까지 안 본다 (안 그러면 5초에 도로 꺼진다)
bool     gBtnStartOnUp = false;       // 2초에 예약해 두고 뗄 때 시작
bool     gBtnOwnsScreen = false;
uint32_t gBtnScreenTill = 0;
uint32_t gBtnDrawnAt    = 0;

// ── 끄기 기다림 (firmware-rak 103-104 · 5347) ──────────────────────────────
constexpr uint32_t kOffWaitMs = 15000;   // 이만큼 못 닫으면 사람에게 강제로 끌지 묻는다
uint32_t gOffRequestedAt = 0;            // 0 이면 요청 없음
bool     gOffForceArmed  = false;        // 15초 넘게 못 닫았다. 한 번 더 5초 누르면 미완료로 끈다

// ── 잠자기 기록 (RTC 메모리, firmware-rak 2555-2574) ────────────────────────
// 깊은잠은 램을 다 날리지만 RTC 영역은 남긴다. 리셋은 못 견딘다 → 카드에도 남긴다 (sleepLogToCard).
// 시각은 gettimeofday — RTC 시계 위에 얹혀 자는 동안에도 간다
// [확인: firmware-idf/sdkconfig CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y]. 차이만 쓴다.
uint64_t nowUs() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
RTC_DATA_ATTR uint32_t gRtcMagic;
RTC_DATA_ATTR uint32_t gRtcSleeps;      // 잠든 횟수
RTC_DATA_ATTR uint32_t gRtcFalse;       // 5초 못 채우고 도로 잔 횟수
RTC_DATA_ATTR uint32_t gRtcFull;        // 5초 채워 켜진 횟수
RTC_DATA_ATTR uint64_t gRtcSleepUs;     // 마지막으로 잠든 RTC 시각
RTC_DATA_ATTR uint32_t gRtcSleepMv;     // 그때 배터리 mV
RTC_DATA_ATTR uint64_t gRtcSleptUs;     // 마지막으로 잔 시간
RTC_DATA_ATTR uint32_t gRtcGpsBytes;    // 깨자마자 GPS 가 뱉은 바이트 수
RTC_DATA_ATTR uint32_t gRtcTestSec;     // 시험용 타이머 (0 이면 안 씀)
bool gWokeFromSleep = false;
constexpr uint32_t kRtcMagic = 0x5A5AC0DE;

// 켤 때 배터리 ADC 가 몇 번째부터 제 값을 내는지 담아 둔다. `battboot` 가 꺼낸다.
constexpr int kBattBootN = 20;
uint32_t gBattBoot[kBattBootN];          // 핀 mV (firmware-rak readBatteryVolts 의 rawMvOut 자리)

bool buttonLow() { return gpio_get_level(static_cast<gpio_num_t>(kButtonPin)) == 0; }

void buttonInputPullup() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << kButtonPin;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

// 누른 시간을 막대 퍼센트로. 5초를 가득 찬 것으로 본다.
int btnPct(uint32_t heldMs) {
    if (heldMs >= kBtnOffMs) return 100;
    return (int)(heldMs * 100 / kBtnOffMs);
}

// 자는 동안 버튼이 보드를 깨울 수 있게 걸어 둔다 (firmware-rak armButtonWake 3044).
// ★ 잠드는 **모든** 길에서 부른다. 헛깨서 도로 자는 길도.
// ★ RTC 주변장치를 켜 둔 채로 잔다 — 안 그러면 풀업이 잠드는 순간 사라져 GPIO2 가 떠서
//   570초에 445번 깼다 (2026-09-08, POWER.md §1).
void armButtonWake() {
    const gpio_num_t btn = static_cast<gpio_num_t>(kButtonPin);
    rtc_gpio_init(btn);
    rtc_gpio_set_direction(btn, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(btn);
    rtc_gpio_pullup_en(btn);
    // ★ 이 줄이 빠지면 위의 풀업이 잠드는 순간 사라진다.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io(1ULL << kButtonPin, ESP_EXT1_WAKEUP_ANY_LOW);
}

void flushConsole() {
    fflush(stdout);
    if (usb_serial_jtag_is_driver_installed()) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
}

// 잠자기 기록을 SD 카드에도 남긴다 (firmware-rak sleepLogToCard 2735)
void sleepLogToCard() {
    if (gRtcMagic != kRtcMagic || gRtcSleptUs == 0) return;
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) return;     // 카드는 사용권으로 쥔다 (sdcard.h)
    FILE* f = fopen("/sd/SLEEP.TXT", "a");
    if (!f) { sdcard::release(sdcard::Owner::Diagnostic); return; }
    const double sec = (double)gRtcSleptUs / 1e6;
    // ★ 깰 때 전압과 거기서 나온 낙차는 안 적는다. 못 믿는 값이다.
    fprintf(f, "잔시간 %.1fs  잘때 %umV  GPS바이트 %u  잠든횟수 %u  헛깸 %u  켜짐 %u\n",
            sec, (unsigned)gRtcSleepMv, (unsigned)gRtcGpsBytes, (unsigned)gRtcSleeps,
            (unsigned)gRtcFalse, (unsigned)gRtcFull);
    fclose(f);
    sdcard::release(sdcard::Owner::Diagnostic);
}

// 깨자마자, 전원을 다시 넣기 전에 GPS 선을 400 ms 들어 본다 (firmware-rak wakeGate 3303-3310)
//   말이 바로 나온다 → 자는 동안 3V3_S 가 안 꺼졌다 · 조용하다 → 꺼져 있었다
// ★ 끝나면 UART1 드라이버를 지운다. gps::begin 은 이미 깔려 있으면 다시 안 깔기 때문이다 (gps.h 달라진 점 1).
void countGpsBytesAfterWake() {
    gRtcGpsBytes = 0;
    constexpr uart_port_t port = UART_NUM_1;
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;                 // firmware-rak kGpsBaud
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    if (uart_driver_install(port, 1024, 0, 0, nullptr, 0) != ESP_OK) return;
    uart_param_config(port, &cfg);
    uart_set_pin(port, rak::kUART1_TX, rak::kUART1_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uint8_t buf[128];
    const uint32_t g0 = millis32();
    while (millis32() - g0 < 400) {
        int n;
        while ((n = uart_read_bytes(port, buf, sizeof buf, 0)) > 0) gRtcGpsBytes += (uint32_t)n;
        delayMs(5);
    }
    uart_driver_delete(port);
}

} // namespace

void setHooks(const Hooks& h) { sH = h; }
bool wokeFromSleep() { return gWokeFromSleep; }
bool buttonOwnsScreen() { return gBtnOwnsScreen; }

// ── 깨어난 직후 — 5초를 채웠나 (firmware-rak wakeGate 3253) ─────────────────
// app_main 의 **맨 앞**에서 부른다. NVS 에 쓰기 전 (주머니에서 눌릴 때마다 플래시가 닳는다),
// 워치독을 30초로 걸기 전 (5초를 세는 동안 물어뜯긴다).
void wakeGate() {
    const bool fromSleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP);

    // 붙들어 둔 핀부터 푼다. **이게 없으면 깨어나도 센서가 영영 안 켜진다.**
    // 배터리 핀(GPIO1)까지 같이 풀어야 한다 — 안 풀면 깨어난 직후 분압비가 0.6 에서 0.34 로 바뀐다 (2026-09-08).
    if (fromSleep) {
        rtc_gpio_force_hold_dis_all();
        static const int kPins[] = {rak::kSensorPowerA, rak::kSensorPowerB, kButtonPin,
                                    rak::kBattAdcPin, rak::kGpsPpsSlotA,
                                    rak::kSPI_CS, rak::kSPI_CLK, rak::kSPI_MOSI};
        for (int pin : kPins) {
            const gpio_num_t g = static_cast<gpio_num_t>(pin);
            rtc_gpio_hold_dis(g);
            rtc_gpio_deinit(g);      // 다시 보통 GPIO 로 돌려 놓는다
        }
    }

    // 잔 시간은 여기서 잰다. 아래로 내려가면 늦다.
    if (fromSleep && gRtcMagic == kRtcMagic) {
        gRtcSleptUs = nowUs() - gRtcSleepUs;
        gWokeFromSleep = true;
        // ★ 전압은 여기서 안 잰다. ADC 가 준비되기 전이라 3504 mV 로 잠든 보드가 2091 mV 로 나온다.
        countGpsBytesAfterWake();
    }

    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) return;

    // 화면만 먼저 켠다. I2C 는 3V3_S 와 무관하다 (VDD).
    sail::displayBegin();

    buttonInputPullup();
    delayMs(20);

    const uint32_t t0 = millis32();
    uint32_t upSince = 0;          // HIGH 가 된 시각. 0 이면 계속 눌려 있다
    while (true) {
        const uint32_t held = millis32() - t0;
        if (held >= kBtnOffMs) break;               // 5초 채웠다. 켠다

        if (buttonLow()) {
            upSince = 0;                            // 튐이었다. 계속 센다
        } else {
            if (upSince == 0) upSince = millis32();
            // 30ms 이어져야 진짜 뗀 것이다. 안 걷어내면 손가락이 1ms 튈 때마다 취소된다.
            if (millis32() - upSince >= kBtnStableMs) {
                gRtcFalse += 1;      // 5초 못 채웠다. 도로 잔다
                sail::displayOff();
                armButtonWake();
                // ★ 시험 타이머도 다시 건다. 안 그러면 한 번 헛깨는 순간 타이머가 사라져 영영 안 깬다.
                if (gRtcTestSec) esp_sleep_enable_timer_wakeup((uint64_t)gRtcTestSec * 1000000ull);
                esp_deep_sleep_start();
            }
        }

        sail::displayHoldBar(btnPct(held), "켜는 중", "놓으면 취소");
        delayMs(50);
    }

    gRtcFull += 1;
    sail::displayNotice("켜집니다", nullptr);
}

// ── 단추 (firmware-rak buttonBegin 2588 · buttonPoll 2614) ─────────────────
void buttonBegin() {
    buttonInputPullup();
    delayMs(20);
    gBtnRaw = buttonLow();
    gBtnRawAt = millis32();
    gBtnDown = gBtnRaw;
    if (gBtnRaw) {
        // 5초 눌러 켠 직후가 거의 다다. 뗄 때까지 버튼을 안 본다 — 안 그러면 5초에 도로 꺼진다.
        gBtnIgnoreUntilUp = true;
        printf("[BTN] GPIO%d 이 LOW 입니다 — 손을 뗄 때까지 버튼을 안 봅니다.\n", kButtonPin);
    } else {
        printf("[BTN] 저장 버튼 GPIO%d (J11 헤더 1번 AIN1 - GND). 짧게=마킹, 2초=시작/종료, 5초=끄기\n", kButtonPin);
    }
}

void buttonPoll(uint32_t nowMs) {
    // ── 튐을 먼저 걷어낸다 ──
    const bool raw = buttonLow();
    if (raw != gBtnRaw) { gBtnRaw = raw; gBtnRawAt = nowMs; }
    const bool settled = (nowMs - gBtnRawAt >= kBtnStableMs);

    // 켤 때 눌린 손을 아직 안 뗐으면 아무것도 안 한다.
    if (gBtnIgnoreUntilUp) {
        if (settled && !gBtnRaw) {
            gBtnIgnoreUntilUp = false;
            printf("[BTN] 손을 뗐습니다 — 이제부터 버튼을 봅니다\n");
        }
        return;
    }

    // 뗀 뒤 결과 글자를 보여주는 시간이 끝났으면 화면을 돌려준다.
    if (gBtnOwnsScreen && !gBtnDown && nowMs >= gBtnScreenTill) gBtnOwnsScreen = false;

    if (settled && gBtnRaw && !gBtnDown) {
        gBtnDown = true;
        // 눌리기 시작한 시각은 **처음 바뀐 그 순간**. 30ms 뒤로 잡으면 "2초 길게" 가 매번 늦어진다.
        gBtnDownAt = gBtnRawAt;
        gBtnLongDone = false;
        gBtnStartOnUp = false;
        return;
    }

    // 누르고 있는 동안 2초가 지나면 그 자리에서 먹는다.
    if (gBtnDown && !gBtnLongDone && nowMs - gBtnDownAt >= kBtnLongMs) {
        gBtnLongDone = true;
        if (hlog::recording()) {
            // 멈추라고 요청만 한다. 닫기는 일꾼이 하고 루프는 안 멈춘다 (닫는 동안 1줄에 SAVING).
            gBtnOwnsScreen = true;
            sail::displayHoldBar(btnPct(nowMs - gBtnDownAt), "기록 종료", "저장 중");
            printf("[BTN] 길게 — 기록 종료\n");
            if (sH.recWantOff) sH.recWantOff("단추");
        } else {
            // 아직 시작하지 않는다. 5초를 넘기면 끄기로 갈 사람일 수 있다.
            gBtnStartOnUp = true;
            printf("[BTN] 길게 — 놓으면 기록 시작 (더 누르면 끄기)\n");
        }
        return;
    }

    // 누르고 있는 동안 화면에 막대를 그린다. 2초부터.
    // ★ gBtnRaw 가 없으면 놓아도 이 안에서 return 해서 아래 "놓음" 에 못 간다 (2026-09-13).
    if (gBtnDown && gBtnRaw && nowMs - gBtnDownAt >= kBtnLongMs) {
        const uint32_t held = nowMs - gBtnDownAt;

        if (held >= kBtnOffMs) {
            gBtnStartOnUp = false;      // 예약해 둔 기록 시작을 버린다
            gBtnOwnsScreen = true;
            sail::displayHoldBar(100, "끄는 중", nullptr);
            // 기록이 닫혀 있으면 여기서 잠든다. 닫는 중이면 닫힌 뒤 루프가 재운다.
            // 15초 넘게 못 닫아 "5초 더 누르면 끔" 을 띄운 뒤라면 이번 누름이 강제 끄기다.
            requestPowerOff(gOffForceArmed);
            // 버튼이 안 떨어져서 못 끄고 돌아왔거나, 닫기를 기다리는 중이다.
            gBtnOwnsScreen = false;
            gBtnLongDone = true;
            gBtnIgnoreUntilUp = true;
            return;
        }

        if (nowMs - gBtnDrawnAt >= 100) {   // 10 Hz
            gBtnDrawnAt = nowMs;
            gBtnOwnsScreen = true;
            sail::displayHoldBar(btnPct(held), "누르면 꺼짐", gBtnStartOnUp ? "놓으면 기록시작" : "기록 멈춤");
        }
        return;
    }

    if (settled && !gBtnRaw && gBtnDown) {
        const uint32_t held = gBtnRawAt - gBtnDownAt;
        gBtnDown = false;

        if (gBtnStartOnUp) {
            gBtnStartOnUp = false;
            printf("[BTN] 놓음(%ums) — 기록 시작\n", (unsigned)held);
            // ★ 사람의 시작 요청은 한 진입점(recWantOn)으로 (2026-09-15 검토 1번).
            const bool okS = sH.recWantOn ? sH.recWantOn("단추") : false;
            gBtnOwnsScreen = true;
            // ★ 결과대로 보여준다. 실패는 3초.
            gBtnScreenTill = millis32() + (okS ? kBtnHintMs : 3000);
            const char* why = sH.recStartErrShort ? sH.recStartErrShort() : nullptr;
            sail::displayNotice(okS ? "기록 시작" : "기록 실패", okS ? nullptr : (why ? why : "원인 모름"));
            return;
        }
        if (gBtnLongDone) {                       // 2초에 이미 멈췄다
            gBtnOwnsScreen = true;
            gBtnScreenTill = nowMs + kBtnHintMs;
            sail::displayNotice("기록 종료", nullptr);
            return;
        }
        if (hlog::recording()) {
            printf("[BTN] 짧게(%ums) — 마킹\n", (unsigned)held);
            hlog::mark();
        } else {
            printf("[BTN] 짧게 — 기록 중이 아닙니다 (2초 누르면 시작)\n");
        }
    }
}

// ── 끄기 요청 (firmware-rak requestPowerOff 3640) ──────────────────────────
void requestPowerOff(bool force) {
    if (sH.pollRecResult) sH.pollRecResult();
    if (force) {
        // 사람이 두 번째로 고른 길. 세션·사유·재개 취소를 NVS 에 남기고, 카드와 일꾼은 건드리지 않고 잔다.
        hlog::Status st; hlog::getStatus(&st);
        char line[160];
        snprintf(line, sizeof line, "세션 %u 닫기가 안 끝난 채 사람이 강제로 껐습니다 (미완료)", (unsigned)st.session);
        nvs_handle_t h;
        if (nvs_open("sail", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "rec_want", 0);
            nvs_set_u32(h, "rec_forced", st.session);
            nvs_set_str(h, "rec_fail", line);
            nvs_commit(h);
            nvs_close(h);
        }
        printf("[SLEEP] ★ %s\n", line);
        if (sH.clearWantFlag) sH.clearWantFlag();
        gOffRequestedAt = 0; gOffForceArmed = false;
        goToSleep(0, true);
        return;
    }
    if (hlog::phase() == recctl::Phase::Recording) {
        if (sH.recWantOff) sH.recWantOff("끄기");
    } else if (sH.dropWantIfSet) {
        sH.dropWantIfSet();
    }
    if (hlog::phase() == recctl::Phase::Idle) { gOffRequestedAt = 0; goToSleep(); return; }
    if (!gOffRequestedAt) {
        gOffRequestedAt = millis32();
        gOffForceArmed = false;
        printf("[SLEEP] 기록을 닫는 중 — 닫히면 끕니다\n");
    }
}

// recControlTick 의 끄기 칸 (firmware-rak 5463-5483). 결과 받기(hlog::poll) 뒤에 부른다.
bool offTick(uint32_t nowMs) {
    if (!gOffRequestedAt) return false;
    switch (recctl::powerOff(hlog::phase(), nowMs - gOffRequestedAt, kOffWaitMs)) {
    case recctl::Off::SleepNow:
        gOffRequestedAt = 0;
        gOffForceArmed = false;
        goToSleep();                // 버튼이 안 떨어졌으면 돌아온다
        return true;
    case recctl::Off::Wait:
        return true;
    case recctl::Off::AskForce:
        if (!gOffForceArmed) {
            gOffForceArmed = true;
            printf("[SLEEP] ★ 15초 넘게 파일이 안 닫힙니다. 기다리면 닫힌 뒤 꺼집니다.\n");
            printf("        지금 끄려면 단추를 5초 더 누르세요 (시리얼: off force). 끝이 잘립니다.\n");
            gBtnOwnsScreen = true;
            gBtnScreenTill = nowMs + 600000;
            sail::displayNotice("저장 안 끝남", "5초 더 누르면 끔");
        }
        return true;
    }
    return true;
}

// ── 끄기 (깊은잠, firmware-rak goToSleep 3091) ─────────────────────────────
// 정상이면 **안 돌아온다.** 돌아오는 경우는 버튼이 안 떨어질 때 하나뿐이다.
// 끄는 순서 (POWER.md §3, 바꾸면 안 된다): 기록 닫힘 → SD 놓기 → 무전기 → IMU → 화면 → 손 뗌 → 3V3_S 내리고 붙듦
// ★ BLE·WiFi 는 firmware-rak 도 따로 안 내렸다 — 깊은잠이 통째로 끈다. 그대로 둔다.
void goToSleep(uint32_t testWakeSec, bool leaveCard) {
    printf("[SLEEP] 끕니다\n");

    // 1) 기록은 여기 오기 전에 닫혀 있다. 강제 끄기거나 아직 닫는 중이면 카드를 안 뗀다 (2026-09-14 리뷰).
    if (leaveCard || hlog::busy()) {
        printf("[SLEEP] ★ 기록 파일이 아직 안 닫혔습니다 — 카드는 안 뗍니다 (미완료 세션)\n");
    } else {
        // 1b) 카드는 VDD 라 3V3_S 를 내려도 안 꺼진다. SPI 를 놓아준다.
        sdcard::endForSleep();
    }

    // 2) 무전기. 안 재우면 자는 동안 혼자 듣느라 수 mA 를 먹는다.
    lora::sleep();

    // 3) IMU. VDD 라 3V3_S 를 내려도 안 꺼진다.
    if (imu::ok()) imu::sleep(true);

    // 4) 화면. 이것도 VDD. 끄기 전에 켜는 법을 적어 둔다.
    sail::displayNotice("꺼졌습니다", "5초 눌러 켜기");

    // 5) 손을 뗄 때까지 기다린다. 눌린 채로 자면 ANY_LOW 가 이미 참이라 잠들자마자 깬다.
    const uint32_t t0 = millis32();
    bool shownFor = false;
    while (true) {
        feed();
        if (!buttonLow()) {
            delayMs(kBtnStableMs);   // 30ms 안 흔들려야 진짜 뗀 것이다
            if (!buttonLow()) break;
        }
        if (millis32() - t0 > kBtnStuckMs) {
            // 물이 접점을 붙들고 있는 것이다. **켜져 있는 쪽이 안전하다.**
            printf("[SLEEP] 버튼이 안 떨어집니다 — 끄지 않고 그대로 돕니다\n");
            sail::displayNotice("버튼이 눌린 채", "끄지 않습니다");
            if (imu::ok()) imu::sleep(false);
            delayMs(1500);
            return;
        }
        if (!shownFor && millis32() - t0 > 1500) {
            shownFor = true;
            sail::displayNotice("손을 떼세요", nullptr);
        }
        delayMs(20);
    }
    if (!shownFor) delayMs(1200);   // "꺼졌습니다" 를 읽을 시간
    sail::displayOff();

    gpio_set_level(static_cast<gpio_num_t>(rak::kLedGreen), 0);
    gpio_set_level(static_cast<gpio_num_t>(rak::kLedBlue), 0);

    // 6) 센서 전원 3V3_S 를 내리고 잠자는 동안 붙든다.
    //    ★ RTC 쪽 API. gpio_hold_en 은 S3 깊은잠 중 디지털 GPIO 를 못 붙든다 (IDF 6.1 gpio.h:439 도 같은 말). GPIO14 는 RTC 핀.
    const gpio_num_t pwr = static_cast<gpio_num_t>(sH.sensorPowerPin);
    rtc_gpio_init(pwr);
    rtc_gpio_set_direction(pwr, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(pwr, 0);
    rtc_gpio_hold_en(pwr);

    // 떠 있는 RTC 핀을 떼어 놓는다 — GPIO1 배터리 분압 마디 · GPIO21 GPS PPS (rtc_io.h rtc_gpio_isolate)
    static const int kIsolate[] = {rak::kBattAdcPin, rak::kGpsPpsSlotA};
    for (int pin : kIsolate) rtc_gpio_isolate(static_cast<gpio_num_t>(pin));

    // SD SPI 선: CS 는 HIGH 로 붙들어 카드를 떼어 놓고, 클럭·MOSI 는 LOW 로 눕힌다. MISO 는 안 건드린다 (POWER.md §5)
    struct Rest { int pin; int level; };
    static const Rest kSpiRest[] = {
        {rak::kSPI_CS,   1},   // 카드를 떼어 놓는다. 이게 핵심이다
        {rak::kSPI_CLK,  0},
        {rak::kSPI_MOSI, 0},
    };
    for (const Rest& r : kSpiRest) {
        const gpio_num_t g = static_cast<gpio_num_t>(r.pin);
        rtc_gpio_init(g);
        rtc_gpio_set_direction(g, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(g, r.level);
        rtc_gpio_hold_en(g);
    }

    armButtonWake();
    gRtcTestSec = testWakeSec;
    if (testWakeSec) {
        esp_sleep_enable_timer_wakeup((uint64_t)testWakeSec * 1000000ull);
        printf("[SLEEP] 시험 모드 — %u초 뒤에 스스로 깹니다\n", (unsigned)testWakeSec);
    }

    // RTC 메모리에 적어 둔다. 깨어난 뒤에 진짜로 잤는지 물어볼 유일한 방법이다.
    gRtcMagic   = kRtcMagic;
    gRtcSleeps += 1;
    gRtcSleepMv = (uint32_t)((sH.readBatteryVolts ? sH.readBatteryVolts() : 0.0f) * 1000.0f);
    gRtcSleepUs = nowUs();

    printf("[SLEEP] 잘 자라. 5초 누르면 깬다. (%u번째, %u mV)\n", (unsigned)gRtcSleeps, (unsigned)gRtcSleepMv);
    flushConsole();
    esp_deep_sleep_start();
}

// ── 잠자기 기록 보기 ───────────────────────────────────────────────────────
diag::SleepStats sleepStats() {
    diag::SleepStats st;
    st.everSlept  = (gRtcMagic == kRtcMagic);
    st.sleeps     = gRtcSleeps;
    st.falseWakes = gRtcFalse;
    st.fullWakes  = gRtcFull;
    st.sleptUs    = gRtcSleptUs;
    st.sleepMv    = gRtcSleepMv;
    st.gpsBytes   = gRtcGpsBytes;
    return st;
}

// firmware-rak diagnostics.cpp sleepReport 와 같은 글자
void sleepReport() {
    const diag::SleepStats st = sleepStats();
    printf("──────────────────────────────────────────\n");
    if (!st.everSlept) {
        printf("  아직 한 번도 안 잤습니다 (off 명령이나 버튼 5초)\n");
        printf("──────────────────────────────────────────\n");
        return;
    }
    printf("  잠든 횟수         %u\n", (unsigned)st.sleeps);
    printf("  헛깸(5초 못 채움) %u   ← 크면 버튼 핀이 뜨는 것이다\n", (unsigned)st.falseWakes);
    printf("  5초 채워 켜짐     %u\n", (unsigned)st.fullWakes);
    if (st.sleptUs == 0) {
        printf("  아직 깨어난 적이 없습니다.\n");
        printf("──────────────────────────────────────────\n");
        return;
    }
    const double sec = (double)st.sleptUs / 1e6;
    printf("  마지막으로 잔 시간 %.1f 초 (%.2f 시간)\n", sec, sec / 3600.0);
    // ★ 깰 때 전압은 안 보여준다 — 깬 직후 값은 못 믿는다 (POWER.md 맨 앞). 보여주면 내가 그 위에 또 쌓는다.
    printf("  잘 때 %u mV\n", (unsigned)st.sleepMv);
    printf("  깰 때  --   (깬 직후 값은 못 믿는다. 원인 미상)\n");
    printf("  깨자마자 GPS 가 뱉은 바이트  %u\n", (unsigned)st.gpsBytes);
    printf("%s\n", st.gpsBytes > 0
        ? "  ★ 0 이 아닙니다 — 자는 동안 3V3_S 가 안 꺼졌습니다. GPS 가 계속 돌았습니다."
        : "  0 입니다 — 3V3_S 는 제대로 꺼져 있었습니다.");
    printf("  ※ 시간당 낙차는 안 계산한다. 깰 때 값을 못 믿으므로\n");
    printf("    거기서 나오는 숫자도 못 믿는다. 잠자기 전류를 재려면\n");
    printf("    멀티미터를 배터리 선에 물려야 한다 (POWER.md).\n");
    printf("  ※ ADC 소스 임피던스가 2.5 MΩ 라 mV 단위는 흔들립니다.\n");
    printf("  ※ USB 를 꽂은 채로 재면 충전 때문에 값이 무의미합니다.\n");
    printf("──────────────────────────────────────────\n");
}

// setup 의 "깊은잠에서 깼으면 그 기록부터 남긴다" (firmware-rak 5603). hlog::begin 뒤에 부른다.
void reportAfterBoot() {
    if (gRtcMagic == kRtcMagic && gRtcSleptUs != 0) {
        sleepReport();
        sleepLogToCard();
    }
}

// ── 켠 뒤 20초 동안 배터리 값을 1초마다 (firmware-rak loop 1f · battboot) ────
// firmware-rak 은 readBatteryVolts(&raw) 로 **핀 mV** 를 담았다. 메인 훅은 배터리 V 만 주므로
// 핀 mV = V × kBattDivider ÷ kBattCorrection 으로 되돌린다 (board_rak.h kBattCorrection 1.0 이라 같은 값).
void battBootTick(uint32_t nowMs) {
    static int curveN = 0;
    static uint32_t curveAt = 0;
    if (curveN < kBattBootN && nowMs - curveAt >= 1000) {
        curveAt = nowMs;
        const float v = sH.readBatteryVolts ? sH.readBatteryVolts() : 0.0f;
        gBattBoot[curveN] = (uint32_t)(v * rak::kBattDivider / rak::kBattCorrection * 1000.0f + 0.5f);
        curveN++;
    }
}

void battBootReport(float battVoltsNow) {
    printf("──────────────────────────────────────────\n");
    printf("  켠 뒤 1초마다 담은 배터리 ADC 값\n");
    for (int i = 0; i < kBattBootN; i++) {
        if (!gBattBoot[i]) continue;
        printf("  %2d초  핀 %4u mV  →  배터리 %.3f V\n", i + 1, (unsigned)gBattBoot[i],
               (gBattBoot[i] / 1000.0f) / rak::kBattDivider);
    }
    printf("  지금     핀 %4u mV  →  배터리 %.3f V\n",
           (unsigned)(battVoltsNow * rak::kBattDivider * 1000.0f), battVoltsNow);
    printf("──────────────────────────────────────────\n");
}

} // namespace power
