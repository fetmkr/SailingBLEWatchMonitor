# firmware-rak → firmware-idf 옮기기

**시작:** 2026-09-15. 기준은 커밋 `2b18b17` 의 `firmware-rak` (HLG 1.2, 보드 기록 시험 통과)이다.
**왜 옮기나:** 아두이노 코어 2.0.17 은 ESP-IDF 를 미리 빌드해서 나눠 준다. TCP 창(5,760 바이트) 같은 설정을 못 바꾼다.
WiFi 파일 받기가 241~574 KB/초에 묶인 이유다 (NEXT.md 11). 공식 PlatformIO 는 최신 7.1.3 도 아두이노 2.0.17 이다.
**도구:** ESP-IDF v6.1 공식 `idf.py` (사용자 결정 2026-09-15). 설치는 `~/esp/esp-idf`, 도구는 `~/.espressif`.

    C=/Library/Frameworks/Python.framework/Versions/3.13/lib/python3.13/site-packages/certifi/cacert.pem
    export SSL_CERT_FILE=$C REQUESTS_CA_BUNDLE=$C          # python.org Python 에 인증서 파일이 없다
    . ~/esp/esp-idf/export.sh
    cd firmware-idf
    idf.py -B ~/esp/build-sail build                        # ★ -B 필수 — 저장소 경로 빈칸 우회
    idf.py -B ~/esp/build-sail -p /dev/cu.usbmodem1101 flash
    # 로그는 pyserial 로 포트를 열어 받는다 (열면 리셋). idf.py monitor 는 대화형이라 안 씀

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
| 1 | SD + HLG 기록기 (hlog, rec_control, SD 사용권) | `board_rec_test.py all` 같은 결과 · 같은 입력으로 HLG 머리글·줄 형식 같음 · `rec check`·`rec hash` | ✅ 09-15 GPS·IMU 없이 (아래). 남은 것: 기록 중 카드 빼기 · 쓰기 일꾼 스택 남은 양 · IMU 끊김(2단계 뒤) |
| 2 | GPS (CASIC·NMEA·NAV-PV) · IMU (FIFO 100 Hz·자력계) | `gps`·`fix`·`imu` 출력 · 세션 94 처럼 1분 기록해 itow 100 ms · IMU 등간격 | 🔶 켜짐·fix·gpscfg·imu·1분 기록·test gps 통과 (아래). 남은 것: board_rec_test imu·clean (USB 멎어 못 돌림) · 밖에서 fix 잡힌 기록 |
| 3 | BLE (광고·텔레메트리 39바이트·제어 특성) | 아이폰·워치가 붙어 값 받음 · `verify.sh` 벡터 | 🔶 코드·메인 연결 · 빌드 경고 0 (USB 멎어 보드 시험 전) |
| 4 | 화면 (U8g2) | 같은 화면 | 🔶 코드·메인 연결 · 맥에서 11장면 프레임버퍼 firmware-rak 과 같음 · 빌드 경고 0 (보드 시험 전) |
| 5 | WiFi · HTTP 파일 전송 · mDNS — **TCP 창 키우기** | `/api/files`·`/file/` Range · 해시 · 받기 속도를 firmware-rak 과 같은 자리에서 비교 | ❌ |
| 6 | LoRa (RadioLib, 칩 버그 셋) | 두 보드 사이 주고받기 | ❌ |
| 7 | 전원·깊은잠·버튼·코어덤프 | 헛깸 0 · 끄는 순서 · 코어덤프 0xFF0000 | ❌ |

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
  → 17:06 사용자가 USB 를 뽑았다 꽂은 뒤 바로 올라감 (해시 확인 3개). LED 고친 판이 보드에 있다 — 부팅 로그 "LED 는 꺼 둔다", SD 89개 그대로.
- 고친 것: 속도 바꿀 때마다 `uart_set_pin` 을 다시 불러 `GPIO 43 is not usable` 경고 → 핀은 한 번, 속도는 `uart_set_baudrate`. 다시 올려 경고 사라짐 확인
- ★ `Core dump data check failed`: 코어덤프 영역에 옛 아두이노판(IDF 4.4, 판 0x00090100)이 쓴 ELF 코어덤프 23,492 바이트가 있다.
  IDF 6.1 이 형식이 달라 체크섬을 못 맞춘 것. 증거라 `~/esp/coredump_ff0000.bin` 으로 떠 뒀다 (09-11 nimble_host 죽음일 가능성 — 추측)
