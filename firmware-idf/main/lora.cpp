// firmware-idf 6단계 — LoRa (RAK3112 안에 든 SX1262). API 는 firmware-rak/include/lora.h 를 **같이 쓴다** (아두이노 흔적 0, PORTING.md 규칙 4).
//
// firmware-rak/src/lora.cpp (커밋 2b18b17) 를 뜻·값·로그 글자 그대로 옮겼다. 달라진 것은 아래 둘뿐이다.
//
//   1) RadioLib 이 쓰는 하드웨어 층(HAL)을 우리가 짰다 — IdfHal.
//      RadioLib 7.7.1 이 주는 ESP-IDF 예제 HAL(examples/NonArduino/ESP-IDF/main/EspHal.h)은
//      `#error This example HAL only supports ESP32 targets` 로 S3 를 막고, SPI2 레지스터를 직접 만진다.
//      SPI2 는 SD 카드가 쓰는 버스(sdcard_idf.cpp SPI2_HOST)라 그대로 쓰면 카드와 부딪친다.
//      그래서 driver/spi_master.h 로 **SPI3_HOST** 에 붙인다. firmware-rak 의 `SPIClass gSpi(HSPI)` 가
//      S3 에서 SPI3 이었다 [확인: framework-arduinoespressif32 esp32-hal-spi.h:30 HSPI=1, esp32-hal-spi.c DR_REG_SPI3_BASE].
//      속도·모드는 RadioLib 이 아두이노에서 쓰던 기본값 2 MHz · 모드 0 · MSB 먼저 [확인: RadioLib BuildOpt.h:183].
//      칩셀렉트(CS)는 아두이노 때처럼 RadioLib 이 digitalWrite 로 직접 내리고 올린다 [확인: RadioLib Module.cpp:218-221] —
//      그래서 SPI 장치에는 CS 핀을 안 준다(spics_io_num = -1).
//   2) Serial.printf → printf, millis/micros → esp_timer, delay → vTaskDelay.
//
// 칩 버그 셋(데이터시트 15.1·15.2·15.3)은 RadioLib 이 알아서 건다 — HAL 과 무관한 SX126x 코드라 판이 같으면 똑같이 걸린다.
// `lora regs` 가 레지스터를 되읽어 보여준다 (memory lora-chip-errata).

#include "lora.h"

#include <cstdio>
#include <cstring>

#include <RadioLib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_rak.h"

namespace lora {
namespace {

inline uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline uint32_t micros32() { return (uint32_t)esp_timer_get_time(); }

// ── RadioLib 하드웨어 층 (IDF) ───────────────────────────────────────────
//
// 아두이노 ArduinoHal 과 같은 뜻으로 맞춘다.
//   pinMode       아두이노 pinMode 처럼 인터럽트 종류는 그대로 둔다 (다시 불러도 DIO1 인터럽트가 안 꺼진다)
//   attachInterrupt  아두이노 2.0.17 과 같이 gpio ISR 서비스(플래그 0)에 핸들러를 건다
//                    [확인: esp32-hal.h:58-61 ARDUINO_ISR_FLAG — CONFIG_ARDUINO_ISR_IRAM 이 S3 sdkconfig 에 없어 0]
//   yield         아두이노 ESP32 yield() 자리
class IdfHal : public RadioLibHal {
  public:
    IdfHal(int sck, int miso, int mosi)
        : RadioLibHal(GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, 0, 1, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE),
          sck_(sck), miso_(miso), mosi_(mosi) {
        for (auto& t : intrType_) t = GPIO_INTR_DISABLE;
    }

    void init() override { spiBegin(); }
    void term() override { spiEnd(); }

    void pinMode(uint32_t pin, uint32_t mode) override {
        if (pin == RADIOLIB_NC || pin >= kPins) return;
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << pin;
        c.mode         = (gpio_mode_t)mode;
        c.pull_up_en   = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        c.intr_type    = intrType_[pin];
        gpio_config(&c);
    }
    void digitalWrite(uint32_t pin, uint32_t value) override {
        if (pin == RADIOLIB_NC) return;
        gpio_set_level((gpio_num_t)pin, value);
    }
    uint32_t digitalRead(uint32_t pin) override {
        if (pin == RADIOLIB_NC) return 0;
        return (uint32_t)gpio_get_level((gpio_num_t)pin);
    }

