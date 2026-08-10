# HOHO BLE 텔레메트리 프로토콜 v1

요트 텔레메트리 모듈(ESP32-S3)과 수신 앱(iOS / watchOS) 사이의 BLE 규격.
**펌웨어(`firmware/`)와 앱(`app/`)은 이 문서를 단일 진실 공급원(single source of truth)으로 삼는다.**

- 구현 위치(펌웨어): [`firmware/include/protocol.h`](firmware/include/protocol.h)
- 구현 위치(앱): [`app/Shared/Protocol.swift`](app/Shared/Protocol.swift)

---

## 1. 식별자

| 항목 | 값 |
|---|---|
| Complete Local Name | `HOHO-<이름>` — 예: `HOHO-hojun` (전체 최대 16자) |
| Service UUID | `B0A70001-0000-4000-8000-000000000001` |
| Telemetry Characteristic UUID | `B0A70002-0000-4000-8000-000000000001` |
| Characteristic 속성 | Read + Notify |
| Manufacturer Company ID | `0xFFFF` (테스트/미할당 ID) |

> `0xFFFF`는 Bluetooth SIG가 "할당되지 않음/테스트용"으로 남겨둔 값이라 개발 단계에서 안전하게 쓸 수 있다.
> 제품화 시에는 실제 할당받은 Company ID로 교체할 것.

### 1.1 모듈 이름과 `module_id`

경기장에는 같은 서비스 UUID를 광고하는 모듈이 여러 대 있을 수 있다.
"내 배의 모듈"을 구분하기 위해 모듈마다 고유한 이름을 갖는다.

- 이름은 `HOHO-` 접두사 + 사용자 지정 문자열. 앱은 이 접두사로 우리 모듈을 골라낸다.
- 사용 가능 문자: 영숫자, `-`, `_`
- **전체 길이 최대 16바이트** (접두사 5 + 사용자 부분 11).
  Scan Response 예산에서 나온 값이다 → §4.2
- 설정하지 않으면 ESP32 MAC 뒷 2바이트로 자동 생성 (`HOHO-A3F2`).
  아무 설정 없이 여러 장을 구워도 이름이 겹치지 않는다.

`module_id`는 **이름에서 결정적으로 유도한 1바이트 해시**(FNV-1a 접기, `0`은 미지정 예약)다.

```
module_id = fold8( FNV-1a(전체 이름) )      // 0 이면 1 로 보정
```

이름은 **광고(scan response)에만** 실리고 GATT 패킷에는 없다.
연결한 뒤 "내가 맞는 모듈에 붙었나"를 매 패킷마다 확인하려면
패킷 안에 들어가는 짧은 식별자가 필요한데, 그 역할이 `module_id`다.
같은 이름이면 재부팅해도 다시 구워도 같은 값이 나온다.

예시:

| 이름 | `module_id` |
|---|---|
| `HOHO-hojun` | 1 (0x01) |
| `HOHO-KOR1234` | 243 (0xF3) |
| `HOHO-A3F2` | 7 (0x07) |

---

## 2. 물리량 인코딩 규칙 (공통)

모든 다중 바이트 필드는 **little-endian**.

| 물리량 | 단위 | 인코딩 | 타입 | 예 |
|---|---|---|---|---|
| SOG (Speed Over Ground) | knot | `round(kn × 100)` | `u16` | 5.53 kn → `553` |
| COG (Course Over Ground) | degree | `round(deg × 10)`, 범위 `0…3599` | `u16` | 315.0° → `3150` |
| Heel (힐 각) | degree | 그대로(정수), **좌현(port) 음수 / 우현(starboard) 양수** | `i8` | −12° → `0xF4` |
| Battery | % | 그대로 | `u8` | 87 → `87` |
| uptime | ms | 부팅 후 경과 시간 | `u32` | |

디코딩: `sog_kn = sog_raw / 100.0`, `cog_deg = cog_raw / 10.0`

---

## 3. GATT Characteristic 페이로드 (12 바이트)

`B0A70002-…` 를 Read 하거나 Notify 로 받을 때의 값. **정확히 12바이트.**