- ★ 빌드 출력은 `idf.py -B ~/esp/build-sail` 로 둔다. 저장소 경로의 빈칸·굽은 따옴표(`hojun’s mbp`) 때문에 picolibc.specs 경로가 깨진다
  (ESP-IDF 문서 "does not support spaces in the paths"). **우회책**이다 — 근본 해결은 빈칸 없는 경로로 저장소를 옮기는 것
- ★ install.sh 는 python.org Python 3.13 에 인증서 파일이 없어 `CERTIFICATE_VERIFY_FAILED`. 명령에만 `SSL_CERT_FILE=<certifi cacert.pem>` 을 줘서 설치 (시스템 설정은 안 바꿈)

**1단계 기록기 코드 (2026-09-15, 나눠 짠 작업 보고)**
- `main/hlog_idf.cpp` · `main/sdcard_idf.cpp` — hlog.h·sdcard.h 함수 전부. 두 파일은 경고·에러 0 으로 컴파일 (전체 빌드는 tinygpsplus 쪽 에러로 멈춤 — GPS 작업 몫)
- 원본과 글자 비교: 원본 문자열이 옮긴 쪽에 다 있음. 새로 생긴 출력은 `@HASH X 해시 계산 실패` 하나 (PSA 해시가 실패할 때)
- 달라진 것: 줄끝이 CRLF (IDF `CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF`) — serial_dump.py `strip()`·board_rec_test.py `rstrip()` 는 괜찮음, 앱 쪽은 모름 ·
  워치독은 이 작업이 등록됐을 때만 먹임 · 파일 이름 자르기는 snprintf 와 같은 결과 · `start` 의 NVS 는 get·set·get 뒤 commit 한 번
**1단계 보드 시험 결과 (2026-09-15 20:23~20:28 보드 시각, 올린 판 `~/esp/stage1` → `-B ~/esp/build-stage1`)**
- 메인 연결 `main/app_main.cpp`: USB 줄 받기(64자) · 켤 때 이유(firmware-rak 과 같은 글자, USB 리셋 한 칸만 더함) · NVS 설정 읽기(키·기본값·범위 검사 firmware-rak loadSettings 와 같음) ·
  recWantOn/Off · recStartFrom · recOnResult · recGiveUp · recControlTick · resumeRecordingIfCut · `rec` 명령 전부 · NAV 10 Hz(GPS 칸은 값 없음 표식, 전압만) · TXT 10초 · LED 기록 중 1초에 80 ms
- 빌드 경고 0. `main/CMakeLists.txt` REQUIRES 에 `esp_driver_usb_serial_jtag` 더함
- `rec ls` 89개 49.69 MB — firmware-rak 과 같음. `S00014_19700103-0043_nosa` 이름 잘림은 firmware-rak 목록에서도 똑같이 보였다 (카드 위 이름)
- **`rec hash 94 hlg/txt` — firmware-rak 이 낸 해시와 같음** (b8fa3047… · 45f00849…) · `rec check 94` 깨끗함 (IMU 등간격 100%)
- `rec on` 1분 → 세션 95: NAV 631줄 10.04 Hz · CRC 틀림 0 · 닫힘 1 · TXT 머리 방위 설정 줄·전압 4143 mV · 없는 값 `---`
- `serial_dump.py 95` 로 USB 받기 → **맥 sha256 = 보드 `rec hash` (b867f87e…)** → `tools/hlog_parse.py` v1.2 깨끗함
- `board_rec_test.py basic`: 머리글 formula 2 · 축 1/2 · off 155.70 · magHi 5.69 16.7 -3.31 · closed 1 (hdg·level·sd 는 아직 없는 명령)
- `fail`: 쓰기 실패 20번 흉내 → 3초 뒤 새 파일 3번(97→98→99→100, 앞 세션 번호 이어받음) → "다시 걸기를 다 썼습니다 (REC FAIL)" → `rec fail clear`
- `slow`: 닫기 20초 지연 중에도 명령 받음 · 닫는 중 `rec on` → "닫히면 시작" → 닫힌 뒤 세션 102 시작
- `resume`: 기록 중 포트 열어 리셋 → 켤 때 "세션 103 가 못 닫히고 끊겼습니다 — 이어서" → 104 · 사람이 멈춘 뒤 리셋 → 이어 시작 안 함
- 시험 세션 95~104 가 카드에 남아 있다 (84~94 와 같이 지울지 사용자 결정 대기)
- 카드를 붙이고 뗄 때마다 `W gpio: conflict found for GPIO[12]` — 0단계에 적은 IDF SD 드라이버 동작

