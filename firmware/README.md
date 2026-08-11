# firmware — HOHO 텔레메트리 모듈 (ESP32-S3)

가상 요트 텔레메트리를 만들어 BLE 로 뿌리는 테스트 펌웨어.
프로토콜은 [`../PROTOCOL.md`](../PROTOCOL.md) 를 따른다.

| | |
|---|---|
| 기본 보드 | **Adafruit ESP32-S3 TFT Feather** ([product 5483](https://www.adafruit.com/product/5483)) |
| 대체 보드 | ESP32-S3 DevKitC-1 (화면 없음, `-e devkit_s3`) |
| 프레임워크 | Arduino (PlatformIO) |
| BLE 스택 | NimBLE-Arduino 2.5.x |
| 화면 | 240x135 ST7789 (보드 내장) |
| 시리얼 | 115200 baud (USB CDC) |

### Feather TFT 핀 (보드 variant 가 정의하는 값)

| 신호 | GPIO | 비고 |
|---|---|---|
| `TFT_I2C_POWER` | 21 | **HIGH 로 올리지 않으면 화면에 전원이 안 들어간다** |
| `TFT_CS` | 7 | |
| `TFT_DC` | 39 | |
| `TFT_RST` | 40 | |
| `TFT_BACKLITE` | 45 | 화면 초기화 후에 켠다 (부팅 시 흰 화면 방지) |
| SCK / MOSI | 36 / 35 | 하드웨어 SPI |

패널은 물리적으로 135x240 세로다. `init(135, 240)` + `setRotation(3)` 으로 240x135 가로로 쓴다.

---

## 파일 구성

```
firmware/
├── platformio.ini
├── include/
│   ├── protocol.h        ← 패킷 인코딩. app/Shared/Protocol.swift 와 쌍을 이룬다
│   ├── simulator.h       ← 가상 데이터 생성. Arduino 의존성 없는 순수 C++
│   ├── display.h         ← TFT 표시 인터페이스
│   └── display_layout.h  ← 화면 좌표·문자열 포맷. 역시 Arduino 비의존
├── src/
│   ├── main.cpp          ← BLE 서버 + 광고 제어 + 루프
│   └── display.cpp       ← ST7789 그리기 (HOHO_HAS_TFT 일 때만 컴파일)
└── tools/
    ├── sim_test.cpp      ← 호스트에서 값·화면 좌표를 실측 검증하는 하네스
    └── Makefile
```

`simulator.h` 와 `display_layout.h` 를 Arduino 와 분리해 둔 이유는, 보드에 굽기 전에
맥에서 그대로 컴파일해 **값과 화면 좌표가 정말 맞는지 확인**하기 위해서다.
특히 화면은 글자가 240px 밖으로 밀려나도 컴파일이 통과해 버린다. 그래서
`display_layout.h` 에 `static_assert` 를 걸고, 값에 따라 달라지는 문자열 길이는
호스트 테스트에서 극단값으로 재본다.

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

이 보드는 네이티브 USB 라 PlatformIO 가 1200bps 터치로 알아서 부트로더에 들어간다.
보드 정의에 `use_1200bps_touch` / `wait_for_upload_port` 가 이미 들어 있다.

> **`Failed to connect` 가 나면** `RESET` 버튼을 **두 번 빠르게** 눌러 부트로더로
> 진입시킨 뒤 다시 업로드한다. (Feather 는 BOOT+RESET 조합이 아니라 더블탭이다)

## 3. 시리얼 모니터

```bash
pio device monitor
```

정상 동작 시 1 Hz 로 이런 로그가 나온다.

```
═══════════════════════════════════════════
  HOHO-hojun — 요트 텔레메트리 BLE 테스트
  module_id 1 (0x01)
  service   b0a70001-0000-4000-8000-000000000001
  telemetry b0a70002-0000-4000-8000-000000000001
  notify 4.0Hz / adv refresh 1.0Hz
  이름을 바꾸려면:  name <이름>   (help 로 전체 명령)
═══════════════════════════════════════════
[BLE] advertising 시작 — HOHO-hojun (ADV_IND / connectable, interval 200ms)
[    1.0s] HOHO-hojun | SOG  5.61 kn | COG  45.0° | HEEL  +12.3° | BATT 100% | seq   1 | ADVERTISING
[    2.0s] HOHO-hojun | SOG  5.74 kn | COG  45.0° | HEEL  +12.9° | BATT 100% | seq   2 | ADVERTISING
[BLE] 연결됨 ← 5f:2a:... (conn=1)
[BLE] advertising 시작 — HOHO-hojun (ADV_SCAN_IND / non-connectable, interval 200ms)
[BLE] notify 구독 ON
[    3.0s] HOHO-hojun | SOG  5.88 kn | COG  45.0° | HEEL  +13.5° | BATT 100% | seq   3 | CONNECTED (notify ON)
```

### 보드 이름 붙이기

시리얼 모니터에서 바로 설정한다. NVS 에 저장되므로 재부팅해도 유지되고,
다시 플래시해도 남는다 (`pio run -t erase` 하면 지워짐).

```
name hojun
```

```
[ID ] 이름 HOHO-hojun | module_id 1 (0x01) | MAC F4:12:FA:59:75:D5
[ID ] 저장 완료 — 앱에서 모듈을 다시 선택해야 합니다.
[BLE] advertising 시작 — HOHO-hojun (ADV_IND / connectable, interval 200ms)
```

| 명령 | 설명 |
|---|---|
| `name <이름>` | 보드 이름 설정. 최대 11자, 영숫자/`-`/`_` |
| `info` | 현재 이름과 `module_id` 출력 |
| `help` | 도움말 |

설정하지 않으면 **MAC 의 뒤쪽 2바이트**로 자동 생성된다.
예) MAC `F4:12:FA:59:75:D5` → `HOHO-75D5`
따라서 아무 설정 없이 여러 장을 구워도 이름이 겹치지 않는다.

> ⚠️ **앞쪽 바이트를 쓰면 안 된다.** `F4:12:FA` 는 Espressif OUI(제조사 공통 접두사)라
> 모든 보드가 같다. 그런데 `ESP.getEfuseMac()` 은 MAC[0] 이 최하위 바이트인 uint64 를
> 돌려주므로 `mac & 0xFF` 가 바로 그 OUI 첫 바이트다. 실제로 이 실수로 모든 보드가
> `HOHO-12F4` 로 나오는 버그가 있었다. 지금은 `esp_read_mac()` 으로 바이트 배열을
> 받아 `mac[4]`, `mac[5]` 를 쓴다.

이름을 바꾸면 `module_id` 도 바뀌므로 앱에서 모듈을 다시 골라야 한다.

## 4. 내장 화면

Feather TFT 는 240x135 화면에 지금 내보내고 있는 값을 그대로 띄운다.
(모듈은 송신 측이므로 "수신" 개념은 없다 — 광고/연결 상태만 표시)

```
┌────────────────────────────────────────┐
│ hojun                    ● ADVERTISING │  이름 + 연결 상태
│                                        │
│   6.96                              kn │  SOG (청록, 큰 숫자)
│                                        │
│   315° NW                              │  COG (노랑) + 나침반 방위
│                                        │
│ HEEL -18  BATT 100%  SEQ  91  UP   91s │
└────────────────────────────────────────┘
```

상태는 `ADVERTISING`(주황) / `LINK`(초록) / `LINK NOTIFY`(초록, 구독 중).

깜빡임을 없애려고 **바뀐 문자열만** 다시 그린다. 모든 값은 고정폭으로 포맷하고
(`%5.2f`, `%03d` 등) 글자 배경색을 함께 칠해 이전 픽셀을 덮는다.
폭이 흔들리면 이전 글자가 남으므로, 고정폭 여부는 호스트 테스트가 극단값으로 검사한다.

---

## 5. 하드웨어 없이 로직 검증

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