    void attachInterrupt(uint32_t pin, void (*cb)(void), uint32_t mode) override {
        if (pin == RADIOLIB_NC || pin >= kPins) return;
        const esp_err_t e = gpio_install_isr_service(0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = 이미 깔려 있음
            printf("[LORA] 인터럽트 서비스를 못 깔았습니다 (%s)\n", esp_err_to_name(e));
            return;
        }
        intrType_[pin] = (gpio_int_type_t)mode;
        gpio_set_intr_type((gpio_num_t)pin, intrType_[pin]);
        gpio_isr_handler_remove((gpio_num_t)pin);
        gpio_isr_handler_add((gpio_num_t)pin, &IdfHal::isrTrampoline, (void*)cb);
        gpio_intr_enable((gpio_num_t)pin);
    }
    void detachInterrupt(uint32_t pin) override {
        if (pin == RADIOLIB_NC || pin >= kPins) return;
        gpio_isr_handler_remove((gpio_num_t)pin);
        intrType_[pin] = GPIO_INTR_DISABLE;
        gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    }

    void delay(RadioLibTime_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
    void delayMicroseconds(RadioLibTime_t us) override { esp_rom_delay_us((uint32_t)us); }
    RadioLibTime_t millis() override { return (RadioLibTime_t)(esp_timer_get_time() / 1000); }
    RadioLibTime_t micros() override { return (RadioLibTime_t)esp_timer_get_time(); }
    void yield() override { taskYIELD(); }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override {
        if (pin == RADIOLIB_NC) return 0;
        pinMode(pin, GPIO_MODE_INPUT);
        const RadioLibTime_t start = micros();
        while (digitalRead(pin) == state) {
            if (micros() - start > timeout) return 0;
        }
        return (long)(micros() - start);
    }

    void spiBegin() override {
        if (dev_) return;   // `lora on` 으로 다시 begin 해도 버스를 두 번 안 만든다
        spi_bus_config_t bus = {};
        bus.mosi_io_num     = mosi_;
        bus.miso_io_num     = miso_;
        bus.sclk_io_num     = sck_;
        bus.quadwp_io_num   = -1;
        bus.quadhd_io_num   = -1;
        bus.max_transfer_sz = 512;
        esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) { printf("[LORA] SPI3 버스를 못 열었습니다 (%s)\n", esp_err_to_name(e)); return; }
        spi_device_interface_config_t d = {};
        d.mode           = 0;
        d.clock_speed_hz = 2000000;
        d.spics_io_num   = -1;     // CS 는 RadioLib 이 digitalWrite 로
        d.queue_size     = 1;
        e = spi_bus_add_device(SPI3_HOST, &d, &dev_);
        if (e != ESP_OK) {
            printf("[LORA] SPI3 장치를 못 붙였습니다 (%s)\n", esp_err_to_name(e));
            dev_ = nullptr;
            spi_bus_free(SPI3_HOST);
        }
    }
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
        if (!dev_ || len == 0) {
            if (in) memset(in, 0, len);
            return;
        }
        spi_transaction_t t = {};
        t.length    = len * 8;
        t.tx_buffer = out;
        t.rx_buffer = in;
        if (spi_device_polling_transmit(dev_, &t) != ESP_OK && in) memset(in, 0, len);
    }
    void spiEndTransaction() override {}
    void spiEnd() override {
        if (!dev_) return;
        spi_bus_remove_device(dev_);
        dev_ = nullptr;
        spi_bus_free(SPI3_HOST);
    }

  private:
    static constexpr uint32_t kPins = GPIO_NUM_MAX;
    static void IRAM_ATTR isrTrampoline(void* arg) { ((void (*)(void))arg)(); }

