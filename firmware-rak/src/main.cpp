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

using sail::Telemetry;

// ── 모듈 신원 ────────────────────────────────────────────────────────────
static Preferences gPrefs;

// 다듬기 세기와 잡음 바닥. 설정에서 먼저 읽으므로 여기 둔다.
static uint8_t gDampLevel  = 2;      // 0~5. 아래 kDampTau 의 칸 번호
static float   gDeadbandKn = 0.10f;  // 이보다 작은 속도는 0 으로 내린다
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

// ── 힐 ───────────────────────────────────────────────────────────────────
//
// 힐은 배가 좌우로 얼마나 누웠는지다. 보드가 어느 축으로 그걸 느끼는지는
// 보드를 어떻게 달았느냐에 달려 있다.
//
// 밖에서 재 보니 방위(HDG)가 맞으려면 보드를 세워서 달아야 했다. 그렇게
// 세우면 예전에 쓰던 roll = atan2(가속Y, 가속Z) 은 힐과 상관없는 회전을
// 재게 된다. 세운 자세에서는 Z 가 눕고 Y 가 서기 때문이다.
//
// 그래서 힐을 **가속도 한 축**에서 바로 구한다. 기울기계가 쓰는 그 방법이다.
//
//     힐 = asin(그 축의 g / 중력 크기)
//
// 배가 평형이면 그 축은 수평이라 0 g 를 읽고, 옆으로 누울수록 중력이 그
// 축으로 흘러 들어온다. 실제로 달아 보고 고른 축은 **Y** 다.
//
// ★ 한계: asin 이라 ±90° 까지만 맞다. 90° 를 넘으면 배가 이미 넘어간 것이라
//   그 뒤의 정확한 각도는 볼 이유가 없다.
//
// 어느 축인지, 부호가 어느 쪽인지는 `heel` 명령으로 바꾸고 NVS 에 남긴다.
// 다는 방법이 또 바뀌어도 다시 굽지 않아도 된다.
static uint8_t gHeelAxis = 1;      // 0 = X, 1 = Y, 2 = Z
static float   gHeelSign = 1.0f;   // -1 이면 좌우가 뒤집혀 있다는 뜻

// "지금 이 자세가 평형" 이라고 알려주는 기준각. 배를 물에 띄우고 평형일 때
// `level` 을 치면 그때 각도를 0 으로 삼는다. NVS 에 저장되므로 재부팅해도,
// 다시 구워도 남는다.
static float gHeelOffsetDeg = 0.0f;

// 각도를 -180 ~ +180 안으로 접는다.
static float wrap180(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static const char* heelAxisName() {
    static char buf[4];
    snprintf(buf, sizeof(buf), "%c%c", gHeelSign < 0.0f ? '-' : '+',
             gHeelAxis == 0 ? 'X' : (gHeelAxis == 1 ? 'Y' : 'Z'));
    return buf;
}

// 기준각을 빼기 전의 날각도. `level` 이 이 값을 기준으로 삼는다.
static float rawHeelDeg() {
    const float a = (gHeelAxis == 0) ? gAcc.x : (gHeelAxis == 1 ? gAcc.y : gAcc.z);
    const float mag = sqrtf(gAcc.x * gAcc.x + gAcc.y * gAcc.y + gAcc.z * gAcc.z);
    if (mag < 0.2f) return 0.0f; // 자유낙하 수준이면 중력 방향을 알 수 없다
    float s = gHeelSign * a / mag;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return asinf(s) * 180.0f / (float)M_PI;
}

static float currentHeelDeg() {
    return wrap180(rawHeelDeg() - gHeelOffsetDeg);
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
    // 힐 기준각의 키가 heel_off → heel_off2 로 바뀌었다. 옛 키에 남아 있는
    // 값은 roll 기준이라 지금 계산법에서는 뜻이 다르다. 그냥 안 읽는다.
    gHeelAxis       = gPrefs.getUChar("heel_axis", 1); // 기본 Y — 위 "힐" 항목 참고
    gHeelSign       = gPrefs.getChar("heel_sgn", 1) < 0 ? -1.0f : 1.0f;
    gHeelOffsetDeg  = gPrefs.getFloat("heel_off2", 0.0f);
    gDampLevel      = gPrefs.getUChar("damp", 2);
    gDeadbandKn     = gPrefs.getFloat("dead_kn", 0.10f);
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
// ── 갱신율과 통신 속도 ───────────────────────────────────────────────────
//
// L76K 는 켜지면 1 Hz / 9600bps 로 시작한다. 1초에 한 번이면 배가 방향을 트는
// 동안 값이 몇 번 안 바뀌어 화면이 굼떠 보인다. notify 를 10 Hz 로 쏴도
// 같은 숫자가 열 번 반복될 뿐이다.
//
// ★ 이 모듈은 PMTK(MediaTek) 명령을 안 받는다. PCAS 명령을 쓴다.
//   처음에 PMTK251/PMTK220 을 보냈다가 모듈이 통신 속도를 안 바꿔서,
//   우리만 38400 으로 듣게 되어 들어오는 바이트가 전부 깨졌다.
//   근거: Quectel L76K GNSS Protocol Specification V1.1 §2.3
//     $PCAS01,<n>     통신 속도   1=9600  3=38400  5=115200
//     $PCAS02,<ms>    갱신 간격   1000=1Hz  500=2Hz  200=5Hz
//     $PCAS03,...     문장 종류별 출력 주기 (0 이면 끔)
//
// ★ 같은 문서가 못을 박는다. 1000ms 미만으로 내리려면 문장 종류를 줄이고
//   통신 속도를 115200 으로 올려야 한다.
//     "It is required to set the type of NMEA sentences output to single and
//      change the baud rate to 115200 bps when the <Interval> is less than 1000."
//   9600bps 는 초당 960바이트뿐인데 NMEA 한 벌이 400~500바이트다. 5 Hz 로
//   올리면 당연히 잘린다.
//
// 우리가 쓰는 문장은 둘뿐이다.
//   RMC — 속도 · 침로 · 위치 · 시각
//   GGA — 위성 수 · HDOP · 고도
// 나머지(GLL / GSA / GSV / VTG / ZDA / ANT)는 꺼서 대역을 아낀다.
static constexpr uint32_t kGpsBaud = 115200;

static uint8_t gGpsHz = 10; // 실제로 건 갱신율 (진단 출력용)

// CASIC 바이너리 쪽은 아래에 정의돼 있다. gpsBegin() 이 먼저 나와서 앞선언한다.
static void casicSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);
static bool casicQuery(uint8_t cls, uint8_t id, uint8_t* out, uint16_t cap, uint16_t* len);
static const char* dyModelName(uint8_t m);

// 체크섬을 붙여 한 줄 보낸다. ($ 와 * 사이 문자들의 XOR)
static void gpsSend(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; ++p) ck ^= (uint8_t)*p;
    Serial1.printf("$%s*%02X\r\n", body, ck);
    Serial1.flush();
    delay(60); // flush 뒤에도 실제로 나갈 시간을 준다. 이걸 아끼면 명령이 씹힌다.
}

// 갱신율을 바꾼다.
//
// 문서(§2.3.2)가 적어 놓은 값은 1000 / 500 / 200 (1·2·5 Hz) 뿐이다.
// 100(10 Hz)은 문서에도, Meshtastic 같은 실제 구현에도 사례가 없다.
// 다만 제품 사양에는 "up to 10 Hz" 라고 적혀 있어서 넣어는 둔다.
// 걸렸는지 아닌지는 `gpshz` 명령이 문장 수를 세서 알려준다.
static void gpsSetRate(uint8_t hz) {
    int ms = 1000;
    if (hz >= 10)     ms = 100;
    else if (hz >= 5) ms = 200;
    else if (hz >= 2) ms = 500;

    char body[24];
    snprintf(body, sizeof(body), "PCAS02,%d", ms);
    gpsSend(body);

    gGpsHz = (ms == 100) ? 10 : (ms == 200 ? 5 : (ms == 500 ? 2 : 1));
}

