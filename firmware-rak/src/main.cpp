// ─────────────────────────────────────────────────────────────────────────
//  Sailing Monitor — RAK3112 (ESP32-S3) + RAK19007 베이스보드
//
//  값을 어디서 가져오나
//
//    SOG·COG   GPS(RAK12501 / L76K)가 위성을 잡았을 때만 실측.
//              못 잡으면 시뮬레이터 값으로 채운다.
//    HEEL      IMU(RAK1905 / MPU-9250)가 붙어 있으면 언제나 실측.
//              GPS 는 배가 얼마나 기울었는지 알려주지 못한다.
//    BATT      GPIO1 의 ADC 실측 (분압 되짚기).
//
//  ★ 시뮬레이터로 채운 값은 로그에 (SIM) 이라고 반드시 표시한다.
//    가짜 값이 실측처럼 보이면 바다에서 엉뚱한 판단을 하게 된다.
//
//  시리얼 명령 (115200) — 자세한 목록은 printHelp()
//    check   붙어 있는 것을 한 번에 점검
//    fix     GPS 파싱 상태 / imu  9축 값 / scan  I2C 목록
//    power   센서 전원 스위치 (실측으로 GPIO14 확정)
//    name <이름> / hz <1~100> / info / batt / sd / gps / calib / help
//
//  규격: ../../PROTOCOL.md
// ─────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <MPU9250_WE.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>

#include "board_rak.h"
#include "display_rak.h"
#include "protocol.h"
#include "simulator.h"

using sail::Telemetry;

// ── 모듈 신원 ────────────────────────────────────────────────────────────
static Preferences gPrefs;
static char        gUserName[sail::kMaxUserNameLen + 1] = {0}; // "hojun"
static char        gFullName[sail::kMaxFullNameLen + 1] = {0}; // "SAIL-hojun"
static uint8_t     gModuleID = 1;

// ── BLE 전역 상태 ────────────────────────────────────────────────────────
static NimBLEServer*         gServer       = nullptr;
static NimBLECharacteristic* gTelemetryChr = nullptr;

static volatile bool gConnected     = false; // 중앙장치 연결 여부
static volatile bool gAdvNeedsApply = true;  // 광고 모드 재적용 필요
static volatile bool gSubscribed    = false; // notify 구독 여부(로그용)

static uint8_t   gSeq = 0; // manufacturer data 시퀀스
static uint32_t  gNotifyPeriodMs = sail::kNotifyPeriodMs;
static Telemetry gLatest;

// ── 실측 센서 ────────────────────────────────────────────────────────────
static MPU9250_WE gImu = MPU9250_WE(rak::kAddrImu);
static TinyGPSPlus gGps;

static bool gImuOk = false; // 가속도·자이로가 붙었나
static bool gMagOk = false; // 자력계(AK8963)까지 붙었나

// 마지막으로 읽은 9축 값 (로그와 표시에 쓴다)
static xyzFloat gAcc, gGyr, gMag;
static float    gRollDeg  = 0.0f;
static float    gPitchDeg = 0.0f;
static float    gImuTempC = 0.0f;

// 자이로 0점 (원시값). 자세히는 아래 "자이로 0점" 항목 참고.
static float gGyrOffX = 0.0f, gGyrOffY = 0.0f, gGyrOffZ = 0.0f;

// ── 힐 0점 ───────────────────────────────────────────────────────────────
//
// 센서가 내놓는 각도는 보드를 어느 방향으로 달았느냐에 따라 통째로 치우친다.
// 실제로 책상에 뒤집어 놓았더니 roll 이 -178° 로 나왔다. 그대로 쓰면 힐이
// 항상 뒤집힌 값이 되고, 프로토콜의 힐은 -128~127 범위라 잘려 나간다.
//
// 그래서 "지금 이 자세가 평형" 이라고 알려주는 기준각을 따로 둔다.
// 배를 물에 띄우고 평형일 때 `level` 명령을 치면 그때 각도를 0 으로 삼는다.
// NVS 에 저장되므로 재부팅해도, 다시 구워도 남는다.
static float gHeelOffsetDeg = 0.0f;

// 각도를 -180 ~ +180 안으로 접는다.
static float wrap180(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static float currentHeelDeg() {
    return wrap180(gRollDeg - gHeelOffsetDeg);
}

// ── 값의 출처 ────────────────────────────────────────────────────────────
//
// GPS 가 위성을 잡으면 실제 속도·침로를, 못 잡으면 시뮬레이터 값을 내보낸다.
// 어느 쪽인지 로그에 반드시 찍는다. 가짜 값이 진짜처럼 보이면 안 된다.
static bool gGpsFix     = false; // 지금 GPS 값을 믿을 수 있나
static bool gEverHadFix = false; // 한 번이라도 잡은 적 있나 (진단용)

// GPS 가 준 값이 이 시간보다 오래됐으면 낡은 것으로 본다.
static constexpr uint32_t kGpsStaleMs = 3000;

// 워치독에게 살아 있다고 알린다. 몸통은 아래 "멈추지 않기 위한 장치" 에 있다.
// 몇 초씩 걸리는 진단 명령들이 이걸 먼저 쓰므로 여기서 미리 알려 둔다.
static void feedWatchdog();

// ── 센서 전원 ────────────────────────────────────────────────────────────
//
// 지금 어느 핀을 전원 스위치로 쓰고 있는지. 0 이면 끈 상태.
// 문서끼리 값이 어긋나므로(board_rak.h 참고) 실기기에서 판정하고 NVS 에 남긴다.
static int gSensorPowerPin = rak::kSensorPowerA;

// ── 이름 관리 ────────────────────────────────────────────────────────────

// 설정된 이름이 없을 때의 기본값. MAC 의 **뒤쪽** 바이트를 쓴다.
//
// ★ MAC 앞 3바이트는 Espressif OUI 라 모든 보드가 같다. 거기를 쓰면
//   기본 이름이 전부 겹쳐서 "보드마다 다르게" 라는 목적이 깨진다.
//   ESP.getEfuseMac() 은 MAC[0] 이 최하위인 uint64 를 돌려주므로
//   `mac & 0xFF` 가 바로 그 OUI 첫 바이트다. 그래서 쓰지 않는다.
static void defaultUserName(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA); // 폴백
    }
    snprintf(out, cap, "%02X%02X", mac[4], mac[5]);
}

static void formatMac(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 광고에 실을 수 있는 문자만 남긴다.
//
// 영숫자와 '-', '_' 에 더해 괄호도 받는다. 이 배의 이름이 "random()" 이라
// 괄호가 잘리면 이름이 달라져 버린다.
// 화면 폰트(5x7)와 BLE 광고 둘 다 아스키라 그대로 나간다.
static void sanitizeName(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < cap; i++) {
        char c = in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '(' || c == ')';
        if (ok) out[j++] = c;
    }
    out[j] = '\0';
}

static void applyIdentity(const char* userName) {
    sanitizeName(userName, gUserName, sizeof(gUserName));
    if (gUserName[0] == '\0') {
        defaultUserName(gUserName, sizeof(gUserName));
    }
    snprintf(gFullName, sizeof(gFullName), "%s%s", sail::kNamePrefix, gUserName);
    gModuleID        = sail::moduleIDFromName(gFullName);
    gLatest.moduleID = gModuleID;
}

