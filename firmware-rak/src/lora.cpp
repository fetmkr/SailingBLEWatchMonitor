#include "lora.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#include "board_rak.h"

namespace lora {
namespace {

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

SPIClass gSpi(HSPI); // SD 가 쓰는 기본 SPI 와 다른 버스다 (board_rak.h)

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

SX1262Probe gRadio = new Module(rak::kLoraCs, rak::kLoraDio1,
                                rak::kLoraReset, rak::kLoraBusy, gSpi);

bool gUp = false;

// ── 코어 0 의 받기 일꾼과 링버퍼 ─────────────────────────────────────────
//
// ISR 은 세마포어만 준다. SPI 로 짐을 꺼내는 것은 일꾼이 한다 —
// ISR 안에서 SPI 를 돌리면 안 된다.
SemaphoreHandle_t gRxSem = nullptr;
TaskHandle_t      gRxTask = nullptr;

Rx       gRing[kRingLen];
volatile size_t   gHead = 0, gTail = 0;
volatile uint32_t gDropped = 0, gReceived = 0, gCrcErrors = 0;

void IRAM_ATTR onDio1() {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(gRxSem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void rxWorker(void*) {
    uint8_t buf[kPayloadLen];
    for (;;) {
        if (xSemaphoreTake(gRxSem, portMAX_DELAY) != pdTRUE) continue;

        const uint32_t at = millis();
        const int16_t  st = gRadio.readData(buf, kPayloadLen);

        // 다음 짐을 받을 자리를 **먼저** 연다. 아래에서 링버퍼가 꽉 차 있어도
        // 무전기는 계속 듣고 있어야 한다.
        gRadio.startReceive();

        if (st == RADIOLIB_ERR_CRC_MISMATCH) { gCrcErrors++; continue; }
        if (st != RADIOLIB_ERR_NONE) continue;

        const size_t next = (gHead + 1) % kRingLen;
        if (next == gTail) { gDropped++; continue; } // 루프가 안 꺼내 가고 있다

        memcpy(gRing[gHead].data, buf, kPayloadLen);
        gRing[gHead].atMs = at;
        gRing[gHead].rssi = (int16_t)gRadio.getRSSI();
        gRing[gHead].snr  = (int8_t)gRadio.getSNR();
        gHead = next;
        gReceived++;
    }
}

} // namespace

bool begin() {
    if (gUp) return true;

    gSpi.begin(rak::kLoraSck, rak::kLoraMiso, rak::kLoraMosi, rak::kLoraCs);

    // TCXO 전압은 DIO3 로 준다. 안 주면 발진기가 안 돌아 아무것도 안 된다.
    int16_t st = gRadio.begin(kFreqMHz, kBwKHz, kSf, kCr, kSyncWord, kTxDbm,
                              kPreamble, rak::kLoraTcxoVolts);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] 시작 실패 %d — 무전기 없이 그대로 갑니다\n", st);
        return false;
    }

    // DIO2 가 송수신 전환 스위치를 직접 몬다. 안 걸면 켜지긴 해도 신호가 안 나간다.
    if (rak::kLoraDio2AsRfSwitch) gRadio.setDio2AsRfSwitch(true);

    // ★ 감도 4 dB 가 여기서 갈린다.
    //   데이터시트의 -117 dBm(SF7/BW500)은 "Rx Boosted gain" 값이다.
    //   RadioLib 은 기본이 절전 쪽이라 안 부르면 그냥 4 dB 를 잃는다.
    //   LBT 냐 듀티냐로 따지던 그 4 dB 와 같은 크기다 (§10.7).
    gRadio.setRxBoostedGainMode(true);

    // implicit 헤더 — 길이가 22바이트로 고정이라 헤더를 안 실어 시간을 아낀다.
    gRadio.implicitHeader(kPayloadLen);
    gRadio.setCRC(2);

    gRxSem = xSemaphoreCreateBinary();
    if (!gRxSem) { Serial.println("[LORA] 세마포어를 못 만들었습니다"); return false; }

    gRadio.setPacketReceivedAction(onDio1);
    gRadio.startReceive();

    // ★ 코어 0 에 붙인다. loop() 는 코어 1 에서 돈다.
    //   우선순위를 루프(1)보다 높게 둬야 SD 가 쓰는 중에도 바로 깬다.
    const BaseType_t ok = xTaskCreatePinnedToCore(
        rxWorker, "lora_rx", 4096, nullptr, /*priority=*/5, &gRxTask, /*core=*/0);
    if (ok != pdPASS) { Serial.println("[LORA] 받기 일꾼을 못 띄웠습니다"); return false; }