**2단계 보드 시험 (2026-09-15 17:30~17:45, 커밋 f1fc8ff, 전체 빌드 `-B ~/esp/build-sail`, 이제 stage 폴더 안 씀)**
- 메인 연결: gps::begin · setWaitHook(워치독 + IMU FIFO) · imu::attach/calibrateGyro · 10 ms FIFO · 100 ms 자력 · 1 Hz 끊김 검사 ·
  buildNav(위치·도플러 원본·위성·hAcc·자력·방위) · IMU 줄 · TXT(힐·피치·방위·속도 셋) · 기록 시작 때 FIFO 비우기 · 멈출 때 IMU 끊김 정산 ·
  워치독 30초 (IDF 기본 5초를 `esp_task_wdt_reconfigure`) · 배터리 0.8·0.2 따라가기 · boot_n · 이어 시작 1분 뒤 rec_try 지우기
  (★ 1단계 판에는 boot_n 올리기와 rec_try 지우기가 빠져 있었다 — 2단계에서 넣음)
- 켤 때 로그: firmware-rak 과 같은 순서·글자 ([BOOT] · [WDT] 30초 · [PWR] GPIO14 · [GPS] 받는 버퍼 4096 · NAV-PV ACK · 130 ms 첫 프레임 · 선박 4 · [IMU] FIFO · |a| 0.99 g · 자이로 0점)
- `fix`: 체크섬 통과 448 / 실패 0 (실내라 위성 0) · `gpscfg`: 115200 · 100 ms · 움직임 4 · 정지 문턱 0 · L76K 체크섬 방식
- `imu`: 가속 +0.03 −0.99 −0.03 g · 자력 약 20 −23 25 µT · 방위 평평 113° / 기울기 보정 114°, 차이 1.3° (수평이라 거의 같아야 맞음) · 힐 축 −X 는 NVS heel_axis 0 그대로
- 1분 기록 세션 105: **IMU 6,269줄 100.00 Hz · 10 ms 등간격 100% · NAV 10.04 Hz · FIFO 넘침 0 · CRC 0 · 머리글 움직임 종류 4** — firmware-rak 세션 94 와 같은 수준
- `test gps 5`: 속도 `--.--` · CASIC 거름 0
- `[시계] GPS 로 맞췄습니다` 가 위성 0 에서 뜬다 — firmware-rak clockFromGps 도 같은 조건(날짜 유효 · 2020년 이후)이라 옮기며 생긴 차이 아님
- ★ 켤 때 로그 앞 몇 초가 안 보였던 것은 **보드 탓이 아니었다.** 올린 직후 2초 쉬고 포트를 열어 이미 지나간 로그를 못 받았고,
  pyserial 기본값으로 열면 리셋이 안 될 때가 있다. DTR·RTS 를 내린 채 열면(board_rec_test.py 방식) 0.1초에 ROM 줄부터 다 온다.
  그걸 모르고 넣었던 "USB 리셋이면 2초 기다리기" 는 뺐다