```
 offset  size  type   name        설명
 ------  ----  -----  ----------  ------------------------------------
 [0]     1     u8     ver         프로토콜 버전. 현재 0x01
 [1]     1     u8     module_id   모듈 식별자. 이름에서 유도 (§1.1)
 [2..5]  4     u32le  uptime_ms   부팅 후 ms
 [6..7]  2     u16le  sog         knots × 100
 [8..9]  2     u16le  cog         deg × 10 (0…3599)
 [10]    1     i8     heel        deg (좌현 음수)
 [11]    1     u8     batt        %
 ------  ----  -----  ----------  ------------------------------------
 total  12
```

바이트 배치도:

```
 0    1    2    3    4    5    6    7    8    9    10   11
+----+----+----+----+----+----+----+----+----+----+----+----+
|ver |mid |     uptime_ms     |  sog    |  cog    |heel|batt|
+----+----+----+----+----+----+----+----+----+----+----+----+
                 u32 LE          u16 LE   u16 LE   i8   u8
```

**Notify 주기: 4 Hz (250 ms)**

예시 (ver=1, module=1, uptime=123456ms, sog=5.53kn, cog=315.0°, heel=−12°, batt=87%):

```
01 01 40 E2 01 00 29 02 4E 0C F4 57
│  │  └── 0x0001E240 = 123456 ──┘  │  │
│  │                 │      │      │  └ 0x57 = 87 %
│  │                 │      │      └──── 0xF4 = −12 °
│  │                 │      └─────────── 0x0C4E = 3150 → 315.0 °
│  │                 └────────────────── 0x0229 = 553  → 5.53 kn
│  └── module_id = 1  (HOHO-hojun)
└───── ver = 1
```

---

## 4. Advertising

- 방식: **Legacy** advertising (Extended advertising 사용 안 함)
- 광고 인터벌: **200 ms** (NimBLE 단위 0.625 ms → `320`)
- 미연결 시: **connectable + scannable** (`ADV_IND`)
- 연결 중: **non-connectable + scannable** (`ADV_SCAN_IND`)
  → 연결되어 있어도 스캐너 앱에서 계속 브로드캐스트를 볼 수 있다.
  (`ADV_NONCONN_IND` 는 scan response 를 실을 수 없어 사용하지 않는다.)
- **광고 데이터 갱신 주기: 1 Hz**

### 4.1 ADV 패킷 (최대 31 바이트 / 실제 21 바이트)

| AD Type | 이름 | 길이 |
|---|---|---|
| `0x01` | Flags (`0x06` = LE General Discoverable + BR/EDR Not Supported) | 3 |
| `0x07` | Complete List of 128-bit Service UUIDs → `B0A70001-0000-4000-8000-000000000001` | 18 |

```
02 01 06                                            ← Flags
11 07 01 00 00 00 00 00 00 80 00 40 00 00 01 00 A7 B0   ← Service UUID (역순 LE)
```

### 4.2 Scan Response (최대 31 바이트)

| AD Type | 이름 | 길이 |
|---|---|---|
| `0xFF` | Manufacturer Specific Data (Company ID `0xFFFF` + 9바이트 페이로드) | 13 |
| `0x09` | Complete Local Name → `HOHO-<이름>` | 2 + N |

**모듈 이름의 길이 한도(§1.1)가 여기서 나온다.**

```
31 (한도) − 13 (Manufacturer Data) − 2 ([len][type]) = 16 바이트
```

접두사를 포함한 전체 이름은 최대 16자.
예) `HOHO-hojun` (10자) → scan response 25 바이트.

### 4.3 Manufacturer Specific Data 페이로드 (Company ID 뒤 9 바이트)

AD 구조 전체는 `[len=0x0C][type=0xFF][FF FF][9바이트 페이로드]` = 13 바이트.