    gUp = true;
    Serial.printf("[LORA] %.2f ㎒ SF%u BW%.0f㎑ CR4:%u  송신 %d dBm (EIRP 10 dBm)\n",
                  kFreqMHz, kSf, kBwKHz, kCr, kTxDbm);
    Serial.printf("[LORA] 받기 일꾼 코어 0.  전파시간 %lu ms (짐 %u바이트)\n",
                  (unsigned long)gRadio.getTimeOnAir(kPayloadLen) / 1000,
                  (unsigned)kPayloadLen);
    return true;
}

bool up() { return gUp; }

bool pop(Rx& out) {
    if (gTail == gHead) return false;
    out = gRing[gTail];
    gTail = (gTail + 1) % kRingLen;
    return true;
}

uint32_t dropped()   { return gDropped; }
uint32_t received()  { return gReceived; }
uint32_t crcErrors() { return gCrcErrors; }

void report() {
    if (!gUp) { Serial.println("[LORA] 안 올라와 있습니다"); return; }
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  주파수     %.2f ㎒   (4번 함대, PROTOCOL.md §10.12)\n", kFreqMHz);
    Serial.printf("  전파       SF%u / BW %.0f ㎑ / CR 4:%u / 프리앰블 %u\n",
                  kSf, kBwKHz, kCr, kPreamble);
    Serial.printf("  헤더       implicit, 짐 %u바이트 고정,  CRC 켬\n", (unsigned)kPayloadLen);
    Serial.printf("  송신       %d dBm + 안테나 2 dBi = EIRP 10 dBm = 10 ㎽\n", kTxDbm);
    Serial.printf("  전파시간   %lu us  (§10 의 계산값 14140 us 와 견줄 것)\n",
                  (unsigned long)gRadio.getTimeOnAir(kPayloadLen));
    Serial.printf("  받음       %u개,  CRC 깨짐 %u개,  버림 %u개\n",
                  (unsigned)gReceived, (unsigned)gCrcErrors, (unsigned)gDropped);
    if (gDropped) Serial.println("  ★ 버린 게 있습니다 — loop 가 링버퍼를 안 꺼내 가고 있습니다");
    Serial.println("──────────────────────────────────────────");
}

/// `lora tx` — 시험 삼아 한 번 보낸다.
///
/// 보내는 김에 15.1 이 실제로 걸리는지 **보내기 전후로 레지스터를 읽어서** 본다.
/// 한 번 보내는 것은 듀티 사이클(20초 중 2%)에 견줘 무시할 양이다 (§10.7).
void txTest() {
    if (!gUp) { Serial.println("[LORA] 안 올라와 있습니다"); return; }
    uint8_t pkt[kPayloadLen] = {0};
    pkt[0] = 0xAA; // 시험용이라는 표. 실제 짐 배치(§10.4)는 아직 안 붙였다

    const uint8_t before = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG);
    const uint32_t t0 = micros();
    const int16_t st = gRadio.transmit(pkt, kPayloadLen);
    const uint32_t us = micros() - t0;
    const uint8_t after = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG);

    gRadio.startReceive(); // 다시 듣는 자리로 돌려놓는다

    Serial.println("──────────────────────────────────────────");
    if (st == RADIOLIB_ERR_NONE) Serial.printf("  보냈다.  실제로 걸린 시간 %lu us\n", (unsigned long)us);
    else                         Serial.printf("  ★ 보내기 실패 %d\n", st);
    Serial.printf("  라이브러리가 미리 계산한 전파시간 %lu us\n",
                  (unsigned long)gRadio.getTimeOnAir(kPayloadLen));
    Serial.printf("  15.1  보내기 전 0x%02X (bit2=%u)  →  보낸 뒤 0x%02X (bit2=%u)  %s\n",
                  before, (before >> 2) & 1, after, (after >> 2) & 1,
                  ((after >> 2) & 1) == 0 ? "걸렸다" : "★ 안 걸렸다");
    Serial.println("──────────────────────────────────────────");
}

/// `lora rssi` — 지금 이 주파수에 뭐가 있나 (바닥 잡음).
///
/// **보드 한 대로 확인할 수 있는 것은 여기까지다.** 짐이 오가는 것은 두 대가
/// 있어야 본다. 대신 이건 받는 길이 살아 있는지, 그리고 922.55 ㎒ 가 조용한지를
/// 알려 준다. 값이 안 움직이거나 터무니없으면 안테나나 받는 쪽이 죽은 것이다.
///
/// -65 dBm 은 고시의 LBT 문턱이다 (§10.7). 우리는 LBT 를 안 쓰지만, 이 값보다
/// 시끄러우면 남이 이 채널을 쓰고 있다는 뜻이라 함대 번호를 옮겨야 한다.
void reportNoise(uint16_t samples) {
    if (!gUp) { Serial.println("[LORA] 안 올라와 있습니다"); return; }
    float mn = 999.0f, mx = -999.0f, sum = 0.0f;
    for (uint16_t i = 0; i < samples; ++i) {
        const float r = gRadio.getRSSI(/*packet=*/false); // 지금 이 순간의 세기
        if (r < mn) mn = r;
        if (r > mx) mx = r;
        sum += r;
        delay(2);
    }
    const float avg = sum / samples;
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  %.2f ㎒ 바닥 잡음 — %u번 재서\n", kFreqMHz, (unsigned)samples);
    Serial.printf("  제일 조용할 때 %.1f dBm   평균 %.1f dBm   제일 시끄러울 때 %.1f dBm\n",
                  mn, avg, mx);
    Serial.printf("  우리 감도 -117 dBm 과의 차이  %.1f dB\n", avg - (-117.0f));
    if (mx > -65.0f) Serial.println("  ★ -65 dBm 를 넘는 순간이 있습니다. 이 채널에 남이 있습니다");
    else             Serial.println("  조용합니다.");
    Serial.println("──────────────────────────────────────────");
}