- ★★ **17:45 쯤 두 번째로 USB 가 멎었다.** `board_rec_test.py gps` 가 끝난 뒤 `imu`·`clean` 단계에 보드가 한 줄도 안 냄.
  포트는 열리고 맥은 USB 칩(303A, 3C:DC:75:70:2F:B4)을 계속 본다. 명령(`rec`)에 답 없음 · DTR·RTS 리셋 안 먹음 ·
  esptool `--before no-reset` / `default-reset` / `usb-reset` 셋 다 `No serial data received` (ROM 다운로드 모드도 아님).
  17:00 에도 똑같았고 USB 를 뽑았다 꽂아 풀렸다. 원인 [모름].
  ★ 고침: "둘 다 포트를 여러 번 열고 닫은 뒤" 는 틀렸다. 17:00 은 로그 받고 6분 가만히 둔 뒤 올리려는 순간이었다 (transcript 07:54Z~08:01Z). 17:45 만 열고 닫은 직후.
  조사 (문서·기록·웹, 포트 안 엶):
  - firmware-rak 시절에는 "칩은 보이는데 데이터 0, 뽑아야 풀림" 기록이 없다 (09-08 한 번 실패 뒤 곧 성공 · 09-14 는 목록에서 사라짐 — 다른 증상)
  - USB Serial/JTAG 는 하드웨어 고정 기능이라 리셋은 앱이 멈춰도 먹어야 한다 [확인: esp-idf docs usb-serial-jtag-console.rst:17-19 · esptool troubleshooting]. 셋 다 안 먹었으니 USB 신호 자체가 칩에 안 닿았다 [추측]
  - 문서의 멎는 길 셋(USB 핀 19·20 다른 용도 · 깊은잠 · 얕은잠)은 우리 코드에 없다 [확인: grep · CONFIG_PM_ENABLE 없음]
  - 후보: 맥 CDC 드라이버 · 보드 USB PHY/전원 · IDF 6.1 앱 · 포트 반복 열기 · 케이블/허브 — 가를 자료 없음
  - 다음에 꽂으면: ① 포트 열기 전 `ioreg -p IOUSB -l -w0 | grep -iE '"USB Product Name"|"USB Serial Number"|"locationID"'` ② DTR·RTS 내리고 **한 번만** 열어 20초 받기만 ([BOOT] 이유 · 코어덤프 줄 · 첫 바이트 시각)
    ③ 시험은 한 번 열어 묶어서 (`board_rec_test.py all` 한 번). 또 멎으면 포트·esptool 두드리지 말고 ioreg 만 남기고 알린다
  - 맥 탓/보드 탓 가르기: 다른 케이블·맥 포트·허브로 바꿔 다시 나는지 (우회책 겸 확인)
  IMU FIFO 읽기 루프는 한 번에 sPending 만큼만 돌아 끝이 있다 (imu.cpp fifoNext) — 멈춤 후보에서 뺌.
  **다음에 할 것: 뽑았다 꽂은 뒤 켤 때 [BOOT] 이유와 코어덤프 검사 줄을 먼저 본다** (워치독·패닉이었으면 거기 남는다)

- 보드 시험 순서 (메인): rec ls → rec check 94 · rec hash 94 hlg (firmware-rak 해시와 같나) → serial_dump + hlog_parse → rec on 1분 → rec check·rec head → board_rec_test.py clean·all → 기록 중 카드 빼기 → 쓰기 일꾼 스택 남은 양

## 작업 나누기 (1·2단계, 2026-09-15)

1단계(기록기)와 2단계(GPS·IMU)를 동시에 짠다. **보드는 하나라 올리기·시험은 메인만 한다.** 나눠 짜는 쪽은 빌드만.

| 누가 | 파일 (이 사람만 만진다) | 약속 |
|---|---|---|
| **기록기** | `main/hlog_idf.cpp` · `main/sdcard_idf.cpp` (+ 필요하면 `main/hlog_idf_*.h`) | `firmware-rak/include/hlog.h` · `sdcard.h` 의 함수를 **이름·뜻 그대로** 구현. HLG·TXT 바이트 형식, TXT 머리·줄 글자, `@DUMP`·`@HASH`·`rec check` 출력 글자를 firmware-rak `hlog.cpp` 와 같게 (앱·파이썬 파서·board_rec_test.py 가 그대로 돌아야 함). 시리얼은 `printf`. 쓰기 일꾼은 코어 0, 링버퍼 PSRAM. 빌드는 `-B ~/esp/build-sail-rec` |
| **GPS·IMU** | `main/gps.h/.cpp` · `main/imu.h/.cpp` · `components/tinygpsplus/` | GPS: UART1 115200, 켤 때 9600→115200 절차·PCAS·CASIC(CFG-MSG NAV-PV 10Hz, 선박 모드 4)·NMEA(TinyGPS++)·NAV-PV 값, firmware-rak `gpsPoll` 과 같은 결과. IMU: MPU-9250 FIFO 100 Hz · AK8963 자력(축 정렬·하드아이언 빼기 전후) · 단위 g · °/s · µT 가 firmware-rak 과 같게. 루프가 부르는 `poll()`/`drain()` 모양 (콜백에서 일 안 함). 빌드는 `-B ~/esp/build-sail-sens` |
| **메인** | `main/app_main.cpp` · `main/CMakeLists.txt` · `sdkconfig.defaults` · `PORTING.md` | 시리얼 명령 줄 받기, 기록 제어(rec_control.h, recWantOn/Off, 이어 시작, NVS rec_want·rec_open), buildNav/buildImu 로 둘을 잇기, 보드 올리기·시험·커밋 |

**3·4단계 나눠 짜기 (2026-09-15 17:50~, USB 멎은 동안 — 사용자 "USB 연결 없이 할거 해")**