static void loadSettings() {
    gPrefs.begin("sail", /*readOnly=*/true);
    String saved    = gPrefs.getString("name", "");
    gNotifyPeriodMs = gPrefs.getUInt("notify_ms", sail::kNotifyPeriodMs);
    gSensorPowerPin = (int)gPrefs.getInt("pwr_pin", rak::kSensorPowerA);
    gHeelOffsetDeg  = gPrefs.getFloat("heel_off", 0.0f);
    gGyrOffX        = gPrefs.getFloat("gyr_x", 0.0f);
    gGyrOffY        = gPrefs.getFloat("gyr_y", 0.0f);
    gGyrOffZ        = gPrefs.getFloat("gyr_z", 0.0f);
    gPrefs.end();

    if (gNotifyPeriodMs < 10 || gNotifyPeriodMs > 2000) {
        gNotifyPeriodMs = sail::kNotifyPeriodMs;
    }
    if (gSensorPowerPin != rak::kSensorPowerA &&
        gSensorPowerPin != rak::kSensorPowerB && gSensorPowerPin != 0) {
        gSensorPowerPin = rak::kSensorPowerA;
    }

    if (saved.length() > 0) {
        applyIdentity(saved.c_str());
    } else {
        char fallback[sail::kMaxUserNameLen + 1];
        defaultUserName(fallback, sizeof(fallback));
        applyIdentity(fallback);
    }
}

static void saveIdentity(const char* userName) {
    applyIdentity(userName);
    gPrefs.begin("sail", /*readOnly=*/false);
    gPrefs.putString("name", gUserName);
    gPrefs.end();
}

// ── 센서 전원 제어 ───────────────────────────────────────────────────────
//
// GPS 예제(RAK12501_GPS_L76K.ino)를 따라 껐다 켠다. 그냥 HIGH 로 올리는 것보다
// 모듈이 깨끗한 상태에서 시작한다. GPS 는 이 리셋이 있어야 잘 붙는다.
static void applySensorPower(int pin, bool cycle) {
    // 안 쓰는 후보 핀은 입력으로 되돌려 둔다. 두 핀을 동시에 몰면
    // 어느 쪽이 진짜인지 판정할 수 없다.
    int other = (pin == rak::kSensorPowerA) ? rak::kSensorPowerB : rak::kSensorPowerA;
    pinMode(other, INPUT);

    if (pin == 0) {
        pinMode(rak::kSensorPowerA, INPUT);
        pinMode(rak::kSensorPowerB, INPUT);
        Serial.println("[PWR] 센서 전원 끔 (두 후보 핀 모두 입력으로)");
        return;
    }

    pinMode(pin, OUTPUT);
    if (cycle) {
        digitalWrite(pin, LOW);
        delay(300);
    }
    digitalWrite(pin, HIGH);
    delay(300);
    Serial.printf("[PWR] 센서 전원 ON — GPIO%d 를 HIGH 로\n", pin);
}

// ── 배터리 실측 ──────────────────────────────────────────────────────────
//
// RAK19007 회로도 기준으로 분압을 되짚는다. 자세한 근거는 board_rak.h.
static float readBatteryVolts(uint32_t* rawMvOut) {
    // 여러 번 재서 평균. 소스 임피던스가 2.5 MΩ 로 높아 값이 흔들린다.
    uint32_t sum = 0;
    const int kSamples = 16;
    for (int i = 0; i < kSamples; i++) {
        sum += analogReadMilliVolts(rak::kBattAdcPin);
        delay(2);
    }
    uint32_t mv = sum / kSamples;
    if (rawMvOut) *rawMvOut = mv;
    return (mv / 1000.0f) / rak::kBattDivider * rak::kBattCorrection;
}

// 방전 곡선 표에서 잔량을 찾는다. 표 사이는 직선으로 잇는다.
// 곡선을 쓰는 이유는 board_rak.h 의 kBattCurve 주석 참고.
static float batteryPercent(float volts) {
    if (volts >= rak::kBattCurve[0].volts) return 100.0f;

    const int last = rak::kBattCurveLen - 1;
    if (volts <= rak::kBattCurve[last].volts) return 0.0f;

    for (int i = 0; i < last; i++) {
        const float vHi = rak::kBattCurve[i].volts;      // 전압이 높은 쪽
        const float vLo = rak::kBattCurve[i + 1].volts;  // 낮은 쪽
        if (volts <= vHi && volts >= vLo) {
            const float pHi = rak::kBattCurve[i].percent;
            const float pLo = rak::kBattCurve[i + 1].percent;
            const float t   = (volts - vLo) / (vHi - vLo); // 0 = 낮은 쪽
            return pLo + (pHi - pLo) * t;
        }
    }
    return 0.0f; // 여기까지 오면 표가 잘못 적힌 것이다
}

static void printBattery() {
    uint32_t mv = 0;
    float    v  = readBatteryVolts(&mv);
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  GPIO%d 실측       %u mV\n", rak::kBattAdcPin, (unsigned)mv);
    Serial.printf("  분압 되짚기       ÷ %.2f\n", rak::kBattDivider);
    Serial.printf("  배터리 전압       %.3f V  (약 %.0f%%)\n", v, batteryPercent(v));
    Serial.println("──────────────────────────────────────────");
    Serial.println("  잔량은 리튬폴리머 방전 곡선으로 환산합니다 (직선 아님).");
    Serial.println("──────────────────────────────────────────");
    Serial.println("  ★ USB 가 꽂혀 있으면 충전 중이라 실제보다 높게 나옵니다.");
    Serial.println("    진짜 잔량은 USB 를 뽑고 재야 합니다.");
    Serial.println("  멀티미터 값과 어긋나면 board_rak.h 의 kBattCorrection 조정.");
}

// ── I2C 스캔 ─────────────────────────────────────────────────────────────
//
// 부품번호를 몰라도 보드가 직접 알려준다. 주소로 부품을 추정해 준다.
static const char* guessI2CDevice(uint8_t addr) {
    switch (addr) {
        // 이 보드에 실제로 붙어 있는 것 (★)
        case 0x3C: return "★ SSD1306 화면 — RAK1921 (J12 헤더)";
        case 0x68: return "★ MPU-9250 IMU — RAK1905";
        case 0x0C: return "★ AK8963 자력계 — RAK1905 안에 들어있음";

        // 나머지는 참고용
        case 0x3D: return "SSD1306 화면 (주소 점퍼가 반대쪽)";
        case 0x69: return "MPU-9250 (AD0 가 HIGH) 또는 다른 IMU";
        case 0x18:
        case 0x19: return "LIS3DH 가속도 (RAK1904)";
        case 0x1D:
        case 0x53: return "ADXL 계열 가속도";
        case 0x0D: return "자력계 (BMM150 등)";
        case 0x76:
        case 0x77: return "BME280/BMP280/BME680 환경센서";
        case 0x42: return "u-blox GNSS (RAK12500)";
        case 0x51:
        case 0x52: return "RTC";
        case 0x28:
        case 0x29: return "BNO055 자세센서 또는 거리센서";
        default:   return "";
    }
}

// 스캔 중에 기대하는 주소를 봤는지 표시해 둔다.
static bool gSawDisplay = false;
static bool gSawImu     = false;

static int scanBus(TwoWire& bus, const char* label, int sda, int scl) {
    bus.begin(sda, scl, 100000);
    Serial.printf("  %s (SDA GPIO%d / SCL GPIO%d)\n", label, sda, scl);

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) {
            const char* guess = guessI2CDevice(addr);
            Serial.printf("    0x%02X  %s\n", addr, guess[0] ? guess : "(알 수 없음)");
            if (addr == rak::kAddrDisplay) gSawDisplay = true;
            if (addr == rak::kAddrImu)     gSawImu     = true;
            found++;
        }
    }
    if (found == 0) Serial.println("    (응답 없음)");
    return found;
}

