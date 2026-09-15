# firmware-rak → firmware-idf 옮기기

**시작:** 2026-09-15. 기준은 커밋 `2b18b17` 의 `firmware-rak` (HLG 1.2, 보드 기록 시험 통과)이다.
**왜 옮기나:** 아두이노 코어 2.0.17 은 ESP-IDF 를 미리 빌드해서 나눠 준다. TCP 창(5,760 바이트) 같은 설정을 못 바꾼다.
WiFi 파일 받기가 241~574 KB/초에 묶인 이유다 (NEXT.md 11). 공식 PlatformIO 는 최신 7.1.3 도 아두이노 2.0.17 이다.
**도구:** ESP-IDF v6.1 공식 `idf.py` (사용자 결정 2026-09-15). 설치는 `~/esp/esp-idf`, 도구는 `~/.espressif`.

    . ~/esp/esp-idf/export.sh
    cd firmware-idf
    idf.py build
    idf.py -p /dev/cu.usbmodem1101 flash monitor

## 규칙

1. **firmware-rak 은 건드리지 않는다.** 물 위에서 검증된 펌웨어다. 옮긴 쪽이 같은 결과를 낼 때까지 그대로 쓴다.
2. **단계마다 보드에 올려 firmware-rak 과 같은 결과인지 대 본다.** 같다고 확인한 뒤에만 다음 단계로 간다.
   "빌드됨" 은 통과가 아니다.
3. **파일 형식·프로토콜·NVS 키·파티션은 그대로다.** 앱·파서·보드 설정을 바꾸지 않고 펌웨어만 갈아 끼울 수 있어야 한다.
4. **아두이노 흔적이 없는 헤더는 복사하지 않고 `firmware-rak/include` 를 같이 쓴다.**
   사본이 둘이면 조용히 어긋난다 (프로토콜 사본 네 벌에서 이미 겪었다). 아래 표의 "같이 씀" 이 그것이다.
5. **콜백에서 일하지 않는다. 스택에 KB 를 올리지 않는다. 이유는 NVS·카드에 남긴다.** (CLAUDE.md 그대로)

## 헤더 — 같이 쓰는 것 (아두이노 흔적 0, 2026-09-15 grep)

`board_rak.h` · `casic.h` · `heading_math.h` · `heading_tilt.h` · `hlog_write.h` · `http_range.h` ·
`imu_math.h` · `mag_sample.h` · `magcal.h` · `protocol.h` · `sog_policy.h` ·
`hlog.h` · `rec_control.h` · `prefs_util.h` · `sdcard.h`

★ 처음 셀 때 `hlog.h` · `rec_control.h` 를 "아두이노 흔적 1곳" 으로 셌는데, 둘 다 **주석 안의 `millis()`** 였다 (2026-09-15 다시 봄).
`prefs_util.h` 는 `begin/end` 만 있으면 되는 템플릿이고, `sdcard.h` 는 선언만 있다. 넷 다 그대로 쓴다.
구현(.cpp)은 아두이노에 기대므로 firmware-idf 에서 새로 짠다.

## 라이브러리 바꾸기

| firmware-rak | firmware-idf | 근거 |
|---|---|---|
| NimBLE-Arduino 2.5.1 | `h2zero/esp-nimble-cpp` 2.5.0 (IDF ≥ 5.3.0) | 부품 목록, 같은 저자 |
| RadioLib 7.7.1 | `jgromes/radiolib` 7.7.1 (IDF ≥ 4.1) | 부품 목록, ESP-IDF 예제 있음 |
| ESPmDNS | `espressif/mdns` 1.13.0 | 부품 목록 |
| WiFi · WebServer | `esp_wifi` · `esp_http_server` | IDF 기본 |
| SD · SPI | `esp_vfs_fat` + `sdspi` | IDF 기본 |
| Wire | `driver/i2c_master.h` | IDF 기본 |
| Preferences | `nvs_flash` · `nvs.h` | IDF 기본 — 키 이름·형식 그대로 |
| U8g2 | U8g2 + `mkfrey/u8g2-hal-esp-idf` | 커뮤니티 HAL (IDF 5.x·6.x) |
| TinyGPS++ | 그대로 가져와 `millis()` 만 바꿈 | 순수 C++ |
| mbedtls SHA-256 (`*_ret`) · base64 | PSA 해시 `psa_hash_setup/update/finish` + `PSA_ALG_SHA_256` | IDF 6.1 은 `mbedtls/sha256.h` 를 `mbedtls/private/` 로 숨겼다 (TF-PSA-Crypto) |
| `SD.open` 등 파일 API | POSIX `fopen/fseek/fread/rename/opendir` (VFS) · `esp_vfs_fat_sdspi_mount` | 긴 이름은 `CONFIG_FATFS_LFN_HEAP` |
| MPU9250_WE | I2C 로 직접 짬 | 대체 부품 없음 |

## 단계

