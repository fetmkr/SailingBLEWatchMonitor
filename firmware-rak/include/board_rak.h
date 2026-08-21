// RAK3112 (ESP32-S3) + RAK19007 베이스보드 핀 정의.
//
// 근거
//   RAK3312 datasheet — WisBlock Pin Assignments 표
//   RAK19007 datasheet — Figure 15(전원) / Figure 16(슬롯) 회로도
//
// RAK 가 배포하는 rakwireless/variants/rak3112/pins_arduino.h 에도 같은 이름의
// 매크로(WB_IO1, WB_A0 …)가 있지만 그 파일과 datasheet 가 서로 어긋나는 곳이
// 있다 (아래 kSensorPowerA/B). 그래서 매크로를 쓰지 않고 여기서 직접 정의한다.
#pragma once

#include <stdint.h>

namespace rak {

// ── 이 보드에 붙어 있는 모듈 ─────────────────────────────────────────────
//
//   RAK12501  GPS    Quectel L76K      UART 9600bps   센서 슬롯 A 또는 D
//   RAK1905   IMU    TDK MPU-9250 9축  I2C  0x68      센서 슬롯 A~D
//   RAK1921   화면   SSD1306 128x64    I2C  0x3C      J12 헤더 ★슬롯 아님
//   RAK15002  SD카드 -                 SPI            IO 슬롯 ★센서슬롯 불가
//
// 화면과 SD 는 센서 슬롯을 쓰지 않는다. 그래서 센서 슬롯 A~D 를 놓고 다투는
// 것은 GPS 와 IMU 둘뿐이다.
static constexpr uint8_t kAddrImu     = 0x68; // MPU-9250 본체
static constexpr uint8_t kAddrImuMag  = 0x0C; // 안에 든 AK8963 자력계
static constexpr uint8_t kAddrDisplay = 0x3C; // SSD1306

// ── 슬롯 배분 — 이렇게 꽂아야 하는 이유 ──────────────────────────────────
//
//   GPS(RAK12501) → 슬롯 A,  IMU(RAK1905) → 슬롯 C
//
// GPS 를 A 에 두는 이유 두 가지.
//
//  1) GPS 모듈은 커넥터 핀10 을 RESET 으로 받는다.
//       슬롯 A 에서 핀10 = IO2 = 센서 전원 스위치
//       슬롯 D 에서 핀10 = IO6 = GPIO39
//     A 에 꽂으면 전원을 켜는 동작이 곧 리셋 해제가 된다. RAK 예제 코드가
//     바로 이걸 전제로 짜여 있다 (IO2 를 LOW→HIGH 로 흔든다).
//     D 에 꽂으면 IO6 을 따로 HIGH 로 올려 줘야 GPS 가 리셋에서 풀린다.
//
//  2) SD카드(RAK15002)는 카드 삽입 감지를 커넥터 핀38 = IO6 = GPIO39 로 낸다.
//     GPS 를 D 에 꽂으면 GPS 의 RESET 과 SD 의 카드감지가 같은 GPIO39 에서
//     서로 싸운다.
//
// IMU 를 C 에 두는 이유. RAK1905 의 인터럽트는 슬롯마다 다른 핀으로 나온다.
//       슬롯 A → IO1   슬롯 B → IO2   슬롯 C → IO3   슬롯 D → IO5
//     B 는 IO2 라 센서 전원 스위치와 겹친다. C 의 IO3(GPIO41)이 전용이라 안전하다.
static constexpr int kGpsResetSlotD = 39; // 슬롯 D 에 꽂았을 때만 필요 (= IO6)
static constexpr int kImuIntSlotC   = 41; // 슬롯 C 에 꽂았을 때의 인터럽트 (= IO3)
static constexpr int kSdCardDetect  = 39; // IO 슬롯 핀38 (= IO6)

// ── 센서 전원 스위치 (WisBlock 신호 이름 WB_IO2) ─────────────────────────
//
// 이 핀이 3V3_S 를 켜고 끈다. LOW 면 센서 슬롯 A~D 에 꽂힌 모듈이 전부
// 죽어 있다. 화면도 GPS 도 IMU 도 SD 도 한꺼번에 안 보인다.
//
// 문서끼리 값이 달랐다.
//     RAK3312 datasheet         WB_IO2 = GPIO14
//     RAK 배포 pins_arduino.h   WB_IO2 = GPIO2   (14 는 주석 처리되어 있음)
//
// ★ 실기기로 판정 끝. GPIO14 가 맞다. (2026-08-21, MAC 3C:DC:75:70:2F:B5)
//
//     power 14  → GPS 가 1380 바이트 보냄
//     power 2   → 한 바이트도 안 옴
//     power off → 한 바이트도 안 옴
//
//   즉 datasheet 가 맞고 RAK 배포 pins_arduino.h 가 틀렸다.
//   그 파일의 WB_IO2 매크로를 쓰지 않는 이유가 이것이다.
//
// ★ 판정에 IMU 를 쓰면 안 된다. 여기서 한 번 헛짚었다.
//   RAK1905 IMU 는 커넥터 핀9/16 의 VDD 에서 전원을 받는다. VDD 는 코어
//   모듈이 늘 내보내는 전원이라 이 스위치와 무관하다. 그래서 전원을 꺼도
//   I2C 스캔에 0x68 이 계속 보인다.
//   반대로 GPS(RAK12501)는 핀11/14 의 3V3_S 를 쓴다. 스위치가 달린 쪽이다.
//   그래서 전원 핀 판정은 `gps` 명령으로 한다.
static constexpr int kSensorPowerA = 14; // ★ 실측으로 확정된 값
static constexpr int kSensorPowerB = 2;  // pins_arduino.h 쪽 — 실측에서 탈락

// ── WisBlock 슬롯 범용 IO ────────────────────────────────────────────────
//
// 슬롯별 배정 (RAK19007 datasheet 센서 커넥터 표)
//   슬롯 A  핀10=IO2  핀12=IO1     ← IO2 는 위의 전원 스위치와 같은 선이다
//   슬롯 B  핀10=IO1  핀12=IO2     ← A 와 뒤바뀐 배치
//   슬롯 C  핀10=IO4  핀12=IO3
//   슬롯 D  핀10=IO6  핀12=IO5
//
// A 와 B 는 IO1/IO2 를 나눠 쓰므로 두 슬롯에 인터럽트를 쓰는 모듈을 함께
// 꽂으면 서로 물린다. C 와 D 는 자기만의 IO 를 갖는다.
static constexpr int kIO1 = 21;
static constexpr int kIO3 = 41;
static constexpr int kIO4 = 42;
static constexpr int kIO5 = 38;
static constexpr int kIO6 = 39;

// ── I2C ──────────────────────────────────────────────────────────────────
//
// 센서 슬롯 A~D 로 나가는 것은 I2C1 하나뿐이다. I2C2 는 코어 커넥터에서
// 끝나고 슬롯으로 가지 않는다. 즉 슬롯에 꽂은 모듈은 전부 한 버스에 매달린다.
// 풀업은 베이스보드에 이미 있다 (R10~R13, 각 4.7k → VDD).
static constexpr int kI2C1_SDA = 9;
static constexpr int kI2C1_SCL = 40;
static constexpr int kI2C2_SDA = 17;
static constexpr int kI2C2_SCL = 18;

// ── UART1 ────────────────────────────────────────────────────────────────
//
// 슬롯 A 와 D 에만 나간다. B 와 C 는 해당 핀이 끊겨 있다(NC).
// GPS(RAK12501 / L76K)는 UART 로만 말하므로 반드시 A 나 D 에 꽂아야 한다.
static constexpr int kUART1_TX = 43;
static constexpr int kUART1_RX = 44;

// ── SPI ──────────────────────────────────────────────────────────────────
//
// 네 슬롯 공통. 칩셀렉트도 네 슬롯이 같은 한 가닥이라, SPI 를 쓰는 모듈을
// 두 개 꽂으면 둘이 동시에 깨어난다.
static constexpr int kSPI_MISO = 10;
static constexpr int kSPI_MOSI = 11;
static constexpr int kSPI_CLK  = 13;
static constexpr int kSPI_CS   = 12;

// ── 베이스보드 LED ───────────────────────────────────────────────────────
// 빨강 LED 는 충전 표시라 MCU 가 건드릴 수 없다.
static constexpr int kLedGreen = 46;
static constexpr int kLedBlue  = 45;

// ── 배터리 전압 ──────────────────────────────────────────────────────────
//
// RAK19007 회로도 (Figure 15/16)
//
//     VBAT ──[R3 1M]──┬──[R4 1.5M]── GND
//                     │
//                  ADC_VBAT ──[R7 0Ω]── 코어 핀21(AIN0) = GPIO1
//
// 핀에 걸리는 전압은 배터리의 1.5/(1.0+1.5) = 0.6 배다.
// 되짚으면 VBAT = 잰 전압 ÷ 0.6.
//
// 저항을 더하면 2.5 MΩ 이라 항상 1.7 µA 가 샌다. datasheet 의
// "모듈 없을 때 누설 2 µA" 와 맞아떨어진다.
//
// ★ 소스 임피던스가 2.5 MΩ 로 높아서 ESP32-S3 ADC 가 실제보다 낮게 읽을 수
//   있다. 멀티미터로 배터리를 직접 재서 `batt` 명령 출력과 대조할 것.
//   어긋나면 kBattCorrection 으로 보정한다.
static constexpr int   kBattAdcPin    = 1;
static constexpr float kBattDivider   = 0.6f;
static constexpr float kBattCorrection = 1.0f; // 실측 대조 후 조정

// 리튬폴리머 한 셀 기준. 방전 곡선은 직선이 아니라서 이 선형 환산은
// 어림값이다. 정확한 잔량이 필요해지면 곡선 표로 바꾼다.
static constexpr float kBattFullV  = 4.20f;
static constexpr float kBattEmptyV = 3.30f;

} // namespace rak