static void doScan() {
    gSawDisplay = false;
    gSawImu     = false;

    Serial.println("──────────────────────────────────────────");
    Serial.printf("  I2C 스캔 — 센서 전원 %s\n",
                  gSensorPowerPin ? "ON" : "OFF (power 명령으로 켜세요)");
    int a = scanBus(Wire, "I2C1 — 센서 슬롯 A~D + J12 헤더",
                    rak::kI2C1_SDA, rak::kI2C1_SCL);
    int b = scanBus(Wire1, "I2C2 — 코어 커넥터에서 끝나는 버스",
                    rak::kI2C2_SDA, rak::kI2C2_SCL);

    // 이 보드에 붙어 있어야 할 것과 대조한다.
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  화면 RAK1921 (0x%02X)  %s\n", rak::kAddrDisplay,
                  gSawDisplay ? "보임" : "안 보임");
    Serial.printf("  IMU  RAK1905 (0x%02X)  %s\n", rak::kAddrImu,
                  gSawImu ? "보임" : "안 보임");
    Serial.println("  GPS(UART)와 SD(SPI)는 I2C 가 아니라 여기 안 나옵니다.");
    Serial.println("──────────────────────────────────────────");
    // 여기서 한 번 헛짚었다. IMU 가 보인다고 센서 전원이 켜진 것은 아니다.
    Serial.println("  ※ IMU 는 항상 켜져 있는 VDD 를 쓴다. 전원 스위치와 무관하다.");
    Serial.println("    전원 핀 판정은 scan 이 아니라 gps 명령으로 한다.");
    Serial.println("──────────────────────────────────────────");

    if (a + b == 0) {
        Serial.println("  아무것도 안 잡혔습니다. 모듈이 덜 꽂혔는지 보세요.");
        Serial.println("  (IMU 조차 안 보이면 I2C 배선 자체를 의심할 상황입니다)");
    } else if (!gSawDisplay) {
        Serial.println("  화면이 안 보입니다. J12 헤더에 꽂혀 있는지 보세요.");
        Serial.println("  (센서 슬롯이 아니라 2.54mm I2C 핀헤더입니다)");
    }

    // 화면을 봤는데 아직 안 붙어 있으면 지금 붙인다. 재부팅할 필요 없다.
    if (gSawDisplay && sail::displayBegin()) {
        Serial.println("  화면을 붙였습니다 — 값이 바로 나옵니다.");
    }
}

// ── GPS (RAK12501 / L76K, 슬롯 A) ────────────────────────────────────────
//
// UART1 로 1 초에 한 번 NMEA 문장 뭉치를 보낸다. 기본 9600 bps.
// 위성을 잡기 전에도 문장은 계속 나오는데, 값이 비어 있고 상태 글자가
// V(무효)로 온다. 그래서 "바이트가 들어온다" 와 "위치를 안다" 는 다른 얘기다.
static void gpsBegin() {
    Serial1.begin(9600, SERIAL_8N1, rak::kUART1_RX, rak::kUART1_TX);
}

// loop() 에서 계속 부른다. 들어온 바이트를 파서에 먹인다.
static void gpsPoll() {
    while (Serial1.available()) {
        gGps.encode((char)Serial1.read());
    }
}

// 지금 GPS 값을 믿어도 되는지 판정한다.
//
// ★ age() 검사가 꼭 필요하다. 한 번 위성을 잡았다가 놓쳐도 라이브러리는
//   마지막 값을 그대로 들고 있다. 낡은 값을 안 걸러내면 신호가 끊긴 뒤에도
//   옛날 속도를 진짜인 양 계속 내보내게 된다. 배 위에서 이건 위험하다.
static void gpsUpdateFix() {
    bool ok = gGps.location.isValid() && gGps.location.age() < kGpsStaleMs &&
              gGps.speed.isValid() && gGps.speed.age() < kGpsStaleMs;
    if (ok) gEverHadFix = true;
    gGpsFix = ok;
}

// 1 Hz 로그에 붙일 한 줄
static void printGpsLine() {
    int sats = gGps.satellites.isValid() ? (int)gGps.satellites.value() : 0;
    if (gGpsFix) {
        Serial.printf("   GPS  위성 %d | %.6f, %.6f | HDOP %.1f\n",
                      sats, gGps.location.lat(), gGps.location.lng(),
                      gGps.hdop.isValid() ? gGps.hdop.hdop() : 99.9);
    } else {
        Serial.printf("   GPS  위성 %d | 아직 못 잡음%s\n",
                      sats, gEverHadFix ? " (한 번 잡았다가 놓침)" : "");
    }
}

// 파싱된 상태를 자세히 본다.
static void doFix() {
    gpsUpdateFix();
    int sats = gGps.satellites.isValid() ? (int)gGps.satellites.value() : 0;

    Serial.println("──────────────────────────────────────────");
    Serial.println("  GPS 파싱 상태 (RAK12501 / L76K)");
    Serial.printf("  받은 글자     %lu\n", (unsigned long)gGps.charsProcessed());
    Serial.printf("  체크섬        통과 %lu / 실패 %lu\n",
                  (unsigned long)gGps.passedChecksum(),
                  (unsigned long)gGps.failedChecksum());
    Serial.printf("  위성 수       %d\n", sats);
    Serial.printf("  fix           %s\n", gGpsFix ? "있음" : "없음");

    if (gGpsFix) {
        Serial.printf("  위치          %.6f, %.6f\n",
                      gGps.location.lat(), gGps.location.lng());
        Serial.printf("  속도(SOG)     %.2f kn\n", gGps.speed.knots());
        Serial.printf("  침로(COG)     %.1f°\n", gGps.course.deg());
        Serial.printf("  HDOP          %.1f (작을수록 정확)\n",
                      gGps.hdop.isValid() ? gGps.hdop.hdop() : 99.9);
    }
    Serial.println("──────────────────────────────────────────");

    if (gGps.charsProcessed() == 0) {
        Serial.println("  한 글자도 안 들어왔습니다.");
        Serial.printf("    → power %d 로 센서 전원이 켜져 있는지 보세요.\n",
                      rak::kSensorPowerA);
    } else if (gGps.failedChecksum() > gGps.passedChecksum() / 4) {
        Serial.println("  체크섬 실패가 많습니다. 통신 속도(9600)를 의심하세요.");
    } else if (!gGpsFix) {
        Serial.println("  문장은 잘 들어옵니다. 위성만 아직 못 잡았습니다.");
        Serial.println("  실내에서는 정상입니다. 창가나 밖으로 나가면 잡힙니다.");
        Serial.println("  차가운 시작은 35초쯤 걸립니다.");
    }
}

// 파싱하지 않은 원시 바이트를 그대로 보여준다. NMEA 문장이 눈에 보이면
// GPS 가 말하고 있다는 뜻이다.
static void peekGps(uint32_t seconds, bool slotD) {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  UART1 (RX GPIO%d / TX GPIO%d) 9600bps — %us 동안 원시 데이터\n",
                  rak::kUART1_RX, rak::kUART1_TX, (unsigned)seconds);
    Serial.printf("  슬롯 %s 로 가정합니다.\n", slotD ? "D" : "A");

    // GPS 모듈은 커넥터 핀10 을 RESET 으로 받는다. 그 핀이 슬롯마다 다르다.
    //   슬롯 A → IO2 (센서 전원 스위치와 같은 선. 전원을 켜면 리셋도 풀린다)
    //   슬롯 D → IO6 (GPIO39. 따로 HIGH 로 올려 줘야 한다)
    if (slotD) {
        Serial.printf("  슬롯 D 이므로 GPIO%d(IO6)을 HIGH 로 올려 리셋을 풉니다.\n",
                      rak::kGpsResetSlotD);
        pinMode(rak::kGpsResetSlotD, OUTPUT);
        digitalWrite(rak::kGpsResetSlotD, LOW);
        delay(200);
        digitalWrite(rak::kGpsResetSlotD, HIGH);
        delay(500);
    }
    Serial.println("──────────────────────────────────────────");

    // Serial1 은 setup 에서 이미 열려 있다. 여기서는 읽기만 한다.
    uint32_t start = millis();
    uint32_t bytes = 0;
    while (millis() - start < seconds * 1000UL) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            Serial.write(c);
            gGps.encode(c); // 보여주면서 파서에도 먹인다
            bytes++;
        }
        feedWatchdog();
        delay(5);
    }

    Serial.println();
    Serial.println("──────────────────────────────────────────");
    if (bytes == 0) {
        Serial.println("  한 바이트도 안 들어왔습니다. 순서대로 의심하세요:");
        Serial.println("    1) 센서 전원이 꺼져 있다 (power 명령)");
        Serial.println("    2) GPS 가 슬롯 B 나 C 에 꽂혀 있다 → A 나 D 로 옮기기");
        if (!slotD) Serial.println("    3) 슬롯 D 에 꽂혀 있다 → gps d 로 다시 해보기");
        else        Serial.println("    3) 슬롯 A 에 꽂혀 있다 → gps 로 다시 해보기");
    } else {
        Serial.printf("  %u 바이트 수신 — GPS 가 말하고 있습니다.\n", (unsigned)bytes);
        Serial.println("  쉼표 사이가 비어 있으면 아직 위성을 못 잡은 것입니다.");
        Serial.println("  자세한 상태는 fix 명령으로 보세요.");
    }
}