static void gpsBegin() {
    // 지금 모듈이 어느 속도로 말하는지 알 수 없다. 방금 전원이 들어왔으면
    // 9600 이지만, 보드만 리셋되고 모듈은 안 꺼졌다면 이미 115200 이다.
    // 두 속도로 각각 보낸다. 못 알아듣는 쪽은 그냥 버려진다.
    for (uint32_t baud : {9600u, kGpsBaud}) {
        Serial1.begin(baud, SERIAL_8N1, rak::kUART1_RX, rak::kUART1_TX);
        delay(150);
        // 위성을 다 본다. 기본값은 3 (GPS + BeiDou) 이라 GLONASS 가 꺼져 있다.
        //   1=GPS  2=BeiDou  3=GPS+BeiDou(기본)  4=GLONASS
        //   5=GPS+GLONASS  6=BeiDou+GLONASS  7=셋 다
        // 많이 볼수록 위성을 빨리 잡고 건물 사이에서도 덜 놓친다.
        // (QZSS 는 기본으로 켜져 있고 끌 수 없다 — 문서 §2.3.4)
        gpsSend("PCAS04,7");
        // GGA, RMC 만 남기고 나머지는 끈다
        gpsSend("PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0,0");
        gpsSend("PCAS01,5"); // 115200
        delay(150);
        Serial1.end();
        delay(50);
    }

    // 이제부터는 바뀐 속도로 듣는다.
    Serial1.begin(kGpsBaud, SERIAL_8N1, rak::kUART1_RX, rak::kUART1_TX);
    delay(200);

    // 10 Hz. 문서에는 5 Hz(200ms)까지만 적혀 있지만 실기기에서 확인했다
    //   요청 10 Hz → 초당 문장 19.6개 (GGA+RMC 두 종류이므로 9.8회), 체크섬 실패 0
    // 문장 두 종류만 켜 두고 115200 을 쓰기 때문에 대역이 남는다.
    gpsSetRate(10);

    // NAV-PV 를 1 Hz 로 계속 내보내게 한다 (측위 10회당 1번).
    //
    // NMEA 로 만들어지기 전의 속도를 보려는 것이다. RMC 가 0 인데 이쪽이
    // 살아 있으면 다듬기가 어디서 걸리는지 알 수 있다. 80바이트에 초당 한 번이라
    // 115200bps 에서 부담이 없다.
    {
        const uint8_t on[4] = {0x01, 0x03, 10, 0};
        casicSend(0x06, 0x01, on, 4);
        delay(80);
    }

    // 저장해 둔 움직임 종류(dyModel)가 있으면 다시 건다.
    // 255 면 저장한 적이 없다는 뜻 — 모듈 기본값(4 선박)을 그대로 둔다.
    const uint8_t want = gPrefs.getUChar("gps_dyn", 255);
    if (want <= 7) {
        uint8_t  p[44];
        uint16_t len = 0;
        if (casicQuery(0x06, 0x07, p, sizeof(p), &len) && len >= 44 && p[4] != want) {
            const uint32_t mask = (1UL << 0);
            memcpy(p + 0, &mask, 4);
            p[4] = want;
            casicSend(0x06, 0x07, p, 44);
            delay(120);
            Serial.printf("[GPS] 움직임 종류를 %u(%s) 로 다시 걸었습니다\n",
                          want, dyModelName(want));
        }
    }
}

// ── 밖에 나갔다 와서 확인하기 위한 기록 ──────────────────────────────────
//
// 노트북을 들고 나갈 수는 없다. 그래서 위성을 잡았을 때 무엇이 왔는지
// 보드가 스스로 기억해 둔다. 돌아와서 `fix` 를 치면 그대로 보여준다.
static char     gLastFixRmc[100] = {0}; // fix 가 있었던 마지막 RMC 원문
static uint32_t gLastFixAtMs     = 0;
static float    gMaxSogKn        = 0.0f; // fix 중에 본 가장 큰 속도 (도플러)
static float    gMaxSogFromPos   = 0.0f; // 같은 구간, 위치 차분 쪽
static float    gMaxSogPv        = 0.0f; // 같은 구간, NAV-PV(NMEA 거치기 전) 쪽
static uint32_t gFixSeenCount    = 0;    // fix 상태로 판정된 횟수

// ── 위치 차분 속도 ──────────────────────────────────────────────────────
//
// ★ 모듈이 주는 속도(도플러)는 느린 구간을 0 으로 뭉갠다.
//   실측: 밖에서 걸을 때(2~3 kn) 계속 0, 뛰니까 8.76 kn 이 나왔다.
//   L76K 문서의 PCAS/CASIC 명령 어디에도 이 동작을 끄는 설정이 없다.
//
// 요트에서는 정박·미풍 구간이 0~2 kn 이라 그냥 둘 수 없다. 그래서 위치를
// 직접 차분해서 속도를 따로 구하고, 둘을 나란히 본다.
//
// 두 방식은 성질이 다르다 (USV 논문 PMC8659471 과 같은 이야기)
//   도플러   분산이 작다. 대신 저속을 뭉갠다.
//   위치차분 평균이 정확하다. 대신 분산이 크고 정지 중에도 드리프트가 뜬다.
//
// ★ 실측으로 결론이 났다 (2026-08-22). 걷는 동안 위치 차분 쪽에 30.31 kn 이
//   찍혔다. 도플러는 같은 구간에서 7.98 kn 이었다. 위치 차분은 못 쓴다.
//   화면에서 뺐고, 시리얼 진단(`fix`)에만 남겨 둔다.
//
//   저속이 0 으로 나오던 진짜 원인은 dyModel 이었다. 움직임 종류를 맞추니
//   도플러가 저속도 제대로 구분한다.
static double   gPrevLat = 0, gPrevLon = 0;
static uint32_t gPrevPosMs = 0;
static float    gSogFromPos = -1.0f; // 음수면 아직 못 구함

// 위치가 갱신될 때마다 부른다. 1초 간격으로만 계산한다 —
// 10 Hz 위치를 그대로 차분하면 GPS 오차가 그대로 속도 노이즈가 된다.
static void updatePositionSpeed() {
    if (!gGps.location.isValid()) return;

    const uint32_t now = millis();
    const double lat = gGps.location.lat();
    const double lon = gGps.location.lng();

    if (gPrevPosMs == 0) {
        gPrevLat = lat; gPrevLon = lon; gPrevPosMs = now;
        return;
    }

    const double dt = (now - gPrevPosMs) / 1000.0;
    if (dt < 1.0) return;

    const double meters = TinyGPSPlus::distanceBetween(gPrevLat, gPrevLon, lat, lon);
    const double mps    = meters / dt;
    gSogFromPos = (float)(mps * 1.943844); // m/s → knot

    gPrevLat = lat; gPrevLon = lon; gPrevPosMs = now;
}

// loop() 에서 계속 부른다. 들어온 바이트를 파서에 먹인다.
// NAV-PV 로 받은 값. NMEA 를 거치기 전의 속도다.
static float    gPvSpeedKn  = -1.0f; // 음수면 아직 없음
static float    gPvCogDeg   = -1.0f;
static float    gPvAccKn    = -1.0f; // 모듈이 밝힌 자기 속도 오차
static bool     gPvVelValid = false;
static uint32_t gPvAtMs     = 0;