/// `lora watch` — 짐이 올 때마다 한 줄씩 뱉는다. **두 대로 시험할 때 쓴다.**
/// 한쪽에서 `lora tx`, 다른 쪽에서 `lora watch`.
bool gWatch = false;
void watchToggle() {
    gWatch = !gWatch;
    Serial.printf("[LORA] 받는 것 보여주기 %s\n", gWatch ? "켬 — 다른 보드에서 lora tx 하세요" : "끔");
}

void pump() {
    Rx r;
    while (pop(r)) {
        if (!gWatch) continue;
        Serial.printf("[LORA] 받음  RSSI %d dBm  SNR %d dB  %ums  짐:", r.rssi, r.snr, (unsigned)r.atMs);
        for (size_t i = 0; i < kPayloadLen; ++i) Serial.printf(" %02X", r.data[i]);
        Serial.println();
    }
}

void reportRegs() {
    if (!gUp) { Serial.println("[LORA] 안 올라와 있습니다"); return; }

    const uint8_t sens  = gRadio.peek(RADIOLIB_SX126X_REG_SENSITIVITY_CONFIG); // 0x0889
    const uint8_t clamp = gRadio.peek(RADIOLIB_SX126X_REG_TX_CLAMP_CONFIG);    // 0x08D8
    const uint8_t gain  = gRadio.peek(RADIOLIB_SX126X_REG_RX_GAIN);            // 0x08AC
    const uint8_t syncH = gRadio.peek(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB);
    const uint8_t syncL = gRadio.peek(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_LSB);

    Serial.println("──────────────────────────────────────────");
    Serial.println("  SX1262 데이터시트 15장 — 우리 설정에 걸리는 칩 버그");

    // 15.1  BW 500 ㎑ 로 보내면 받는 쪽 감도가 떨어진다. bit2 가 0 이어야 한다.
    //
    // ★ 이건 **송신 직전마다** 거는 것이다. 데이터시트가 "Before any packet
    //   transmission" 이라고 했고, RadioLib 도 stageMode() 의 송신 갈래에서
    //   fixSensitivity() 를 부른다 [확인: SX126x.cpp:1092].
    //   그러니 한 번도 안 보낸 상태에서 읽으면 bit2 가 1 인 게 맞다.
    //   `lora tx` 로 하나 보내고 다시 읽어야 걸린 것을 볼 수 있다.
    Serial.printf("  15.1 감도    0x0889 = 0x%02X  bit2=%u  → %s\n", sens, (sens >> 2) & 1,
                  ((sens >> 2) & 1) == 0
                      ? "걸렸다 (BW500 에 맞다)"
                      : "아직 안 보냈다 — `lora tx` 뒤에 다시 보세요");

    // 15.2  PA 클램프. bit4~1 이 1111 (0x1E) 여야 한다.
    //       안 걸리면 송신이 5~6 dB 깎이는데 화면에는 아무 표시도 안 난다.
    Serial.printf("  15.2 PA클램프 0x08D8 = 0x%02X  bit4~1=0x%02X → %s\n",
                  clamp, (clamp & 0x1E) >> 1,
                  (clamp & 0x1E) == 0x1E ? "걸렸다" : "★ 안 걸렸다 — 송신이 5~6 dB 깎인다");

    Serial.println("  15.3 implicit 헤더 타이머 — RadioLib 이 Rx 마다 처리한다 (읽어서 볼 수 없음)");

    // 감도 4 dB 가 갈리는 자리 (§10.7 의 그 4 dB 와 같은 크기다)
    Serial.printf("  RxGain      0x08AC = 0x%02X → %s\n", gain,
                  gain == RADIOLIB_SX126X_RX_GAIN_BOOSTED ? "Boosted (-117 dBm)"
                  : gain == RADIOLIB_SX126X_RX_GAIN_POWER_SAVING
                        ? "★ 절전 — 4 dB 손해다"
                        : "★ 모르는 값");

    Serial.printf("  SyncWord    0x%02X%02X → %s\n", syncH, syncL,
                  (syncH == 0x14 && syncL == 0x24) ? "private (LoRaWAN 과 안 섞인다)"
                                                   : "★ private 가 아니다");
    Serial.println("──────────────────────────────────────────");
}

} // namespace lora