// ── IMU (RAK1905 / MPU-9250, 슬롯 C) ─────────────────────────────────────
//
// 9축이다. 가속도로 기울기를, 자이로로 회전 속도를, 자력계로 방위를 잰다.
// 힐(좌우 기울기)은 여기서만 나온다 — GPS 는 배가 얼마나 기울었는지 모른다.
static bool imuBegin() {
    Wire.begin(rak::kI2C1_SDA, rak::kI2C1_SCL, 400000);

    gImuOk = gImu.init();
    if (!gImuOk) return false;

    // ★ autoOffsets() 를 여기서 부르지 않는 이유
    //   그 함수는 "지금 평평하고 멈춰 있다" 를 전제로 0점을 잡는다.
    //   배 위에서 부팅하면 그때 기울어져 있던 각도가 0 이 되어 힐이 통째로
    //   어긋난다. 0점이 필요하면 배가 평평할 때 `calib` 명령으로 잡는다.

    gImu.enableGyrDLPF();
    gImu.setGyrDLPF(MPU9250_DLPF_6); // 가장 조용한 설정
    gImu.setSampleRateDivider(5);

    gMagOk = gImu.initMagnetometer();
    return true;
}

// ── 자이로 0점 ───────────────────────────────────────────────────────────
//
// MPU-9250 은 가만히 있어도 자이로가 0 이 아니다. 공장에서 나올 때부터
// 축마다 조금씩 치우쳐 있다. 실제로 이 보드는 책상에 가만히 뒀는데도
// -0.5 / +0.9 / +1.1 °/s 가 계속 나왔다.
//
// 이건 "드리프트" 가 아니라 영점이 어긋난 것이다. 값 자체는 안정적이라
// 평균을 내서 빼주면 사라진다.
//
// ★ 자이로 0점은 자세와 상관없다. 보드가 기울어 있어도, 뒤집혀 있어도
//   정지해 있기만 하면 제대로 잡힌다. (가속도 0점은 그렇지 않다 —
//   그래서 라이브러리의 autoOffsets() 는 쓰지 않는다. 그건 가속도까지
//   건드려서 배 위에서 부르면 힐이 통째로 어긋난다.)
//
// setGyrOffsets() 가 받는 값은 °/s 가 아니라 원시값이다.
//   °/s = 원시값 x 250 / 32768   (±250°/s 범위) → 1 °/s 가 약 131
// 재는 동안 이 폭보다 크게 흔들렸으면 못 믿는다. 원시값 500 은 약 3.8 °/s.
static constexpr float kGyrCalMaxSpreadRaw = 500.0f;

static void applyGyrOffsets() {
    if (gImuOk) gImu.setGyrOffsets(gGyrOffX, gGyrOffY, gGyrOffZ);
}

// 지금 자이로 값을 0 으로 삼는다. 보드가 멈춰 있어야 한다.
static bool calibrateGyro() {
    if (!gImuOk) return false;

    gImu.setGyrOffsets(0.0f, 0.0f, 0.0f); // 보정을 지우고 날값을 본다

    const int kN = 64;
    float sx = 0, sy = 0, sz = 0;
    float mnx = 1e9f, mny = 1e9f, mnz = 1e9f;
    float mxx = -1e9f, mxy = -1e9f, mxz = -1e9f;

    for (int i = 0; i < kN; i++) {
        xyzFloat r = gImu.getGyrRawValues();
        sx += r.x; sy += r.y; sz += r.z;
        if (r.x < mnx) mnx = r.x;  if (r.x > mxx) mxx = r.x;
        if (r.y < mny) mny = r.y;  if (r.y > mxy) mxy = r.y;
        if (r.z < mnz) mnz = r.z;  if (r.z > mxz) mxz = r.z;
        feedWatchdog();
        delay(5);
    }

    float spread = mxx - mnx;
    if (mxy - mny > spread) spread = mxy - mny;
    if (mxz - mnz > spread) spread = mxz - mnz;

    if (spread > kGyrCalMaxSpreadRaw) {
        // 흔들리는 동안 잰 값은 쓰면 안 된다. 이전 보정을 되돌린다.
        applyGyrOffsets();
        Serial.printf("[IMU] 자이로 0점 실패 — 재는 동안 흔들렸습니다 "
                      "(폭 %.0f, 한계 %.0f)\n", spread, kGyrCalMaxSpreadRaw);
        return false;
    }

    gGyrOffX = sx / kN;
    gGyrOffY = sy / kN;
    gGyrOffZ = sz / kN;
    applyGyrOffsets();

    gPrefs.begin("sail", false);
    gPrefs.putFloat("gyr_x", gGyrOffX);
    gPrefs.putFloat("gyr_y", gGyrOffY);
    gPrefs.putFloat("gyr_z", gGyrOffZ);
    gPrefs.end();

    Serial.printf("[IMU] 자이로 0점 잡음 — 원시 %.0f %.0f %.0f "
                  "(= %.2f %.2f %.2f °/s) 만큼 빼둡니다\n",
                  gGyrOffX, gGyrOffY, gGyrOffZ,
                  gGyrOffX * 250.0f / 32768.0f,
                  gGyrOffY * 250.0f / 32768.0f,
                  gGyrOffZ * 250.0f / 32768.0f);
    return true;
}

// 최신 9축 값을 전역에 담는다. 10 Hz 로 부른다.
static void imuUpdate() {
    if (!gImuOk) return;
    gAcc = gImu.getGValues();
    gGyr = gImu.getGyrValues();
    if (gMagOk) gMag = gImu.getMagValues();
    gRollDeg  = gImu.getRoll();
    gPitchDeg = gImu.getPitch();
}

// 자력계로 뱃머리 방위를 구한다. 못 구하면 음수.
//
// GPS 의 침로(COG)는 배가 "실제로 가는 방향" 이고, 이 방위(HDG)는 뱃머리가
// "보는 방향" 이다. 요트에서는 조류와 바람 때문에 둘이 다르고, 배가 멈춰
// 있으면 COG 는 아예 안 나온다. 그래서 둘을 따로 보여준다.
//
// ★ 아직 거친 값이다. 두 가지가 빠져 있다.
//     1) 기울기 보정 — 배가 기울면 방위가 틀어진다
//     2) 자기 편각   — 자북과 진북의 차이 (한국은 약 8도 서편)
//   배에 달고 실제 방위와 대조한 뒤에 보정을 넣는다.
static float headingDeg() {
    if (!gMagOk) return -1.0f;
    float h = atan2f(gMag.y, gMag.x) * 180.0f / (float)M_PI;
    if (h < 0.0f) h += 360.0f;
    return h;
}

// 9축 한 줄 요약
static void printImuLine() {
    if (!gImuOk) {
        Serial.println("   9축  IMU 없음");
        return;
    }
    Serial.printf("   9축  가속 %+5.2f %+5.2f %+5.2f g", gAcc.x, gAcc.y, gAcc.z);
    Serial.printf(" | 자이로 %+7.1f %+7.1f %+7.1f °/s", gGyr.x, gGyr.y, gGyr.z);
    if (gMagOk) {
        Serial.printf(" | 자력 %+6.1f %+6.1f %+6.1f µT", gMag.x, gMag.y, gMag.z);
    } else {
        Serial.print(" | 자력계 없음");
    }
    Serial.printf(" | roll %+6.1f° pitch %+6.1f° → 힐 %+6.1f°\n",
                  gRollDeg, gPitchDeg, currentHeelDeg());
}