    int sck_, miso_, mosi_;
    spi_device_handle_t dev_ = nullptr;
    gpio_int_type_t intrType_[GPIO_NUM_MAX];
};

// ── 전파 설정 (PROTOCOL.md §10.2) ────────────────────────────────────────
//
// 값 하나하나가 §10 의 계산에 물려 있다. 여기를 바꾸면 거기도 바꿔야 한다.
constexpr float   kFreqMHz   = 922.55f; // 4번 함대 (§10.12)
constexpr float   kBwKHz     = 500.0f;
constexpr uint8_t kSf        = 7;
constexpr uint8_t kCr        = 5;       // 4:5. RadioLib 은 분모를 받는다
constexpr uint8_t kSyncWord  = 0x12;    // private. 레지스터에는 0x1424 로 들어간다
constexpr uint8_t kPreamble  = 8;

// 송신 8 dBm + 안테나 2 dBi = EIRP 10 dBm = 10 ㎽.
// **안테나 이득을 포함한 값이 규제 대상이다** (§10.8). 안테나를 바꾸면 여기도 바꾼다.
constexpr int8_t  kTxDbm     = 8;

// 링버퍼. 30척이 1초에 한 번 보내니 한 프레임치보다 넉넉하면 된다.
// 64개면 루프가 2초를 통째로 멈춰도 안 흘린다.
constexpr size_t kRingLen = 64;

// ★ HAL 을 무전기보다 먼저 만든다 (같은 파일 안 전역은 적힌 순서대로 만들어진다).
IdfHal gHal(rak::kLoraSck, rak::kLoraMiso, rak::kLoraMosi);

// RadioLib 은 readRegister 를 protected 로 감춰 뒀다. 설정이 실제로 들어갔는지
// **되읽어서 눈으로 봐야** 하므로 (reportRegs) 한 겹만 열어 준다.
class SX1262Probe : public SX1262 {
  public:
    using SX1262::SX1262;
    uint8_t peek(uint16_t addr) {
        uint8_t v = 0;
        readRegister(addr, &v, 1);
        return v;
    }
};

SX1262Probe gRadio = new Module(&gHal, rak::kLoraCs, rak::kLoraDio1, rak::kLoraReset, rak::kLoraBusy);

bool gUp = false;

// ── 코어 0 의 받기 일꾼과 링버퍼 ─────────────────────────────────────────
//
// ISR 은 세마포어만 준다. SPI 로 짐을 꺼내는 것은 일꾼이 한다 —
// ISR 안에서 SPI 를 돌리면 안 된다.
SemaphoreHandle_t gRxSem = nullptr;

// ★ 무전기 객체는 한 번에 한 작업만 만진다.
//   코어 0 받기 일꾼(rxWorker)이 readData·startReceive·getRSSI 를 부르는데, 메인 루프의
//   lora tx / rssi / regs / sleep 도 같은 gRadio 로 SPI 를 돌린다. 잠금이 없으면 두 SPI
//   거래가 섞인다 (2026-09-14 검토). RadioLock 으로 감싼다.
SemaphoreHandle_t gRadioMux = nullptr;
struct RadioLock {
    bool ok = false;
    explicit RadioLock(uint32_t waitMs = 1000) {
        ok = gRadioMux && xSemaphoreTake(gRadioMux, pdMS_TO_TICKS(waitMs)) == pdTRUE;
    }
    ~RadioLock() { if (ok) xSemaphoreGive(gRadioMux); }
};
TaskHandle_t      gRxTask = nullptr;

Rx       gRing[kRingLen];
volatile size_t   gHead = 0, gTail = 0;
volatile uint32_t gDropped = 0, gReceived = 0, gCrcErrors = 0;

// 짐이 하나 들어왔다고 칩이 DIO1 을 흔들 때 불린다.
//
// ★ 여기서 하는 일은 **깨우는 것 하나뿐이다.** SPI 로 짐을 꺼내지 않는다.
//   IRAM_ATTR 은 이 함수를 램에 두라는 표시다. 플래시를 읽는 중에도
//   인터럽트가 들어올 수 있는데, 그때 플래시에 있는 코드는 못 부른다.
void IRAM_ATTR onDio1() {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(gRxSem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// 받기 일꾼. **코어 0 에서 혼자 돈다.** 깨워 주면 짐을 꺼내 링버퍼에 옮긴다.
//
// ★ 순서가 중요하다. 짐을 읽자마자 **먼저** 다시 듣는 자리로 돌려놓는다.
//   아래에서 링버퍼가 꽉 차서 버리게 되더라도 무전기는 계속 듣고 있어야 한다.
//
// CRC 가 깨진 것은 세기만 하고 버린다. 짐 안에 배 번호가 있는데 그게 틀리면
// 남의 배를 내 배로 그릴 수 있다. 의심스러우면 안 쓰는 쪽이 맞다.
void rxWorker(void*) {
    uint8_t buf[kPayloadLen];
    for (;;) {
        if (xSemaphoreTake(gRxSem, portMAX_DELAY) != pdTRUE) continue;

        RadioLock lk;
        if (!lk.ok) {
            // 잠금을 못 잡았다고 깨움을 버리면 readData 가 안 불려 IRQ 가 안 지워지고,
            // 다음 짐이 새 에지를 못 만들어 수신이 조용히 죽는다 [추측]. 깨움을 되돌려 다시 한다.
            xSemaphoreGive(gRxSem);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const uint32_t at = millis32();
        const int16_t  st = gRadio.readData(buf, kPayloadLen);

        // 다음 짐을 받을 자리를 **먼저** 연다.
        gRadio.startReceive();

        // (volatile 에 ++ 는 C++20 부터 막혀서 풀어 쓴다. 넣는 쪽이 이 일꾼 하나라 뜻은 같다)
        if (st == RADIOLIB_ERR_CRC_MISMATCH) { gCrcErrors = gCrcErrors + 1; continue; }
        if (st != RADIOLIB_ERR_NONE) continue;

        const size_t next = (gHead + 1) % kRingLen;
        if (next == gTail) { gDropped = gDropped + 1; continue; } // 루프가 안 꺼내 가고 있다

        memcpy(gRing[gHead].data, buf, kPayloadLen);
        gRing[gHead].atMs = at;
        gRing[gHead].rssi = (int16_t)gRadio.getRSSI();
        gRing[gHead].snr  = (int8_t)gRadio.getSNR();
        gHead = next;
        gReceived = gReceived + 1;
    }
}

} // namespace

bool begin() {
    if (gUp) return true;

    // (firmware-rak 은 여기서 gSpi.begin 을 불렀다. 여기서는 RadioLib 이 begin 안에서 HAL init → spiBegin 을 부른다.)

    // TCXO 전압은 DIO3 로 준다. 안 주면 발진기가 안 돌아 아무것도 안 된다.
    int16_t st = gRadio.begin(kFreqMHz, kBwKHz, kSf, kCr, kSyncWord, kTxDbm,
                              kPreamble, rak::kLoraTcxoVolts);
    if (st != RADIOLIB_ERR_NONE) {
        printf("[LORA] 시작 실패 %d — 무전기 없이 그대로 갑니다\n", st);
        return false;
    }

    // DIO2 가 송수신 전환 스위치를 직접 몬다. 안 걸면 켜지긴 해도 신호가 안 나간다.
    if (rak::kLoraDio2AsRfSwitch) gRadio.setDio2AsRfSwitch(true);

    // ★ 감도 4 dB 가 여기서 갈린다. 데이터시트의 -117 dBm(SF7/BW500)은 "Rx Boosted gain" 값이다.
    gRadio.setRxBoostedGainMode(true);

    // implicit 헤더 — 길이가 22바이트로 고정이라 헤더를 안 실어 시간을 아낀다.
    gRadio.implicitHeader(kPayloadLen);
    gRadio.setCRC(2);

    if (!gRxSem) gRxSem = xSemaphoreCreateBinary();
    if (!gRxSem) { printf("[LORA] 세마포어를 못 만들었습니다\n"); return false; }
    if (!gRadioMux) gRadioMux = xSemaphoreCreateMutex();
    if (!gRadioMux) { printf("[LORA] 무전기 잠금을 못 만들었습니다\n"); return false; }

    gRadio.setPacketReceivedAction(onDio1);
    gRadio.startReceive();

    // ★ 코어 0 에 붙인다. 메인 루프는 코어 1 에서 돈다.
    //   우선순위를 루프(1)보다 높게 둬야 SD 가 쓰는 중에도 바로 깬다.
    if (!gRxTask) {
        const BaseType_t ok = xTaskCreatePinnedToCore(
            rxWorker, "lora_rx", 4096, nullptr, /*priority=*/5, &gRxTask, /*core=*/0);
        if (ok != pdPASS) { gRxTask = nullptr; printf("[LORA] 받기 일꾼을 못 띄웠습니다\n"); return false; }
    }

    gUp = true;
    printf("[LORA] %.2f ㎒ SF%u BW%.0f㎑ CR4:%u  송신 %d dBm (EIRP 10 dBm)\n",
           kFreqMHz, kSf, kBwKHz, kCr, kTxDbm);
    printf("[LORA] 받기 일꾼 코어 0.  전파시간 %lu ms (짐 %u바이트)\n",
           (unsigned long)gRadio.getTimeOnAir(kPayloadLen) / 1000,
           (unsigned)kPayloadLen);
    return true;
}

bool up() { return gUp; }

void sleep() {
    if (!gUp) return;
    RadioLock lk;
    const int16_t st = lk.ok ? gRadio.sleep() : (int16_t)-1;
    printf("[LORA] 재웁니다 (st=%d)\n", (int)st);
}

// 링버퍼에서 하나 꺼낸다. 메인 루프가 부른다. 없으면 false.
// 꺼내는 쪽이 하나(메인 루프)고 넣는 쪽도 하나(코어 0 일꾼)라 자물쇠가 없다.
bool pop(Rx& out) {
    if (gTail == gHead) return false;
    out = gRing[gTail];
    gTail = (gTail + 1) % kRingLen;
    return true;
}

uint32_t dropped()   { return gDropped; }
uint32_t received()  { return gReceived; }
uint32_t crcErrors() { return gCrcErrors; }

// `lora` — 설정과 지금까지의 성적을 사람이 읽게 뱉는다.
// 제일 중요한 줄은 마지막의 "버림" 이다.
void report() {
    if (!gUp) { printf("[LORA] 안 올라와 있습니다\n"); return; }
    printf("──────────────────────────────────────────\n");
    printf("  주파수     %.2f ㎒   (4번 함대, PROTOCOL.md §10.12)\n", kFreqMHz);
    printf("  전파       SF%u / BW %.0f ㎑ / CR 4:%u / 프리앰블 %u\n",
           kSf, kBwKHz, kCr, kPreamble);
    printf("  헤더       implicit, 짐 %u바이트 고정,  CRC 켬\n", (unsigned)kPayloadLen);
    printf("  송신       %d dBm + 안테나 2 dBi = EIRP 10 dBm = 10 ㎽\n", kTxDbm);
    printf("  전파시간   %lu us  (§10 의 계산값 14140 us 와 견줄 것)\n",
           (unsigned long)gRadio.getTimeOnAir(kPayloadLen));
    printf("  받음       %u개,  CRC 깨짐 %u개,  버림 %u개\n",
           (unsigned)gReceived, (unsigned)gCrcErrors, (unsigned)gDropped);
    if (gDropped) printf("  ★ 버린 게 있습니다 — loop 가 링버퍼를 안 꺼내 가고 있습니다\n");
    printf("──────────────────────────────────────────\n");
}

// `lora tx` — 시험 삼아 한 번 보낸다. 보내기 전후로 0x0889 를 읽어 15.1 이 걸리는지 본다.
void txTest() {
    if (!gUp) { printf("[LORA] 안 올라와 있습니다\n"); return; }
    RadioLock lk;
    if (!lk.ok) { printf("[LORA] 무전기가 바쁩니다\n"); return; }
    uint8_t pkt[kPayloadLen] = {0};
    pkt[0] = 0xAA; // 시험용이라는 표. 실제 짐 배치(§10.4)는 아직 안 붙였다

    const uint8_t before = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG);
    const uint32_t t0 = micros32();
    const int16_t st = gRadio.transmit(pkt, kPayloadLen);
    const uint32_t us = micros32() - t0;
    const uint8_t after = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG);

    gRadio.startReceive(); // 다시 듣는 자리로 돌려놓는다

    printf("──────────────────────────────────────────\n");
    if (st == RADIOLIB_ERR_NONE) printf("  보냈다.  실제로 걸린 시간 %lu us\n", (unsigned long)us);
    else                         printf("  ★ 보내기 실패 %d\n", st);
    printf("  라이브러리가 미리 계산한 전파시간 %lu us\n",
           (unsigned long)gRadio.getTimeOnAir(kPayloadLen));
    printf("  15.1  보내기 전 0x%02X (bit2=%u)  →  보낸 뒤 0x%02X (bit2=%u)  %s\n",
           before, (before >> 2) & 1, after, (after >> 2) & 1,
           ((after >> 2) & 1) == 0 ? "걸렸다" : "★ 안 걸렸다");
    printf("──────────────────────────────────────────\n");
}

// `lora rssi` — 지금 이 주파수에 뭐가 있나 (바닥 잡음). -65 dBm 은 고시의 LBT 문턱이다 (§10.7).
void reportNoise(uint16_t samples) {
    if (!gUp) { printf("[LORA] 안 올라와 있습니다\n"); return; }
    RadioLock lk;
    if (!lk.ok) { printf("[LORA] 무전기가 바쁩니다\n"); return; }
    float mn = 999.0f, mx = -999.0f, sum = 0.0f;
    for (uint16_t i = 0; i < samples; ++i) {
        const float r = gRadio.getRSSI(/*packet=*/false); // 지금 이 순간의 세기
        if (r < mn) mn = r;
        if (r > mx) mx = r;
        sum += r;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    const float avg = sum / samples;
    printf("──────────────────────────────────────────\n");
    printf("  %.2f ㎒ 바닥 잡음 — %u번 재서\n", kFreqMHz, (unsigned)samples);
    printf("  제일 조용할 때 %.1f dBm   평균 %.1f dBm   제일 시끄러울 때 %.1f dBm\n",
           mn, avg, mx);
    printf("  우리 감도 -117 dBm 과의 차이  %.1f dB\n", avg - (-117.0f));
    if (mx > -65.0f) printf("  ★ -65 dBm 를 넘는 순간이 있습니다. 이 채널에 남이 있습니다\n");
    else             printf("  조용합니다.\n");
    printf("──────────────────────────────────────────\n");
}

// `lora watch` — 받는 것을 시리얼에 그대로 찍을지 켜고 끈다. 한쪽에서 `lora tx`, 다른 쪽에서 이것.
bool gWatch = false;
void watchToggle() {
    gWatch = !gWatch;
    printf("[LORA] 받는 것 보여주기 %s\n", gWatch ? "켬 — 다른 보드에서 lora tx 하세요" : "끔");
}

// 메인 루프가 부른다. 링버퍼를 비워 준다.
// ★ `lora watch` 가 꺼져 있어도 **반드시 불러야 한다.** 안 부르면 일꾼이 받은 짐을 버리기 시작한다.
void pump() {
    Rx r;
    while (pop(r)) {
        if (!gWatch) continue;
        printf("[LORA] 받음  RSSI %d dBm  SNR %d dB  %ums  짐:", r.rssi, r.snr, (unsigned)r.atMs);
        for (size_t i = 0; i < kPayloadLen; ++i) printf(" %02X", r.data[i]);
        printf("\n");
    }
}

// `lora regs` — 데이터시트 15장의 칩 버그 세 개가 실제로 걸렸는지 되읽는다.
// 이 셋은 **안 걸려도 아무 표시가 안 난다.** 설정을 써 넣은 것과 칩에 실제로 들어간 것은 다른 이야기라 눈으로 본다.
void reportRegs() {
    if (!gUp) { printf("[LORA] 안 올라와 있습니다\n"); return; }

    RadioLock lk;
    if (!lk.ok) { printf("[LORA] 무전기가 바쁩니다\n"); return; }
    const uint8_t sens  = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG); // 0x0889
    const uint8_t clamp = gRadio.peek(RADIOLIB_SX126X_REG_TX_CLAMP_CONFIG);    // 0x08D8
    const uint8_t gain  = gRadio.peek(RADIOLIB_SX126X_REG_RX_GAIN);            // 0x08AC
    const uint8_t syncH = gRadio.peek(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB);
    const uint8_t syncL = gRadio.peek(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_LSB);

    printf("──────────────────────────────────────────\n");
    printf("  SX1262 데이터시트 15장 — 우리 설정에 걸리는 칩 버그\n");

    // 15.1  BW 500 ㎑ 로 보내면 받는 쪽 감도가 떨어진다. bit2 가 0 이어야 한다. **송신 직전마다** 건다.
    printf("  15.1 감도    0x0889 = 0x%02X  bit2=%u  → %s\n", sens, (sens >> 2) & 1,
           ((sens >> 2) & 1) == 0
               ? "걸렸다 (BW500 에 맞다)"
               : "아직 안 보냈다 — `lora tx` 뒤에 다시 보세요");

    // 15.2  PA 클램프. bit4~1 이 1111 (0x1E) 여야 한다.
    printf("  15.2 PA클램프 0x08D8 = 0x%02X  bit4~1=0x%02X → %s\n",
           clamp, (clamp & 0x1E) >> 1,
           (clamp & 0x1E) == 0x1E ? "걸렸다" : "★ 안 걸렸다 — 송신이 5~6 dB 깎인다");

    printf("  15.3 implicit 헤더 타이머 — RadioLib 이 Rx 마다 처리한다 (읽어서 볼 수 없음)\n");

    printf("  RxGain      0x08AC = 0x%02X → %s\n", gain,
           gain == RADIOLIB_SX126X_RX_GAIN_BOOSTED ? "Boosted (-117 dBm)"
           : gain == RADIOLIB_SX126X_RX_GAIN_POWER_SAVING
                 ? "★ 절전 — 4 dB 손해다"
                 : "★ 모르는 값");

    printf("  SyncWord    0x%02X%02X → %s\n", syncH, syncL,
           (syncH == 0x14 && syncL == 0x24) ? "private (LoRaWAN 과 안 섞인다)"
                                            : "★ private 가 아니다");
    printf("──────────────────────────────────────────\n");
}

} // namespace lora