| # | 무엇 | 통과 기준 (firmware-rak 과 대조) | 상태 |
|---|---|---|---|
| 0 | 뼈대 — 부팅 로그, 핀, 센서 전원, LED, NVS 읽기, USB 콘솔 | 켜짐 로그 · NVS `sail` 설정값이 firmware-rak 이 쓴 값 그대로 읽힘 · GPS 바이트 들어옴 | ✅ 09-15 (아래) |

**0단계 결과 (2026-09-15, 보드 3C:DC:75:70:2F:B4)**
- 부팅: IDF v6.1 · QIO 80 MHz 16 MB · PSRAM 8 MB 메모리 시험 통과 · 240 MHz · 파티션 표 firmware-rak 과 같음
- NVS `sail` 28키 그대로 읽힘 — hdg_a 1 · hdg_b 2 · hdg_off 155.700 (세션 46 분석 값과 같음) · sess_n 94 (오늘 마지막 시험 세션) · float 는 아두이노 Preferences 대로 4바이트 blob
- GPS 115200 bps 2초에 3,500~4,025 바이트, 첫 문장 `$GNGGA`
- I2C 훑기 (400 kHz, SDA 9 · SCL 40): 0x3C 화면 · 0x68 IMU — 기대와 같음
- 배터리 (GPIO1 = ADC1 채널 0, 곡선 보정, 16번 평균, 11 dB) — **firmware-idf 2,485 mV → 4.142 V · firmware-rak `batt` 2,486 mV → 4.143 V** (같은 USB 꽂힌 상태, 몇 분 사이). 1 mV 차이
- 저장 버튼 GPIO2 = 1 (안 누름)
- 시험 뒤 보드는 firmware-rak 으로 되돌려 둠 (`pio run -t upload` SUCCESS, 부팅 상태줄 확인)
- SD 목록 (읽기만, 포맷 안 함, SPI2 20 MHz): 붙는 데 65 ms · 카드 ED4QT · **파일 89개 · 49.69 MB · 남은 자리 122002 MB — firmware-rak `rec ls` 와 같음** ·
  S00046 HLG 23,868,416 · S00094 HLG 140,179 바이트도 같음
- 카드를 내릴 때 `W gpio: conflict found for GPIO[12]` (CS 핀) — **IDF SD 드라이버 안의 동작이다.** 붙일 때 CS 를 출력으로 잡으며 핀을 예약하고
  (`sdspi_host.c:378-391` → `gpio.c:389-393` esp_gpio_reserve), 뗄 때 `deinit_slot` 이 예약을 안 풀고 입력으로 바꾸어 `gpio.c:395-401` 이 경고만 찍는다.
  우리 코드 문제가 아니고 내릴 때만 난다. 기록 중에는 카드를 붙인 채 둔다.
- `cmd=52/5 R1 response: command not supported` — IDF `sdspi_transaction.c:80` 의 정보 줄(ESP_LOGI). 카드 초기화가 SDIO 명령을 물어볼 때 SD 카드가 모른다고 답한 것

**보드 운용 (사용자 결정 2026-09-15):** 이제 보드에는 firmware-idf 를 올려 둔다. 시험은 단계마다 나눠서 한다.

- LED: 0단계 첫 판이 "작업이 도나" 보려고 초록을 0.5초마다 **늘** 깜박였다. 사용자 "rec 중이 아닌데 연두색 불 깜박이네?" →
  지웠다. 켤 때 초록·파랑 끔, 기록기를 붙이면 firmware-rak 과 같게 기록 중에만 1초에 80 ms.
- 나눠 짜는 작업이 `main/` 에 반쯤 쓴 파일을 넣는 동안 메인은 **`~/esp/stage0`** (0단계 파일만 복사, 절대 경로 include) 을
  `-B ~/esp/build-stage0` 로 지어 올린다. 합칠 때 지운다.
- ★ 17:00 쯤부터 보드에 연결이 안 된다 — `Failed to connect to ESP32-S3: No serial data received` 세 번 (default-reset 둘, usb-reset 하나).
  맥은 USB 칩(303A:1001, 일련번호 3C:DC:75:70:2F:B4)을 계속 본다. 포트를 열어도 0 바이트. 사용자에게 USB·전원 뽑았다 꽂기를 부탁함.
  LED 고친 판은 빌드만 되고 아직 못 올렸다.
- 고친 것: 속도 바꿀 때마다 `uart_set_pin` 을 다시 불러 `GPIO 43 is not usable` 경고 → 핀은 한 번, 속도는 `uart_set_baudrate`. 다시 올려 경고 사라짐 확인
- ★ `Core dump data check failed`: 코어덤프 영역에 옛 아두이노판(IDF 4.4, 판 0x00090100)이 쓴 ELF 코어덤프 23,492 바이트가 있다.
  IDF 6.1 이 형식이 달라 체크섬을 못 맞춘 것. 증거라 `~/esp/coredump_ff0000.bin` 으로 떠 뒀다 (09-11 nimble_host 죽음일 가능성 — 추측)