| 누가 | 파일 | 빌드 |
|---|---|---|
| **BLE (3)** | `main/ble.h/.cpp` · `main/idf_component.yml` (esp-nimble-cpp) · sdkconfig 추가 줄은 제안만 | `-B ~/esp/build-sail-ble` |
| **화면 (4)** | `main/display.h/.cpp` · `components/u8g2*` | `-B ~/esp/build-sail-disp` |
| **USB 멎음 조사** | 읽기·웹만 | — (끝남, 아래) |
| **WiFi·HTTP·mDNS (5)** | `main/netsrv.h/.cpp` · TCP 창·WiFi 버퍼 설정 후보는 제안만 | 격리 복사본 `~/esp/stage5` → `-B ~/esp/build-stage5` |
| **LoRa (6)** | `main/lora.h/.cpp` · `components/radiolib*` | 격리 복사본 `~/esp/stage6` → `-B ~/esp/build-stage6` |

- ★ 나눠 짜는 파일이 `main/` 에 반쯤 있으면 `SRC_DIRS "."` 전체 빌드가 깨진다 (display.cpp 가 그랬다). 그래서 각자 **SRCS 를 적은 격리 복사본**으로 짓는다. 메인도 링크 확인은 `~/esp/stage3` 같은 복사본으로.

**3단계 BLE 코드 보고 (2026-09-15, 커밋 전)** — `main/ble.h/.cpp` · `main/idf_component.yml` (esp-nimble-cpp 2.5.0). ble.cpp 경고 0 컴파일, 링크는 display.cpp 가 u8g2.h 를 못 찾아 아직.
- API: `ble::loadIdentity` · `start(t,e)`/`stop` · `pump()`(광고 다시 걸기) · `publish(t,e)` · `refreshAdvPayload()` · `takeControlLine(buf,cap)` · `controlSay` · `setNotifyPeriodMs` · `saveIdentity` · `requestAdvApply`.
  controlLine 몸통(wifi·magcal·status)은 안 옮김 (5단계·메인). buildTelemetry/buildExtra 는 메인이 main.cpp 3834-3882 에서 옮긴다. batteryPercent 도.
- sdkconfig 제안: BT_ENABLED · BT_NIMBLE_ENABLED · MAX_CONNECTIONS 3 · HOST_TASK_STACK 4096 · PINNED_TO_CORE_0 · ATT_PREFERRED_MTU 255 · MAX_BONDS 3 · NVS_PERSIST · LOG_LEVEL_NONE · CTRL_BLE_MAX_ACT 6 · CTRL_DFT_TX_POWER_LEVEL_P9
- ★ **사용자에게 물을 것 1 — 송신 출력.** firmware-rak 도 `setPower(ESP_PWR_LVL_P9)` 에 열거값(11)을 dBm 자리에 넣어 실제로는 **+12 dBm** 이다 [확인: 보고 — 두 판 NimBLEDevice.cpp setPower 몸통 같음]. 옮긴 코드는 그대로 둠
- ★ **고칠 것 2 — CLAUDE.md 의 "nimble_host 스택 5120" 은 틀렸다.** firmware-rak 실제 값은 NimBLE-Arduino 기본 4096 [확인: 보고 — .pio nimconfig.h:217]. 09-11 nimble_host PANIC 과 맞는 크기. (CLAUDE.md 는 사용자 확인 뒤 고친다)
- 보드 시험 순서: 광고 로그 → 맥 스캐너로 이름·UUID·제조사 11바이트·seq → 아이폰 연결·39바이트 10 Hz → 워치 2/3 → 제어 help/status 가 루프에서 → 한 대 끊기 → verify.sh → name·hz 재부팅 유지

**3단계 메인 연결 (2026-09-15, 보드 없이 빌드만)**
- app_main: `ble::loadIdentity` (loadSettings 뒤) · 배너에 이름·module_id·notify · `batteryPercent` + gBattPct 0.8·0.2 · `buildExtra`/`buildTelemetry` (main.cpp 3834-3882 그대로) ·
  켤 때 `ble::start` (resumeRecordingIfCut 바로 앞) · 루프: `takeControlLine` → `controlLine` · `ble::pump` · notify 주기 칸 더하기 `publish` · 1 Hz `refreshAdvPayload` ·
  시리얼 `info` · `hz <1~100>` · `name <이름>` (firmware-rak 글자)