// 지금 자세를 평형(힐 0°)으로 삼는다. 배를 물에 띄우고 평형일 때 쓴다.
static void doLevel() {
    if (!gImuOk) {
        Serial.println("[IMU] 붙어 있지 않습니다.");
        return;
    }
    imuUpdate();
    gHeelOffsetDeg = gRollDeg;
    gPrefs.begin("sail", false);
    gPrefs.putFloat("heel_off", gHeelOffsetDeg);
    gPrefs.end();

    Serial.println("──────────────────────────────────────────");
    Serial.printf("  지금 자세를 평형으로 삼았습니다.\n");
    Serial.printf("  기준각 %+.1f°  →  지금 힐 %+.1f°\n",
                  gHeelOffsetDeg, currentHeelDeg());
    Serial.println("  NVS 에 저장했습니다. 다시 구워도 남습니다.");
    Serial.println("──────────────────────────────────────────");
    Serial.println("  ★ 배를 물에 띄우고 평형일 때 다시 한 번 잡으세요.");
    Serial.println("    책상에서 잡은 기준은 배 위에서 맞지 않습니다.");
}

static void doImu() {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  RAK1905 IMU — I2C 0x%02X (슬롯 C)\n", rak::kAddrImu);

    if (!gImuOk) {
        Serial.println("  아직 안 붙었습니다. 다시 붙여 봅니다...");
        if (!imuBegin()) {
            Serial.println("  여전히 응답 없음. 모듈이 덜 꽂혔는지 보세요.");
            Serial.println("──────────────────────────────────────────");
            return;
        }
    }

    Serial.println("  가속도·자이로  OK");
    Serial.printf("  자력계         %s\n", gMagOk ? "OK" : "응답 없음");
    gImuTempC = gImu.getTemperature();
    Serial.printf("  칩 온도        %.1f °C\n", gImuTempC);
    Serial.println("  5초 동안 값을 보여줍니다. 보드를 좌우로 기울여 보세요.");
    Serial.println("  ─────────────────────────────────────");

    uint32_t start = millis();
    while (millis() - start < 5000) {
        imuUpdate();
        printImuLine();
        feedWatchdog();
        delay(400);
    }

    Serial.println("──────────────────────────────────────────");
    Serial.println("  가만히 뒀을 때 가속도 세 축을 합치면 1 g 이면 정상입니다.");
    Serial.println("  기울여서 roll 이 따라 움직이면 힐 실측을 붙일 수 있습니다.");
}

// 자이로 0점 다시 잡기.
//
// 라이브러리의 autoOffsets() 는 쓰지 않는다. 그건 가속도 0점까지 함께
// 건드려서, 배가 기울어 있을 때 부르면 힐이 통째로 어긋난다.
// 여기서는 자이로만 만진다. 자이로 0점은 자세와 무관하므로 배가 기울어
// 있어도 안전하다. 멈춰 있기만 하면 된다.
static void doCalib() {
    if (!gImuOk) {
        Serial.println("[IMU] 붙어 있지 않습니다.");
        return;
    }
    Serial.println("──────────────────────────────────────────");
    Serial.println("  자이로 0점을 다시 잡습니다. 보드를 움직이지 마세요.");
    Serial.println("  (기울어 있어도 괜찮습니다. 멈춰 있기만 하면 됩니다)");
    if (calibrateGyro()) {
        Serial.println("  됐습니다. 이제 가만히 두면 자이로가 0 근처로 나옵니다.");
    } else {
        Serial.println("  다시 해보세요. 손을 떼고 보드가 멈춘 뒤에 치면 됩니다.");
    }
    Serial.println("──────────────────────────────────────────");
}

// ── SD카드 확인 (RAK15002, IO 슬롯) ──────────────────────────────────────
static void doSd() {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  RAK15002 SD — SPI (CLK%d MISO%d MOSI%d CS%d)\n",
                  rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);

    // 카드 삽입 감지. LOW 일 때 카드가 들어 있다 (내부 풀업).
    pinMode(rak::kSdCardDetect, INPUT_PULLUP);
    delay(10);
    int cd = digitalRead(rak::kSdCardDetect);
    Serial.printf("  카드 감지 GPIO%d = %s\n", rak::kSdCardDetect,
                  cd == LOW ? "LOW (카드 있음)" : "HIGH (카드 없음?)");

    feedWatchdog();
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);

    // 4 MHz 로 시작한다. 붙고 나서 필요하면 올린다.
    if (!SD.begin(rak::kSPI_CS, SPI, 4000000, "/sd", 5)) {
        Serial.println("  마운트 실패. 순서대로 의심하세요:");
        Serial.println("    1) 카드가 안 꽂혔다");
        Serial.println("    2) 카드가 FAT32 가 아니다");
        Serial.println("    3) 모듈이 IO 슬롯이 아닌 곳에 꽂혔다 (IO 슬롯 전용)");
        Serial.println("──────────────────────────────────────────");
        return;
    }

    uint8_t     t = SD.cardType();
    const char* typeName = (t == CARD_MMC)    ? "MMC"
                           : (t == CARD_SD)   ? "SDSC"
                           : (t == CARD_SDHC) ? "SDHC/SDXC"
                                              : "알 수 없음";
    Serial.printf("  카드 종류  %s\n", typeName);
    Serial.printf("  크기       %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));

    // 쓰기까지 돼야 기록에 쓸 수 있다.
    File f = SD.open("/sail_test.txt", FILE_WRITE);
    if (f) {
        f.printf("sailing monitor write test, uptime %lu ms\n", (unsigned long)millis());
        f.close();
        Serial.println("  쓰기       OK (/sail_test.txt)");
    } else {
        Serial.println("  쓰기       실패 — 카드가 쓰기 잠금이거나 가득 찼을 수 있습니다");
    }

    SD.end();
    Serial.println("──────────────────────────────────────────");
}

// ── 멈추지 않기 위한 장치 ────────────────────────────────────────────────
//
// 센서 하나가 죽었다고 계기가 통째로 멈추면 안 된다. 바다에서는 그게 제일
// 위험하다. 세 겹으로 막는다.
//
//   1) I2C 타임아웃   — 버스가 물려도 50 ms 만에 포기하고 돌아온다
//   2) 격리와 재연결  — 응답이 끊긴 센서는 끄고 나머지로 계속 간다.
//                       다시 나타나면 알아서 붙는다.
//   3) 워치독         — 그래도 어딘가에서 멈추면 보드를 다시 시작시킨다
//
// 실제로 화면 초기화 한 줄 때문에 I2C 버스가 죽어 펌웨어 전체가 정지한 적이
// 있다. 그때는 아무 로그도 안 나와서 원인을 찾는 데 오래 걸렸다.

// 워치독이 이 시간 동안 소식을 못 들으면 보드를 다시 시작시킨다.
// check 명령 하나가 20초 넘게 걸리므로 넉넉히 잡고, 긴 작업 안에서는
// feedWatchdog() 으로 살아 있다고 알린다.
static constexpr uint32_t kWatchdogSec = 30;

static bool gWatchdogOn = false;

static void feedWatchdog() {
    if (gWatchdogOn) esp_task_wdt_reset();
}

static void watchdogBegin() {
    // 이미 초기화돼 있으면 그대로 두고 이 태스크만 등록한다.
    esp_task_wdt_init(kWatchdogSec, /*panic=*/true);
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        gWatchdogOn = true;
        Serial.printf("[WDT] 워치독 %lu초 — 멈추면 스스로 다시 시작합니다\n",
                      (unsigned long)kWatchdogSec);
    } else {
        Serial.println("[WDT] !! 워치독 등록 실패 — 멈춤 보호 없이 돕니다");
    }
}