static void gpsPoll() {
    static char   line[100];
    static size_t n = 0;

    // ── 바이너리 프레임 골라내기 ─────────────────────────────────────────
    // NMEA 는 전부 아스키(0x80 미만)라 0xBA 가 나올 수 없다. 그래서 0xBA 를
    // 만나면 바이너리 프레임이 시작된 것으로 봐도 안전하다.
    static int      bs = 0;   // 0=NMEA 읽는 중
    static uint16_t blen = 0, bneed = 0, bn = 0;
    static uint8_t  bcls = 0, bid = 0;
    static uint8_t  bbody[96];

    while (Serial1.available()) {
        const uint8_t u = (uint8_t)Serial1.read();
        const char    c = (char)u;

        if (bs != 0) {
            switch (bs) {
                case 1: bs = (u == 0xCE) ? 2 : (u == 0xBA ? 1 : 0); break;
                case 2: blen = u;                  bs = 3; break;
                case 3: blen |= (uint16_t)u << 8;  bs = 4; break;
                case 4: bcls = u;                  bs = 5; break;
                case 5:
                    bid   = u;
                    bneed = (blen > sizeof(bbody)) ? sizeof(bbody) : blen;
                    bn    = 0;
                    bs    = (bneed > 0) ? 6 : 7;
                    break;
                case 6:
                    bbody[bn++] = u;
                    if (bn >= bneed) { bn = 0; bs = 7; }
                    break;
                case 7:
                    if (++bn >= 4) { // 체크섬까지 다 받았다
                        if (bcls == 0x01 && bid == 0x03 && bneed >= 80) {
                            float sp, hd, ac;
                            memcpy(&sp, bbody + 64, 4);
                            memcpy(&hd, bbody + 68, 4);
                            memcpy(&ac, bbody + 72, 4);
                            gPvVelValid = (bbody[5] != 0);
                            gPvSpeedKn  = sp * 1.943844f;
                            gPvCogDeg   = hd;
                            gPvAccKn    = (ac > 0.0f) ? sqrtf(ac) * 1.943844f : -1.0f;
                            gPvAtMs     = millis();
                            if (gPvVelValid && gPvSpeedKn > gMaxSogPv) gMaxSogPv = gPvSpeedKn;
                        }
                        bs = 0;
                    }
                    break;
            }
            continue;
        }
        if (u == 0xBA) { bs = 1; continue; }

        gGps.encode(c);

        // 파서에 먹이는 것과 별개로 원문도 한 줄씩 모은다.
        // 값이 이상할 때 "모듈이 실제로 뭘 보냈나" 를 봐야 하기 때문이다.
        if (c == '\n' || c == '\r') {
            if (n > 6) {
                line[n] = '\0';
                // RMC 를 다 읽은 시점이라 파서 상태가 갱신되어 있다.
                if (strstr(line, "RMC") != nullptr && gGps.location.isValid()) {
                    strncpy(gLastFixRmc, line, sizeof(gLastFixRmc) - 1);
                    gLastFixRmc[sizeof(gLastFixRmc) - 1] = '\0';
                    gLastFixAtMs = millis();
                }
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

// ── 다듬기 (damping) ─────────────────────────────────────────────────────
//
// 왜 필요한가
//   도플러 속도는 원래 정확하다. 정지 중에 0.1 kn 이 뜨는 건 고장이 아니라
//   도플러의 이론 잡음이다 — 0.1 kn 은 초당 5 cm 이고, Inside GNSS 가 "raw
//   Doppler 는 초당 몇 cm 수준" 이라고 못 박는다. Velocitek ProStart V2 가
//   파는 물건의 사양도 ±0.1 kn 이다. 우리가 그 수준에서 돌고 있다.
//   다만 초당 10번 오는 값이 그만큼 떨려서 화면 숫자가 가만있질 않는다.
//
// 왜 세기를 고르게 하는가
//   세게 다듬으면 숫자는 안정되지만 택 할 때 반응이 늦는다. 어느 쪽이 나은지는
//   바람과 물결에 따라 다르다. 실제 요트 계기들이 그래서 사용자에게 맡긴다.
//     "잔잔한 바람과 평평한 물에서는 다듬기가 필요 없다(0단계). 바람이 세고
//      물결이 거칠면 다듬기가 큰 흔들림을 없애 준다" — Velocitek 안내
//   그래서 우리도 숫자를 박지 않고 단계로 둔다. `smooth` 명령으로 고른다.
//
// 어떻게 다듬는가
//   1차 IIR 이다. 새 값을 통째로 받지 않고 조금씩만 반영한다.
//     다듬은값 += (원본 - 다듬은값) * dt / (시상수 + dt)
//   COG 는 각도라 359 도와 1 도가 이웃이다. 숫자를 그냥 평균 내면 180 도
//   근처로 튄다. 그래서 방향을 단위벡터(cos, sin)로 바꿔 각각 다듬고 다시
//   각도로 되돌린다.
static constexpr float kDampTau[6] = {0.0f, 0.3f, 0.6f, 1.2f, 2.5f, 5.0f}; // 초
static float    gSogDamped   = -1.0f;  // 음수면 아직 없음
static float    gCogCos = 0.0f, gCogSin = 0.0f;
static float    gCogDamped   = -1.0f;
static uint32_t gDampAtMs    = 0;

// 잡음 바닥 아래는 0 으로 보여준다.
//
// 도플러의 이론 잡음이 초당 몇 cm, 노트로 0.1 언저리다. 그보다 작은 값은
// 배가 정말 그만큼 움직인 건지 잡음인지 우리가 구별할 수 없다. 구별 못 하는
// 값을 소수점까지 띄우면 읽는 사람이 없는 의미를 붙이게 된다.
//
// ★ 지어낸 값을 쓰는 것과는 다르다. "이보다 작은 건 못 잰다" 는 사실을
//   그대로 보여주는 것이다. fix 가 없을 때 숫자를 아예 안 그리는 것과 같은 뜻이다.
static float sogOut() {
    const float v = (gSogDamped >= 0.0f) ? gSogDamped : (float)gGps.speed.knots();
    return (v < gDeadbandKn) ? 0.0f : v;
}

static void dampingReset() {
    gSogDamped = -1.0f;
    gCogDamped = -1.0f;
    gDampAtMs  = 0;
}

static void dampingUpdate(float rawSog, float rawCog, uint32_t nowMs) {
    const float tau = kDampTau[gDampLevel <= 5 ? gDampLevel : 2];

    // 0 단계거나 처음이면 원본을 그대로 쓴다.
    if (tau <= 0.0f || gDampAtMs == 0 || gSogDamped < 0.0f) {
        gSogDamped = rawSog;
        gCogDamped = rawCog;
        gCogCos    = cosf(rawCog * DEG_TO_RAD);
        gCogSin    = sinf(rawCog * DEG_TO_RAD);
        gDampAtMs  = nowMs;
        return;
    }

    const float dt = (nowMs - gDampAtMs) / 1000.0f;
    if (dt <= 0.0f) return;
    gDampAtMs = nowMs;

    // 오래 끊겼다가 돌아온 값은 이어 붙이면 안 된다. 새로 시작한다.
    if (dt > 2.0f) {
        gSogDamped = rawSog;
        gCogCos    = cosf(rawCog * DEG_TO_RAD);
        gCogSin    = sinf(rawCog * DEG_TO_RAD);
        gCogDamped = rawCog;
        return;
    }

    const float a = dt / (tau + dt);
    gSogDamped += (rawSog - gSogDamped) * a;

    gCogCos += (cosf(rawCog * DEG_TO_RAD) - gCogCos) * a;
    gCogSin += (sinf(rawCog * DEG_TO_RAD) - gCogSin) * a;
    float deg = atan2f(gCogSin, gCogCos) * RAD_TO_DEG;
    if (deg < 0.0f) deg += 360.0f;
    gCogDamped = deg;
}

// 지금 GPS 값을 믿어도 되는지 판정한다.
//
// ★ age() 검사가 꼭 필요하다. 한 번 위성을 잡았다가 놓쳐도 라이브러리는
//   마지막 값을 그대로 들고 있다. 낡은 값을 안 걸러내면 신호가 끊긴 뒤에도
//   옛날 속도를 진짜인 양 계속 내보내게 된다. 배 위에서 이건 위험하다.
static void gpsUpdateFix() {
    bool ok = gGps.location.isValid() && gGps.location.age() < kGpsStaleMs &&
              gGps.speed.isValid() && gGps.speed.age() < kGpsStaleMs;
    if (ok) {
        gEverHadFix = true;
        gFixSeenCount++;
        const float kn = (float)gGps.speed.knots();
        if (kn > gMaxSogKn) gMaxSogKn = kn;
        updatePositionSpeed();
        if (gSogFromPos > gMaxSogFromPos) gMaxSogFromPos = gSogFromPos;
        dampingUpdate(kn, (float)gGps.course.deg(), millis());
    } else {
        dampingReset();
        // fix 를 놓쳤으면 이전 위치를 버린다. 안 버리면 다시 잡았을 때
        // 그동안 움직인 거리가 통째로 한 번의 속도가 되어 엉뚱한 값이 튄다.
        gPrevPosMs  = 0;
        gSogFromPos = -1.0f;
    }
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
    Serial.printf("  갱신율        %u Hz (%lubps)\n", gGpsHz, (unsigned long)kGpsBaud);
    Serial.printf("  fix           %s\n", gGpsFix ? "있음" : "없음");

    if (gGpsFix) {
        Serial.printf("  위치          %.6f, %.6f\n",
                      gGps.location.lat(), gGps.location.lng());
        Serial.printf("  속도(SOG)     %.2f kn\n", gGps.speed.knots());
        Serial.printf("  침로(COG)     %.1f°\n", gGps.course.deg());
        Serial.printf("  HDOP          %.1f (작을수록 정확)\n",
                      gGps.hdop.isValid() ? gGps.hdop.hdop() : 99.9);
    }
    // ── 밖에 나갔다 온 뒤에 볼 기록 ──────────────────────────────────────
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  fix 판정 횟수  %lu\n", (unsigned long)gFixSeenCount);
    Serial.printf("  본 최고 속도   RMC %.2f kn / NAV-PV %.2f kn / 위치차분 %.2f kn\n",
                  gMaxSogKn, gMaxSogPv, gMaxSogFromPos);
    if (gPvAtMs != 0) {
        Serial.printf("  지금 NAV-PV    %.2f kn (오차 ±%.2f)  침로 %.1f  유효 %s  %lu초 전\n",
                      gPvSpeedKn, gPvAccKn, gPvCogDeg,
                      gPvVelValid ? "예" : "아니오",
                      (unsigned long)((millis() - gPvAtMs) / 1000));
    }
    if (gLastFixRmc[0] != '\0') {
        Serial.printf("  마지막 fix RMC (%.0f초 전)\n",
                      (millis() - gLastFixAtMs) / 1000.0f);
        Serial.printf("    %s\n", gLastFixRmc);
        Serial.println("    필드 순서: 시각,상태,위도,N/S,경도,E/W,속도,침로,날짜");
        Serial.println("                                          ↑ 7번째가 속도(kn)");
    } else {
        Serial.println("  아직 fix 된 적이 없어 기억해 둔 RMC 가 없습니다");
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
// ── CASIC 바이너리 메시지 ────────────────────────────────────────────────
//
// NMEA($PCAS...) 로는 못 보고 못 건드리는 것들이 바이너리 쪽에 있다.
// 두 가지가 특히 중요하다.
//   1. 우리가 건 설정이 실제로 걸렸는지 **되물어볼 수 있다.**
//      지금까지는 보내기만 하고 확인한 적이 없었다.
//   2. 저속을 정지로 뭉개는 문턱값(staticHoldTh)이 여기 있다.
//
//   틀 (문서 §2.2)
//     0xBA 0xCE | 길이(U2,LE) | class(U1) | id(U1) | 내용 | 체크섬(U4,LE)
//
//   체크섬 (문서 §2.2 알고리즘 그대로)
//     ckSum = (class << 24) + (id << 16) + len;
//     for (i = 0; i < len/4; i++) ckSum += payload[i];   // 4바이트씩 리틀엔디안
//
//   CFG 를 보내면 수신기가 반드시 답한다 (문서 §2.5)
//     ACK-ACK  0x05 0x01  받아들였다
//     ACK-NACK 0x05 0x00  거절했다
//
// 문서: docs/gps/CASIC_protocol_en.pdf (Hangzhou Zhongke Microelectronics)
//       L76K 문서(docs/gps/Quectel_L76K_*.pdf)에는 이 내용이 통째로 없다.
//       칩이 모듈 문서보다 훨씬 많은 걸 알아듣는다.
// ★ 문서 두 개가 체크섬 공식을 서로 다르게 적어 놨다. 하나는 오타다.
//   L76K  (우리 모듈 정본) : Checksum = (ID    << 24) + (Class << 16) + Len
//   CASIC (칩 원본)        : ckSum    = (class << 24) + (id    << 16) + len
// 어느 쪽이 맞는지는 답이 오는 쪽이 정답이다. 둘 다 해 보고 되는 쪽을 기억한다.
static bool gCasicIdFirst = true; // 우리 모듈 문서(L76K) 방식부터

static void casicSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    const uint8_t hdr[6] = {0xBA, 0xCE,
                            (uint8_t)(len & 0xFF), (uint8_t)(len >> 8),
                            cls, id};

    const uint8_t hi = gCasicIdFirst ? id  : cls;
    const uint8_t lo = gCasicIdFirst ? cls : id;
    uint32_t ck = ((uint32_t)hi << 24) + ((uint32_t)lo << 16) + len;
    for (uint16_t i = 0; i + 3 < len; i += 4) {
        uint32_t w;
        memcpy(&w, payload + i, 4); // ESP32 도 리틀엔디안이라 그대로 맞는다
        ck += w;
    }

    Serial1.write(hdr, 6);
    if (len > 0) Serial1.write(payload, len);
    const uint8_t ckb[4] = {(uint8_t)(ck), (uint8_t)(ck >> 8),
                            (uint8_t)(ck >> 16), (uint8_t)(ck >> 24)};
    Serial1.write(ckb, 4);
    Serial1.flush();
}

// 바이너리 응답 한 개를 기다린다. wantId 가 0xFF 면 그 class 의 아무거나 받는다.
//
// NMEA 문장이 초당 2000바이트씩 같이 흘러들어오므로 0xBA 0xCE 를 찾아가며 읽는다.
// 지나가는 NMEA 는 버리지 않고 파서에 먹인다 — 기다리는 동안 위치가 멎으면 안 된다.
static bool casicWait(uint32_t waitMs, uint8_t wantCls, uint8_t wantId,
                      uint8_t* out, uint16_t outCap, uint16_t* outLen,
                      uint8_t* gotId) {
    const uint32_t start = millis();
    int      state = 0;
    uint16_t len = 0, need = 0, n = 0;
    uint8_t  cls = 0, id = 0;
    uint8_t  body[128];

    while (millis() - start < waitMs) {
        while (Serial1.available()) {
            const uint8_t c = (uint8_t)Serial1.read();
            switch (state) {
                case 0: if (c == 0xBA) state = 1; else gGps.encode((char)c); break;
                case 1: state = (c == 0xCE) ? 2 : (c == 0xBA ? 1 : 0); break;
                case 2: len = c;                 state = 3; break;
                case 3: len |= (uint16_t)c << 8; state = 4; break;
                case 4: cls = c;                 state = 5; break;
                case 5:
                    id   = c;
                    need = (len > sizeof(body)) ? sizeof(body) : len;
                    n    = 0;
                    state = (need > 0) ? 6 : 7;
                    break;
                case 6:
                    body[n++] = c;
                    if (n >= need) { n = 0; state = 7; }
                    break;
                case 7: // 체크섬 4바이트를 다 받으면 한 개 완성
                    if (++n >= 4) {
                        if (cls == wantCls && (wantId == 0xFF || id == wantId)) {
                            if (gotId)  *gotId  = id;
                            const uint16_t cp = (need > outCap) ? outCap : need;
                            if (out && cp) memcpy(out, body, cp);
                            if (outLen) *outLen = cp;
                            return true;
                        }
                        state = 0;
                    }
                    break;
            }
        }
        feedWatchdog();
        delay(2);
    }
    return false;
}

// 설정을 보내고 ACK 를 확인한다. 짐작하지 않는다.
static bool casicSetAcked(uint8_t cls, uint8_t id,
                          const uint8_t* payload, uint16_t len, const char* what) {
    while (Serial1.available()) gGps.encode((char)Serial1.read()); // 밀린 것 비우기
    casicSend(cls, id, payload, len);

    uint8_t ackId = 0;
    if (!casicWait(2000, 0x05, 0xFF, nullptr, 0, nullptr, &ackId)) {
        Serial.printf("  %s — 응답 없음. 이 칩이 모르는 설정입니다\n", what);
        return false;
    }
    if (ackId == 0x01) {
        Serial.printf("  %s — ACK. 받아들였습니다\n", what);
        return true;
    }
    Serial.printf("  %s — NACK. 거절당했습니다\n", what);
    return false;
}

// 조회는 길이 0 으로 보낸다 (문서 §2.11).
static bool casicQuery(uint8_t cls, uint8_t id, uint8_t* out, uint16_t cap, uint16_t* len) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        while (Serial1.available()) gGps.encode((char)Serial1.read());
        casicSend(cls, id, nullptr, 0);
        if (casicWait(1500, cls, id, out, cap, len, nullptr)) return true;
        gCasicIdFirst = !gCasicIdFirst; // 반대 공식으로 한 번 더
    }
    return false;
}

// CFG-NAVX 의 dyModel 값 이름 (문서 §2.11.8 Remark[2])
static const char* dyModelName(uint8_t m) {
    switch (m) {
        case 0: return "Portable 일반 휴대";
        case 1: return "Static   정지";
        case 2: return "Walking  보행";
        case 3: return "Car      자동차";
        case 4: return "Nautical 선박";
        case 5: return "Flight   항공 <1g";
        case 6: return "Flight   항공 <2g";
        case 7: return "Flight   항공 <4g";
        default: return "알 수 없는 값";
    }
}

// 지금 걸려 있는 설정을 전부 되물어서 보여준다.
//
// ★ 여기 나오는 값은 우리가 "보냈다" 가 아니라 모듈이 "이렇게 되어 있다" 고
//   답한 값이다. 둘이 다를 수 있다. 실제로 확인해 본 적이 없었다.
static void gpsCfgDump() {
    uint8_t  buf[64];
    uint16_t len = 0;

    Serial.println("──────────────────────────────────────────");
    Serial.println("  모듈이 답한 실제 설정 (CASIC 바이너리 조회)");
    Serial.println("──────────────────────────────────────────");

    // ── 통신 속도 ────────────────────────────────────────────────────────
    if (casicQuery(0x06, 0x00, buf, sizeof(buf), &len) && len >= 8) {
        uint32_t baud; memcpy(&baud, buf + 4, 4);
        Serial.printf("  통신 속도     %lu bps", (unsigned long)baud);
        Serial.printf("   %s\n", baud == kGpsBaud ? "— 우리가 건 값과 같습니다"
                                                  : "★ 우리가 건 값과 다릅니다");
        Serial.printf("  포트 %u  프로토콜마스크 0x%02X (B1 텍스트입력 B5 텍스트출력)\n",
                      buf[0], buf[1]);
    } else {
        Serial.println("  통신 속도     조회 실패 (CFG-PRT 무응답)");
    }

    // ── 갱신율 ───────────────────────────────────────────────────────────
    if (casicQuery(0x06, 0x04, buf, sizeof(buf), &len) && len >= 4) {
        uint16_t interval; memcpy(&interval, buf + 0, 2);
        Serial.printf("  측위 간격     %u ms", (unsigned)interval);
        if (interval > 0) Serial.printf(" (= %.1f Hz)", 1000.0f / interval);
        Serial.printf("   %s\n", interval == 100 ? "— 10 Hz 로 걸렸습니다"
                                                 : "★ 10 Hz 가 아닙니다");
    } else {
        Serial.println("  측위 간격     조회 실패 (CFG-RATE 무응답)");
    }

    // ── 항법 엔진 ────────────────────────────────────────────────────────
    if (casicQuery(0x06, 0x07, buf, sizeof(buf), &len) && len >= 44) {
        float    staticTh; memcpy(&staticTh, buf + 40, 4);
        uint32_t mask;     memcpy(&mask,     buf + 0,  4);
        const uint8_t nav = buf[13];

        // ★ 선박 모드(4)가 아니라 휴대 모드(0)를 쓴다. 실측 결과다.
        //   4(선박)로 두면 느린 움직임을 정지로 보고 0 으로 뭉갠다.
        //   요트는 정박·미풍이 0~2 kn 이라 그 구간을 잃으면 안 된다.
        Serial.printf("  움직임 종류   %u  %s%s\n", buf[4], dyModelName(buf[4]),
                      buf[4] == 0 ? "   — 우리가 고른 값" : "   ★ 우리는 0(휴대)을 쓴다");
        Serial.printf("  정지 문턱값   %.2f m/s (= %.2f kn)%s\n",
                      staticTh, staticTh * 1.943844f,
                      staticTh > 0.0f ? "   ★ 이 아래 속도는 0 으로 뭉갠다" : "   — 꺼져 있다");
        Serial.printf("  쓰는 위성     %s%s%s  (GPS/BDS/GLONASS 중)\n",
                      (nav & 1) ? "GPS " : "", (nav & 2) ? "BeiDou " : "",
                      (nav & 4) ? "GLONASS" : "");
        Serial.printf("  최소 신호     %u dB-Hz   위성 %u~%u개   최소 고도각 %d도\n",
                      buf[8], buf[6], buf[7], (int8_t)buf[11]);
        Serial.printf("  mask          0x%08lX\n", (unsigned long)mask);

        // 해석이 맞는지 눈으로 확인할 수 있게 원본 44바이트를 그대로 찍는다.
        // 필드 위치를 잘못 잡으면 그럴듯한 값이 나와도 전부 헛것이다.
        Serial.print("  원본 44바이트 ");
        for (uint16_t i = 0; i < len; i++) {
            if (i && i % 16 == 0) Serial.print("\n                ");
            Serial.printf("%02X ", buf[i]);
        }
        Serial.println();
    } else {
        Serial.println("  항법 엔진     조회 실패 (CFG-NAVX 무응답)");
    }
    Serial.printf("  (체크섬 공식은 %s 문서 방식이 먹혔습니다)\n",
                  gCasicIdFirst ? "L76K" : "CASIC");
    Serial.println("──────────────────────────────────────────");
}

// NMEA 로 만들어지기 전의 속도를 직접 본다 (NAV-PV, 문서 §2.7.4).
//
// ★ 무엇을 가리려는 것인가
//   RMC 의 속도가 0 인데 여기 velN/velE 가 살아 있으면, 다듬기는 NMEA 문장을
//   만드는 단계에 있다는 뜻이다. 둘 다 0 이면 항법 엔진 안에서 눌린 것이다.
//   짐작으로 못 가르는 것을 값으로 가른다.
//
// sAcc 는 분산(m/s)^2 이라 제곱근을 씌워야 m/s 가 된다. 모듈이 자기 속도를
// 얼마나 믿는지 알려주는 값이다.
static void gpsNavPv(int samples) {
    Serial.println("──────────────────────────────────────────");
    Serial.println("  RMC 속도 vs NAV-PV 속도 (NMEA 거치기 전)");
    Serial.println("──────────────────────────────────────────");

    for (int i = 0; i < samples; i++) {
        uint8_t  p[80];
        uint16_t len = 0;

        // NAV 메시지는 길이 0 조회로는 안 나온다 (실측). 대신 CFG-MSG 로
        // 갱신율 0xFFFF 를 주면 "즉시 한 번만 출력" 이고 조회와 같다
        // (문서 §2.11.2 Remark[1]).
        const uint8_t poll[4] = {0x01, 0x03, 0xFF, 0xFF};
        while (Serial1.available()) gGps.encode((char)Serial1.read());
        casicSend(0x06, 0x01, poll, 4);

        if (!casicWait(2000, 0x01, 0x03, p, sizeof(p), &len, nullptr) || len < 80) {
            Serial.printf("  NAV-PV 응답 없음 (%u 바이트)\n", (unsigned)len);
            break;
        }

        float velN, velE, velU, speed2D, heading, sAcc, pDop;
        memcpy(&pDop,    p + 12, 4);
        memcpy(&velN,    p + 48, 4);
        memcpy(&velE,    p + 52, 4);
        memcpy(&velU,    p + 56, 4);
        memcpy(&speed2D, p + 64, 4);
        memcpy(&heading, p + 68, 4);
        memcpy(&sAcc,    p + 72, 4);

        const float rmcKn = gGps.speed.isValid() ? (float)gGps.speed.knots() : -1.0f;
        const float pvKn  = speed2D * 1.943844f;
        const float sAccMs = (sAcc > 0.0f) ? sqrtf(sAcc) : 0.0f;

        Serial.printf("  RMC %6s kn | NAV-PV %6.2f kn | N%+6.2f E%+6.2f U%+6.2f m/s"
                      " | 침로 %5.1f | 오차 ±%.2f kn | 위성 %u | pDop %.1f | 유효 위치%u 속도%u\n",
                      rmcKn >= 0 ? String(rmcKn, 2).c_str() : " --- ",
                      pvKn, velN, velE, velU, heading,
                      sAccMs * 1.943844f, p[7], pDop, p[4], p[5]);

        feedWatchdog();
        delay(900);
    }
    Serial.println("──────────────────────────────────────────");
}

// CFG-NAVX 에서 항목 몇 개만 바꾼다.
//
// 지금 값을 먼저 읽어서 나머지는 그대로 두고, mask 에 세운 항목만 고친다.
// mask 가 1 인 항목만 적용된다 (문서 §2.11.8 Remark[1]).
//   B0  dyModel        움직임 종류
//   B13 staticHoldTh   정지로 볼 속도
//
// ★ ACK 가 왔다고 믿지 않는다. 반드시 되읽어서 값이 바뀌었는지 확인한다.
static void gpsCfgSetNavx(bool setModel, uint8_t model,
                          bool setStatic, float staticTh) {
    uint8_t  p[44];
    uint16_t len = 0;

    Serial.println("──────────────────────────────────────────");
    if (!casicQuery(0x06, 0x07, p, sizeof(p), &len) || len < 44) {
        Serial.println("  지금 값을 못 읽었습니다. 아무것도 바꾸지 않습니다.");
        Serial.println("──────────────────────────────────────────");
        return;
    }

    float beforeTh; memcpy(&beforeTh, p + 40, 4);
    Serial.printf("  바꾸기 전   dyModel %u (%s)   staticHoldTh %.2f m/s\n",
                  p[4], dyModelName(p[4]), beforeTh);

    uint32_t mask = 0;
    if (setModel)  { mask |= (1UL << 0);  p[4] = model; }
    if (setStatic) { mask |= (1UL << 13); memcpy(p + 40, &staticTh, 4); }
    memcpy(p + 0, &mask, 4);

    if (!casicSetAcked(0x06, 0x07, p, 44, "CFG-NAVX")) {
        Serial.println("──────────────────────────────────────────");
        return;
    }

    if (casicQuery(0x06, 0x07, p, sizeof(p), &len) && len >= 44) {
        float afterTh; memcpy(&afterTh, p + 40, 4);
        Serial.printf("  바꾼 뒤     dyModel %u (%s)   staticHoldTh %.2f m/s\n",
                      p[4], dyModelName(p[4]), afterTh);
        const bool okModel  = !setModel  || p[4] == model;
        const bool okStatic = !setStatic || afterTh == staticTh;
        if (okModel && okStatic) {
            Serial.println("  ✓ 값이 실제로 바뀌었습니다");
            // 밖에서 노트북 없이 쓰려면 껐다 켜도 유지돼야 한다.
            // 보드에 적어 두고 부팅할 때마다 다시 건다.
            if (setModel) {
                gPrefs.putUChar("gps_dyn", model);
                Serial.println("  보드에 적어 뒀습니다. 껐다 켜도 이 모드로 다시 겁니다.");
            }
        } else {
            Serial.println("  ★ ACK 는 왔는데 값이 안 바뀌었습니다 — 이 칩은 이 항목을 안 받습니다");
        }
    }
    Serial.println("──────────────────────────────────────────");
}

// GPS 모듈에 NMEA 명령을 하나 보내고, 응답을 3초 동안 본다.
//
// ★ 명령이 먹혔는지는 ACK 로만 알 수 있다. 전에 $PMTK251 을 보내고
//   "무시당했다" 고 적었는데, 그건 통신 속도가 안 바뀐 걸 보고 짐작한
//   것이었지 응답을 본 게 아니었다.
//     MTK   → $PMTK001,<명령번호>,<결과>   결과 3 이 성공, 0 이 거절
//     CASIC → 0xBA 0xCE ... 로 시작하는 바이너리 ACK
//
// 늘 오는 위치 문장(GGA/RMC 등)은 걸러내고 나머지만 보여준다.
static void gpsSendAndWatch(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; ++p) ck ^= (uint8_t)*p;

    Serial.println("──────────────────────────────────────────");
    Serial.printf("  보냅니다   $%s*%02X\n", body, ck);
    Serial.println("  3초 동안 응답을 봅니다 (위치 문장은 걸러냅니다)");
    Serial.println("──────────────────────────────────────────");

    gpsSend(body);

    char     line[140];
    size_t   n     = 0;
    int      shown = 0;
    uint32_t start = millis();

    while (millis() - start < 3000) {
        while (Serial1.available()) {
            const char c = (char)Serial1.read();
            gGps.encode(c); // 보는 동안에도 파서는 계속 먹인다
            if (c == '\n' || c == '\r') {
                if (n > 0) {
                    line[n] = 0;
                    const bool routine =
                        line[0] == '$' &&
                        (strstr(line, "GGA") || strstr(line, "RMC") ||
                         strstr(line, "GSV") || strstr(line, "GSA") ||
                         strstr(line, "VTG") || strstr(line, "GLL"));
                    if (!routine) {
                        Serial.printf("  <<  %s\n", line);
                        shown++;
                    }
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
        }
        feedWatchdog();
        delay(2);
    }

    if (shown == 0) {
        Serial.println("  응답 없음 — 모듈이 이 명령을 모릅니다.");
        Serial.println("  (문장은 계속 들어오고 있으니 통신 자체는 멀쩡합니다)");
    }
    Serial.println("──────────────────────────────────────────");
}

static void peekGps(uint32_t seconds, bool slotD) {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  UART1 (RX GPIO%d / TX GPIO%d) %lubps — %us 동안 원시 데이터\n",
                  rak::kUART1_RX, rak::kUART1_TX, (unsigned long)kGpsBaud, (unsigned)seconds);
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
    Serial.printf(" | roll %+6.1f° pitch %+6.1f° → 힐 %+6.1f° (가속 %s)\n",
                  gRollDeg, gPitchDeg, currentHeelDeg(), heelAxisName());
}

// 지금 자세를 평형(힐 0°)으로 삼는다. 배를 물에 띄우고 평형일 때 쓴다.
static void doLevel() {
    if (!gImuOk) {
        Serial.println("[IMU] 붙어 있지 않습니다.");
        return;
    }
    imuUpdate();
    gHeelOffsetDeg = rawHeelDeg();
    gPrefs.begin("sail", false);
    gPrefs.putFloat("heel_off2", gHeelOffsetDeg);
    gPrefs.end();

    Serial.println("──────────────────────────────────────────");
    Serial.printf("  지금 자세를 평형으로 삼았습니다.\n");
    Serial.printf("  힐을 재는 축 가속 %s\n", heelAxisName());
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
// ★ 값이 없으면 없다고 보낸다. 지어내지 않는다.
//
// 예전에는 위성을 못 잡으면 시뮬레이터가 만든 5~7 kn 을 채워 보냈다.
// 표시에 (SIM) 을 붙였지만 결국 헷갈렸다 — 화면에 그럴듯한 숫자가 떠 있으면
// 사람은 그걸 읽는다. 0 을 보내는 것도 안 된다. 정박 중 SOG 0.0 과
// 구별되지 않는다. 그래서 무효 표식을 보내고 받는 쪽이 숫자를 아예 안 그린다.
//
//   SOG·COG   GPS 가 위성을 잡았을 때만 값이 있다
//   HEEL      IMU 가 살아 있으면 GPS 와 무관하게 늘 값이 있다
//   BATT      항상 있다 (1 Hz 로 갱신되는 캐시)

static float gBattPct   = 100.0f; // 1 Hz 로 갱신
// 잔량(%) 옆에 전압을 같이 내보낸다. 3.8~3.9 V 구간은 방전 곡선이 거의
// 평평해서, 전압이 조금만 떨어져도 퍼센트가 크게 내려앉는다. 퍼센트만 보면
// 배터리가 갑자기 닳는 것처럼 보인다. 둘을 나란히 봐야 판단이 선다.
static float gBattVolts = 0.0f;

// BLE 로 함께 내보낼 확장 필드를 모은다.
//
// 앞 12바이트 뒤에 붙는 값들이라, 옛 앱은 이걸 못 보고도 그대로 돈다
// (PROTOCOL.md §7 전방 호환). 자세한 배치는 protocol.h 의 encodeTelemetryExt().
static sail::TelemetryExtra buildExtra() {
    sail::TelemetryExtra e;
    e.gpsFix       = gGpsFix;
    e.imuOk        = gImuOk;
    e.magOk        = gMagOk;
    e.satellites   = gGps.satellites.isValid() ? (uint8_t)gGps.satellites.value() : 0;

    // HDOP — 작을수록 정확하다. 음수는 "모름" 이라는 뜻이다.
    //
    // ★ 위성을 못 잡으면 L76K 가 25.5 같은 값을 채워 보낸다. 자리를 비워 두지
    //   않으려고 넣는 숫자일 뿐 정확도가 아니다. 그대로 흘리면 화면에
    //   "fix 없음 / HDOP 25.4" 처럼 앞뒤가 안 맞는 값이 뜬다.
    //   실제 GPS 문장:  $GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5
    float hdop = gGps.hdop.isValid() ? (float)gGps.hdop.hdop() : -1.0f;
    if (!gGpsFix || hdop > 20.0f) hdop = -1.0f; // 20 넘는 HDOP 은 어차피 못 쓴다
    e.hdop = hdop;

    e.headingDeg = headingDeg();
    e.pitchDeg   = gPitchDeg;
    e.accX = gAcc.x; e.accY = gAcc.y; e.accZ = gAcc.z;
    e.gyrX = gGyr.x; e.gyrY = gGyr.y; e.gyrZ = gGyr.z;
    e.magX = gMag.x; e.magY = gMag.y; e.magZ = gMag.z;
    e.battVolts = gBattVolts;
    return e;
}

static Telemetry buildTelemetry(uint32_t nowMs) {
    Telemetry t;
    t.moduleID = gModuleID;
    t.uptimeMs = nowMs;

    t.sogValid = gGpsFix;
    t.cogValid = gGpsFix;
    if (gGpsFix) {
        // 다듬은 값을 내보낸다. 세기는 `smooth` 로 고른다 (0 이면 원본 그대로).
        t.sogKn  = sogOut();
        t.cogDeg = (gCogDamped >= 0.0f) ? gCogDamped : (float)gGps.course.deg();
    }

    // 어느 축을 힐로 볼지는 보드를 배에 어떻게 다느냐에 달렸다.
    // `heel` 명령으로 고르고 NVS 에 남는다. 위 "힐" 항목 참고.
    t.heelValid = gImuOk;
    if (gImuOk) t.heelDeg = currentHeelDeg();

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
//   자리 남음 → ADV_IND       (connectable + scannable)
//   정원 참   → ADV_SCAN_IND  (non-connectable + scannable, scan response 유지)
//
// ★ 예전에는 한 대만 붙으면 바로 연결 불가로 바꿨다. 그래서 워치가 먼저
//   붙으면 아이폰이 광고밖에 못 읽었다. 광고에는 9축과 방위가 안 실려서
//   아이폰 화면이 COG 로 떨어졌다 (실제로 겪었다).
//
//   NimBLE 은 3대까지 받는다 (nimconfig.h 의 CONFIG_BT_NIMBLE_MAX_CONNECTIONS).
//   정원이 찰 때까지는 계속 연결을 받는다. 아이폰과 워치가 둘 다 붙어야
//   둘 다 제대로 된 값을 본다.
static uint8_t connectedCount() {
    NimBLEServer* srv = NimBLEDevice::getServer();
    return srv ? (uint8_t)srv->getConnectedCount() : 0;
}

static void applyAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();

    // 주의: setConnectableMode/setDiscoverableMode 는 내부 m_advData 의 Flags 를
    //       건드리므로 반드시 setAdvertisementData() 보다 먼저 호출해야 한다.
    const bool full = connectedCount() >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    adv->setConnectableMode(full ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND);
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
    Serial.printf("[BLE] advertising 시작 — %s (%s, 연결 %u/%d, interval %ums)\n",
                  gFullName,
                  full ? "ADV_SCAN_IND / non-connectable"
                       : "ADV_IND / connectable",
                  connectedCount(), CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
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
        gConnected = server->getConnectedCount() > 0;
        // 남은 연결이 없을 때만 구독을 지운다. 두 대가 붙어 있는데 한 대가
        // 나갔다고 나머지 구독까지 없던 일로 만들면 notify 가 멎는다.
        if (!gConnected) gSubscribed = false;
        gAdvNeedsApply = true; // loop() 가 즉시 connectable 광고로 되돌린다
        digitalWrite(rak::kLedBlue, LOW);
        Serial.printf("[BLE] 연결 끊김 → %s (reason=%d, 남은 연결 %u) — 재광고 준비\n",
                      info.getAddress().toString().c_str(), reason,
                      server->getConnectedCount());
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
        const bool on = (subValue & 0x0001) != 0; // bit0 = notify
        // 여러 대가 붙을 수 있다. 한 대라도 구독 중이면 계속 내보낸다.
        // 마지막 한 대가 끊기는 건 onDisconnect 에서 정리한다.
        if (on) gSubscribed = true;
        Serial.printf("[BLE] notify 구독 %s ← %s (연결 %u)\n",
                      on ? "ON" : "OFF", info.getAddress().toString().c_str(),
                      NimBLEDevice::getServer()->getConnectedCount());
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
    Serial.println("  gpshz <1|2|5|10> GPS 갱신율 (기본 5). 실제로 걸렸는지 세어 줍니다");
    Serial.println("  gpscfg        모듈에 실제로 걸린 설정을 되물어봅니다");
    Serial.println("  smooth <0~5>  속도·침로 다듬기 세기 (0 원본, 기본 2)");
    Serial.println("  dead <kn>     잡음 바닥. 이보다 작은 속도는 0 (기본 0.10)");
    Serial.println("  navpv         NMEA 거치기 전 속도를 RMC 와 나란히 (navpv l 은 길게)");
    Serial.println("  gpscfg mode <0~7>  움직임 종류 (0휴대 1정지 2보행 3자동차 4선박)");
    Serial.println("  gpscfg static <m/s> 정지로 볼 속도 문턱값");
    Serial.println("  nmea <본문>   NMEA 명령을 보내고 응답을 봅니다 (체크섬 자동)");
    Serial.println("  batt          배터리 전압 실측");
    Serial.println("  level         ★ 지금 자세를 힐 0° 로 삼기 (배가 평형일 때)");
    Serial.println("  heel [x|y|z]  힐을 어느 가속도 축에서 볼지 (앞에 - 로 뒤집기)");
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

    // 힐을 어느 가속도 축에서 볼지. 보드를 다는 방법이 바뀌면 여기만 고친다.
    if (line == "heel" || line.startsWith("heel ")) {
        String arg = line.substring(4);
        arg.trim();
        if (arg.length() > 0) {
            float sign = 1.0f;
            if (arg.startsWith("-")) { sign = -1.0f; arg = arg.substring(1); }
            else if (arg.startsWith("+")) { arg = arg.substring(1); }
            arg.toLowerCase();

            int axis = -1;
            if (arg == "x") axis = 0;
            else if (arg == "y") axis = 1;
            else if (arg == "z") axis = 2;
            if (axis < 0) {
                Serial.println("[IMU] heel x | heel y | heel z (앞에 - 를 붙이면 좌우 뒤집기)");
                return;
            }

            gHeelAxis = (uint8_t)axis;
            gHeelSign = sign;
            // 축을 바꾸면 옛 기준각은 다른 축에서 잡은 값이라 뜻이 없다.
            gHeelOffsetDeg = 0.0f;
            gPrefs.begin("sail", false);
            gPrefs.putUChar("heel_axis", gHeelAxis);
            gPrefs.putChar("heel_sgn", sign < 0.0f ? -1 : 1);
            gPrefs.putFloat("heel_off2", 0.0f);
            gPrefs.end();
            Serial.println("  기준각은 0 으로 되돌렸습니다. 평형일 때 level 을 다시 치세요.");
        }

        imuUpdate();
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  힐을 재는 축   가속 %s\n", heelAxisName());
        Serial.printf("  기준각         %+.1f°\n", gHeelOffsetDeg);
        Serial.printf("  지금 가속      %+.2f %+.2f %+.2f g\n", gAcc.x, gAcc.y, gAcc.z);
        Serial.printf("  지금 힐        %+.1f°  (기준각 빼기 전 %+.1f°)\n",
                      currentHeelDeg(), rawHeelDeg());
        Serial.println("──────────────────────────────────────────");
        Serial.println("  배가 평형일 때 그 축이 0 g 에 가까워야 맞는 축입니다.");
        return;
    }

    // GPS 갱신율. 밖에서 값이 굼뜨면 올리고, 문장이 깨지면 내린다.
    if (line.startsWith("gpshz ")) {
        long hz = line.substring(6).toInt();
        if (hz != 1 && hz != 2 && hz != 5 && hz != 10) {
            Serial.println("[GPS] 1, 2, 5, 10 중에서 고르세요. 예) gpshz 5");
            Serial.println("      1/2/5 는 문서에 있는 값, 10 은 확인 안 된 값입니다.");
            return;
        }
        gpsSetRate((uint8_t)hz);
        Serial.printf("[GPS] 갱신율 → %u Hz 로 요청했습니다. 실제로 걸렸는지 셉니다...\n",
                      gGpsHz);

        // 말로만 바뀌었는지 모르니 실제로 들어오는 문장을 센다.
        // 우리가 켜 둔 문장은 GGA 와 RMC 둘뿐이므로, 갱신 한 번에 두 개가 온다.
        const uint32_t before = gGps.passedChecksum();
        const uint32_t bad0   = gGps.failedChecksum();
        const uint32_t t0     = millis();
        while (millis() - t0 < 5000) {
            gpsPoll();
            feedWatchdog();
            delay(2);
        }
        const uint32_t got  = gGps.passedChecksum() - before;
        const uint32_t bad  = gGps.failedChecksum() - bad0;
        const float perSec  = got / 5.0f;

        Serial.println("──────────────────────────────────────────");
        Serial.printf("  5초 동안 문장 %lu개 (초당 %.1f개)\n",
                      (unsigned long)got, perSec);
        Serial.printf("  GGA+RMC 두 종류이므로 → 초당 %.1f 번 갱신\n", perSec / 2.0f);
        Serial.printf("  체크섬 실패 %lu\n", (unsigned long)bad);
        Serial.println("──────────────────────────────────────────");
        if (bad > got / 10) {
            Serial.println("  ★ 깨진 문장이 많습니다. 대역폭이 모자랍니다.");
            Serial.println("    gpshz 5 로 되돌리세요.");
        } else if (perSec / 2.0f < gGpsHz * 0.7f) {
            Serial.printf("  ★ 요청한 %u Hz 만큼 안 옵니다. 모듈이 무시한 것입니다.\n",
                          gGpsHz);
        } else {
            Serial.println("  요청한 만큼 들어옵니다.");
        }
        return;
    }

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

    // gpscfg      — 모듈에 실제로 걸린 설정을 되물어본다
    // gpscfg sea  — 요트용으로 바꾼다 (움직임=선박, 정지 문턱=0)
    if (line == "gpscfg") { gpsCfgDump(); return; }
    // navpv  — NMEA 거치기 전 속도를 RMC 와 나란히 본다
    // dead <kn>  잡음 바닥. 이보다 작은 속도는 0 으로 보여준다.
    if (line.startsWith("dead")) {
        String a = line.substring(4); a.trim();
        if (a.length() > 0) {
            const float v = a.toFloat();
            if (v < 0.0f || v > 2.0f) { Serial.println("  0 ~ 2.0 kn 사이로 주세요"); return; }
            gDeadbandKn = v;
            gPrefs.putFloat("dead_kn", gDeadbandKn);
        }
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  잡음 바닥  %.2f kn — 이보다 작으면 0 으로 보여줍니다\n", gDeadbandKn);
        Serial.println("  도플러의 이론 잡음이 초당 몇 cm(=0.1 kn 언저리)라서 기본값이 0.10 입니다.");
        Serial.println("  Velocitek ProStart V2 가 파는 물건의 사양도 ±0.1 kn 입니다.");
        if (gGpsFix) Serial.printf("  지금  다듬은 값 %.2f  →  보여주는 값 %.2f kn\n",
                                   gSogDamped, sogOut());
        Serial.println("──────────────────────────────────────────");
        return;
    }

    // smooth <0~5>  다듬기 세기. 0 이면 원본 그대로.
    if (line.startsWith("smooth")) {
        String a = line.substring(6); a.trim();
        if (a.length() > 0) {
            const int lv = a.toInt();
            if (lv < 0 || lv > 5) { Serial.println("  0~5 중에서 고르세요"); return; }
            gDampLevel = (uint8_t)lv;
            gPrefs.putUChar("damp", gDampLevel);
            dampingReset();
        }
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  다듬기 세기  %u단계  (시상수 %.1f초)\n",
                      gDampLevel, kDampTau[gDampLevel]);
        Serial.println("  0 없음 / 1 0.3초 / 2 0.6초 / 3 1.2초 / 4 2.5초 / 5 5초");
        Serial.println("  잔잔하면 낮게, 물결이 거칠면 높게. 요트 계기들이 쓰는 방식이다.");
        if (gGpsFix) {
            Serial.printf("  지금  원본 %.2f kn %5.1f°  →  다듬은 값 %.2f kn %5.1f°\n",
                          gGps.speed.knots(), gGps.course.deg(), gSogDamped, gCogDamped);
        }
        Serial.println("──────────────────────────────────────────");
        return;
    }

    if (line == "navpv")   { gpsNavPv(8);  return; }
    if (line == "navpv l") { gpsNavPv(40); return; }

    // gpscfg mode <0~7>   움직임 종류를 바꾼다 (4=선박, 2=보행, 0=휴대)
    if (line.startsWith("gpscfg mode ")) {
        const int m = line.substring(12).toInt();
        if (m < 0 || m > 7) { Serial.println("  0~7 중에서 고르세요"); return; }
        gpsCfgSetNavx(true, (uint8_t)m, false, 0.0f);
        return;
    }
    // gpscfg static <m/s>  정지로 볼 속도를 바꾼다
    if (line.startsWith("gpscfg static ")) {
        const float th = line.substring(14).toFloat();
        gpsCfgSetNavx(false, 0, true, th);
        return;
    }

    // nmea <본문>  — 체크섬은 알아서 붙인다. 예: nmea PMTK386,0
    if (line.startsWith("nmea ")) {
        String body = line.substring(5);
        body.trim();
        if (body.startsWith("$")) body = body.substring(1);
        const int star = body.indexOf('*');
        if (star >= 0) body = body.substring(0, star);
        if (body.length() == 0) { Serial.println("  보낼 내용이 없습니다"); return; }
        gpsSendAndWatch(body.c_str());
        return;
    }

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
    gBattVolts = readBatteryVolts(nullptr);
    gBattPct   = batteryPercent(gBattVolts);


    // ── 센서 붙이기 ─────────────────────────────────────────────────────
    gpsBegin();
    Serial.printf("[GPS] UART1 %lubps / %u Hz (RX GPIO%d / TX GPIO%d)\n",
                  (unsigned long)kGpsBaud, gGpsHz, rak::kUART1_RX, rak::kUART1_TX);

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

    uint8_t initial[sail::kTelemetryExtLen];
    sail::encodeTelemetryExt(gLatest, buildExtra(), initial);
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
        const float freshV = readBatteryVolts(nullptr);
        gBattVolts  = gBattVolts * 0.8f + freshV * 0.2f;
        gBattPct    = gBattPct * 0.8f + batteryPercent(freshV) * 0.2f;

        // 같은 주기로 센서가 아직 붙어 있는지도 확인한다.
        // 사라졌으면 끄고 나머지로 계속 간다. 돌아오면 다시 붙는다.
        checkSensors();
    }

    // 3) gNotifyPeriodMs 주기 — 값 조립 + characteristic 갱신 + notify
    if (now - lastNotify >= gNotifyPeriodMs) {
        lastNotify = now;
        gpsUpdateFix();
        gLatest = buildTelemetry(now);

        // 12바이트 뒤에 9축과 GPS 상태를 덧붙여 보낸다. 옛 앱은 앞 12바이트만
        // 읽으므로 그대로 돈다 (PROTOCOL.md §7).
        uint8_t packet[sail::kTelemetryExtLen];
        sail::encodeTelemetryExt(gLatest, buildExtra(), packet);
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
        ds.battVolts    = gBattVolts;

        ds.sogKn      = gLatest.sogKn; // 다듬고 잡음 바닥까지 적용된 값
        ds.cogDeg     = gLatest.cogDeg;
        ds.headingDeg = headingDeg();
        ds.heelDeg    = gLatest.heelDeg;
        ds.pitchDeg   = gPitchDeg;

        ds.accX  = gAcc.x; ds.accY = gAcc.y; ds.accZ = gAcc.z;
        ds.gyrX  = gGyr.x; ds.gyrY = gGyr.y; ds.gyrZ = gGyr.z;
        ds.magX  = gMag.x; ds.magY = gMag.y; ds.magZ = gMag.z;
        ds.imuOk = gImuOk;
        ds.magOk = gMagOk;

        ds.sogValid   = gLatest.sogValid;
        ds.heelValid  = gLatest.heelValid;
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
        char sogTxt[16], cogTxt[16], heelTxt[16];
        if (gLatest.sogValid)  snprintf(sogTxt, sizeof(sogTxt), "%5.2f", gLatest.sogKn);
        else                   snprintf(sogTxt, sizeof(sogTxt), "%5s", "--.--");
        if (gLatest.cogValid)  snprintf(cogTxt, sizeof(cogTxt), "%5.1f", gLatest.cogDeg);
        else                   snprintf(cogTxt, sizeof(cogTxt), "%5s", "---");
        if (gLatest.heelValid) snprintf(heelTxt, sizeof(heelTxt), "%+6.1f", gLatest.heelDeg);
        else                   snprintf(heelTxt, sizeof(heelTxt), "%6s", "---");

        Serial.printf(
            "[%7.1fs] %s | SOG %s kn | COG %s° | HEEL %s° | BATT %3d%% %.2fV | seq %3u | %s%s\n",
            now / 1000.0f, gFullName, sogTxt, cogTxt, heelTxt,
            (int)sail::encodeBatt(gLatest.battPct), gBattVolts, gSeq,
            gConnected ? "CONNECTED" : "ADVERTISING",
            gConnected ? (gSubscribed ? " (notify ON)" : " (notify OFF)") : "");
        printGpsLine();
        // 두 방식을 나란히 본다. 어느 쪽을 쓸지 정하기 전까지는 재기만 한다.
        if (gGpsFix) {
            Serial.printf("   속도 비교  도플러 %5.2f kn  |  위치차분 %s kn\n",
                          gGps.speed.knots(),
                          gSogFromPos >= 0 ? String(gSogFromPos, 2).c_str() : " --- ");
        }
        printImuLine();
    }

    feedWatchdog(); // 여기까지 왔으면 살아 있다는 뜻
    delay(2);
}