- controlLine: `status` · `help` 만. wifi …·magcal 은 firmware-rak 이 모르는 줄에 하던 대로 `err unknown …` (5단계·magcal 옮길 때 채움).
  ★ `status` 의 `ip` 칸은 firmware-rak `netsrv::ipText()` 자리인데 5단계 전이라 `-` 로 보낸다 — firmware-rak 이 꺼져 있을 때 무엇을 보냈는지는 [모름], 5단계에서 맞춘다
- sdkconfig.defaults 에 BT 12줄 (이름 전부 IDF Kconfig 에서 확인). ★ 저장소의 `firmware-idf/sdkconfig` 는 옛 설정이 남아 BT 가 꺼진 채라,
  다음 전체 빌드 전에 지우고(빌드가 다시 만든다) defaults 로 새로 만들어야 한다
- 링크 확인: display.cpp 가 아직 짓는 중이라 `~/esp/stage3` (display 뺀 SRCS, 자체 sdkconfig) 를 `-B ~/esp/build-stage3` 로 지음 → **경고 0 · 0xb8220 (88% 남음)** · BT_ENABLED=y · 호스트 스택 4096 · 연결 3 반영

**4단계 화면 코드 보고 (2026-09-15, 커밋 전)** — `main/display.h/.cpp` · `components/u8g2/` (U8g2 2.36.18 C 원본, firmware-rak PlatformIO 판과 같은 판, BSD-2. 글꼴은 쓰는 셋만 `clib/u8g2_fonts_sail.c`)
- ★ 라이브러리 표 고침: `u8g2-hal-esp-idf` 는 안 씀 — 자체 I2C 드라이버를 세워 IMU 버스를 나눠 못 쓴다 [추측: 그 HAL 소스는 안 읽음]. U8g2 C + 우리 I2C 콜백(`imu::bus()` 공유, 400 kHz, 한 번에 128바이트까지)
- 맥 대조 [확인]: 같은 U8g2 C 에 firmware-rak display_rak.cpp(아두이노 흉내) 와 새 display.cpp(IDF 흉내)를 넣고 11장면 그림 → **프레임버퍼 1024바이트 전부 같음**, `oledw` 16개 폭도 같음. 다시 돌리기: scratchpad `oledcmp/build.sh`
- 다른 점: I2C 보내기 실패를 센다(`displayI2cErrors`, firmware-rak 은 버렸다) · 화면이 IMU 보다 먼저 시작하면 `imu::begin()` 이 버스를 만든다 · 한 장 그리는 시간 [모름] (firmware-rak 33.6 ms)
- 메인 연결 (보드 없이 빌드만): 켤 때 IMU 뒤 `displayBegin` + 부트 문구 · 1 Hz `displayHealthCheck` · 그리기 기록 중 1초·평소 250 ms, DisplayState 는 firmware-rak 5833-5871 그대로
  (속도·침로·힐은 `ble::latest()`, 모드 글자는 `gps::state().dyModel` 과 `kBoatMode`) · 배 번호 NVS `boat` 읽기 · 저전압 3.0 V · 명령 `oled` · `oledw` · REQUIRES `u8g2`.
  버튼 막대(gBtnOwnsScreen)는 7단계. `~/esp/stage3` 에 display 넣어 링크 → **경고 0 · 0xce4f0 (87% 남음)**
- 보드 시험: [OLED] 붙음·부트 문구 → 평소 화면 firmware-rak 과 같나 → oledw 폭 58 89 66 87 121 74 57 98 74 58 74 82 103 113 105 89 → 화면 켠 채 10분 기록 IMU 100 Hz·FIFO 넘침 0·I2C 오류 0 → 화면 뽑기·꽂기

- ★ **보드를 다시 꽂을 때까지 누구도 포트·esptool 을 열지 않는다.** 꽂은 뒤에도 조사 보고의 "한 번만 열어 볼 것" 부터.
- 나눠 짜는 쪽은 **커밋하지 않는다**, **보드·시리얼 포트를 열지 않는다**, `app_main.cpp`·`CMakeLists.txt` 를 안 만진다.
- 필요한 IDF 부품이 `REQUIRES` 에 없으면 메인에게 말한다.
- 막히면 추측으로 채우지 말고 무엇이 모르는지 적어 보고한다.

## 파티션

firmware-rak 과 **같은 표**를 쓴다 (`partitions.csv`). NVS 자리가 같아야 보드에 저장된 설정(방위 축·자력 보정·세션 번호)이
그대로 이어진다. 코어덤프 자리도 같아야 옛 기록을 읽는다.