// 응답이 있는지만 가볍게 두드려 본다.
static bool i2cPing(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// 1 Hz 로 부른다. 사라진 센서는 끄고, 돌아온 센서는 다시 붙인다.
static void checkSensors() {
    const bool imuNow = i2cPing(rak::kAddrImu);
    if (gImuOk && !imuNow) {
        gImuOk = false;
        gMagOk = false;
        Serial.println("[IMU] 응답이 끊겼습니다 — 힐을 시뮬레이터 값으로 돌립니다");
    } else if (!gImuOk && imuNow) {
        Serial.println("[IMU] 다시 보입니다 — 붙입니다");
        imuBegin();
        applyGyrOffsets();
    }

    sail::displayHealthCheck();
}

// ── 텔레메트리 조립 ──────────────────────────────────────────────────────
//
// 실측이 있으면 실측을, 없으면 시뮬레이터 값을 쓴다. 항목마다 따로 정한다.
//
//   SOG·COG   GPS 가 위성을 잡았을 때만 실측. 못 잡으면 시뮬레이터.
//   HEEL      IMU 가 붙어 있으면 언제나 실측. GPS 와 상관없다.
//   BATT      ADC 실측 (1 Hz 로만 갱신해서 캐시).
//
// 시뮬레이터 본체는 include/simulator.h (호스트에서도 검증 가능한 순수 C++).
static float arduinoRand01() {
    return (float)random(0, 10001) / 10000.0f; // [0, 1]
}

// 지금 무엇을 실측으로 쓰고 있는지. 로그에 그대로 찍는다.
static bool  gUsingGpsSog  = false;
static bool  gUsingImuHeel = false;
static float gBattPct      = 100.0f; // 1 Hz 로 갱신

static Telemetry buildTelemetry(uint32_t nowMs) {
    // 시뮬레이터는 폴백으로 늘 돌려 둔다. GPS 를 놓친 순간에도 값이 이어진다.
    Telemetry sim = sail::sim::simulate(nowMs, &arduinoRand01);

    Telemetry t;
    t.moduleID = gModuleID;
    t.uptimeMs = nowMs;

    gUsingGpsSog = gGpsFix;
    if (gGpsFix) {
        t.sogKn  = (float)gGps.speed.knots();
        t.cogDeg = (float)gGps.course.deg();
    } else {
        t.sogKn  = sim.sogKn;
        t.cogDeg = sim.cogDeg;
    }

    // 힐은 IMU 가 있으면 언제나 진짜 값이다.
    // 어느 축을 힐로 볼지는 보드를 배에 어떻게 다느냐에 달렸다. 지금은 roll 을
    // 쓴다. 실제로 달아 보고 pitch 가 맞으면 여기 한 줄만 바꾸면 된다.
    gUsingImuHeel = gImuOk;
    t.heelDeg     = gImuOk ? currentHeelDeg() : sim.heelDeg;

    t.battPct = gBattPct;
    return t;
}

// ── 광고 데이터 구성 ─────────────────────────────────────────────────────

// ADV 패킷: Flags + Complete 128-bit Service UUID  (3 + 18 = 21 바이트)
static NimBLEAdvertisementData buildAdvData() {
    NimBLEAdvertisementData d;
    d.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP); // 0x06
    d.setCompleteServices(NimBLEUUID(sail::kServiceUUID));
    return d;
}

// Scan Response: Manufacturer Data + Complete Local Name  (13 + 2+N 바이트)
static NimBLEAdvertisementData buildScanData(const Telemetry& tm, uint8_t seq) {
    uint8_t mfg[2 + sail::kMfgLen];
    sail::encodeManufacturerData(tm, seq, mfg);

    NimBLEAdvertisementData d;
    d.setManufacturerData(mfg, sizeof(mfg));
    d.setName(gFullName, /*isComplete=*/true);
    return d;
}

// 연결 상태에 맞춰 광고를 (재)시작한다.
//   미연결 → ADV_IND       (connectable + scannable)
//   연결중 → ADV_SCAN_IND  (non-connectable + scannable, scan response 유지)
static void applyAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();

    // 주의: setConnectableMode/setDiscoverableMode 는 내부 m_advData 의 Flags 를
    //       건드리므로 반드시 setAdvertisementData() 보다 먼저 호출해야 한다.
    adv->setConnectableMode(gConnected ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND);
    adv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN); // NON 이 아니어야 ADV_SCAN_IND 가 된다
    adv->enableScanResponse(true);
    adv->setMinInterval(sail::kAdvIntervalUnits);
    adv->setMaxInterval(sail::kAdvIntervalUnits);

    adv->setAdvertisementData(buildAdvData());
    adv->setScanResponseData(buildScanData(gLatest, gSeq));

    if (!adv->start()) {
        Serial.println("[BLE] !! advertising start 실패");
        return;
    }
    Serial.printf("[BLE] advertising 시작 — %s (%s, interval %ums)\n",
                  gFullName,
                  gConnected ? "ADV_SCAN_IND / non-connectable"
                             : "ADV_IND / connectable",
                  sail::kAdvIntervalMs);
}

// 광고를 멈추지 않고 scan response 안의 manufacturer data 만 1 Hz 로 교체
static void refreshAdvPayload() {
    gSeq++;
    NimBLEDevice::getAdvertising()->setScanResponseData(buildScanData(gLatest, gSeq));
}

// ── 서버 콜백 ────────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        gConnected     = true;
        gAdvNeedsApply = true;
        digitalWrite(rak::kLedBlue, HIGH);
        Serial.printf("[BLE] 연결됨 ← %s (conn=%u)\n",
                      info.getAddress().toString().c_str(),
                      server->getConnectedCount());
        // 높은 notify 주기를 소화할 수 있도록 연결 파라미터를 조인다.
        // 15ms~30ms interval, latency 0, supervision timeout 4s
        server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    }

    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
        gConnected     = server->getConnectedCount() > 0;
        gSubscribed    = false;
        gAdvNeedsApply = true; // loop() 가 즉시 connectable 광고로 되돌린다
        digitalWrite(rak::kLedBlue, LOW);
        Serial.printf("[BLE] 연결 끊김 → %s (reason=%d) — 재광고 준비\n",
                      info.getAddress().toString().c_str(), reason);
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        (void)info;
        Serial.printf("[BLE] MTU = %u\n", mtu);
    }
};

// ── Characteristic 콜백 ─────────────────────────────────────────────────
class TelemetryCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        (void)chr;
        (void)info;
        gSubscribed = (subValue & 0x0001) != 0; // bit0 = notify
        Serial.printf("[BLE] notify 구독 %s\n", gSubscribed ? "ON" : "OFF");
    }
};

// ── 시리얼 명령 ──────────────────────────────────────────────────────────
static void printIdentity() {
    char mac[20];
    formatMac(mac, sizeof(mac));
    Serial.printf("[ID ] 이름 %s | module_id %u (0x%02X) | MAC %s\n",
                  gFullName, gModuleID, gModuleID, mac);
    Serial.printf("[ID ] notify %.1f Hz (%ums) | adv %.1f Hz\n",
                  1000.0f / gNotifyPeriodMs, (unsigned)gNotifyPeriodMs,
                  1000.0f / sail::kAdvRefreshMs);
    if (gSensorPowerPin) {
        Serial.printf("[PWR] 센서 전원 GPIO%d (ON)\n", gSensorPowerPin);
    } else {
        Serial.println("[PWR] 센서 전원 꺼짐");
    }
}