```
 offset  size  type   name        설명
 ------  ----  -----  ----------  ------------------------------------
 [0]     1     u8     ver         0x01
 [1]     1     u8     module_id   모듈 식별자. 이름에서 유도 (§1.1)
 [2..3]  2     u16le  sog         knots × 100
 [4..5]  2     u16le  cog         deg × 10
 [6]     1     i8     heel        deg
 [7]     1     u8     batt        %
 [8]     1     u8     seq         광고 갱신마다 +1 (0…255 wrap)
 ------  ----  -----  ----------  ------------------------------------
 total   9
```

> `offset` 은 **Company ID(2바이트) 를 제외한** 페이로드 기준.
> CoreBluetooth 의 `CBAdvertisementDataManufacturerDataKey` 는 Company ID 를 **포함한**
> 전체 바이트열(11바이트)을 준다. 따라서 앱에서는 앞 2바이트를 확인/스킵하고 파싱한다.

**`seq` 활용:** 연속 수신한 두 광고의 `seq` 차이가 1보다 크면 그 사이 패킷이 유실된 것이다.
스캐너 탭은 이를 이용해 수신율(%)을 계산한다.

```
수신율(%) = 수신 패킷 수 / (수신 패킷 수 + 유실 패킷 수) × 100
유실 = Σ ((seq_now − seq_prev + 256) mod 256 − 1)
```

---

## 5. 연결 수명주기

```
       ┌──────────────────────────────────────────────┐
       │                                              │
       ▼                                              │
  [ADV_IND 200ms]  ──central 연결──▶  [Connected]      │
  connectable                          Notify 4Hz     │
  + scannable                          ADV_SCAN_IND   │
                                       (non-conn)     │
                                          │           │
                                          └─연결 끊김─┘
                                            즉시 재개
```

- 중앙장치 연결 → 즉시 4 Hz Notify 시작
- 연결 해제 감지 → **즉시** connectable advertising 재개 (지연 없음)
- 앱 측 재연결: `didDisconnectPeripheral` 수신 즉시 `connect()` 재요청.
  CoreBluetooth 의 `connect()` 는 타임아웃이 없으므로 pending 상태로 두면
  주변장치가 다시 광고를 시작하는 순간 자동으로 붙는다.

---

## 6. 모듈 선택 규약

경기장에는 같은 서비스 UUID를 광고하는 모듈이 여러 대 있을 수 있으므로,
앱은 **먼저 발견된 모듈에 붙으면 안 된다.** 사용자가 고른 모듈에만 붙는다.

수신 측 구현이 지켜야 할 3단 방어:

| 단계 | 시점 | 방법 |
|---|---|---|
| ① | 스캔 | `scanForPeripherals(withServices:)` 에 Service UUID 를 넘겨 OS 레벨에서 필터. 다른 BLE 기기는 콜백조차 오지 않는다 |
| ② | 연결 전 | 광고의 Complete Local Name 이 고정한 이름과 **정확히** 같은지 확인. 이름을 아직 못 읽었으면 Manufacturer Data 의 `module_id` 로 대체 판정. 둘 다 없으면 판단을 미루고 다음 광고를 기다린다 |
| ③ | 연결 후 | **매 패킷**의 `module_id` 가 고정값과 다르면 즉시 연결 해제 후 재탐색 |

③이 필요한 이유: 앱은 빠른 재연결을 위해 주변장치 식별자
(`CBPeripheral.identifier`)를 저장해 두고 스캔 없이 바로 `connect()` 한다.
이 지름길은 ②를 건너뛰므로, 저장된 식별자가 다른 기기를 가리키게 된 경우를
③에서 잡아야 한다.

고정 정보는 앱에 영구 저장하며, 사용자가 명시적으로 해제할 때만 지워진다.

```
저장 항목: { 전체 이름, module_id, 주변장치 식별자 }
```

> 주변장치 식별자는 **중앙장치(아이폰/워치)마다 다른 값**이 부여된다.
> 따라서 아이폰에서 고른 선택을 워치가 그대로 쓸 수는 없고, 각자 한 번씩 골라야 한다.

---

## 7. 버전 정책

- `ver` 필드가 `0x01` 이 아니면 수신 측은 해당 패킷을 **무시**한다.
- 페이로드 길이가 규정보다 짧으면 무시. 길면 앞부분만 파싱(전방 호환).