- ★ 빌드 출력은 `idf.py -B ~/esp/build-sail` 로 둔다. 저장소 경로의 빈칸·굽은 따옴표(`hojun’s mbp`) 때문에 picolibc.specs 경로가 깨진다
  (ESP-IDF 문서 "does not support spaces in the paths"). **우회책**이다 — 근본 해결은 빈칸 없는 경로로 저장소를 옮기는 것
- ★ install.sh 는 python.org Python 3.13 에 인증서 파일이 없어 `CERTIFICATE_VERIFY_FAILED`. 명령에만 `SSL_CERT_FILE=<certifi cacert.pem>` 을 줘서 설치 (시스템 설정은 안 바꿈)
| 1 | SD + HLG 기록기 (hlog, rec_control, SD 사용권) | `board_rec_test.py all` 같은 결과 · 같은 입력으로 HLG 머리글·줄 형식 같음 · `rec check`·`rec hash` | ❌ |
| 2 | GPS (CASIC·NMEA·NAV-PV) · IMU (FIFO 100 Hz·자력계) | `gps`·`fix`·`imu` 출력 · 세션 94 처럼 1분 기록해 itow 100 ms · IMU 등간격 | ❌ |
| 3 | BLE (광고·텔레메트리 39바이트·제어 특성) | 아이폰·워치가 붙어 값 받음 · `verify.sh` 벡터 | ❌ |
| 4 | 화면 (U8g2) | 같은 화면 | ❌ |
| 5 | WiFi · HTTP 파일 전송 · mDNS — **TCP 창 키우기** | `/api/files`·`/file/` Range · 해시 · 받기 속도를 firmware-rak 과 같은 자리에서 비교 | ❌ |
| 6 | LoRa (RadioLib, 칩 버그 셋) | 두 보드 사이 주고받기 | ❌ |
| 7 | 전원·깊은잠·버튼·코어덤프 | 헛깸 0 · 끄는 순서 · 코어덤프 0xFF0000 | ❌ |

## 작업 나누기 (1·2단계, 2026-09-15)

1단계(기록기)와 2단계(GPS·IMU)를 동시에 짠다. **보드는 하나라 올리기·시험은 메인만 한다.** 나눠 짜는 쪽은 빌드만.

| 누가 | 파일 (이 사람만 만진다) | 약속 |
|---|---|---|
| **기록기** | `main/hlog_idf.cpp` · `main/sdcard_idf.cpp` (+ 필요하면 `main/hlog_idf_*.h`) | `firmware-rak/include/hlog.h` · `sdcard.h` 의 함수를 **이름·뜻 그대로** 구현. HLG·TXT 바이트 형식, TXT 머리·줄 글자, `@DUMP`·`@HASH`·`rec check` 출력 글자를 firmware-rak `hlog.cpp` 와 같게 (앱·파이썬 파서·board_rec_test.py 가 그대로 돌아야 함). 시리얼은 `printf`. 쓰기 일꾼은 코어 0, 링버퍼 PSRAM. 빌드는 `-B ~/esp/build-sail-rec` |
| **GPS·IMU** | `main/gps.h/.cpp` · `main/imu.h/.cpp` · `components/tinygpsplus/` | GPS: UART1 115200, 켤 때 9600→115200 절차·PCAS·CASIC(CFG-MSG NAV-PV 10Hz, 선박 모드 4)·NMEA(TinyGPS++)·NAV-PV 값, firmware-rak `gpsPoll` 과 같은 결과. IMU: MPU-9250 FIFO 100 Hz · AK8963 자력(축 정렬·하드아이언 빼기 전후) · 단위 g · °/s · µT 가 firmware-rak 과 같게. 루프가 부르는 `poll()`/`drain()` 모양 (콜백에서 일 안 함). 빌드는 `-B ~/esp/build-sail-sens` |
| **메인** | `main/app_main.cpp` · `main/CMakeLists.txt` · `sdkconfig.defaults` · `PORTING.md` | 시리얼 명령 줄 받기, 기록 제어(rec_control.h, recWantOn/Off, 이어 시작, NVS rec_want·rec_open), buildNav/buildImu 로 둘을 잇기, 보드 올리기·시험·커밋 |

- 나눠 짜는 쪽은 **커밋하지 않는다**, **보드·시리얼 포트를 열지 않는다**, `app_main.cpp`·`CMakeLists.txt` 를 안 만진다.
- 필요한 IDF 부품이 `REQUIRES` 에 없으면 메인에게 말한다.
- 막히면 추측으로 채우지 말고 무엇이 모르는지 적어 보고한다.

## 파티션

firmware-rak 과 **같은 표**를 쓴다 (`partitions.csv`). NVS 자리가 같아야 보드에 저장된 설정(방위 축·자력 보정·세션 번호)이
그대로 이어진다. 코어덤프 자리도 같아야 옛 기록을 읽는다.
