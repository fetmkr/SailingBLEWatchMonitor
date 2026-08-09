# firmware — HOHO-01 (ESP32-S3)

가상 요트 텔레메트리를 만들어 BLE 로 뿌리는 테스트 펌웨어.
프로토콜은 [`../PROTOCOL.md`](../PROTOCOL.md) 를 따른다.

| | |
|---|---|
| 보드 | ESP32-S3 DevKitC-1 (대부분의 S3 DevKit 호환) |
| 프레임워크 | Arduino (PlatformIO) |
| BLE 스택 | NimBLE-Arduino 2.5.x |
| 시리얼 | 115200 baud |

---

## 파일 구성

```
firmware/
├── platformio.ini
├── include/
│   ├── protocol.h      ← 패킷 인코딩. app/Shared/Protocol.swift 와 쌍을 이룬다
│   └── simulator.h     ← 가상 데이터 생성. Arduino 의존성 없는 순수 C++
├── src/
│   └── main.cpp        ← BLE 서버 + 광고 제어 + 루프
└── tools/
    ├── sim_test.cpp    ← 호스트에서 값을 실측 검증하는 하네스
    └── Makefile
```

`simulator.h` 를 Arduino 와 분리해 둔 이유는, 보드에 굽기 전에 맥에서 그대로
컴파일해 **값이 정말 스펙대로 나오는지 눈으로 확인**하기 위해서다.

---

## 1. 빌드

```bash
cd firmware
pio run
```

첫 실행 시 espressif32 플랫폼과 NimBLE-Arduino 를 내려받는다 (수 분).

## 2. 플래시

보드를 USB 로 연결하고:

```bash
pio run -t upload
```

포트를 못 찾으면 직접 지정한다.

```bash
pio device list                     # 포트 확인
pio run -t upload --upload-port /dev/cu.usbmodem101
```

> **부팅 모드 진입이 필요한 경우** (`Failed to connect` 에러)
> `BOOT` 버튼을 누른 채 `RST` 를 한 번 눌렀다 떼고, `BOOT` 도 뗀 뒤 다시 업로드.

## 3. 시리얼 모니터

```bash
pio device monitor
```

정상 동작 시 1 Hz 로 이런 로그가 나온다.

```
═══════════════════════════════════════════
  HOHO-01 — 요트 텔레메트리 BLE 테스트
  service   b0a70001-0000-4000-8000-000000000001
  telemetry b0a70002-0000-4000-8000-000000000001
  notify 4.0Hz / adv refresh 1.0Hz
═══════════════════════════════════════════
[BLE] advertising 시작 — ADV_IND / connectable (interval 200ms)
[    1.0s] SOG  5.61 kn | COG  45.0° | HEEL  +12.3° | BATT 100% | seq   1 | ADVERTISING
[    2.0s] SOG  5.74 kn | COG  45.0° | HEEL  +12.9° | BATT 100% | seq   2 | ADVERTISING
[BLE] 연결됨 ← 5f:2a:... (conn=1)
[BLE] advertising 시작 — ADV_SCAN_IND / non-connectable (interval 200ms)
[BLE] notify 구독 ON
[    3.0s] SOG  5.88 kn | COG  45.0° | HEEL  +13.5° | BATT 100% | seq   3 | CONNECTED (notify ON)
```

### USB CDC 보드에서 로그가 안 보일 때

일부 S3 보드는 UART 브리지 없이 USB-OTG 포트만 있다. 이 경우
`platformio.ini` 의 아래 두 줄 주석을 해제하고 다시 빌드·플래시한다.

```ini
-DARDUINO_USB_CDC_ON_BOOT=1
-DARDUINO_USB_MODE=1
```

---

## 4. 하드웨어 없이 로직 검증

보드가 없어도 시뮬레이션과 패킷 인코딩은 맥에서 그대로 검증할 수 있다.

```bash
cd firmware
make -C tools
```

출력 예:

```
── 1. GATT characteristic 12바이트 인코딩 ──
  실측: 01 01 40 E2 01 00 29 02 4E 0C F4 57
  기대: 01 01 40 E2 01 00 29 02 4E 0C F4 57   (PROTOCOL.md §3)
  [ OK ] PROTOCOL.md §3 예시와 바이트 단위 일치
  ...
── 4. 택 전환 시 COG 경로 (t=60.0 → 65.0s, 0.5s 간격) ──
   t=  60.0s  COG   45.0°
   t=  62.5s  COG    0.0°
   t=  65.0s  COG  315.0°
  [ OK ] COG 가 북쪽(0°/360°)을 경유 — 최단 90° 회전
```

앱 디코더까지 함께 맞춰보려면 저장소 루트의 [`../tools/verify.sh`](../tools/verify.sh) 를 쓴다.

---

## 동작 설계 메모

### 가상 데이터 (`include/simulator.h`)

| 항목 | 식 |
|---|---|
| SOG | `5.5 + 1.5·sin(2πt/40) + noise(±0.2)` kn |
| COG | 60초마다 택. 45° ↔ 315° 를 5초에 걸쳐 smoothstep 보간 |
| HEEL | `sign · (12 + 6·sin(2πt/40)) + noise(±0.5)`, 택마다 부호 반전 |
| BATT | 100% 에서 10분마다 1% 계단식 감소 |

- COG 보간은 **최단 회전**을 쓴다. 45° → 315° 는 북쪽(0°)을 지나는 −90°.
  단순 선형 보간이면 남쪽으로 270° 를 돌아 실제 택과 정반대가 된다.
- HEEL 부호도 같은 smoothstep 계수로 +1 → −1 로 흘러가므로,
  택 도중에는 힐이 0 을 지나며 자연스럽게 넘어간다.
- 배터리를 연속 감소가 아닌 계단식으로 둔 이유: 연속 감소 + 반올림으로 하면
  부팅 5분 만에 99% 로 떨어져 "10분당 1%" 라는 스펙과 어긋난다.

### 광고 제어 (`src/main.cpp`)

| 상태 | 광고 타입 | 이유 |
|---|---|---|
| 미연결 | `ADV_IND` (connectable + scannable) | 앱이 붙을 수 있어야 함 |
| 연결 중 | `ADV_SCAN_IND` (non-connectable + scannable) | 연결되어 있어도 스캐너 탭에서 관측 가능 |

`ADV_NONCONN_IND` 를 쓰지 않는 이유: 이 타입은 scan response 를 실을 수 없는데,
manufacturer data(텔레메트리)가 scan response 에 들어 있기 때문이다
(ADV 패킷 31바이트에 Flags + 128-bit UUID 를 넣고 나면 자리가 없다).
NimBLE 에서는 `conn_mode = NON` + `disc_mode = GEN` 조합이 `ADV_SCAN_IND` 로 매핑된다.

연결/해제 시 광고 모드 전환은 GAP 콜백 안에서 직접 하지 않고
`gAdvNeedsApply` 플래그를 세워 `loop()` 에서 처리한다 (재진입 위험 회피, 지연 ~2 ms).
`advertiseOnDisconnect(false)` 로 NimBLE 의 자동 재광고를 끈 것도 같은 이유 —
자동 재개는 직전 모드(non-connectable)를 그대로 물려받아 재연결이 막힌다.

1 Hz 페이로드 갱신은 광고를 멈추지 않고 `ble_gap_adv_rsp_set_data` 만 다시 쏜다.