static void printHelp() {
    Serial.println("──────────────────────────────────────────");
    Serial.println("  name <이름>   보드 이름 설정 (최대 11자, 영숫자/-/_)");
    Serial.println("                예) name hojun  →  SAIL-hojun");
    Serial.println("  hz <1~100>    notify 주기 설정. 예) hz 20  (기본 10)");
    Serial.println("  info          현재 설정 출력");
    Serial.println("");
    Serial.println("  ── 보드 진단 ──");
    Serial.printf("  power %-2d      센서 전원을 GPIO%d 로 (datasheet 쪽)\n",
                  rak::kSensorPowerA, rak::kSensorPowerA);
    Serial.printf("  power %-2d      센서 전원을 GPIO%d 로 (pins_arduino.h 쪽)\n",
                  rak::kSensorPowerB, rak::kSensorPowerB);
    Serial.println("  power off     센서 전원 끄기");
    Serial.println("  check         ★ 아래를 한 번에 — 처음엔 이것부터");
    Serial.println("  fix           GPS 파싱 상태 (위성 수, 위치, 속도, 침로)");
    Serial.println("  imu           9축 값 5초 출력 — 기울여 보세요");
    Serial.println("  scan          I2C — 화면 RAK1921(0x3C) / IMU RAK1905(0x68)");
    Serial.println("  sd            SD카드 마운트 + 쓰기 시험");
    Serial.println("  oled          화면을 나중에 꽂았을 때 다시 붙이기");
    Serial.println("  gps           UART1 원시 NMEA 5초 (GPS 가 슬롯 A)");
    Serial.println("  gps d         같은 것 (GPS 가 슬롯 D — IO6 로 리셋 해제)");
    Serial.println("  batt          배터리 전압 실측");
    Serial.println("  level         ★ 지금 자세를 힐 0° 로 삼기 (배가 평형일 때)");
    Serial.println("  calib         자이로 0점 다시 잡기 (기울어 있어도 OK)");
    Serial.println("  help          이 도움말");
    Serial.println("──────────────────────────────────────────");
    Serial.println("  붙어 있는 것:  GPS 슬롯A · IMU 슬롯C · 화면 J12 · SD IO슬롯");
    Serial.println("  값 뒤의 (GPS)(IMU)(SIM) 이 그 값의 출처입니다.");
    Serial.println("    SIM = 위성을 못 잡아 시뮬레이터로 채운 값");
    Serial.println("──────────────────────────────────────────");
}

static void handleCommand(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line == "help" || line == "?") { printHelp();  return; }
    if (line == "info")                { printIdentity(); return; }
    if (line == "scan")                { doScan();     return; }
    if (line == "batt")                { printBattery(); return; }
    if (line == "imu")                 { doImu();      return; }
    if (line == "sd")                  { doSd();       return; }
    if (line == "fix")                 { doFix();      return; }
    if (line == "calib")               { doCalib();    return; }
    if (line == "level")               { doLevel();    return; }

    // 화면을 나중에 꽂았을 때 다시 붙인다. 재부팅할 필요 없다.
    if (line == "oled") {
        if (sail::displayBegin()) {
            Serial.println("[OLED] 붙었습니다 — 화면에 값이 나옵니다.");
            sail::displayBootMessage(gFullName, "hello");
        } else {
            Serial.println("[OLED] 0x3C 응답 없음. J12 헤더(2.54mm I2C)에 꽂혀 있나요?");
            Serial.println("       센서 슬롯 A~D 가 아닙니다.");
        }
        return;
    }

    if (line == "gps")                 { peekGps(5, /*slotD=*/false); return; }
    if (line == "gps d" || line == "gps D") { peekGps(5, /*slotD=*/true); return; }

    // 붙어 있는 것을 한 번에 훑는다. 보드를 처음 구웠을 때 이것부터 친다.
    if (line == "check") {
        Serial.println();
        Serial.println("════ 모듈 전체 점검 ════");
        printIdentity();
        doScan();
        doImu();
        doSd();
        doFix();
        Serial.println("════ 점검 끝 ════");
        Serial.println("  안 잡힌 게 있으면 power 값을 바꿔 다시 check 하세요.");
        return;
    }

    if (line.startsWith("power")) {
        String arg = line.substring(5);
        arg.trim();
        if (arg.length() == 0) {
            printIdentity();
            return;
        }

        int pin;
        if (arg == "off") {
            pin = 0;
        } else {
            pin = arg.toInt();
            if (pin != rak::kSensorPowerA && pin != rak::kSensorPowerB) {
                Serial.printf("[PWR] %d 은 후보가 아닙니다. %d, %d, off 중에서 고르세요.\n",
                              pin, rak::kSensorPowerA, rak::kSensorPowerB);
                return;
            }
        }

        gSensorPowerPin = pin;
        applySensorPower(pin, /*cycle=*/true);
        gPrefs.begin("sail", false);
        gPrefs.putInt("pwr_pin", pin);
        gPrefs.end();

        if (pin) Serial.println("[PWR] 이어서 scan 을 쳐서 모듈이 보이는지 확인하세요.");
        return;
    }

    if (line.startsWith("hz ")) {
        long hz = line.substring(3).toInt();
        if (hz < 1 || hz > 100) {
            Serial.println("[ID ] 1~100 Hz 범위로 입력하세요. 예) hz 20");
            return;
        }
        gNotifyPeriodMs = (uint32_t)(1000.0f / hz + 0.5f);
        gPrefs.begin("sail", false);
        gPrefs.putUInt("notify_ms", gNotifyPeriodMs);
        gPrefs.end();
        Serial.printf("[ID ] notify 주기 → %.1f Hz (%ums)\n",
                      1000.0f / gNotifyPeriodMs, (unsigned)gNotifyPeriodMs);
        return;
    }

    if (line.startsWith("name ")) {
        String arg = line.substring(5);
        arg.trim();
        if (arg.length() == 0) {
            Serial.println("[ID ] 이름이 비어 있습니다. 예) name hojun");
            return;
        }
        if (arg.length() > sail::kMaxUserNameLen) {
            Serial.printf("[ID ] 이름이 너무 깁니다 (최대 %u자). 잘라서 저장합니다.\n",
                          (unsigned)sail::kMaxUserNameLen);
        }
        saveIdentity(arg.c_str());
        printIdentity();
        gAdvNeedsApply = true; // 광고 이름이 바뀌었으니 다시 올린다
        Serial.println("[ID ] 저장 완료 — 앱에서 모듈을 다시 선택해야 합니다.");
        return;
    }

    Serial.printf("[ID ] 알 수 없는 명령: %s   (help 입력)\n", line.c_str());
}

