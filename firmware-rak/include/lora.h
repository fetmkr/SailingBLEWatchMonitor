// LoRa (RAK3112 안에 든 SX1262) — 배와 배 사이.
//
// 규격은 PROTOCOL.md §10 이다. 여기서 하는 일은 둘이다.
//   1) SX1262 에 SPI 로 설정값을 써 넣고, 되읽어서 들어갔는지 확인한다
//   2) 받은 짐을 하나도 안 놓치게 링버퍼에 옮긴다
//
// ★ 받기를 loop() 에 두지 않는다.
//
//   SX1262 는 받은 짐을 하나만 들고 있다. 다음 차례 것이 오면 덮어쓴다.
//   차례 간격이 25 ms 이므로 **루프가 25 ms 넘게 딴 데 가 있으면 그 배를
//   잃는다.** SD 쓰기는 제일 오래 멈춘 게 14 ms 라 괜찮지만 [확인: sdbench
//   9 MB, 15000번 중 10 ms 넘은 것 한 번], 루프에는 화면 I2C·GPS 파싱·BLE 가
//   같이 있다. 지금 재서 괜찮다고 나중에도 괜찮은 게 아니다.
//
//   그래서 ESP32-S3 의 두 코어를 갈라 쓴다.
//
//     코어 1   loop() — SD · 화면 · GPS · IMU · BLE · WiFi
//     코어 0   받기 일꾼 — DIO1 을 기다렸다가 바로 링버퍼에 옮긴다
//
//   로라 SPI 는 SD SPI 와 핀이 하나도 안 겹치므로 (board_rak.h 참고) 두 코어가
//   서로 기다릴 일이 없다.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace lora {

/// 짐 길이. implicit 헤더라 양쪽이 같은 값을 알고 있어야 한다 (§10.4).
static constexpr size_t kPayloadLen = 22;

/// 받은 짐 하나. 일꾼이 채우고 loop 가 꺼내 간다.
struct Rx {
    uint8_t  data[kPayloadLen];
    uint32_t atMs;    ///< 받은 시각 (millis). 어느 차례였는지 여기서 나온다
    int16_t  rssi;    ///< dBm
    int8_t   snr;     ///< dB
};

/// SX1262 에 설정을 써 넣고, 코어 0 에 받기 일꾼을 띄운다.
/// 실패해도 보드는 그대로 돌아야 한다 — false 만 돌려주고 조용히 지나간다.
bool begin();

/// SX1262 가 설정을 받고 응답하고 있나
bool up();

/// 링버퍼에서 하나 꺼낸다. 없으면 false.
bool pop(Rx& out);

/// 일꾼이 링버퍼가 꽉 차서 버린 개수. 0 이 아니면 loop 가 너무 느린 것이다.
uint32_t dropped();

/// 지금까지 받은 개수와 CRC 가 깨진 개수
uint32_t received();
uint32_t crcErrors();

/// `lora` — 설정과 상태를 사람이 읽게 뱉는다
void report();

/// `lora regs` — 데이터시트 15장의 칩 버그 세 개와 RxBoostedGain 이 실제로
/// 걸렸는지 레지스터를 읽어서 확인한다.
///
/// **이걸 눈으로 봐야 한다.** 15.2(PA 클램프)가 안 걸리면 송신이 5~6 dB
/// 깎이는데 화면에는 아무 표시도 안 난다. 나중에 물 위에서 거리가 안 나올 때
/// 원인을 여기서 찾게 하면 안 된다.
void reportRegs();

/// `lora tx` — 시험 삼아 하나 보낸다. 보내기 전후로 0x0889 를 읽어서
/// 데이터시트 15.1 이 실제로 걸리는지 눈으로 본다.
void txTest();

/// `lora rssi` — 이 주파수의 바닥 잡음. 보드 한 대로 볼 수 있는 마지막 확인이다.
void reportNoise(uint16_t samples = 200);

/// `lora watch` — 짐이 올 때마다 한 줄씩. 두 대로 시험할 때 쓴다.
void watchToggle();

/// loop 에서 부른다. 링버퍼를 비우고, watch 가 켜져 있으면 뱉는다.
void pump();

} // namespace lora