static void pollSerial() {
    static String buf;
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (buf.length() > 0) {
                handleCommand(buf);
                buf = "";
            }
        } else if (buf.length() < 64) {
            buf += c;
        }
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);

    // 초기화 도중에 멈추는 경우까지 잡으려면 여기서 먼저 켜야 한다.
    // 실제로 화면 초기화에서 멈춰 아무 로그도 없이 죽은 적이 있다.
    watchdogBegin();

    pinMode(rak::kLedGreen, OUTPUT);
    pinMode(rak::kLedBlue, OUTPUT);
    digitalWrite(rak::kLedGreen, LOW);
    digitalWrite(rak::kLedBlue, LOW);

    loadSettings();

    Serial.println();
    Serial.println("═══════════════════════════════════════════");
    Serial.printf("  %s — RAK3112 / RAK19007\n", gFullName);
    Serial.printf("  module_id %u (0x%02X)\n", gModuleID, gModuleID);
    Serial.printf("  service   %s\n", sail::kServiceUUID);
    Serial.printf("  telemetry %s\n", sail::kTelemetryUUID);
    Serial.printf("  notify %.1fHz / adv refresh %.1fHz\n",
                  1000.0f / gNotifyPeriodMs, 1000.0f / sail::kAdvRefreshMs);
    Serial.println("  help 로 명령 목록을 보세요.");
    Serial.println("═══════════════════════════════════════════");

    // 센서 전원부터 켠다. 이게 없으면 GPS 가 통째로 죽어 있다.
    // (IMU 는 늘 켜져 있는 VDD 를 쓰므로 이 스위치와 무관하다 — board_rak.h 참고)
    applySensorPower(gSensorPowerPin, /*cycle=*/true);

    // 배터리 ADC. 분압 뒤 최대 2.52 V 라 12 dB 감쇠 범위 안에 들어온다.
    analogSetPinAttenuation(rak::kBattAdcPin, ADC_11db);
    gBattPct = batteryPercent(readBatteryVolts(nullptr));

    randomSeed(esp_random());

    // ── 센서 붙이기 ─────────────────────────────────────────────────────
    gpsBegin();
    Serial.printf("[GPS] UART1 9600bps 열림 (RX GPIO%d / TX GPIO%d)\n",
                  rak::kUART1_RX, rak::kUART1_TX);

    if (imuBegin()) {
        Serial.printf("[IMU] MPU-9250 붙음 | 자력계 %s\n",
                      gMagOk ? "OK" : "응답 없음");
        applyGyrOffsets();   // 지난번에 잡아둔 0점을 먼저 넣고
        calibrateGyro();     // 지금 다시 잡아본다 (흔들리면 지난 값 그대로)
    } else {
        Serial.println("[IMU] !! 응답 없음 — 힐은 시뮬레이터 값을 씁니다");
    }

    // 버스가 물려도 오래 붙들려 있지 않게 한다. 기본값도 50 ms 지만
    // 이 값에 기대는 코드라 명시해 둔다. (Wire.h — "default timeout ... 50ms")
    Wire.setTimeOut(50);

    // 화면은 J12 헤더에 꽂는다. 없어도 그냥 지나간다.
    if (sail::displayBegin()) {
        Serial.println("[OLED] RAK1921 붙음 (128x64)");
        sail::displayBootMessage(gFullName, "starting...");
    } else {
        Serial.println("[OLED] 없음 — J12 헤더에 꽂으면 자동으로 잡힙니다");
    }

    Serial.println("[SRC] SOG/COG 는 GPS 가 위성을 잡으면 실측, 못 잡으면 시뮬레이터");
    Serial.println("      HEEL 은 IMU 가 붙어 있으면 언제나 실측");

    gLatest = buildTelemetry(millis());

    NimBLEDevice::init(gFullName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // 최대 송신 출력

    gServer = NimBLEDevice::createServer();
    gServer->setCallbacks(new ServerCallbacks());
    // 광고 재개는 applyAdvertising() 이 모드까지 맞춰서 직접 처리한다.
    gServer->advertiseOnDisconnect(false);

    NimBLEService* svc = gServer->createService(sail::kServiceUUID);
    gTelemetryChr      = svc->createCharacteristic(
        sail::kTelemetryUUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    gTelemetryChr->setCallbacks(new TelemetryCallbacks());

    uint8_t initial[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(gLatest, initial);
    gTelemetryChr->setValue(initial, sizeof(initial));

    // NimBLE 2.x 에서는 서버가 시작될 때 서비스도 함께 시작된다(svc->start() 는 no-op).
    gServer->start();

    applyAdvertising();
    gAdvNeedsApply = false;

    digitalWrite(rak::kLedGreen, HIGH); // 초록 = 살아서 광고 중
}

void loop() {
    const uint32_t now = millis();

    static uint32_t lastNotify = 0;
    static uint32_t lastAdv    = 0;
    static uint32_t lastLog    = 0;
    static uint32_t lastImu    = 0;
    static uint32_t lastBatt   = 0;

    pollSerial();

    // GPS 는 쉬지 않고 읽는다. UART 버퍼가 넘치면 문장 중간이 잘려 나간다.
    gpsPoll();

    // 1) 10 Hz — IMU 갱신
    if (now - lastImu >= 100) {
        lastImu = now;
        imuUpdate();
    }

    // 2) 1 Hz — 배터리. ADC 를 16번 재느라 30 ms 쯤 걸려서 자주 하면 손해다.
    //    2.5 MΩ 분압이라 값이 몇 % 씩 흔들린다. 천천히 따라가게 눌러 준다.
    if (now - lastBatt >= 1000) {
        lastBatt = now;
        float fresh = batteryPercent(readBatteryVolts(nullptr));
        gBattPct    = gBattPct * 0.8f + fresh * 0.2f;

        // 같은 주기로 센서가 아직 붙어 있는지도 확인한다.
        // 사라졌으면 끄고 나머지로 계속 간다. 돌아오면 다시 붙는다.
        checkSensors();
    }

    // 3) gNotifyPeriodMs 주기 — 값 조립 + characteristic 갱신 + notify
    if (now - lastNotify >= gNotifyPeriodMs) {
        lastNotify = now;
        gpsUpdateFix();
        gLatest = buildTelemetry(now);

        uint8_t packet[sail::kTelemetryLen];
        sail::encodeTelemetryPacket(gLatest, packet);
        gTelemetryChr->setValue(packet, sizeof(packet)); // Read 용 값도 항상 최신
        if (gConnected) {
            gTelemetryChr->notify(); // 구독자가 없으면 NimBLE 가 알아서 무시
        }
    }

    // 4) 광고 모드 전환 (연결/해제 직후, 또는 이름 변경 직후 한 번)
    if (gAdvNeedsApply) {
        gAdvNeedsApply = false;
        applyAdvertising();
    }

    // 5) 1 Hz — 광고 페이로드 갱신
    if (now - lastAdv >= sail::kAdvRefreshMs) {
        lastAdv = now;
        refreshAdvPayload();
    }

    // 6) 4 Hz — 화면. 사람 눈에는 이걸로 충분하고 더 빨리 그려도 의미가 없다.
    //    128x64 를 한 장 보내는 데 I2C 400 kHz 에서 25 ms 쯤 걸린다.
    static uint32_t lastDraw = 0;
    if (now - lastDraw >= 250) {
        lastDraw = now;
        sail::DisplayState ds;
        ds.userName     = gUserName;
        ds.bleConnected = gConnected;
        ds.bleNotifying = gSubscribed;
        ds.battPct      = gLatest.battPct;

        ds.sogKn      = gLatest.sogKn;
        ds.cogDeg     = gLatest.cogDeg;
        ds.headingDeg = headingDeg();
        ds.heelDeg    = gLatest.heelDeg;
        ds.pitchDeg   = gPitchDeg;

        ds.accX  = gAcc.x; ds.accY = gAcc.y; ds.accZ = gAcc.z;
        ds.gyrX  = gGyr.x; ds.gyrY = gGyr.y; ds.gyrZ = gGyr.z;
        ds.magX  = gMag.x; ds.magY = gMag.y; ds.magZ = gMag.z;
        ds.imuOk = gImuOk;
        ds.magOk = gMagOk;

        ds.sogFromGps = gUsingGpsSog;
        ds.gpsFix     = gGpsFix;
        ds.satellites = gGps.satellites.isValid() ? (int)gGps.satellites.value() : 0;
        ds.hdop       = gGps.hdop.isValid() ? (float)gGps.hdop.hdop() : -1.0f;

        sail::displayUpdate(ds);
    }

    // 7) 1 Hz — 시리얼 로그.
    //    값 옆에 그 값이 어디서 왔는지를 반드시 붙인다. 시뮬레이터 값이
    //    실측처럼 보이면 나중에 바다에서 엉뚱한 판단을 하게 된다.
    if (now - lastLog >= sail::kLogPeriodMs) {
        lastLog = now;
        Serial.printf(
            "[%7.1fs] %s | SOG %5.2f kn %s | COG %5.1f° %s | HEEL %+6.1f° %s | BATT %3d%% | seq %3u | %s%s\n",
            now / 1000.0f,
            gFullName,
            gLatest.sogKn,   gUsingGpsSog ? "(GPS)" : "(SIM)",
            gLatest.cogDeg,  gUsingGpsSog ? "(GPS)" : "(SIM)",
            gLatest.heelDeg, gUsingImuHeel ? "(IMU)" : "(SIM)",
            (int)sail::encodeBatt(gLatest.battPct),
            gSeq,
            gConnected ? "CONNECTED" : "ADVERTISING",
            gConnected ? (gSubscribed ? " (notify ON)" : " (notify OFF)") : "");
        printGpsLine();
        printImuLine();
    }

    feedWatchdog(); // 여기까지 왔으면 살아 있다는 뜻
    delay(2);
}
