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
#include <driver/rtc_io.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

#include "board_rak.h"
#include "display_rak.h"
#include "lora.h"
#include "hlog.h"
#include "netsrv.h"
#include "protocol.h"

using sail::Telemetry;

// ── 모듈 신원 ────────────────────────────────────────────────────────────
static Preferences gPrefs;

// 다듬기 세기와 잡음 바닥. 설정에서 먼저 읽으므로 여기 둔다.
static float gBattPct   = 100.0f; // 1 Hz 로 갱신
// 잔량(%) 옆에 전압을 같이 내보낸다. 3.8~3.9 V 구간은 방전 곡선이 거의
// 평평해서, 전압이 조금만 떨어져도 퍼센트가 크게 내려앉는다. 퍼센트만 보면
// 배터리가 갑자기 닳는 것처럼 보인다. 둘을 나란히 봐야 판단이 선다.
// ★ 기록에는 전압 원시값만 남긴다. 퍼센트는 곡선에서 나온 파생값이라
//   곡선을 고치면 옛 데이터의 뜻이 달라진다 (로그포맷 명세 §7).
static float gBattVolts = 0.0f;

static uint8_t gDampLevel  = 2;      // 0~5. 아래 kDampTau 의 칸 번호
static float   gDeadbandKn = 0.10f;  // 이보다 작은 속도는 0 으로 내린다
static char        gUserName[sail::kMaxUserNameLen + 1] = {0}; // "hojun"
static char        gFullName[sail::kMaxFullNameLen + 1] = {0}; // "SAIL-hojun"
static uint8_t     gModuleID = 1;

// ── 로라 배 번호 (PROTOCOL.md §10.11) ────────────────────────────────────
//
// gModuleID 와 **다른 값이다.** gModuleID 는 이름을 1바이트로 접은 해시라
// 배가 30척이면 어딘가 겹칠 확률이 83 % 다. 말할 차례를 정하는 데는 못 쓴다.
// gBoatId 는 사람이 준 번호이고, 이것만 로라 차례를 정한다.
//
//   0        번호 없음. 로라로 안 보낸다
//   1 ~ 30   선수 배            차례 0 ~ 29
//   31       코치보트 · 본부     차례 30
//   32       예비               차례 31
static uint8_t  gBoatId       = 0;
static uint32_t gBoatIdSetAt  = 0; // 바꾼 시각. 30초 동안 flags 로 알린다
static constexpr uint8_t  kBoatIdMax     = 32;
static constexpr uint32_t kBoatIdShoutMs = 30000;

// ── BLE 전역 상태 ────────────────────────────────────────────────────────
static NimBLEServer*         gServer       = nullptr;
static NimBLECharacteristic* gTelemetryChr = nullptr;

static volatile bool gConnected     = false; // 중앙장치 연결 여부
static volatile bool gAdvNeedsApply = true;  // 광고 모드 재적용 필요
static bool          gBleUp         = false; // BLE 가 올라와 있나 (WiFi 켜면 내린다)
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
static float    gImuTempC = 0.0f;

// 자이로 0점 (원시값). 자세히는 아래 "자이로 0점" 항목 참고.
static float gGyrOffX = 0.0f, gGyrOffY = 0.0f, gGyrOffZ = 0.0f;

// ── 힐과 피치 ────────────────────────────────────────────────────────────
//
// 힐은 배가 좌우로 얼마나 누웠는지, 피치는 뱃머리가 얼마나 들렸는지다.
// 보드가 어느 축으로 그걸 느끼는지는 보드를 어떻게 달았느냐에 달려 있다.
//
// 밖에서 재 보니 방위(HDG)가 맞으려면 보드를 세워서 달아야 했다. 그렇게
// 세우면 예전에 쓰던 roll = atan2(가속Y, 가속Z) 은 힐과 상관없는 회전을
// 재게 된다. 세운 자세에서는 Z 가 눕고 Y 가 서기 때문이다.
//
// 그래서 둘 다 **가속도 한 축**에서 바로 구한다. 기울기계가 쓰는 그 방법이다.
//
//     각도 = asin(그 축의 g / 중력 크기)
//
// 배가 평형이면 그 축은 수평이라 0 g 를 읽고, 기울수록 중력이 그 축으로 흘러
// 들어온다. 실제로 달아 보고 고른 축은 **힐 -Y, 피치 +Z** 다.
// 남은 X 가 위아래를 향하는 축이다. 힐에 마이너스가 붙은 건 아래 "부호" 대로
// 기울여 봤더니 좌우가 반대로 나왔기 때문이다.
//
// ── 부호 ─────────────────────────────────────────────────────────────────
//
// 해양 장비 관례를 따른다. NMEA 2000 의 자세 메시지(PGN 127257)가 정한 것과
// 같다.
//
//     힐    우현(starboard)으로 누우면 양수, 좌현(port)이면 음수
//     피치  뱃머리가 들리면 양수, 처박히면 음수
//
// 우리 프로토콜의 힐도 이 규칙이다 (PROTOCOL.md §2). 배에 달고 실제로 기울여
// 확인하고, 반대로 나오면 `heel -y` / `pitch -z` 로 뒤집는다.
//
// ★ 한계: asin 이라 ±90° 까지만 맞다. 90° 를 넘으면 배가 이미 넘어간 것이라
//   그 뒤의 정확한 각도는 볼 이유가 없다.
//
// 어느 축인지, 부호가 어느 쪽인지는 `heel` / `pitch` 명령으로 바꾸고 NVS 에
// 남긴다. 다는 방법이 또 바뀌어도 다시 굽지 않아도 된다.
static uint8_t gHeelAxis  = 1;      // 0 = X, 1 = Y, 2 = Z
static float   gHeelSign  = -1.0f;  // 실물에서 좌우가 반대로 나와 뒤집었다
static uint8_t gPitchAxis = 2;
static float   gPitchSign = 1.0f;   // -1 이면 앞뒤가 뒤집혀 있다는 뜻

// ── 방위를 만드는 두 축 ──────────────────────────────────────────────────
//
// 방위는 자력계의 **수평 두 축**으로 만든다. atan2(A, B) 다.
// 어느 둘이 수평인지는 보드를 어떻게 다느냐에 달려 있다. 위아래를 향하는
// 축을 빼고 남은 둘이 그것이다.
//
// 힐·피치와 마찬가지로 시리얼에서 바꾸고 NVS 에 남긴다. 케이스를 바꿔
// 달 때마다 다시 구울 이유가 없다 (`hdg` 명령).
//
// ★ 여기에 자기 편각은 안 들어 있다. 한국은 약 8도 서편이다.
//   `hdg off <도>` 로 그 보정과 보드 방향 어긋남을 한꺼번에 넣는다.
static uint8_t gHdgAxisA = 1;       // atan2 의 첫 인자 (기본 Y)
static uint8_t gHdgAxisB = 0;       // 두 번째 인자 (기본 X)
static float   gHdgSignA = 1.0f;
static float   gHdgSignB = 1.0f;
static float   gHdgOffsetDeg = 0.0f;

// "지금 이 자세가 평형" 이라고 알려주는 기준각. 배를 물에 띄우고 평형일 때
// `level` 을 치면 그때 각도를 0 으로 삼는다. NVS 에 저장되므로 재부팅해도,
// 다시 구워도 남는다.
static float gHeelOffsetDeg  = 0.0f;
static float gPitchOffsetDeg = 0.0f;

// 각도를 -180 ~ +180 안으로 접는다.
static float wrap180(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

// "+Y" 처럼 부호와 축 이름을 담는다. 부르는 쪽이 버퍼를 준다 —
// 한 printf 안에서 힐과 피치를 같이 찍는 곳이 있어서 공용 버퍼를 쓰면 겹친다.
struct AxisName {
    char text[4];
    AxisName(uint8_t axis, float sign) {
        text[0] = (sign < 0.0f) ? '-' : '+';
        text[1] = (axis == 0) ? 'X' : (axis == 1 ? 'Y' : 'Z');
        text[2] = '\0';
    }
};

// 기준각을 빼기 전의 날각도. `level` 이 이 값을 기준으로 삼는다.
static float rawTiltDeg(uint8_t axis, float sign) {
    const float a = (axis == 0) ? gAcc.x : (axis == 1 ? gAcc.y : gAcc.z);
    const float mag = sqrtf(gAcc.x * gAcc.x + gAcc.y * gAcc.y + gAcc.z * gAcc.z);
    if (mag < 0.2f) return 0.0f; // 자유낙하 수준이면 중력 방향을 알 수 없다
    float s = sign * a / mag;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return asinf(s) * 180.0f / (float)M_PI;
}

static float rawHeelDeg()  { return rawTiltDeg(gHeelAxis, gHeelSign); }
static float rawPitchDeg() { return rawTiltDeg(gPitchAxis, gPitchSign); }

static float currentHeelDeg()  { return wrap180(rawHeelDeg() - gHeelOffsetDeg); }
static float currentPitchDeg() { return wrap180(rawPitchDeg() - gPitchOffsetDeg); }

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
// 우리가 걸어 두기로 정한 GPS 움직임 종류 (NVS 의 gps_dyn). 255 = 정한 적 없음.
//
// 모듈에 실제로 걸린 값(gGpsDyModel)이 이것과 다르면 화면에 띄운다.
// 2026-08-30 세션 27 이 이 값을 정해 두지 않아 모듈 기본값 0(휴대)으로 돌아갔고,
// 29분 내내 속도가 0.00 kn 으로 찍혔다. 물 위에서는 화면 말고 볼 것이 없다.
static uint8_t gGpsDynWant = 255;

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
    gBoatId         = gPrefs.getUChar("boat", 0);       // 0 = 번호 없음
    // 힐 기준각의 키가 heel_off → heel_off2 로 바뀌었다. 옛 키에 남아 있는
    // 값은 roll 기준이라 지금 계산법에서는 뜻이 다르다. 그냥 안 읽는다.
    gHeelAxis       = gPrefs.getUChar("heel_axis", 1);  // 기본 힐 Y
    gHeelSign       = gPrefs.getChar("heel_sgn", -1) < 0 ? -1.0f : 1.0f;
    gHeelOffsetDeg  = gPrefs.getFloat("heel_off2", 0.0f);
    gPitchAxis      = gPrefs.getUChar("pitch_axis", 2); // 기본 피치 Z
    gPitchSign      = gPrefs.getChar("pitch_sgn", 1) < 0 ? -1.0f : 1.0f;
    gPitchOffsetDeg = gPrefs.getFloat("pitch_off", 0.0f);
    gHdgAxisA       = gPrefs.getUChar("hdg_a", 1);
    gHdgAxisB       = gPrefs.getUChar("hdg_b", 0);
    gHdgSignA       = gPrefs.getChar("hdg_sa", 1) < 0 ? -1.0f : 1.0f;
    gHdgSignB       = gPrefs.getChar("hdg_sb", 1) < 0 ? -1.0f : 1.0f;
    gHdgOffsetDeg   = gPrefs.getFloat("hdg_off", 0.0f);
    gGpsDynWant     = gPrefs.getUChar("gps_dyn", 255);
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
    //
    // ★ 저장 버튼 자리는 절대 건드리지 않는다.
    //   후보 B(GPIO2)는 2026-08-21 실측에서 탈락했고, 지금은 저장 버튼이
    //   달려 있다. 여기서 pinMode(2, INPUT) 을 하면 버튼의 내부 풀업이
    //   풀려 핀이 뜬 채로 남고, 그게 LOW 로 흘러 "2초 길게 눌림" 으로
    //   먹힌다. 실제로 그랬다 — `power 14` 를 쳤더니 기록이 시작됐다
    //   (2026-08-27).
    const int btn = rak::kAin1;
    int other = (pin == rak::kSensorPowerA) ? rak::kSensorPowerB : rak::kSensorPowerA;
    if (other != btn) pinMode(other, INPUT);

    if (pin == 0) {
        if (rak::kSensorPowerA != btn) pinMode(rak::kSensorPowerA, INPUT);
        if (rak::kSensorPowerB != btn) pinMode(rak::kSensorPowerB, INPUT);
        Serial.println("[PWR] 센서 전원 끔 (버튼 자리는 그대로 둡니다)");
        return;
    }
    if (pin == btn) {
        Serial.printf("[PWR] GPIO%d 은 저장 버튼 자리입니다. 안 건드립니다.\n", pin);
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
// 모듈에 실제로 걸려 있는 움직임 종류. gpscfg 로 되물어볼 때마다 갱신한다.
// 255 면 아직 안 물어봤다는 뜻이다. 기록 헤더에 이 값이 들어간다 —
// 나중에 "왜 이 세션만 저속이 뭉개졌지" 를 따질 때 이게 없으면 못 찾는다.
static uint8_t gGpsDyModel = 255;


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

    // 움직임 종류(dyModel)를 모듈에 물어본다.
    //
    // 두 가지를 한 번에 한다.
    //   1) 저장해 둔 값이 있으면 다르면 다시 건다 (255 면 저장한 적 없음)
    //   2) 실제로 걸려 있는 값을 기억한다 → 기록 파일 머리글에 들어간다
    //
    // 2번이 없으면 헤더에 255(모름)가 찍힌다. dyModel 하나 때문에 저속을
    // 통째로 날린 적이 있어서, 그 설정이 파일에 안 남으면 나중에 원인을
    // 못 찾는다.
    {
        const uint8_t want = gPrefs.getUChar("gps_dyn", 255);
        uint8_t  p[44];
        uint16_t len = 0;
        if (casicQuery(0x06, 0x07, p, sizeof(p), &len) && len >= 44) {
            gGpsDyModel = p[4];
            if (want <= 7 && p[4] != want) {
                const uint32_t mask = (1UL << 0);
                memcpy(p + 0, &mask, 4);
                p[4] = want;
                casicSend(0x06, 0x07, p, 44);
                delay(120);
                gGpsDyModel = want;
                Serial.printf("[GPS] 움직임 종류를 %u(%s) 로 다시 걸었습니다\n",
                              want, dyModelName(want));
            } else {
                Serial.printf("[GPS] 움직임 종류 %u (%s)\n",
                              gGpsDyModel, dyModelName(gGpsDyModel));
            }
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
//   ★ 여기 "저속이 0 으로 나오던 진짜 원인은 dyModel 이었다" 고 적어
//     뒀었다. **틀린 말이었다.** 설정이 4→0 으로 바뀐 것만 되물어보고
//     증상은 다시 안 재봤다. 2026-08-27 에 0(휴대)인 채로 밖에서 걸었더니
//     HDOP 1.7 인데도 속도가 계속 0 이었다. 손으로 휘둘러야 나왔다.
//     지금은 2(보행)로 바꿔 두고 다시 재는 중이다.
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
static float    gPvHAccM    = -1.0f; // 모듈이 밝힌 수평 위치 오차 (m)
static float    gPvCogAccDeg= -1.0f; // 모듈이 밝힌 침로 오차 (도). 얼면 커진다
// ── 속도 표식 (CASIC 문서 NAV-PV 의 velValid) ────────────────────────────
//
// 칩이 값을 줄 때마다 **이게 방금 잰 값인지, 옛날 값을 들고 있는 것인지**
// 같이 알려준다. 그냥 있고 없고가 아니다.
//
//   0  속도를 아예 모름
//   1  바깥에서 넣은 값
//   2  대충 어림잡은 값
//   3  마지막에 잰 값을 그대로 들고 있음   ← 지금 잰 게 아니다
//   4~8 방금 제대로 잰 값
//
// 전에는 `!= 0` 으로 봤다. 그러면 3 번(옛날 값)도 잰 값 취급을 받아서
// **없는 값을 있는 것처럼 적게 된다.** 배가 천천히 떠 있을 때가 정확히
// 그 상황이라, 우리가 제일 알고 싶은 구간에서 거짓말을 하게 된다.
static constexpr uint8_t kPvVelMeasured = 4;   // 이보다 작으면 잰 값이 아니다
static uint8_t  gPvVelFlag  = 0;
static bool     gPvVelValid = false;           // gPvVelFlag >= 4 일 때만 참
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
                            float sp, hd, ac, ha, ca;
                            memcpy(&ha, bbody + 40, 4); // 수평 위치 정확도 (m²)
                            memcpy(&sp, bbody + 64, 4);
                            memcpy(&hd, bbody + 68, 4);
                            memcpy(&ac, bbody + 72, 4);
                            memcpy(&ca, bbody + 76, 4); // 침로 정확도 (도²)
                            gPvHAccM    = (ha > 0.0f) ? sqrtf(ha) : -1.0f;
                            gPvCogAccDeg= (ca > 0.0f) ? sqrtf(ca) : -1.0f;
                            gPvVelFlag  = bbody[5];
                            gPvVelValid = (gPvVelFlag >= kPvVelMeasured);
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
/**
 * GPS 로 보드 시계를 맞춘다. 한 번만.
 *
 * 이 보드에는 시계 부품(RTC)이 없다. 전원을 켜면 1970년부터 센다. 그러면
 * SD 에 만든 파일의 날짜도 1970년으로 찍혀서, 카드를 뽑아 파인더에서 봐도
 * 언제 것인지 모른다.
 *
 * 위성을 잡으면 그때 맞춘다. **기록 중이 아니어도 맞춘다** — 파일은 기록을
 * 시작할 때 만들어지니, 그 전에 맞아 있어야 날짜가 제대로 박힌다.
 * 전원을 빼면 다시 잊는다. 켤 때마다 다시 맞춘다.
 */
static bool gClockSet = false;

static void clockFromGps() {
    if (gClockSet) return;
    if (!gGps.date.isValid() || !gGps.time.isValid()) return;
    if (gGps.date.year() < 2020) return;          // 아직 안 채워진 값

    struct tm tmv = {};
    tmv.tm_year = gGps.date.year() - 1900;
    tmv.tm_mon  = gGps.date.month() - 1;
    tmv.tm_mday = gGps.date.day();
    tmv.tm_hour = gGps.time.hour();
    tmv.tm_min  = gGps.time.minute();
    tmv.tm_sec  = gGps.time.second();
    // timegm 이 없는 판이 있어서 직접 셈한다. NMEA 시각은 UTC 다.
    //   1970-01-01 부터 그 해 1월 1일까지의 날 수 + 그 해의 날 수
    static const int mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    const int y = gGps.date.year();
    long days = (y - 1970) * 365L + (y - 1969) / 4;          // 윤년 보정
    days += mdays[tmv.tm_mon];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (leap && tmv.tm_mon > 1) days += 1;
    days += tmv.tm_mday - 1;
    const time_t utc = (time_t)(days * 86400L +
                                tmv.tm_hour * 3600L + tmv.tm_min * 60L + tmv.tm_sec);
    if (utc <= 0) return;

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    gClockSet = true;
    Serial.printf("[시계] GPS 로 맞췄습니다 — %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  gGps.date.year(), gGps.date.month(), gGps.date.day(),
                  gGps.time.hour(), gGps.time.minute(), gGps.time.second());
}

static void gpsUpdateFix() {
    clockFromGps();
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
        Serial.printf("  속도 표식      %u %s\n", gPvVelFlag,
                      gPvVelFlag >= kPvVelMeasured ? "(방금 잰 값)" :
                      gPvVelFlag == 3 ? "★ 옛날 값을 들고 있음" :
                      gPvVelFlag == 2 ? "★ 대충 어림잡은 값" : "★ 속도를 모름");
        if (gPvCogAccDeg >= 0) Serial.printf("  침로 오차      ±%.1f도\n", gPvCogAccDeg);
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

        // ── 지금은 4(선박)를 쓴다. 여기까지 오는 데 뒤집힌 이야기가 많다 ──
        //
        // 8/22  선박(4)에서 걸으면 0, 뛰면 8.76 kn. "선박이 원인" 으로 보고
        //       0(휴대)로 바꾸고 「해결」이라고 적었다. **그런데 고친 뒤
        //       걸어서 다시 재보지 않았다.**
        // 8/27  0(휴대)인 채로 밖에서 걸었더니 여전히 0. HDOP 1.7 로 신호는
        //       좋았다. 2(보행)로 바꾸니 561줄 전부 값이 나왔다.
        // 8/28  20초마다 선박↔보행을 번갈아 걸며 걸어 봤다.
        //       **둘 다 잘 나왔다.** 모드가 원인이 아니었다.
        //
        // 그 사이 펌웨어를 예닐곱 번 다시 구우며 모듈 전원이 여러 번 오르
        // 내렸다. 무엇이 되살렸는지는 **아직 모른다.**
        //
        // 4(선박)를 고른 이유는 물 위에서 속도를 재는 사람들이 그걸 쓰기
        // 때문이다. 윈드서핑용 ESP-GPS-Logger 는 평소 Sea(5, u-blox 번호)로
        // 두고 25 m/s 를 넘을 때만 Portable 로 바꾼다
        // [확인: github.com/RP6conrad/ESP-GPS-Logger 의 GPS_data.cpp].
        //
        // ★ 어느 모드가 옳은지 단정하지 않는다. 화면에 경고를 띄우지도
        //   않는다. 전에 "선박은 저속을 뭉갠다. 바꿀 것" 이라고 띄워 뒀는데
        //   그 판정 자체가 뒤집혔다. `ab` 명령으로 언제든 다시 재면 된다.
        gGpsDyModel = buf[4];   // 실제로 걸려 있는 값을 기억해 둔다
        Serial.printf("  움직임 종류   %u  %s\n", buf[4], dyModelName(buf[4]));
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
                // ★ 여닫지 않으면 안 적힌다.
                //
                // 여기만 begin/end 가 빠져 있었다. Preferences 는 닫힌 손잡이에
                // 써도 아무 말 없이 실패한다. 그래서 "적어 뒀습니다" 라고
                // 말해 놓고 실제로는 안 적혔다. 2026-08-28 에 4(선박)로 걸어
                // 뒀다고 적었는데, 8/30 세션 27·28 이 모듈 기본값 0(휴대)으로
                // 돌아가 29분치 속도를 통째로 0 으로 잃었다.
                gPrefs.begin("sail", false);
                gPrefs.putUChar("gps_dyn", model);
                gPrefs.end();
                gGpsDyModel = model;
                gGpsDynWant = model;   // 화면 경고의 기준도 같이 옮긴다
                // 말만 하지 않는다. 되읽어서 진짜 적혔는지 보고 말한다.
                gPrefs.begin("sail", true);
                const uint8_t back = gPrefs.getUChar("gps_dyn", 255);
                gPrefs.end();
                if (back == model) {
                    Serial.println("  보드에 적어 뒀습니다. 껐다 켜도 이 모드로 다시 겁니다.");
                } else {
                    Serial.printf("  ★ 보드에 못 적었습니다 (되읽으니 %u). 껐다 켜면 돌아갑니다.\n",
                                  back);
                }
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

    // ── 재는 범위 ────────────────────────────────────────────────────────
    //
    // 라이브러리 init() 이 ±2 g / ±250 °/s 로 잡아 둔다. 요트에는 좁다.
    // 딩기가 파도에 내리꽂히면 2 g 를 쉽게 넘고, 그러면 잘려서 기록된다.
    //
    // 넓혀도 손해가 없다. 로그 포맷이 정한 저장 단위가 원시 해상도보다
    // 굵기 때문이다.
    //
    //            원시 한 칸        우리가 저장하는 단위
    //   ±2 g      61 µg            1 mg      ← 이미 굵게 뭉개고 있다
    //   ±16 g    488 µg            1 mg      ← 그래도 저장 단위가 더 굵다
    //   ±250°/s  7.6 m°/s          31 m°/s
    //   ±1000°/s 30.5 m°/s         31 m°/s   ← 딱 맞는다
    //
    // 자이로를 ±2000 으로 더 넓히지 않는 이유: 포맷의 자이로 칸이
    // 1/32 °/s 단위 int16 이라 ±1024 °/s 까지밖에 못 담는다.
    gImu.setAccRange(MPU9250_ACC_RANGE_16G);
    gImu.setGyrRange(MPU9250_GYRO_RANGE_1000);

    gImu.enableGyrDLPF();
    gImu.setGyrDLPF(MPU9250_DLPF_6); // 가장 조용한 설정
    gImu.setSampleRateDivider(9);    // 1000/(1+9) = 100 Hz (FIFO 주기)

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

// 가속도·자이로만 읽는다. 100 Hz 로 부른다.
//
// 파도·펌핑·태킹 같은 동역학은 10 Hz 로는 뭉개진다. 실제 요트 계기가
// 50~100 Hz 로 돈다 (README 의 계기 비교표).
static void imuFast() {
    if (!gImuOk) return;
    gAcc = gImu.getGValues();
    gGyr = gImu.getGyrValues();
}

// ── 등간격 100 Hz — 칩 안의 FIFO 를 쓴다 ─────────────────────────────────
//
// 우리가 직접 100 Hz 로 읽으면 간격이 고르지 않다. 화면 한 장 그리는 데
// I2C 가 33 ms 묶이고 (실측), 그동안 못 읽는다. 몰아서 읽어 놓고 "10 ms
// 간격" 이라고 적으면 데이터에 거짓말을 넣는 것이다.
//
// 그래서 **칩이 스스로 재게 한다.** MPU-9250 은 자기 클럭으로 정해진 주기에
// 값을 떠서 512바이트 FIFO 에 쌓는다. 우리는 한가할 때 퍼 오기만 하면 된다.
// 언제 퍼 오든 값들 사이 간격은 칩이 만든 그대로 10 ms 다.
//
//   설정   SMPLRT_DIV = 9  →  1000 Hz / (1+9) = **정확히 100 Hz**
//   한 벌  가속 6바이트 + 자이로 6바이트 = 12바이트
//   FIFO   512바이트 = 42벌 = 420 ms 치. 화면이 33 ms 묶어도 넉넉하다
//
// 시각은 10 ms 씩 더해 나가되, 실제 시각과 200 ms 넘게 벌어지면 다시 맞춘다.
// 칩 클럭과 보드 클럭이 조금씩 다르기 때문이다.
static void logWriteImu(uint32_t nowMs); // 아래 "기록 (hlog)" 항목

static bool     gFifoOn      = false;
static uint32_t gImuTickMs   = 0;   // 다음 한 벌의 시각
static uint32_t gFifoOverrun = 0;   // FIFO 가 넘칠 뻔한 횟수. 0 이어야 정상
static uint32_t gFifoSets    = 0;   // 퍼 온 벌 수 (진단용)

static void imuFifoBegin() {
    if (!gImuOk) return;
    gImu.setSampleRateDivider(9);              // 1000/(1+9) = 100 Hz
    gImu.enableFifo(true);
    gImu.setFifoMode(MPU9250_CONTINUOUS);
    gImu.resetFifo();
    gImu.startFifo(MPU9250_FIFO_ACC_GYR);
    gFifoOn = true;
    gImuTickMs = millis();
    Serial.println("[IMU] FIFO 켜짐 — 칩이 100 Hz 로 떠서 쌓습니다 (등간격)");
}

// FIFO 를 퍼 온다. 20 ms 마다 부르면 보통 두 벌씩 나온다.
static void imuDrainFifo() {
    if (!gImuOk || !gFifoOn) return;
    int16_t sets = gImu.getNumberOfFifoDataSets();
    if (sets <= 0) return;

    const uint32_t nowMs = millis();

    // 42벌이 꽉 차면 오래된 것부터 덮어써서 벌 경계가 어긋난다.
    // 그 전에 비우고 시각을 다시 맞춘다. 조용히 넘기지 않고 센다.
    if (sets >= 38) {
        gImu.resetFifo();
        gImuTickMs = nowMs;
        ++gFifoOverrun;
        return;
    }

    // 제일 오래된 벌의 시각. 여기서 200 ms 넘게 벌어졌으면 다시 맞춘다.
    const uint32_t oldest = nowMs - (uint32_t)(sets - 1) * 10u;
    const int32_t  gap    = (int32_t)(oldest - gImuTickMs);
    if (gImuTickMs == 0 || gap > 200 || gap < -200) gImuTickMs = oldest;

    for (int16_t i = 0; i < sets; ++i) {
        gAcc = gImu.getGValuesFromFifo();       // 순서 중요: 가속 6바이트 먼저
        gGyr = gImu.getGyrValuesFromFifo();     // 그다음 자이로 6바이트
        if (hlog::recording()) logWriteImu(gImuTickMs);
        gImuTickMs += 10;
        ++gFifoSets;
    }
}

// 자력계까지 포함한 갱신. 10 Hz 로 부른다.
// 자력계(AK8963)는 100 Hz 로 못 따라오므로 여기서만 읽는다.
static void imuUpdate() {
    if (!gImuOk) return;
    if (!gFifoOn) imuFast();   // FIFO 가 켜져 있으면 가속·자이로는 거기서 온다
    if (gMagOk) gMag = gImu.getMagValues();
    // 라이브러리의 getRoll()/getPitch() 는 안 쓴다. 보드를 세워 달아서
    // 그 각도는 힐·피치와 다른 회전을 잰다. 아래 "힐과 피치" 항목 참고.
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
/** 자력계의 한 축 값을 축 번호로 꺼낸다. */
static float magAxis(uint8_t axis) {
    return axis == 0 ? gMag.x : axis == 1 ? gMag.y : gMag.z;
}

static float headingDeg() {
    if (!gMagOk) return -1.0f;
    const float a = magAxis(gHdgAxisA) * gHdgSignA;
    const float b = magAxis(gHdgAxisB) * gHdgSignB;
    float h = atan2f(a, b) * 180.0f / (float)M_PI + gHdgOffsetDeg;
    while (h < 0.0f)    h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    return h;
}

// ── 기울기를 보정한 방위 (INSLIB 에서 가져온 식) ─────────────────────────
//
// ★ 위의 headingDeg() 는 자력계 두 축을 그냥 atan2 한다. **배가 기울면 틀린다.**
//   자기장은 한국에서 아래로 53° 로 꽂힌다. 배가 누우면 그 아래 성분이 옆 축으로
//   새어 들어와서 방위를 밀어 버린다.
//
//   계산해 본 값이다 (복각 53°, 뱃머리 방향을 5° 씩 다 돌려본 최대 오차).
//
//       힐 10°  →  13.6° 틀림
//       힐 20°  →  29.1° 틀림
//       힐 30°  →  50.4° 틀림
//
//   딩기는 20~30° 로 눕는다. 그리고 우리가 재려는 leeway 는 3~8° 다.
//   **오차가 재려는 값보다 네 배에서 열 배 크다.** 그대로면 leeway 는 잡음이다.
//
// 식은 github.com/jnz/inslib 의 ahrs_mag_detilt 를 그대로 옮겼다 (MIT).
// 자력 벡터를 힐·피치만큼 되돌려 수평으로 눕힌 뒤 atan2 한다.
//
// ── 앞·오른쪽·아래를 어떻게 아나 ────────────────────────────────────────
//
// 지금 설정이 이미 두 축을 알려주고 있다. 방위를 atan2(a, b) 로 구하는데,
// 자기 방위의 정의가 atan2(-오른쪽, 앞) 이므로
//
//       앞     = magAxis(gHdgAxisB) * gHdgSignB
//       오른쪽 = magAxis(gHdgAxisA) * (-gHdgSignA)
//
// 남은 축이 아래고, 부호는 오른손 법칙으로 정해진다 (아래 = 앞 × 오른쪽).
//
// ★ 이 부호가 맞는지는 **손으로 기울여 봐야 안다.** `hdgtilt` 명령이 두 방위를
//   나란히 찍는다. 보드를 좌우로 기울일 때 보정한 쪽이 안 움직이면 맞는 것이고,
//   반대로 두 배로 흔들리면 아래 축 부호가 뒤집힌 것이다.
//
// ★ 힐·피치는 **보정 전 값(raw)** 을 쓴다. currentHeelDeg 는 사람이 잡아 둔
//   평형 기준을 뺀 값이라 "배가 평평한가" 를 말하지, "센서가 중력에 대해 얼마나
//   기울었나" 를 말하지 않는다. 자력계를 눕히려면 뒤쪽이 필요하다.
static uint8_t magDownAxis() {
    // 앞도 오른쪽도 아닌 나머지 축
    return (uint8_t)(3 - gHdgAxisA - gHdgAxisB);
}

// 오른손 좌표계가 되도록 아래 축의 부호를 정한다.
// (앞, 오른쪽, 아래) 가 오른손이려면 세 축 번호의 순서와 부호 곱이 맞아야 한다.
static float magDownSign() {
    const uint8_t f = gHdgAxisB, r = gHdgAxisA, d = magDownAxis();
    if (f == r) return 0.0f;                    // 설정이 잘못됐다
    // (0,1,2) 와 그 순환은 +, 나머지는 -
    const bool even = (f == 0 && r == 1) || (f == 1 && r == 2) || (f == 2 && r == 0);
    const float parity = even ? 1.0f : -1.0f;
    (void)d;
    return parity * gHdgSignB * (-gHdgSignA);
}

// 가속도계 값을 **자력계 좌표계로** 옮긴다.
//
// 한 칩인데 자력계만 따로 든 칩(AK8963)이라 축이 다르다. 라이브러리가 안 맞춰 준다.
//     자력 X = 가속 Y,   자력 Y = 가속 X,   자력 Z = -가속 Z
//     [확인: MPU-9250 데이터시트 Orientation of Axes + 세션 27 실측]
// 바꾸는 식이 제 짝이라 그대로 뒤집어 쓰면 된다.
static void accInMagFrame(float out[3]) {
    out[0] =  gAcc.y;
    out[1] =  gAcc.x;
    out[2] = -gAcc.z;
}

// 자이로도 같은 자리로. 가속·자이로는 같은 좌표계라 식이 같다.
static void gyrInMagFrame(float out[3]) {
    out[0] =  gGyr.y;
    out[1] =  gGyr.x;
    out[2] = -gGyr.z;
}

// 아래 축을 도는 각속도 = 뱃머리가 돌아가는 속도 (°/s).
static float yawRateDegS() {
    if (!gImuOk) return 0.0f;
    float g[3];
    gyrInMagFrame(g);
    return g[magDownAxis()] * magDownSign();
}

// 기울기를 보정한 방위. 자력계가 없거나 중력을 못 재면 -1.
//
// ★ 힐·피치 설정(gHeelAxis / gPitchAxis)을 **안 쓴다.** 그쪽은 아직 정리가 안 됐고
//   (보드를 세워 달아서 평형에서 raw 피치가 86° 로 나온다) 그걸 넣으면 보정이
//   엉뚱해진다. 대신 **가속도계로 중력 방향을 직접 재서** 쓴다.
//   그러면 보드를 어떻게 달았든 상관없다.
static float headingTiltDeg() {
    if (!gMagOk || !gImuOk) return -1.0f;

    const uint8_t dAx = magDownAxis();
    const float   dSg = magDownSign();
    if (dSg == 0.0f) return -1.0f;              // 앞뒤 축이 같게 설정됐다

    // 자력을 (앞, 오른쪽, 아래) 로 옮긴다
    const float mf =  magAxis(gHdgAxisB) * gHdgSignB;
    const float mr = -magAxis(gHdgAxisA) * gHdgSignA;
    const float md =  magAxis(dAx) * dSg;

    // 가속을 같은 자리로 옮긴다. 쉬고 있을 때 가속도계는 **위쪽**을 가리키므로
    // 부호를 뒤집어야 중력(아래) 방향이 된다.
    float a[3];
    accInMagFrame(a);
    const float pick[3] = { a[gHdgAxisB], a[gHdgAxisA], a[dAx] };
    const float gf = -pick[0] * gHdgSignB;
    const float gr =  pick[1] * gHdgSignA;      // (-1) x (-1)
    const float gd = -pick[2] * dSg;

    const float gn = sqrtf(gf * gf + gr * gr + gd * gd);
    if (gn < 0.2f) return -1.0f;                // 자유낙하 수준. 아래를 모른다

    // 중력에서 힐과 피치를 뽑는다 (Tait-Bryan ZYX)
    const float roll  = atan2f(gr, gd);
    const float pitch = atan2f(-gf, sqrtf(gr * gr + gd * gd));

    // INSLIB 의 ahrs_mag_detilt 를 그대로 옮긴 부분
    const float cr = cosf(roll),  sr = sinf(roll);
    const float ct = cosf(pitch), st = sinf(pitch);
    const float ty = cr * mr - sr * md;
    const float tz = sr * mr + cr * md;
    const float hx = ct * mf + st * tz;
    const float hy = ty;

    float h = atan2f(-hy, hx) * 180.0f / (float)M_PI + gHdgOffsetDeg;
    while (h < 0.0f)    h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
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
    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    Serial.printf(" | 힐 %+6.1f° (가속 %s)  피치 %+6.1f° (가속 %s)\n",
                  currentHeelDeg(), hAx.text, currentPitchDeg(), pAx.text);
    // 두 방위를 나란히 찍는다. 어느 쪽을 쓸지 정하기 전까지는 재기만 한다.
    // (속도의 도플러 대 위치차분과 같은 방식이다)
    if (gMagOk) {
        const float flat = headingDeg(), tilt = headingTiltDeg();
        Serial.printf("   방위 비교  평평 %5.1f°  |  기울기보정 %5.1f°  |  차이 %+5.1f°\n",
                      flat, tilt, wrap180(tilt - flat));
    }
}

// 지금 자세를 평형(힐 0°, 피치 0°)으로 삼는다. 배를 물에 띄우고 평형일 때 쓴다.
static void doLevel() {
    if (!gImuOk) {
        Serial.println("[IMU] 붙어 있지 않습니다.");
        return;
    }
    imuUpdate();
    gHeelOffsetDeg  = rawHeelDeg();
    gPitchOffsetDeg = rawPitchDeg();
    gPrefs.begin("sail", false);
    gPrefs.putFloat("heel_off2", gHeelOffsetDeg);
    gPrefs.putFloat("pitch_off", gPitchOffsetDeg);
    gPrefs.end();

    const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  지금 자세를 평형으로 삼았습니다.\n");
    Serial.printf("  힐   가속 %s  기준각 %+.1f°  →  지금 %+.1f°\n",
                  hAx.text, gHeelOffsetDeg, currentHeelDeg());
    Serial.printf("  피치 가속 %s  기준각 %+.1f°  →  지금 %+.1f°\n",
                  pAx.text, gPitchOffsetDeg, currentPitchDeg());
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
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        Serial.println("  마운트 실패.");
        Serial.println("");
        Serial.println("  ★ 진짜 이유는 바로 위의 [E] 로 시작하는 줄에 있습니다.");
        Serial.println("    라이브러리가 FatFs 오류 번호를 그대로 찍어 줍니다.");
        Serial.println("");
        Serial.println("    (13) There is no valid FAT volume");
        Serial.println("        → 카드는 읽히는데 FAT 가 아니다. 64GB 이상은 공장에서");
        Serial.println("          exFAT 로 나오고 우리 빌드는 exFAT 를 안 읽는다.");
        Serial.println("          맥에서 FAT32 로 다시 포맷하면 된다:");
        Serial.println("            diskutil list");
        Serial.println("            diskutil eraseDisk FAT32 SAIL MBRFormat /dev/diskN");
        Serial.println("");
        Serial.println("    (3) The physical drive cannot work / (1) hard error");
        Serial.println("        → SPI 가 안 통한다. 카드가 덜 꽂혔거나 모듈이");
        Serial.println("          IO 슬롯이 아닌 곳에 꽂혔다 (IO 슬롯 전용).");
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

// ── SD 쓰기 속도 실측 (sdbench) ──────────────────────────────────────────
//
// 계획(SDLOG.md §4)이 기대는 숫자를 짐작하지 않고 잰다. 알아야 할 것은 둘이다.
//
//   1) 평균 속도   — 초당 1.5 KB 를 감당하나
//   2) **한 번 쓸 때 제일 오래 걸린 시간** — 이게 진짜 문제다.
//      평균이 아무리 빨라도 가끔 200 ms 씩 멈추면 10 Hz notify 가 끊긴다.
//
// 그래서 실제 기록과 같은 모양으로 쓴다. 153바이트짜리 줄을 512바이트씩 모아
// 한 번에 내보내고, 5초치마다 flush 한다.
static void doSdBench(uint32_t rows) {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  SD 쓰기 실측 — %u줄 (10 Hz 로 %.0f초치)\n",
                  (unsigned)rows, rows / 10.0f);

    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        Serial.println("  마운트 실패 — 먼저 sd 로 확인하세요.");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    SD.mkdir("/SAIL");
    SD.remove("/SAIL/BENCH.CSV");

    File f = SD.open("/SAIL/BENCH.CSV", FILE_WRITE);
    if (!f) {
        Serial.println("  파일을 못 열었습니다.");
        SD.end();
        Serial.println("──────────────────────────────────────────");
        return;
    }

    // 실제 기록 줄과 길이를 맞춘 본보기 (SDLOG.md §3 의 예시 그대로 153바이트)
    static const char kSample[] =
        "1234500,1787492994100,1,37.5123456,126.9123456,5.53,5.61,315.0,11,1.4,"
        "0.05,-12.3,2.1,344,-0.034,0.012,-1.005,0.2,0.4,-0.1,3.0,-21.2,-16.7,"
        "68,3.91,0,A3F2\n";
    const size_t lineLen = strlen(kSample);

    char     buf[1024];
    size_t   used      = 0;
    uint32_t maxWrite  = 0, maxFlush = 0;
    uint32_t writes    = 0, flushes  = 0;
    uint64_t bytes     = 0;
    uint32_t sinceFlush = 0;

    // 얼마나 자주 얼마나 오래 멈추는지. 평균만 보면 원인을 못 찾는다.
    //   칸 경계 (ms):  1, 2, 5, 10, 20, 50, 100, 200, 그 위
    static const uint32_t kEdge[8] = {1, 2, 5, 10, 20, 50, 100, 200};
    uint32_t hist[9] = {0};

    // 제일 오래 걸린 여덟 번이 파일의 어느 자리에서 났나.
    //   일정한 간격이면 → FAT 갱신이나 클러스터 경계 (우리 탓)
    //   들쭉날쭉하면    → 카드가 속으로 정리하는 것 (카드 탓)
    uint32_t worstMs[8]  = {0};
    uint64_t worstAt[8]  = {0};

    const uint32_t t0 = millis();
    for (uint32_t i = 0; i < rows; ++i) {
        memcpy(buf + used, kSample, lineLen);
        used += lineLen;

        if (used >= 512) {
            const uint32_t a = millis();
            f.write((const uint8_t*)buf, used);
            const uint32_t dt = millis() - a;
            bytes += used;
            used = 0;
            ++writes;

            if (dt > maxWrite) maxWrite = dt;
            int b = 8;
            for (int k = 0; k < 8; ++k) { if (dt < kEdge[k]) { b = k; break; } }
            hist[b]++;

            // 제일 느린 여덟 개를 자리와 함께 남긴다
            if (dt > worstMs[7]) {
                int k = 7;
                while (k > 0 && worstMs[k - 1] < dt) {
                    worstMs[k] = worstMs[k - 1];
                    worstAt[k] = worstAt[k - 1];
                    --k;
                }
                worstMs[k] = dt;
                worstAt[k] = bytes;
            }

            // 5초치(50줄)마다 디스크에 못박는다. 전원이 끊겨도 여기까지는 남는다.
            if (++sinceFlush >= 50) {
                sinceFlush = 0;
                const uint32_t bb = millis();
                f.flush();
                const uint32_t df = millis() - bb;
                if (df > maxFlush) maxFlush = df;
                ++flushes;
            }
        }
        if ((i & 0xFF) == 0) feedWatchdog();
    }
    if (used > 0) { f.write((const uint8_t*)buf, used); bytes += used; }
    f.flush();
    const uint32_t elapsed = millis() - t0;
    const uint32_t size    = f.size();
    f.close();

    const uint64_t totalB = SD.totalBytes();
    const uint64_t usedB  = SD.usedBytes();
    SD.end();

    const float sec  = elapsed / 1000.0f;
    const float kbps = (bytes / 1024.0f) / (sec > 0 ? sec : 1);

    Serial.println("  ─────────────────────────────────────");
    Serial.printf("  쓴 양            %llu 바이트 (파일 %u)\n",
                  (unsigned long long)bytes, (unsigned)size);
    Serial.printf("  걸린 시간        %.2f 초\n", sec);
    Serial.printf("  평균 속도        %.0f KB/초\n", kbps);
    Serial.printf("  우리가 쓸 양     1.5 KB/초  →  여유 %.0f배\n", kbps / 1.5f);
    Serial.printf("  카드 전체 %llu MB  쓴 자리 %llu MB\n",
                  (unsigned long long)(totalB / 1048576ULL),
                  (unsigned long long)(usedB / 1048576ULL));
    Serial.println("  ─────────────────────────────────────");
    Serial.printf("  한 번 쓰기       %u번,  제일 오래 %u ms\n",
                  (unsigned)writes, (unsigned)maxWrite);
    Serial.printf("  flush            %u번,  제일 오래 %u ms\n",
                  (unsigned)flushes, (unsigned)maxFlush);

    Serial.println("  ─── 얼마나 오래 멈췄나 ───");
    static const char* kLabel[9] = {
        "     ~1 ms", "  1~2 ms", "  2~5 ms", " 5~10 ms", "10~20 ms",
        "20~50 ms", "50~100 ms", "100~200 ms", "200 ms 위"};
    for (int k = 0; k < 9; ++k) {
        if (!hist[k]) continue;
        Serial.printf("  %-10s %7u번  (%.3f%%)\n",
                      kLabel[k], (unsigned)hist[k], 100.0f * hist[k] / writes);
    }

    Serial.println("  ─── 제일 오래 멈춘 자리 ───");
    Serial.println("  일정한 간격이면 FAT 갱신·클러스터 경계, 들쭉날쭉하면 카드 사정이다.");
    uint64_t prev = 0;
    // 자리 순서로 다시 보여 준다 (간격을 눈으로 보려고)
    for (int a = 0; a < 8; ++a) {
        for (int b2 = a + 1; b2 < 8; ++b2) {
            if (worstAt[b2] && (!worstAt[a] || worstAt[b2] < worstAt[a])) {
                uint64_t ta = worstAt[a]; worstAt[a] = worstAt[b2]; worstAt[b2] = ta;
                uint32_t tm = worstMs[a]; worstMs[a] = worstMs[b2]; worstMs[b2] = tm;
            }
        }
    }
    for (int k = 0; k < 8; ++k) {
        if (!worstMs[k]) continue;
        Serial.printf("  %3u ms  파일 %8.2f MB 자리", (unsigned)worstMs[k],
                      worstAt[k] / 1048576.0);
        if (prev) Serial.printf("   (앞것과 %.2f MB 차이)", (worstAt[k] - prev) / 1048576.0);
        Serial.println();
        prev = worstAt[k];
    }

    Serial.println("  ─────────────────────────────────────");
    const uint32_t worst = maxWrite > maxFlush ? maxWrite : maxFlush;
    if (worst < 100) {
        Serial.printf("  제일 오래 멈춘 시간 %u ms — notify 주기 100 ms 안이다.\n",
                      (unsigned)worst);
    } else {
        Serial.printf("  ★ 제일 오래 멈춘 시간 %u ms — notify 주기 100 ms 를 넘는다.\n",
                      (unsigned)worst);
        Serial.println("    메인 루프에서 직접 쓰면 안 된다. 쓰기 작업을 따로 띄운다.");
    }
    Serial.println("──────────────────────────────────────────");
}


// ── 저장 버튼 ────────────────────────────────────────────────────────────
//
// 기능명세 「조작」의 "저장 버튼을 눌러 기록 시작/종료 — 선수가 명확히 통제".
//
// RAK19007 에는 리셋 버튼밖에 없다 [확인: 데이터시트 261번째 줄]. 그래서
// 2.54 mm 헤더에 직접 단다. **J11 헤더 1번(AIN1 = GPIO2)과 GND** 다.
//
// 쓸 수 있는 핀인지 짐작하지 않고 쟀다 (`pin` 명령, 2026-08-27).
//
//   AIN1 (GPIO2)   풀업 HIGH · 풀다운 LOW   비어 있다        ← 여기 단다
//   IO1  (GPIO21)  풀업 LOW  · 풀다운 LOW   GPS 가 잡고 있다
//   IO2  (GPIO14)  센서 전원 스위치         쓰면 센서가 다 꺼진다
//   BOOT (GPIO0)   풀업 HIGH · 풀다운 LOW   비어 있지만 함정이 있다
//
// **왜 IO1 이 안 되나.** IO1 과 IO2 는 J11 헤더와 센서 슬롯으로 같이 나가는
// 한 선이다. 슬롯 A 12번 핀이 IO1 인데 거기 GPS 가 꽂혀 있어서 LOW 로
// 잡고 있다. 센서 전원을 끄면 HIGH 로 올라오는 것으로 확인했다.
// AIN0/AIN1 은 슬롯으로 안 나가서 안 물린다.
//
// **왜 BOOT 가 아닌가.** GPIO0 은 전원을 넣는 순간 눌려 있으면 보드가
// 다운로드 모드로 들어가 안 켜진다. 배 위에서 그러면 곤란하다.
// GPIO2 는 ESP32-S3 의 strapping 핀이 아니라 그런 게 없다.
//
// 누르는 법
//   짧게(0.05~1초)  이벤트 표식 (마킹)
//   2초 이상        기록 시작 / 종료
//   5초 이상        보드 끄기 (깊은잠)
//
// 시작·종료를 길게로 둔 이유는 실수로 세션이 끊기면 안 되기 때문이다.
// 훈련 중에 자주 쓰는 건 마킹 쪽이다.
//
// ── 2초와 5초가 부딪히는 곳 ─────────────────────────────────────────────
//
// 5초를 누르면 도중에 2초를 반드시 밟는다. 그래서 두 경우를 갈라 놓았다.
//
//   기록 중이면    2초에 그 자리에서 멈춘다. 어차피 끄면서 멈출 것이라
//                  순서만 앞당긴 셈이다. **여기는 예전 코드 그대로다.**
//                  덤으로 5초에 도달했을 때는 이미 다 닫혀 있어서 빨리 꺼진다.
//
//   기록 중 아니면 2초에 시작하지 **않는다.** 화면에만 "놓으면 기록시작"
//                  이라고 띄우고, 손을 뗄 때 시작한다. 5초를 넘기면 그 예약을
//                  버리고 끈다. 안 그러면 끄려고 누를 때마다 쓸모없는 파일이
//                  하나씩 열렸다 닫힌다.
//
// 시작 문턱을 2.5초로 미루는 방법도 있었는데 안 했다. 5초보다 작아서 어차피
// 밟고 지나간다. 문제는 안 풀리고, "2초쯤" 누르고 2.4초에 떼는 사람만 손해다.
static constexpr int      kButtonPin   = rak::kAin1;   // GPIO2, J11 1번
static constexpr uint32_t kBtnLongMs   = 2000;         // 기록 시작 / 종료
static constexpr uint32_t kBtnOffMs    = 5000;         // 끄기
static constexpr uint32_t kBtnHintMs   = 800;          // 뗀 뒤 결과를 보여주는 시간
static constexpr uint32_t kBtnStuckMs  = 10000;        // 이만큼 안 떨어지면 안 끈다

// ── 채터링(접점 튐) 걷어내기 ─────────────────────────────────────────────
//
// 기계 접점은 붙을 때와 떨어질 때 수 ms 동안 여러 번 튄다. 그대로 읽으면
// 한 번 눌렀는데 마킹이 서너 개 찍힌다.
//
// **값이 바뀐 순간부터 이만큼 안 흔들려야** 진짜 바뀐 것으로 본다.
// 예전에는 뗄 때만 "50ms 보다 짧으면 버린다" 로 걸렀는데, 그건 누를 때의
// 튐을 못 막는다. 누르는 순간 튀면 그 자리에서 눌린 것으로 쳐 버렸다.
static constexpr uint32_t kBtnStableMs = 30;

static bool     gBtnRaw      = false;  // 방금 읽은 값 (튐 포함)
static uint32_t gBtnRawAt    = 0;      // 그 값이 된 시각
static bool     gBtnDown     = false;  // 튐을 걷어낸 값
static uint32_t gBtnDownAt   = 0;
static bool     gBtnLongDone = false;  // 길게가 이미 먹었나 (떼면서 또 먹지 않게)

// 켜자마자 손을 안 뗐으면 뗄 때까지 버튼을 아예 안 본다.
//
// ★ 이게 없으면 5초 눌러 켠 보드가 그대로 다시 꺼진다. 부팅이 끝나도 손은
//   여전히 눌린 채라 buttonPoll 이 처음부터 세기 시작하고, 2초에 기록이 켜지고
//   5초에 도로 꺼진다. **켜지지 않는 보드가 된다.**
static bool     gBtnIgnoreUntilUp = false;

// 뗄 때 기록을 시작할까. 2초에 예약해 두고 뗄 때 실행한다.
static bool     gBtnStartOnUp = false;

// 화면을 버튼이 쓰고 있는 동안은 평소 계기 화면을 안 그린다.
// 안 그러면 4 Hz 로 막대를 지워 버린다.
static bool     gBtnOwnsScreen = false;
static uint32_t gBtnScreenTill = 0;   // 뗀 뒤 결과 글자를 언제까지 보여주나
static uint32_t gBtnDrawnAt    = 0;   // 막대를 마지막으로 그린 시각

// ── 잠자기 기록 (RTC 메모리) ─────────────────────────────────────────────
//
// 깊은잠은 램을 다 날리지만 RTC 영역은 남긴다. 여기에 적어 두면 깨어난 뒤에
// "정말 잤는지" 를 물어볼 수 있다.
//
// ★ 이게 왜 필요한가. 2026-09-07, 하루 조금 넘게 재워 뒀더니 배터리가 바닥까지
//   닳았다. 자는 게 10 µA 라면 몇 달이 가야 한다. 그러니 뭔가 안 잔 것이다.
//   그런데 **잠든 보드는 아무 말도 못 한다.** 깨어나면 램이 비어 있어서 무슨
//   일이 있었는지 알 방법이 없었다. 그래서 잠들기 직전에 여기 적어 둔다.
//
//   깬 횟수    자꾸 깼다 잤다 했으면 여기 쌓인다
//   잠든 시각  RTC 시계는 자는 동안에도 돈다. 뺄셈하면 진짜 잔 시간이 나온다
//   그때 전압  깨어나서 다시 재면 시간당 몇 볼트가 빠졌는지 나온다
//
// 시각은 gettimeofday 로 읽는다. millis() 는 깨어날 때 0 으로 돌아가서 못 쓴다.
// gettimeofday 는 RTC 시계 위에 얹혀 있어서 자는 동안에도 계속 간다
// [확인: `nm libnewlib.a` 에 `U esp_rtc_get_time_us` — RTC 시계를 참조한다].
// 시각을 맞춘 적이 없어도 상관없다. 두 값의 **차이**만 쓴다.
static uint64_t nowUs() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
static RTC_DATA_ATTR uint32_t gRtcMagic;
static RTC_DATA_ATTR uint32_t gRtcSleeps;      // 잠든 횟수
static RTC_DATA_ATTR uint32_t gRtcFalse;       // 5초 못 채우고 도로 잔 횟수
static RTC_DATA_ATTR uint32_t gRtcFull;        // 5초 채워 켜진 횟수
static RTC_DATA_ATTR uint64_t gRtcSleepUs;     // 마지막으로 잠든 RTC 시각
static RTC_DATA_ATTR uint32_t gRtcSleepMv;     // 그때 배터리 mV
static RTC_DATA_ATTR uint32_t gRtcWakeMv;      // 마지막으로 깼을 때 배터리 mV
static RTC_DATA_ATTR uint64_t gRtcSleptUs;     // 마지막으로 잔 시간
static RTC_DATA_ATTR uint32_t gRtcGpsBytes;    // 깨자마자 GPS 가 뱉은 바이트 수
static RTC_DATA_ATTR uint32_t gRtcTestSec;     // 시험용 타이머 (0 이면 안 씀)
static bool gWokeFromSleep = false;            // 이번 부팅이 깊은잠에서 깬 것인가

// 켤 때 배터리 ADC 가 몇 번째부터 제 값을 내는지 담아 둔다. `battboot` 가 꺼낸다.
static constexpr int kBattBootN = 20;
static uint32_t gBattBoot[kBattBootN];
static constexpr uint32_t kRtcMagic = 0x5A5AC0DE;

// ── 잠자기 전류 재기 (drain) ─────────────────────────────────────────────
//
// 자면서 스스로 깨서 배터리 전압만 적고 다시 잔다.
//
// ★ 왜 이게 필요한가. 끝점 두 개만 재면 **표면 전압과 진짜 방전을 못 가른다.**
//   충전기를 뗀 배터리는 표면에 전압이 떠 있고 그게 수십 분에 걸쳐 저절로
//   빠진다. 방전이 아닌데 방전처럼 보이고, 크기가 30 mV 라 우리 신호와 같다.
//
//   실제로 그것 때문에 같은 보드 같은 코드가 세 번 다 다르게 나왔다.
//     10.47시간  32 mV → 3.06 mV/h    2.46시간  31 mV → 12.60 mV/h
//   낙차는 셋 다 30 mV 언저리인데 시간만 달랐던 것이다.
//
//   점이 많으면 갈린다. 표면 전압은 처음에 꺾이다 평평해지는 곡선이고,
//   진짜 방전은 처음부터 끝까지 같은 기울기의 직선이다.
//
// 시각표를 앞은 촘촘히, 뒤는 성기게 잡았다. 곡선이 꺾이는 데가 앞이라서다.
// 같은 간격으로 스무 번 깨는 것보다 이쪽이 적게 깨고 더 잘 보인다.
//
// ★ 재는 것 자체가 전기를 먹는다. 한 번 깰 때 1.6초쯤 걸리고 15번이면 24초다.
//   13시간에 걸쳐 평균 20 µA 쯤 된다. 그래서 깨어 있던 시간을 합쳐 두고
//   drainstat 이 같이 보여준다. 빼고 봐야 한다.
static const uint16_t kDrainAt[] = { 5, 10, 20, 30, 45, 60, 90, 120,
                                     180, 240, 300, 420, 540, 660, 780 };
static constexpr int kDrainMax = sizeof(kDrainAt) / sizeof(kDrainAt[0]);

static RTC_DATA_ATTR uint8_t  gDrainOn;
static RTC_DATA_ATTR uint8_t  gDrainN;
static RTC_DATA_ATTR uint16_t gDrainMv[kDrainMax];       // 1.35초 기다린 뒤 값
static RTC_DATA_ATTR uint16_t gDrainEarly[kDrainMax];    // 0.15초 만에 잰 값
static RTC_DATA_ATTR uint32_t gDrainSec[kDrainMax];
static RTC_DATA_ATTR uint64_t gDrainT0;
static RTC_DATA_ATTR uint32_t gDrainAwakeMs;
// 시각표의 단위. 60 이면 분(진짜), 1 이면 초(`drain fast` — 코드 길을 확인하는 용).
static RTC_DATA_ATTR uint16_t gDrainScale;

// ── 눈금을 맞추는 두 점 ──────────────────────────────────────────────────
//
// ★ 깨자마자 잰 값은 진짜보다 낮게 나온다. 원인은 아직 모른다.
//   [확인: 2026-09-09 probe] 눈금 1420 이 3.3초 내내 평평하다가, setup 이 다 돌고
//   나면 2647 이 된다. 천천히 차는 게 아니라 딱 두 값 사이를 오간다.
//   세 가지를 짚어봤는데 셋 다 아니었다.
//     핀 떼어놓기(rtc_gpio_isolate) · 디지털 입력 버퍼 끄기 · 3V3_S 켜기
//
// **그런데 비율은 아주 안정적이다.**
//     1417/2644 = 0.5359   1421/2645 = 0.5372   1420/2647 = 0.5365
//   세 번이 0.25% 안에 든다.
//
// 그래서 원인을 모르는 채로도 쓸 수 있게 만들었다. 재는 값은 **모양**을 주고,
// 시작과 끝에 제대로 잰 값 두 개가 **눈금**을 준다. 둘을 맞추면 진짜 mV 가 나온다.
// 두 앵커로 되짚은 비율이 0.536 근처면 이 방법이 맞는 것이고, 아니면 못 쓴다.
static RTC_DATA_ATTR uint16_t gDrainAnchor0;   // 시작할 때 제대로 잰 배터리 mV
static RTC_DATA_ATTR uint16_t gDrainAnchor1;   // 끝나고 제대로 잰 배터리 mV
static RTC_DATA_ATTR uint8_t  gDrainWantAnchor1;

// `probe <초>` — 깬 뒤 배터리 값이 언제 제자리로 오는지 재는 용.
// 깨자마자 0.3초 간격으로 열두 번 찍고 정상 부팅한다.
static RTC_DATA_ATTR uint8_t  gProbeOn;
static RTC_DATA_ATTR uint16_t gProbeMv[12];
static RTC_DATA_ATTR uint16_t gProbeRaw[12];   // 눈금 그대로. 변환을 안 거친 값

static void armButtonWake();                       // 아래 "끄기 (깊은잠)" 항목
static void drainGate();
static void doSleepStat();                         // 아래 "잠자기 기록" 항목
static void sleepLogToCard();
static bool logStartNow(uint32_t prevSession = 0);  // 아래 "기록 (hlog)" 항목
static void feedWatchdog();                        // 아래 "워치독" 항목

static void buttonBegin() {
    pinMode(kButtonPin, INPUT_PULLUP);
    delay(20);
    gBtnRaw = (digitalRead(kButtonPin) == LOW);
    gBtnRawAt = millis();
    gBtnDown = gBtnRaw;
    if (gBtnRaw) {
        // 5초 눌러 켠 직후가 거의 다다. 아직 손을 안 뗀 것이다.
        // 뗄 때까지 버튼을 안 본다 — 안 그러면 5초에 도로 꺼진다.
        gBtnIgnoreUntilUp = true;
        Serial.printf("[BTN] GPIO%d 이 LOW 입니다 — 손을 뗄 때까지 버튼을 안 봅니다.\n",
                      kButtonPin);
    } else {
        Serial.printf("[BTN] 저장 버튼 GPIO%d (J11 헤더 1번 AIN1 - GND). "
                      "짧게=마킹, 2초=시작/종료, 5초=끄기\n", kButtonPin);
    }
}

static void goToSleep(uint32_t testWakeSec = 0);   // 아래 "끄기 (깊은잠)" 항목

// 누른 시간을 막대 퍼센트로. 5초를 가득 찬 것으로 본다.
static int btnPct(uint32_t heldMs) {
    if (heldMs >= kBtnOffMs) return 100;
    return (int)(heldMs * 100 / kBtnOffMs);
}

static void buttonPoll(uint32_t nowMs) {
    // ── 튐을 먼저 걷어낸다 ──
    // 값이 바뀌면 그 시각을 적어 두고, 30ms 동안 그대로여야 받아들인다.
    const bool raw = (digitalRead(kButtonPin) == LOW);
    if (raw != gBtnRaw) { gBtnRaw = raw; gBtnRawAt = nowMs; }
    const bool settled = (nowMs - gBtnRawAt >= kBtnStableMs);

    // 켤 때 눌린 손을 아직 안 뗐으면 아무것도 안 한다.
    if (gBtnIgnoreUntilUp) {
        if (settled && !gBtnRaw) {
            gBtnIgnoreUntilUp = false;
            Serial.println("[BTN] 손을 뗐습니다 — 이제부터 버튼을 봅니다");
        }
        return;
    }

    // 뗀 뒤 결과 글자를 보여주는 시간이 끝났으면 화면을 돌려준다.
    if (gBtnOwnsScreen && !gBtnDown && nowMs >= gBtnScreenTill) {
        gBtnOwnsScreen = false;
    }

    if (settled && gBtnRaw && !gBtnDown) {
        gBtnDown = true;
        // 눌리기 시작한 시각은 **처음 바뀐 그 순간**으로 잡는다. 30ms 뒤로
        // 잡으면 "2초 길게" 가 매번 30ms 씩 늦어진다.
        gBtnDownAt = gBtnRawAt;
        gBtnLongDone = false;
        gBtnStartOnUp = false;
        return;
    }

    // 누르고 있는 동안 2초가 지나면 그 자리에서 먹는다.
    // 떼야 반응하면 "먹었나?" 를 알 수가 없다. 지금은 화면과 LED 로 바로 알려준다.
    if (gBtnDown && !gBtnLongDone && nowMs - gBtnDownAt >= kBtnLongMs) {
        gBtnLongDone = true;
        if (hlog::recording()) {
            // ★ stop() 은 다 쓸 때까지 최대 15초 루프를 붙잡는다 (hlog.cpp).
            //   그 동안 막대가 얼어붙는다. 그래서 **부르기 전에** 화면을 먼저
            //   그려서, 멈춘 그 한 장이 무슨 일이 벌어지는지 말하게 한다.
            gBtnOwnsScreen = true;
            sail::displayHoldBar(btnPct(nowMs - gBtnDownAt), "기록 종료", "저장 중");
            Serial.println("[BTN] 길게 — 기록 종료");
            hlog::stop();
        } else {
            // 아직 시작하지 않는다. 5초를 넘기면 끄기로 갈 사람일 수 있다.
            gBtnStartOnUp = true;
            Serial.println("[BTN] 길게 — 놓으면 기록 시작 (더 누르면 끄기)");
        }
        return;
    }

    // 누르고 있는 동안 화면에 막대를 그린다. 2초부터다.
    //
    // 2초 전에는 안 그린다. 마킹하려고 톡 누를 때마다 계기 화면이 번쩍이면
    // 안 된다. 마킹은 경고할 것도 없다 — 이미 벌어졌고 되돌릴 것도 없다.
    if (gBtnDown && nowMs - gBtnDownAt >= kBtnLongMs) {
        const uint32_t held = nowMs - gBtnDownAt;

        if (held >= kBtnOffMs) {
            gBtnStartOnUp = false;      // 예약해 둔 기록 시작을 버린다
            gBtnOwnsScreen = true;
            sail::displayHoldBar(100, "끄는 중", nullptr);
            goToSleep();                // 여기서 안 돌아오는 게 정상이다
            // 버튼이 안 떨어져서 못 끄고 돌아온 경우다.
            gBtnOwnsScreen = false;
            gBtnLongDone = true;
            gBtnIgnoreUntilUp = true;
            return;
        }

        if (nowMs - gBtnDrawnAt >= 100) {   // 10 Hz
            gBtnDrawnAt = nowMs;
            gBtnOwnsScreen = true;
            sail::displayHoldBar(btnPct(held), "누르면 꺼짐",
                                 gBtnStartOnUp ? "놓으면 기록시작" : "기록 멈춤");
        }
        return;
    }

    if (settled && !gBtnRaw && gBtnDown) {
        const uint32_t held = gBtnRawAt - gBtnDownAt;
        gBtnDown = false;

        if (gBtnStartOnUp) {
            gBtnStartOnUp = false;
            Serial.printf("[BTN] 놓음(%ums) — 기록 시작\n", (unsigned)held);
            logStartNow();
            gBtnOwnsScreen = true;
            gBtnScreenTill = nowMs + kBtnHintMs;
            sail::displayNotice("기록 시작", nullptr);
            return;
        }
        if (gBtnLongDone) {                       // 2초에 이미 멈췄다
            gBtnOwnsScreen = true;
            gBtnScreenTill = nowMs + kBtnHintMs;
            sail::displayNotice("기록 종료", nullptr);
            return;
        }
        if (hlog::recording()) {
            Serial.printf("[BTN] 짧게(%ums) — 마킹\n", (unsigned)held);
            hlog::mark();
        } else {
            Serial.println("[BTN] 짧게 — 기록 중이 아닙니다 (2초 누르면 시작)");
        }
    }
}

// 잠자기 기록을 SD 카드에도 남긴다.
//
// ★ RTC 메모리는 깊은잠은 견디지만 리셋은 못 견딘다. 실제로 시리얼을 잘못 열어
//   보드가 리셋되는 바람에 애써 잰 값을 통째로 날렸다 (2026-09-08).
//   카드에 적어 두면 그런 일과 무관하게 남는다. 배 위에서 하룻밤 재워 두고
//   나중에 카드를 뽑아 읽을 수도 있다.
static void sleepLogToCard() {
    if (gRtcMagic != kRtcMagic || gRtcSleptUs == 0) return;
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) return;
    File f = SD.open("/SLEEP.TXT", FILE_APPEND);
    if (!f) return;
    const double sec = (double)gRtcSleptUs / 1e6;
    const double drop = (double)gRtcSleepMv - (double)gRtcWakeMv;
    f.printf("잔시간 %.1fs  %umV->%umV  낙차 %.1f mV/h  GPS바이트 %u  "
             "잠든횟수 %u  헛깸 %u  켜짐 %u\n",
             sec, (unsigned)gRtcSleepMv, (unsigned)gRtcWakeMv,
             sec > 30.0 ? drop * 3600.0 / sec : 0.0,
             (unsigned)gRtcGpsBytes, (unsigned)gRtcSleeps,
             (unsigned)gRtcFalse, (unsigned)gRtcFull);
    f.close();
}

// `drain` — 잠자기 전류 재기를 시작한다.
static void doDrainStart(bool fast) {
    gDrainScale = fast ? 1 : 60;
    gDrainOn = 1;
    gDrainN  = 0;
    gDrainAwakeMs = 0;
    for (int i = 0; i < kDrainMax; i++) { gDrainMv[i] = 0; gDrainEarly[i] = 0; gDrainSec[i] = 0; }
    gDrainT0 = nowUs();
    gDrainAnchor1 = 0;
    gDrainWantAnchor1 = 1;
    { uint32_t mv = 0; readBatteryVolts(&mv);
      gDrainAnchor0 = (uint16_t)(mv / rak::kBattDivider); }
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  잠자기 전류 재기 — %d번 찍고 %u%s 뒤에 끝납니다\n",
                  kDrainMax, (unsigned)kDrainAt[kDrainMax - 1],
                  fast ? "초 (빠른 확인용)" : "분");
    Serial.println("  ★ USB 를 뽑고 30분쯤 쉬게 한 뒤에 시작해야 합니다.");
    Serial.println("    갓 충전한 배터리는 표면 전압이 떠 있어 방전처럼 보입니다.");
    Serial.println("  끝나면 저절로 켜집니다. 그 전에 보려면 버튼 5초.");
    Serial.println("  결과는 drainstat.");
    Serial.println("──────────────────────────────────────────");
    Serial.flush();
    goToSleep((uint32_t)kDrainAt[0] * gDrainScale);
}

// `drainstat` — 찍어 둔 표를 뱉는다.
static void doDrainStat() {
    Serial.println("──────────────────────────────────────────");
    if (gDrainN == 0) {
        Serial.println("  아직 찍은 게 없습니다 (drain 으로 시작)");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    Serial.printf("  %u/%d 점%s\n", (unsigned)gDrainN, kDrainMax,
                  gDrainOn ? "  (아직 도는 중)" : "  (끝)");
    Serial.printf("  깨어 있던 시간 합 %.1f초 — 이만큼은 재느라 쓴 것이라 빼고 봐야 합니다\n",
                  gDrainAwakeMs / 1000.0f);
    Serial.printf("  눈금 맞추는 두 점   시작 %u mV   끝 %u mV%s\n",
                  (unsigned)gDrainAnchor0, (unsigned)gDrainAnchor1,
                  gDrainAnchor1 ? "" : "  (아직 없음)");
    if (gDrainAnchor1 && gDrainN >= 2) {
        const float k0 = (float)gDrainMv[0] / rak::kBattDivider / gDrainAnchor0;
        const float k1 = (float)gDrainMv[gDrainN-1] / rak::kBattDivider / gDrainAnchor1;
        Serial.printf("  되짚은 비율        처음 %.4f   끝 %.4f", k0, k1);
        Serial.println((k0 > 0.50f && k0 < 0.57f && k1 > 0.50f && k1 < 0.57f)
            ? "   → 0.536 근처. 쓸 수 있다" : "   ★ 어긋난다. 이 표는 못 쓴다");
    }
    Serial.println("  ──────────────────────────────────────");
    Serial.println("     지난 시간      빨리 잰 값   기다린 값   처음 대비");
    const int32_t base = gDrainMv[0];
    for (int i = 0; i < gDrainN; i++) {
        const uint32_t sec = gDrainSec[i];
        Serial.printf("  %3u분 %02us     %5u mV     %5u mV   %+5d mV\n",
                      (unsigned)(sec / 60), (unsigned)(sec % 60),
                      (unsigned)(gDrainEarly[i] / rak::kBattDivider),
                      (unsigned)(gDrainMv[i]    / rak::kBattDivider),
                      (int)((gDrainMv[i] - base) / rak::kBattDivider));
    }
    Serial.println("  ──────────────────────────────────────");
    Serial.println("  ※ 빨리 잰 값과 기다린 값이 같으면 다음부터 안 기다려도 됩니다.");
    Serial.println("  ※ 앞쪽이 빠르게 꺾이면 그건 표면 전압입니다. 빼고 뒤쪽에 직선을 맞추세요.");
    Serial.println("──────────────────────────────────────────");
}

// `sleepstat` — 지난번에 정말 잤나.
//
// 자는 보드는 아무 말도 못 한다. 그래서 잠들기 직전에 RTC 메모리에 적어 둔 것을
// 깨어난 뒤에 읽는다. 여기서 갈리는 것은 두 가지다.
//
//   헛깬 횟수가 많다  → 자다 깨다를 반복한 것이다. 버튼 핀이 뜨고 있다
//   시간당 낙차가 크다 → 자는 동안 뭔가가 켜져 있다
//
// 낙차는 배터리 용량을 몰라도 쓸 수 있다. 리튬폴리머는 3.7~3.9 V 구간이
// 제일 평평해서, 거기서 시간당 10 mV 넘게 빠지면 µA 급이 아니다.
static void doSleepStat() {
    Serial.println("──────────────────────────────────────────");
    if (gRtcMagic != kRtcMagic) {
        Serial.println("  아직 한 번도 안 잤습니다 (off 명령이나 버튼 5초)");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    Serial.printf("  잠든 횟수         %u\n", (unsigned)gRtcSleeps);
    Serial.printf("  헛깸(5초 못 채움) %u   ← 크면 버튼 핀이 뜨는 것이다\n",
                  (unsigned)gRtcFalse);
    Serial.printf("  5초 채워 켜짐     %u\n", (unsigned)gRtcFull);

    if (gRtcSleptUs == 0) {
        Serial.println("  아직 깨어난 적이 없습니다.");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    const double sec = (double)gRtcSleptUs / 1e6;
    Serial.printf("  마지막으로 잔 시간 %.1f 초 (%.2f 시간)\n", sec, sec / 3600.0);
    Serial.printf("  잘 때 %u mV  →  깰 때 %u mV\n",
                  (unsigned)gRtcSleepMv, (unsigned)gRtcWakeMv);
    Serial.printf("  깨자마자 GPS 가 뱉은 바이트  %u\n", (unsigned)gRtcGpsBytes);
    Serial.println(gRtcGpsBytes > 0
        ? "  ★ 0 이 아닙니다 — 자는 동안 3V3_S 가 안 꺼졌습니다. GPS 가 계속 돌았습니다."
        : "  0 입니다 — 3V3_S 는 제대로 꺼져 있었습니다.");

    const double dropMv = (double)gRtcSleepMv - (double)gRtcWakeMv;
    if (sec > 30.0) {
        const double perHour = dropMv * 3600.0 / sec;
        Serial.printf("  시간당 낙차       %.1f mV/h\n", perHour);
        Serial.println(perHour > 10.0
            ? "  ★ 10 mV/h 를 넘습니다 — 자는 동안 뭔가가 켜져 있습니다."
            : "  낙차가 작습니다 — 자는 쪽은 괜찮아 보입니다.");
    } else {
        Serial.println("  ※ 30초 넘게 재워야 낙차가 뜻이 있습니다.");
    }
    Serial.println("  ※ ADC 소스 임피던스가 2.5 MΩ 라 mV 단위는 흔들립니다.");
    Serial.println("  ※ USB 를 꽂은 채로 재면 충전 때문에 값이 무의미합니다.");
    Serial.println("──────────────────────────────────────────");
}

// `hdgtilt` — 기울기 보정이 제대로 되는지 손으로 기울여 확인한다.
//
// 5초 동안 두 방위를 계속 찍는다. 보드를 좌우로 기울이면서 본다.
//
//   보정한 쪽이 거의 안 움직인다        → 맞다
//   보정한 쪽이 오히려 두 배로 흔들린다  → 아래 축 부호가 뒤집혔다
//   둘이 똑같이 움직인다                → 보정이 안 들어가고 있다
//
// 뱃머리 방향은 그대로 두고 **기울이기만** 해야 한다. 돌리면서 기울이면
// 무엇 때문에 바뀐 건지 못 가른다.
static void doHeadingTilt() {
    if (!gMagOk) { Serial.println("[HDG] 자력계가 없습니다."); return; }
    const uint8_t d = magDownAxis();
    const char* nm[3] = {"X", "Y", "Z"};
    Serial.println("──────────────────────────────────────────");
    Serial.println("  기울기 보정 방위 확인 — 5초. 뱃머리는 두고 좌우로 기울여 보세요.");
    Serial.printf("  자력계 축 배정   앞 %s%s   오른쪽 %s%s   아래 %s%s\n",
                  gHdgSignB < 0 ? "-" : "+", nm[gHdgAxisB],
                  gHdgSignA > 0 ? "-" : "+", nm[gHdgAxisA],
                  magDownSign() < 0 ? "-" : "+", nm[d]);
    Serial.println("  ※ 아래 축 부호는 오른손 법칙으로 정한 것이라 확인이 필요합니다.");
    Serial.println("  ──────────────────────────────────────");
    Serial.println("      힐     피치     평평     보정     차이    자력크기");
    const uint32_t t0 = millis();
    float flatMin = 999, flatMax = -999, tiltMin = 999, tiltMax = -999;
    float magMin = 9999, magMax = -9999;
    float yawSum = 0.0f, yawAbs = 0.0f;   // 손으로 기울이다 같이 돈 양
    uint32_t tPrev = millis();
    // 자이로가 말하는 회전을 뺀 나머지. 이게 진짜 남은 오차다.
    float resMin = 999, resMax = -999;
    bool  resFirst = true; float res0 = 0;
    float heelMin = 999, heelMax = -999;
    while (millis() - t0 < 5000) {
        // ★ FIFO 가 켜져 있으면 imuUpdate() 는 자력계만 갱신한다. 가속·자이로는
        //   FIFO 에서 오는데 그건 메인 루프가 퍼 온다. 여기서 안 퍼 오면
        //   **가속도계가 얼어붙은 채로 자력계만 움직인다.**
        //   실제로 그렇게 재서 힐이 20줄 내내 +46.5° 로 똑같이 나왔다 (2026-09-09).
        imuDrainFifo();
        imuUpdate();
        const float flat = headingDeg(), tilt = headingTiltDeg();
        if (flat < flatMin) flatMin = flat;   if (flat > flatMax) flatMax = flat;
        if (tilt < tiltMin) tiltMin = tilt;   if (tilt > tiltMax) tiltMax = tilt;
        const float hh = currentHeelDeg();
        if (hh < heelMin) heelMin = hh;   if (hh > heelMax) heelMax = hh;
        const float mm = sqrtf(gMag.x*gMag.x + gMag.y*gMag.y + gMag.z*gMag.z);
        if (mm < magMin) magMin = mm;   if (mm > magMax) magMax = mm;
        Serial.printf("  %+6.1f  %+6.1f   %5.1f°   %5.1f°   %+5.1f°   %5.1f\n",
                      currentHeelDeg(), currentPitchDeg(), flat, tilt,
                      wrap180(tilt - flat), mm);
        const uint32_t tNow = millis();
        const float dt = (tNow - tPrev) / 1000.0f;
        tPrev = tNow;
        const float yr = yawRateDegS();
        yawSum += yr * dt;
        yawAbs += fabsf(yr) * dt;

        // 보정한 방위에서 자이로가 말하는 회전을 뺀다.
        // 손이 돌린 만큼은 방위가 진짜 바뀐 것이라 오차가 아니다.
        const float res = wrap180(tilt - yawSum);
        if (resFirst) { res0 = res; resFirst = false; }
        const float r = wrap180(res - res0);
        if (r < resMin) resMin = r;   if (r > resMax) resMax = r;

        delay(250);
        feedWatchdog();
    }
    Serial.println("  ──────────────────────────────────────");
    Serial.printf("  흔들린 폭   평평 %.1f°   보정 %.1f°\n",
                  flatMax - flatMin, tiltMax - tiltMin);
    // ★ 충분히 기울이지 않았으면 판정하지 않는다.
    //   기울기가 작으면 두 값이 원래 비슷하다. 그걸 보고 "부호가 뒤집혔다" 고
    //   단정한 적이 있다 (2026-09-09). 조건 없는 판정은 판정이 아니다.
    Serial.printf("  기울인 폭   %.0f°  (%.0f° 에서 %.0f° 까지)\n",
                  heelMax - heelMin, heelMin, heelMax);
    if (heelMax - heelMin < 40.0f) {
        Serial.println("  ※ 기울기가 모자랍니다. 좌우로 40° 넘게 흔들어야 판정할 수 있습니다.");
    } else if (tiltMax - tiltMin < (flatMax - flatMin) * 0.6f) {
        Serial.println("  ★ 보정한 쪽이 훨씬 덜 흔들립니다 — 잘 되고 있습니다.");
    } else if (tiltMax - tiltMin > (flatMax - flatMin) * 1.5f) {
        Serial.println("  ★ 보정한 쪽이 더 흔들립니다 — 아래 축 부호를 의심하세요.");
    } else {
        Serial.println("  ※ 차이가 뚜렷하지 않습니다.");
    }
    // ── 손으로 기울이다 같이 돌지 않았나 ────────────────────────────────
    //
    // 사람 손으로 기울이면 살짝 돌기도 한다. 그러면 방위가 **진짜로** 바뀐 것이라
    // 보정한 값이 움직이는 게 맞다. 그걸 오차로 세면 안 된다.
    // 자이로로 아래 축을 도는 각을 모아서 얼마나 돌았는지 재 둔다.
    Serial.printf("  돌아간 각   알짜 %+.1f°   합계 %.1f°  (자이로)\n", yawSum, yawAbs);
    Serial.printf("  자이로가 말하는 회전을 빼면 남는 흔들림 %.1f°\n", resMax - resMin);
    Serial.println((resMax - resMin) < 10.0f
        ? "  ★ 10° 밑입니다 — 기울기 보정이 제대로 되고 있습니다."
        : "  ※ 아직 남습니다. 자력계 크기가 흔들리면 그것부터 잡아야 합니다.");

    // ── 자력계 크기가 일정한가 ──────────────────────────────────────────
    //
    // 지구 자기장은 한자리에서 세기가 일정하다. 돌리기만 하면 방향만 바뀌고
    // 크기는 그대로여야 한다. 크기가 변하면 **보드 위의 쇠붙이가 만드는 고정
    // 편차(하드아이언)** 가 얹혀 있는 것이다.
    //
    // 이게 남으면 기울기 보정 식이 아무리 정확해도 결과가 틀린다. 입력이
    // 틀렸기 때문이다. [확인: 2026-09-09 첫 시험 — 29 에서 42 µT 로 45% 흔들림]
    Serial.printf("  자력 크기   %.1f ~ %.1f µT   흔들림 %.0f%%\n",
                  magMin, magMax,
                  magMax > 0 ? (magMax - magMin) * 100.0f / magMax : 0.0f);
    Serial.println(((magMax - magMin) > magMax * 0.15f)
        ? "  ★ 크기가 15% 넘게 변합니다 — 하드아이언 보정이 먼저입니다."
        : "  자력 크기는 거의 일정합니다.");
    Serial.println("  ※ 한국의 지구 자기장은 약 50 µT 입니다. 크게 벗어나면 주변 쇠붙이");
    Serial.println("    때문입니다. 책상·노트북에서 떨어진 데서 다시 해보세요.");
    Serial.println("──────────────────────────────────────────");
}

// `oledw` — 화면에 쓸 한글 줄이 실제로 들어가나 재본다.
//
// 눈대중으로 자리를 잡지 않는다. 그리고 **글꼴에 없는 글자는 폭 0 으로 나온다.**
// 그래서 폭을 재면 빠진 글자까지 같이 잡아낸다. 한글 한 자는 16px, 빈칸과
// 숫자는 8px 여야 맞다.
static void doOledWidth() {
    static const char* kLines[] = {
        "켜는 중", "놓으면 취소", "켜집니다",
        "누르면 꺼짐", "놓으면 기록시작", "기록 멈춤",
        "끄는 중", "기록 저장 중", "기록 종료", "저장 중",
        "기록 시작", "꺼졌습니다", "5초 눌러 켜기",
        "버튼이 눌린 채", "끄지 않습니다", "손을 떼세요",
    };
    Serial.println("──────────────────────────────────────────");
    Serial.println("  화면 글자 폭 (128px 안에 들어가야 한다)");
    Serial.println("  한글 16px · 빈칸/숫자 8px. 0 이 섞이면 글꼴에 없는 글자다.");
    for (const char* t : kLines) {
        const int w = sail::displayTextWidth(t);
        Serial.printf("  %-20s %4d px  %s\n", t, w,
                      w < 0 ? "화면 없음" : (w <= 128 ? "OK" : "★ 넘침"));
    }
    Serial.println("──────────────────────────────────────────");
}

// 자는 동안 버튼이 보드를 깨울 수 있게 걸어 둔다.
//
// ★ 이 함수는 잠드는 **모든** 길에서 불러야 한다. 헛깨서 도로 자는 길도 포함이다.
//   그 길에서 빠뜨렸다가 하루치 배터리를 날렸다.
//
// ── 570초에 445번 깼던 일 (2026-09-08) ──────────────────────────────────
//
// 처음에는 이렇게만 했다.
//     rtc_gpio_pullup_en(GPIO2);
//     esp_sleep_enable_ext1_wakeup(BIT(2), ANY_LOW);
//
// 잠들기는 잘 잤다. 그런데 1.3초에 한 번씩 스스로 깼다. 깰 때마다 부팅에
// 1초 남짓 걸리니 보드는 자는 게 아니라 거의 계속 켜져 있었다. 배터리가
// 하루 만에 비었다.
//
//   [확인: `off 100` 뒤 `sleepstat` — 잔 시간 570초, 헛깸 445회]
//
// 이유는 헤더에 적혀 있었다.
//   "Internal pullups and pulldowns don't work when RTC peripherals are
//    shut down. ... Alternatively, RTC peripherals (and pullups/pulldowns)
//    may be kept enabled using esp_sleep_pd_config function."
//   [확인: esp_sleep.h:298-301]
//
// 풀업을 걸어 놔도 잠드는 순간 RTC 주변장치가 꺼지면서 같이 사라진다.
// 풀업이 없어진 GPIO2 는 떠 있고, 뜬 핀은 LOW 로 읽힌다. ANY_LOW 조건이
// 그 자리에서 참이 되어 다시 깬다. 그게 445번이었다.
//
// 그래서 RTC 주변장치를 켜 둔 채로 잔다. 그만큼 전류를 더 쓰지만, 1.3초마다
// 보드가 통째로 켜지는 것에 비하면 아무것도 아니다.
static void armButtonWake() {
    const gpio_num_t btn = (gpio_num_t)kButtonPin;

    rtc_gpio_init(btn);
    rtc_gpio_set_direction(btn, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(btn);
    rtc_gpio_pullup_en(btn);

    // ★ 이 줄이 빠지면 위의 풀업이 잠드는 순간 사라진다.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_sleep_enable_ext1_wakeup(1ULL << kButtonPin, ESP_EXT1_WAKEUP_ANY_LOW);
}

// ── 끄기 (깊은잠) ────────────────────────────────────────────────────────
//
// 버튼을 5초 누르면 여기로 온다. 정상이면 이 함수는 **안 돌아온다.**
// 돌아오는 경우는 버튼이 안 떨어질 때 하나뿐이다.
//
// 깨우는 것은 같은 버튼이다. GPIO2 는 ESP32-S3 의 RTC 핀이라 (0~21번이 RTC)
// 깊은잠에서 깨우는 데 쓸 수 있다 [확인: soc/esp32s3/include/soc/rtc_io_channel.h
// 의 `RTCIO_GPIO2_CHANNEL 2`, soc_caps.h 의 `SOC_RTCIO_PIN_COUNT 22`].
//
// ★ ext0 가 아니라 ext1 을 쓴다.
//   ext0 는 RTC 주변장치를 켜 둬야만 돌아간다. ext1 은 꺼도 돌아간다
//   [확인: esp_sleep.h "It will work even if RTC peripherals are shut down"].
//   전류가 갈리는 자리다. 내부 풀업은 RTC 주변장치를 끄면 안 먹지만, 잠들기
//   직전에 HOLD 가 걸려서 유지된다 [확인: 같은 헤더의 ext1 설명]. 그래서 바깥에
//   저항을 달 필요가 없다.
//
// ── 끄는 순서가 있다 ────────────────────────────────────────────────────
//
//  1) 기록을 먼저 닫는다. **카드 전원을 내리기 전에 끝나야 한다.**
//     쓰는 도중에 3V3_S 가 끊기면 파일이 깨진다.
//  2) 무전기를 재운다 (모듈 안이라 3V3_S 밖이다)
//  3) IMU 를 재운다 (VDD 라 3V3_S 밖이다)
//  4) 화면에 안내를 띄우고 끈다 (VDD 라 3V3_S 밖이다)
//  5) 손을 뗄 때까지 기다린다
//  6) 3V3_S 를 내리고 붙든다 (GPS·SD 가 여기서 꺼진다)
//
// 5번을 빼먹으면 안 된다. 버튼이 눌린 채로 자면 ANY_LOW 조건이 이미 참이라
// 잠들자마자 다시 깬다. 부팅을 무한히 반복하고 배터리가 몇 시간에 죽는다.
//
// 목표는 10 µA 미만이다 [확인: docs/board/RAK19007_datasheet.txt "With WisBlock
// Core and WisBlock Sensor on it, the sleep current is lower than 10 µA"].
// testWakeSec 이 0 이 아니면 그 초 뒤에 **스스로** 깬다. 재보려고 만든 길이다.
// 사람이 버튼을 눌러 줄 때까지 기다리면 한 번 재는 데 사람이 붙어 있어야 한다.
static void goToSleep(uint32_t testWakeSec) {
    Serial.println("[SLEEP] 끕니다");

    // 1) 기록부터 닫는다. 최대 15초 걸린다.
    if (hlog::recording()) {
        sail::displayHoldBar(100, "끄는 중", "기록 저장 중");
        hlog::stop();
        feedWatchdog();
    }

    // 1b) SD 카드를 놓아준다.
    //
    // ★ 카드는 3V3_S 가 아니라 VDD 에 물려 있다. 3V3_S 를 내려도 안 꺼진다.
    //   [확인: 2026-09-08, `power off` 뒤에도 `sd` 가 카드를 찾고 글씨까지 썼다]
    //   SPI 를 안 놓아주면 칩셀렉트가 눌린 채로 남아 카드가 계속 깨어 있다.
    SD.end();

    // 2) 무전기. 안 재우면 자는 동안 혼자 듣느라 수 mA 를 먹는다.
    lora::sleep();

    // 3) IMU. 항상 켜진 VDD 를 쓰므로 3V3_S 를 내려도 안 꺼진다.
    if (gImuOk) gImu.sleep(true);

    // 4) 화면. 이것도 VDD 다 [확인: `power off` 뒤에도 I2C 에 0x3C 가 그대로
    //    보였다, 2026-09-04]. 안 끄면 마지막 그림을 켠 채로 남는다.
    //    끄기 전에 켜는 법을 적어 둔다. 아무것도 안 남기고 까매지면 방법을 모른다.
    sail::displayNotice("꺼졌습니다", "5초 눌러 켜기");

    // 5) 손을 뗄 때까지 기다린다.
    const uint32_t t0 = millis();
    bool shownFor = false;
    while (true) {
        feedWatchdog();
        if (digitalRead(kButtonPin) != LOW) {
            // 30ms 안 흔들려야 진짜 뗀 것이다. 여기서도 튐을 걷어낸다.
            delay(kBtnStableMs);
            if (digitalRead(kButtonPin) != LOW) break;
        }
        if (millis() - t0 > kBtnStuckMs) {
            // 물이 접점을 붙들고 있는 것이다. 이대로 자면 잠들자마자 깨서
            // 무한히 반복한다. **켜져 있는 쪽이 안전하다.**
            Serial.println("[SLEEP] 버튼이 안 떨어집니다 — 끄지 않고 그대로 돕니다");
            sail::displayNotice("버튼이 눌린 채", "끄지 않습니다");
            if (gImuOk) gImu.sleep(false);
            delay(1500);
            return;
        }
        if (!shownFor && millis() - t0 > 1500) {
            shownFor = true;
            sail::displayNotice("손을 떼세요", nullptr);
        }
        delay(20);
    }
    if (!shownFor) delay(1200);   // "꺼졌습니다" 를 읽을 시간
    sail::displayOff();

    digitalWrite(rak::kLedGreen, LOW);
    digitalWrite(rak::kLedBlue, LOW);

    // 6) 센서 전원 3V3_S 를 내리고 잠자는 동안 붙든다.
    //
    //    ★ RTC 쪽 API 를 쓴다. gpio_hold_en 은 S3 에서 깊은잠 중 디지털 GPIO 를
    //      못 붙든다 [확인: driver/gpio.h "For ESP32/S2/C3/S3/C2, this function
    //      cannot be used to hold the state of a digital GPIO during Deep-sleep"].
    //      헷갈려서 그걸 쓰면 조용히 안 걸리고 자는 동안 센서만 계속 켜져 있다.
    //      GPIO14 는 RTC 핀이라 rtc_gpio_hold_en 이 맞다.
    const gpio_num_t pwr = (gpio_num_t)gSensorPowerPin;
    rtc_gpio_init(pwr);
    rtc_gpio_set_direction(pwr, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(pwr, 0);
    rtc_gpio_hold_en(pwr);

    // ── SD 카드 SPI 선을 정해진 자리에 두고 잔다 ────────────────────────
    //
    // 카드는 VDD 라 전원을 못 끊는다. 그리고 SPI 모드에는 카드를 재우는 명령이
    // 없다 — CMD15(GO_INACTIVE_STATE)가 SPI 에서는 "No" 다
    // [확인: docs/sd/SD_Physical_Layer_Simplified_Spec_v3.01.txt §7 명령표].
    // SD.end() 가 CMD0 를 보내는 것이 소프트웨어로 닿는 제일 깊은 자리다
    // [확인: framework-arduinoespressif32 SD/src/sd_diskio.cpp sdcard_uninit].
    //
    // ★ 그런데 SD.end() 는 **핀을 안 놓아준다.** 깊은잠에 들면 패드가 놓여서
    //   뜬다. 뜬 칩셀렉트가 LOW 로 읽히면 카드는 선택된 채로 남아 계속 깨어
    //   있는다. 칩셀렉트에 바깥 풀업이 없는 것을 쟀다.
    //     [확인: 2026-09-08 `pin 12` — 풀업 HIGH · 풀다운 LOW → 비어 있음]
    //
    // 규격서도 같은 취지를 말한다. §6.4.2 —
    //   "DAT, CMD, and CLK should be disconnected or driven to logical 0 by the
    //    host to avoid a situation that the operating current is drawn through
    //    the signal lines."
    //
    // 그래서 칩셀렉트는 HIGH 로 붙들어 카드를 확실히 떼어 놓고, 클럭과 MOSI 는
    // LOW 로 눕힌다. MISO 는 카드가 모는 선이라 건드리지 않는다.
    //
    // GPIO12 는 ESP32-S3 의 strapping 핀이 아니라 붙들어도 부팅에 지장이 없다
    // (S3 의 strapping 은 GPIO0·3·45·46).
    // ── 떠 있는 핀을 떼어 놓는다 ────────────────────────────────────────
    //
    // 자는 동안 아무도 안 잡고 있는 RTC 핀은 중간 전압에 떠 있게 된다. 디지털
    // 입력 버퍼는 딱 그 중간 전압에서 위아래가 동시에 열려 전류가 그냥 흐른다.
    // 헤더가 이 함수를 그래서 두었다.
    //
    //   "Use this function if an RTC IO needs to be disconnected from internal
    //    circuits in deep sleep, to minimize leakage current."
    //   [확인: driver/rtc_io.h:264-268]
    //
    // 우리 보드에서 뜨는 자리 둘.
    //   GPIO1  배터리 분압 마디. 2.1 V 언저리 — 정확히 제일 나쁜 자리다
    //   GPIO21 GPS 의 PPS. GPS 를 껐으니 아무도 안 잡는다
    //
    // 덤이 하나 있다. 떼어 놓으면 분압 마디가 자는 동안 안 빠져서, 깨자마자
    // 잰 배터리 값이 낮게 나오던 것도 같이 고쳐질 수 있다. 확인해야 한다.
    for (int pin : { rak::kBattAdcPin, rak::kGpsPpsSlotA }) {
        rtc_gpio_isolate((gpio_num_t)pin);
    }

    struct { int pin; int level; } spiRest[] = {
        { rak::kSPI_CS,   1 },   // 카드를 떼어 놓는다. 이게 핵심이다
        { rak::kSPI_CLK,  0 },
        { rak::kSPI_MOSI, 0 },
    };
    for (auto& r : spiRest) {
        const gpio_num_t g = (gpio_num_t)r.pin;
        rtc_gpio_init(g);
        rtc_gpio_set_direction(g, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(g, r.level);
        rtc_gpio_hold_en(g);
    }

    armButtonWake();
    gRtcTestSec = testWakeSec;
    if (testWakeSec) {
        esp_sleep_enable_timer_wakeup((uint64_t)testWakeSec * 1000000ull);
        Serial.printf("[SLEEP] 시험 모드 — %u초 뒤에 스스로 깹니다\n",
                      (unsigned)testWakeSec);
    }

    // RTC 메모리에 적어 둔다. 깨어난 뒤에 진짜로 잤는지 물어볼 유일한 방법이다.
    gRtcMagic   = kRtcMagic;
    gRtcSleeps += 1;
    gRtcSleepMv = (uint32_t)(readBatteryVolts(nullptr) * 1000.0f);
    gRtcSleepUs = nowUs();

    Serial.printf("[SLEEP] 잘 자라. 5초 누르면 깬다. (%u번째, %u mV)\n",
                  (unsigned)gRtcSleeps, (unsigned)gRtcSleepMv);
    Serial.flush();
    esp_deep_sleep_start();
}

// setup() 의 **맨 첫 줄**에서 부른다. Serial.begin 보다도 먼저다.
//
// drain 중이면 여기서 전압만 적고 도로 잠들어 아래로 안 내려간다. 화면도 GPS 도
// BLE 도 안 올린다 — 깨어 있는 시간을 최대한 짧게 해야 재는 값이 안 흔들린다.
//
// 사람이 버튼을 누르면 (깨운 이유가 타이머가 아니면) drain 을 끝내고 정상 부팅한다.
// 깬 뒤 배터리 ADC 가 언제 제자리로 오는지 열두 번 찍어 둔다.
static void probeGate() {
    if (!gProbeOn) return;
    gProbeOn = 0;
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return;

    rtc_gpio_hold_dis((gpio_num_t)rak::kBattAdcPin);
    rtc_gpio_deinit((gpio_num_t)rak::kBattAdcPin);
    // ★ 디지털 입력 버퍼를 끈다.
    //   deinit 은 핀을 보통 GPIO 로 돌려놓는데 그때 입력 버퍼가 켜진다.
    //   분압 마디가 2.1 V 라 버퍼의 위아래가 동시에 열려 전류가 흐르고,
    //   소스 저항이 2.5 MΩ 이라 그 전류만으로 전압이 절반으로 끌려 내려간다.
    //   [확인: 2026-09-09 probe — 눈금 1417 이 3.3초 내내 평평하다가 2644 로 뜀]
    gpio_set_direction((gpio_num_t)rak::kBattAdcPin, GPIO_MODE_DISABLE);
    analogSetPinAttenuation(rak::kBattAdcPin, ADC_11db);
    for (int i = 0; i < 12; i++) {
        uint32_t mv = 0;
        readBatteryVolts(&mv);
        gProbeMv[i]  = (uint16_t)mv;
        gProbeRaw[i] = (uint16_t)analogRead(rak::kBattAdcPin);
        delay(268);          // 재는 데 32ms 걸리니 합쳐서 0.3초 간격
    }
}

static void drainGate() {
    if (!gDrainOn) return;
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP)                  { gDrainOn = 0; return; }
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)   { gDrainOn = 0; return; }

    const uint32_t t0 = millis();

    // 자는 동안 떼어 놨던 배터리 핀을 되살린다
    rtc_gpio_hold_dis((gpio_num_t)rak::kBattAdcPin);
    rtc_gpio_deinit((gpio_num_t)rak::kBattAdcPin);
    analogSetPinAttenuation(rak::kBattAdcPin, ADC_11db);

    // 두 번 잰다. 빨리 잰 것과 기다렸다 잰 것.
    // 둘이 같으면 다음부터는 안 기다려도 된다는 뜻이라 깨는 값이 싸진다.
    uint32_t early = 0, late = 0;
    delay(150);   readBatteryVolts(&early);
    delay(1200);  readBatteryVolts(&late);

    const int i = gDrainN;
    if (i < kDrainMax) {
        gDrainSec[i]   = (uint32_t)((nowUs() - gDrainT0) / 1000000ull);
        gDrainEarly[i] = (uint16_t)early;
        gDrainMv[i]    = (uint16_t)late;
        gDrainN        = (uint8_t)(i + 1);
    }
    gDrainAwakeMs += (millis() - t0) + 250;   // 부팅 앞부분을 어림해 얹는다

    if (gDrainN >= kDrainMax) { gDrainOn = 0; return; }   // 다 찍었다. 정상 부팅

    const uint32_t waitSec = ((uint32_t)kDrainAt[gDrainN] -
                              (uint32_t)kDrainAt[gDrainN - 1]) * gDrainScale;
    armButtonWake();
    esp_sleep_enable_timer_wakeup((uint64_t)waitSec * 1000000ull);
    rtc_gpio_isolate((gpio_num_t)rak::kBattAdcPin);   // 다시 떼어 놓는다
    esp_deep_sleep_start();
}

// ── 깨어난 직후 — 5초를 채웠나 ───────────────────────────────────────────
//
// setup() 의 **맨 앞**에서 부른다. 순서가 중요하다.
//
//   · 플래시(NVS)에 쓰기 전에 와야 한다. 주머니에서 눌릴 때마다 부팅 횟수를
//     올리면 플래시가 그만큼 닳는다.
//   · 워치독을 걸기 전에 와야 한다. 5초를 세는 동안 물어뜯긴다.
//
// ★ ext1 은 누르는 순간 깨운다. 얼마나 눌렸는지는 못 센다. 그래서 여기서
//   직접 센다. 5초를 못 채우고 떼면 도로 잠든다.
//
// 세는 동안 화면으로 알려준다. 안 보여주면 사람이 5초를 못 기다리고 손을 뗀다.
// 화면은 3V3_S 밖(VDD)이라 센서를 하나도 안 붙인 이 자리에서도 그릴 수 있다.
static void wakeGate() {
    const bool fromSleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP);

    // 붙들어 둔 핀부터 푼다. **이게 없으면 깨어나도 센서가 영영 안 켜진다.**
    // RTC 홀드는 깨어난 뒤에도 계속 걸려 있어서, applySensorPower 가 HIGH 를
    // 써도 패드가 안 움직인다. 로그에는 "센서 전원 ON" 이 찍히는데 GPS 와 SD 는
    // 죽어 있는, 보드가 거짓말하는 상태가 된다.
    if (fromSleep) {
        // 깊은잠에 들 때 칩이 RTC 핀을 붙들어 놓는다. 그걸 통째로 푼다.
        //   "Force hold signal is enabled before going into deep sleep for pins
        //    which are used for EXT1 wakeup." [확인: driver/rtc_io.h:250]
        //
        // ★ 배터리 핀(GPIO1)까지 같이 풀어야 한다. 안 풀면 깨어난 직후 배터리가
        //   3.55 V 인데 2.00 V 로 읽힌다. 여덟 번을 재도 여덟 번 다 1200 mV 다
        //   (제 값은 2133 mV). 천천히 차오르는 게 아니라 핀에 뭔가 매달려서
        //   분압비가 0.6 에서 0.34 로 바뀐 것이다
        //   [확인: 2026-09-08, `off 30` 뒤 `battboot`].
        //   보통 부팅에서는 첫 번째부터 제 값이 나온다. 깼을 때만 그렇다.
        rtc_gpio_force_hold_dis_all();
        for (int pin : {rak::kSensorPowerA, rak::kSensorPowerB, kButtonPin,
                        rak::kBattAdcPin, rak::kGpsPpsSlotA,
                        rak::kSPI_CS, rak::kSPI_CLK, rak::kSPI_MOSI}) {
            const gpio_num_t g = (gpio_num_t)pin;
            rtc_gpio_hold_dis(g);
            rtc_gpio_deinit(g);      // 다시 보통 GPIO 로 돌려 놓는다
        }
    }

    // 잔 시간과 전압 낙차를 여기서 잰다. 아래로 내려가면 늦다.
    if (fromSleep && gRtcMagic == kRtcMagic) {
        gRtcSleptUs = nowUs() - gRtcSleepUs;
        gWokeFromSleep = true;
        // ★ 전압은 여기서 안 잰다. 이 자리에서 재면 3504 mV 로 잠든 보드가
        //   2091 mV 로 깨어난 것처럼 나온다. USB 로 충전 중인데 그럴 리가 없다.
        //   ADC 가 아직 준비되기 전이라 눈금이 다른 값이 나온 것이다
        //   [확인: 2026-09-08, off 180 두 번 모두 같은 식으로 어긋났다].
        //   setup() 에서 analogSetPinAttenuation 을 거친 뒤에 잰다.

        // ── 자는 동안 3V3_S 가 정말 꺼져 있었나 ──────────────────────────
        //
        // 이게 제일 굵은 의심이다. GPS 는 위성을 찾는 동안 30 mA 쯤 먹는다.
        // 3V3_S 를 못 내렸으면 자는 내내 그게 돌아간 것이고, 하루 만에 배터리가
        // 비는 것도 설명이 된다.
        //
        // 재는 법. **깨자마자, 전원을 다시 넣기 전에** GPS 선을 들어본다.
        //   말이 바로 나온다   → 자는 동안 계속 켜져 있었다. 홀드가 안 먹은 것이다
        //   조용하다           → 꺼져 있었다. 전원을 넣어야 몇 초 뒤에 말을 시작한다
        //
        // 여기서만 할 수 있는 측정이다. 아래로 내려가면 applySensorPower 가
        // 전원을 다시 넣어 버려서 둘을 구분할 수 없다.
        gRtcGpsBytes = 0;
        Serial1.begin(kGpsBaud, SERIAL_8N1, rak::kUART1_RX, rak::kUART1_TX);
        const uint32_t g0 = millis();
        while (millis() - g0 < 400) {
            while (Serial1.available()) { Serial1.read(); gRtcGpsBytes++; }
            delay(5);
        }
        Serial1.end();
    }

    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;

    // 화면만 먼저 켠다. I2C 는 3V3_S 와 무관하다.
    // (main 이 Wire.begin 을 한 뒤여야 U8g2 가 핀을 안 건드린다 — display_rak.cpp)
    Wire.begin(rak::kI2C1_SDA, rak::kI2C1_SCL, 400000);
    sail::displayBegin();

    pinMode(kButtonPin, INPUT_PULLUP);
    delay(20);

    const uint32_t t0 = millis();
    uint32_t upSince = 0;          // HIGH 가 된 시각. 0 이면 계속 눌려 있다
    while (true) {
        const uint32_t held = millis() - t0;
        if (held >= kBtnOffMs) break;               // 5초 채웠다. 켠다

        if (digitalRead(kButtonPin) == LOW) {
            upSince = 0;                            // 튐이었다. 계속 센다
        } else {
            if (upSince == 0) upSince = millis();
            // 30ms 이어져야 진짜 뗀 것이다. 여기서 튐을 안 걷어내면 손가락이
            // 1ms 튈 때마다 취소된다. 5초를 꾹 눌렀는데 안 켜지고, 왜 안 켜지는지
            // 알 방법도 없다.
            if (millis() - upSince >= kBtnStableMs) {
                gRtcFalse += 1;      // 5초 못 채웠다. 도로 잔다
                sail::displayOff();
                armButtonWake();
                // ★ 시험 타이머도 다시 건다. 안 그러면 한 번 헛깨는 순간
                //   타이머가 사라져서 보드가 영영 안 깨어난다. 실제로 그랬다.
                if (gRtcTestSec) {
                    esp_sleep_enable_timer_wakeup((uint64_t)gRtcTestSec * 1000000ull);
                }
                esp_deep_sleep_start();
            }
        }

        sail::displayHoldBar(btnPct(held), "켜는 중", "놓으면 취소");
        delay(50);
    }

    gRtcFull += 1;
    sail::displayNotice("켜집니다", nullptr);
}

// ── 기록 (hlog) ──────────────────────────────────────────────────────────
//
// 규격은 docs/spec/로그포맷_v1.0_draft_2026-08-24.md, 계획과 실측은 SDLOG.md.
//
// ★ 저장하는 값은 **다듬기 전 원본**이다. 앱에 보이는 속도는 다듬고 잡음
//   바닥을 적용한 값인데, 그걸 저장하면 원본을 되살릴 수 없다. 잡음 바닥
//   0.1 kn 때문에 진짜 저속 데이터가 조용히 0 이 되어 버린다.
//   헤더의 sog_src 가 항상 0(도플러 원본)이어야 하는 이유다.

// UTC 날짜·시각을 GPS 주 번호와 주중 밀리초로 바꾼다.
//
// ※ NMEA 가 주는 것은 UTC 다. 진짜 GPS 시각은 여기에 윤초(지금 18초)를
//   더한 값이다. 그래서 헤더의 time_ref 에 1(UTC 환산)이라고 적어 둔다.
//   나중에 진짜 GPS 시각을 주는 모듈로 바꾸면 0 으로 적으면 된다.
static bool gpsWeekTow(uint16_t* week, uint32_t* tow) {
    if (!gGps.date.isValid() || !gGps.time.isValid()) return false;
    const int y = gGps.date.year(), m = gGps.date.month(), d = gGps.date.day();
    if (y < 2000) return false;

    // 1980-01-06 부터 며칠 지났나 (Howard Hinnant 의 days_from_civil)
    int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468; // 1970-01-01 기준
    const long gpsDays = days - 3657;                          // 1980-01-06 까지 3657일
    if (gpsDays < 0) return false;

    *week = (uint16_t)(gpsDays / 7);
    const uint32_t dow = (uint32_t)(gpsDays % 7);
    *tow = ((dow * 86400u) + gGps.time.hour() * 3600u +
            gGps.time.minute() * 60u + gGps.time.second()) * 1000u +
           gGps.time.centisecond() * 10u;
    return true;
}

static void logWriteImu(uint32_t nowMs) {
    hlog::ImuSample b;
    b.localMs = nowMs;
    // 1 mg/LSB
    b.acc[0] = (int16_t)lroundf(gAcc.x * 1000.0f);
    b.acc[1] = (int16_t)lroundf(gAcc.y * 1000.0f);
    b.acc[2] = (int16_t)lroundf(gAcc.z * 1000.0f);
    // 1/32 °/s/LSB
    b.gyr[0] = (int16_t)lroundf(gGyr.x * 32.0f);
    b.gyr[1] = (int16_t)lroundf(gGyr.y * 32.0f);
    b.gyr[2] = (int16_t)lroundf(gGyr.z * 32.0f);
    // 자세는 여기 안 넣는다. 가속·자이로 원본이 100 Hz 로 남으므로 후처리로
    // 뽑는다. 어느 축이 힐인지는 머리글에 적혀 있다.
    hlog::writeImu(b);
}

static hlog::NavSample buildNav(uint32_t nowMs) {
    hlog::NavSample a;
    a.localMs = nowMs;

    uint16_t wk; uint32_t tow;
    if (gpsWeekTow(&wk, &tow)) { a.week = wk; a.itow = tow; }

    if (gGpsFix && gGps.location.isValid()) {
        a.lat = (int32_t)lround(gGps.location.lat() * 1e7);
        a.lon = (int32_t)lround(gGps.location.lng() * 1e7);
        a.fix = 1;
    }

    // ★ 다듬기 전 도플러 원본. sogOut() 을 쓰면 안 된다.
    if (gGpsFix && gGps.speed.isValid()) {
        const float kn = (float)gGps.speed.knots();
        long mmps = lroundf(kn / 1.943844f * 1000.0f);
        if (mmps < 0) mmps = 0;
        if (mmps > 65534) mmps = 65534;
        a.sog = (uint16_t)mmps;
    }
    if (gGpsFix && gGps.course.isValid()) {
        long cd = lroundf((float)gGps.course.deg() * 100.0f) % 36000;
        if (cd < 0) cd += 36000;
        a.cog = (uint16_t)cd;
    }

    if (gGps.satellites.isValid()) a.numSv = (uint8_t)gGps.satellites.value();

    // 수평 정확도. 모듈이 직접 알려 주는 값이 있으면 그걸 쓴다.
    if (gPvHAccM >= 0.0f) {
        long cm = lroundf(gPvHAccM * 100.0f);
        a.hAcc = (uint16_t)(cm > 65534 ? 65534 : (cm < 0 ? 0 : cm));
    }

    a.battMv = (uint16_t)lroundf(gBattVolts * 1000.0f);

    // 자력계 원본. 0.1 µT/LSB (헤더 mag_scale = 1)
    if (gMagOk) {
        a.mag[0] = (int16_t)lroundf(gMag.x * 10.0f);
        a.mag[1] = (int16_t)lroundf(gMag.y * 10.0f);
        a.mag[2] = (int16_t)lroundf(gMag.z * 10.0f);
    }
    return a;
}

static void logWriteNav(uint32_t nowMs) {
    const hlog::NavSample a = buildNav(nowMs);
    hlog::writeNav(a);

    // 첫 fix 의 UTC 를 알려 둔다. 세션을 닫을 때 머리글에 박힌다.
    // 이게 있어야 나중에 목록만 보고 언제 찍은 세션인지 안다.
    if (a.fix && a.week != hlog::kWeekInvalid && a.itow != hlog::kItowInvalid) {
        // GPS 주 번호 + 주중 밀리초 → UNIX 초
        //   GPS 시각의 0점은 1980-01-06, UNIX 는 1970-01-01. 차이 315964800초.
        //   ※ 우리는 NMEA 의 UTC 로 week/itow 를 만들었으므로 (헤더 time_ref=1)
        //     여기서 윤초를 더하지 않는다. 그대로 UTC 다.
        const uint32_t sec = 315964800UL + (uint32_t)a.week * 604800UL + a.itow / 1000UL;
        hlog::noteUtcStart(sec, (uint16_t)(a.itow % 1000UL));
    }
}

static void logWriteText(uint32_t nowMs) {
    hlog::TextSample t;
    t.attOk    = gImuOk;
    t.heelDeg  = currentHeelDeg();
    t.pitchDeg = currentPitchDeg();
    t.hdgDeg   = headingDeg();
    // 1노트 아래에서 어느 길이 살아남는지 보려고 셋을 같이 남긴다 (hlog.h 참고).
    // ★ 표식이 4 이상일 때만 값을 넣는다. 3 은 옛날 값이라 넣으면 거짓말이 된다.
    t.sogPvKn  = (gPvVelValid && gPvSpeedKn >= 0) ? gPvSpeedKn : -1.0f;
    t.sogPosKn = gSogFromPos;
    t.pvFlag   = (gPvAtMs && millis() - gPvAtMs < 3000) ? gPvVelFlag : 255;
    t.cogAccDeg = gPvCogAccDeg;
    hlog::writeText(buildNav(nowMs), t);
}

bool logStartNow(uint32_t prevSession) {
    hlog::Header h;
    h.prevSession = prevSession;
    esp_read_mac(h.mac, ESP_MAC_WIFI_STA);
    h.fwVersion = 0x0100;
    h.hwRev     = 1;
    // 규격은 0=RYS8839, 1=SE868SY-D, 2=예약(RTK) 뿐이다. 우리 시제품의 L76K 는
    // 없다. 2 로 적으면 "RTK 유닛" 이라는 거짓말이 되므로 0xFF(모름)로 둔다.
    // freeze 전에 규격에 값을 하나 받아야 한다.
    h.gnssType  = 0xFF;
    h.imuType   = hlog::kImuMPU9250;
    h.timeRef   = 1;                    // UTC 에서 환산 (윤초만큼 다름)
    h.magScale  = 1;                    // 0.1 µT/LSB
    h.gnssHz    = gGpsHz;
    h.sogSrc    = 0;                    // 도플러 원본
    h.quatSrc   = 1;                    // 융합 없음 — 가속·자이로 원본만
    h.heelAxis  = gHeelAxis;
    h.heelSign  = (gHeelSign < 0.0f) ? 1 : 0;
    h.pitchAxis = gPitchAxis;
    h.pitchSign = (gPitchSign < 0.0f) ? 1 : 0;
    h.heelOff   = gHeelOffsetDeg;
    h.pitchOff  = gPitchOffsetDeg;
    if (gGpsDyModel != 255) {
        h.gnssDyn = gGpsDyModel;            // 모듈에서 실제로 읽은 값이 있으면 그것
    } else {
        gPrefs.begin("sail", true);
        h.gnssDyn = gPrefs.getUChar("gps_dyn", 255);
        gPrefs.end();
    }
    if (!hlog::start(h)) return false;

    // 카드를 마운트하는 동안 FIFO 에 옛 값이 쌓인다. 그걸 그대로 쓰면
    // 세션 첫머리에 "기록 시작 전" 값이 섞여 들어간다. 비우고 시작한다.
    if (gFifoOn) {
        gImu.resetFifo();
        gImuTickMs = millis();
        gFifoOverrun = 0;
    }
    return true;
}

// ── 한 바퀴에 얼마나 걸리나 (probe) ──────────────────────────────────────
//
// IMU 를 100 Hz 로 올렸는데 78 Hz 밖에 안 나왔다. 짐작하지 말고 어디서
// 시간을 쓰는지 잰다. `loopstat` 으로 켜고 끈다.
static bool     gLoopStat = false;
static uint32_t gLoopCount = 0;
static uint32_t gLoopMaxUs = 0;
struct SecStat { uint32_t maxUs = 0; uint32_t sumUs = 0; };
static SecStat gStGps, gStImu, gStNotify, gStDraw, gStLog;
static inline void secDone(SecStat& st, uint32_t us) {
    if (us > st.maxUs) st.maxUs = us;
    st.sumUs += us;
}
static void loopStatPrint(uint32_t elapsedMs) {
    if (!gLoopStat) return;
    const float sec = elapsedMs / 1000.0f;
    Serial.printf("   루프  %.0f바퀴/초  한 바퀴 최대 %.1fms\n",
                  gLoopCount / sec, gLoopMaxUs / 1000.0f);
    Serial.printf("   ├ GPS읽기 최대 %5.1fms  합 %5.1fms/초\n",
                  gStGps.maxUs / 1000.0f, gStGps.sumUs / 1000.0f / sec);
    Serial.printf("   ├ IMU읽기 최대 %5.1fms  합 %5.1fms/초\n",
                  gStImu.maxUs / 1000.0f, gStImu.sumUs / 1000.0f / sec);
    Serial.printf("   ├ notify  최대 %5.1fms  합 %5.1fms/초\n",
                  gStNotify.maxUs / 1000.0f, gStNotify.sumUs / 1000.0f / sec);
    Serial.printf("   ├ 화면    최대 %5.1fms  합 %5.1fms/초\n",
                  gStDraw.maxUs / 1000.0f, gStDraw.sumUs / 1000.0f / sec);
    Serial.printf("   └ 기록    최대 %5.1fms  합 %5.1fms/초\n",
                  gStLog.maxUs / 1000.0f, gStLog.sumUs / 1000.0f / sec);
    gLoopCount = 0; gLoopMaxUs = 0;
    gStGps = gStImu = gStNotify = gStDraw = gStLog = SecStat();
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

// ── 지난번에 왜 다시 켜졌나 ──────────────────────────────────────────────
//
// 2026-08-30 세션 27 이 29분 만에 그 자리에서 끊겼다. 전압 3.96V 평평,
// 버퍼 3%, 버린 줄 0, 마지막까지 10 ms 등간격 — 전원도 카드도 워치독도
// 아니었다. 그런데 **왜 끊겼는지를 알 방법이 없었다.** 이유가 램에 있다가
// 같이 날아갔기 때문이다.
//
// 칩은 그 이유를 RTC 영역에 남겨 둔다. 켤 때 읽어서 찍어 둔다.
//   POWERON   전원이 실제로 끊겼다 (배터리 커넥터·USB 접촉)
//   BROWNOUT  전압이 순간 내려앉았다
//   PANIC     코드가 죽었다 (Guru Meditation)
//   TASK_WDT / INT_WDT  워치독이 물었다
//   SW / SW_CPU  다시 굽거나 reset 명령
static const char* gResetWhy = "?";

static void reportResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  gResetWhy = "POWERON 전원이 새로 들어왔다"; break;
        case ESP_RST_BROWNOUT: gResetWhy = "BROWNOUT ★ 전압이 내려앉았다"; break;
        case ESP_RST_PANIC:    gResetWhy = "PANIC ★ 코드가 죽었다"; break;
        case ESP_RST_TASK_WDT: gResetWhy = "TASK_WDT ★ 워치독이 물었다"; break;
        case ESP_RST_INT_WDT:  gResetWhy = "INT_WDT ★ 인터럽트 워치독"; break;
        case ESP_RST_WDT:      gResetWhy = "WDT ★ 워치독"; break;
        case ESP_RST_SW:       gResetWhy = "SW 소프트웨어가 다시 켰다"; break;
        case ESP_RST_DEEPSLEEP:gResetWhy = "DEEPSLEEP 깊은잠에서 깼다"; break;
        case ESP_RST_EXT:      gResetWhy = "EXT 바깥에서 리셋"; break;
        default:               gResetWhy = "UNKNOWN 알 수 없음"; break;
    }
    Serial.printf("[BOOT] 지난번 꺼진 이유 — %s\n", gResetWhy);
    hlog::noteBootReason(gResetWhy);
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
    e.pitchDeg   = currentPitchDeg();
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
// ── 설정 통로 (BLE) ──────────────────────────────────────────────────────
//
// 글자 한 줄을 써 넣으면 한 줄로 답한다. 규격은 PROTOCOL.md §9.
//
// 왜 글자냐면 — 시리얼 명령과 같은 말을 쓰려는 것이다. 새 규격을 외울 게
// 없고, 앱이 없어도 시리얼로 똑같이 시험할 수 있다.
//
// ★ "wifi on" 을 받으면 답을 **먼저** 보내고 나서 WiFi 를 켠다.
//   답에 "어디로 찾아오라" 를 미리 다 적어 준다. WiFi 가 붙는 데 몇 초가
//   걸리는데 그동안 앱을 기다리게 할 이유가 없다.
//
//   WiFi 를 켜도 BLE 는 그대로 있다. 파일을 보내는 동안에만 잠깐 내려간다
//   (netsrv.cpp 의 fastOn 주석).
static NimBLECharacteristic* gControlChr = nullptr;

// WiFi 를 켜는 일은 콜백 안에서 하면 안 된다. NimBLE 안쪽에서 부르는데
// 거기서 BLE 를 내리면 자기 발등을 찍는다. 깃발만 세우고 loop 에서 한다.
static volatile uint8_t gWifiWant = 0;   // 0 없음, 1 join, 2 ap, 3 off

static void controlSay(const char* line) {
    Serial.printf("[CTL] → %s\n", line);
    if (!gControlChr) return;
    gControlChr->setValue((const uint8_t*)line, strlen(line));
    gControlChr->notify();
}

/** 한 줄을 처리한다. 시리얼에서도 부를 수 있게 따로 뒀다. */
static void controlLine(const char* raw) {
    String line(raw);
    line.trim();
    if (line.length() == 0) return;
    Serial.printf("[CTL] ← %s\n", line.c_str());

    char out[240];

    // wifi ssid <이름>
    if (line.startsWith("wifi ssid ")) {
        String v = line.substring(10); v.trim();
        netsrv::setCreds(v.c_str(), nullptr);
        snprintf(out, sizeof(out), "ok wifi ssid %s", v.c_str());
        controlSay(out);
        return;
    }
    // wifi pass <비밀번호>
    if (line.startsWith("wifi pass ")) {
        String v = line.substring(10);       // 뒤 공백도 비밀번호일 수 있다
        netsrv::setCreds(netsrv::staSsid(), v.c_str());
        // ★ 비밀번호는 되읽어 주지 않는다. 길이만 알린다.
        snprintf(out, sizeof(out), "ok wifi pass %u자", (unsigned)v.length());
        controlSay(out);
        return;
    }
    // wifi scan — BLE 를 안 내리고 할 수 있다
    if (line == "wifi scan") {
        netsrv::ScanEntry list[20];
        const int n = netsrv::scan(list, 20);
        snprintf(out, sizeof(out), "scan begin %d", n);
        controlSay(out);
        for (int i = 0; i < n; i++) {
            snprintf(out, sizeof(out), "scan %d %d %s %s",
                     i, (int)list[i].rssi, list[i].locked ? "lock" : "open",
                     list[i].ssid);
            controlSay(out);
        }
        controlSay("scan end");
        return;
    }
    // wifi on / wifi ap — 답을 먼저 보내고 켠다
    //
    // 뒤에 앱 번호를 붙일 수 있다 ("wifi on a1b2c3d4"). 그 번호를 뺀
    // 나머지가 몇 대인지 답에 적어 준다. 앱은 그 수가 0 인지만 보면 된다.
    // 시리얼에서 그냥 "wifi on" 이라고 쳐도 된다. 그때는 아무도 안 뺀다.
    if (line == "wifi on" || line == "wifi join" ||
        line.startsWith("wifi on ") || line.startsWith("wifi join ")) {
        if (hlog::recording()) { controlSay("err wifi recording"); return; }
        String appId = "";
        {
            const int sp = line.indexOf(' ', 5);   // "wifi on" 의 두 번째 빈칸
            if (sp > 0) { appId = line.substring(sp + 1); appId.trim(); }
        }
        // 이미 붙어 있으면 다시 붙지 않는다. 다시 붙으면 지금 파일을 받고
        // 있는 다른 기기가 끊긴다. 지금 주소를 그대로 알려준다.
        if (netsrv::mode() == netsrv::Mode::Join) {
            // 이미 켜져 있다. 몇 대가 쓰는지 같이 알려준다. 앱이 그걸 보고
            // "다른 기기도 씁니다" 라고 사람에게 말한다.
            const int others = netsrv::othersThan(appId.c_str());
            const char* oip = netsrv::otherIpText(appId.c_str());
            snprintf(out, sizeof(out),
                     "ok wifi joining %s mdns %s.local last %s users %d by %s",
                     netsrv::staSsid(), netsrv::mdnsHost(), netsrv::ipText(),
                     others, (others && *oip) ? oip : "-");
            controlSay(out);
            return;
        }
        const char* ss = netsrv::staSsid();
        if (!ss || !*ss) { controlSay("err wifi no-ssid"); return; }
        // 붙고 나면 주소는 공유기가 준다. 미리 못 알려주니 이름으로 찾으라고 한다.
        // 이름과 함께 지난번 주소도 알려준다. 이름이 늦게 잡히거나 아예
        // 안 잡힐 때 앱이 이 주소부터 두드려 볼 수 있다.
        const char* last = netsrv::lastIp();
        snprintf(out, sizeof(out),
                 "ok wifi joining %s mdns %s.local last %s users 0 by -",
                 ss, netsrv::mdnsHost(), (last && *last) ? last : "-");
        controlSay(out);
        gWifiWant = 1;
        return;
    }
    if (line == "wifi ap" || line.startsWith("wifi ap ")) {
        if (hlog::recording()) { controlSay("err wifi recording"); return; }
        String apId = line.length() > 8 ? line.substring(8) : String("");
        apId.trim();
        // AP 는 이름도 주소도 미리 안다. 그대로 알려준다.
        const bool apUp = netsrv::mode() == netsrv::Mode::AP;
        const int others = apUp ? netsrv::othersThan(apId.c_str()) : 0;
        const char* oip = netsrv::otherIpText(apId.c_str());
        snprintf(out, sizeof(out),
                 "ok wifi ap %s pass %s ip 192.168.4.1 users %d by %s",
                 gFullName, netsrv::apPass(), others,
                 (others && *oip) ? oip : "-");
        controlSay(out);
        // 이미 열어 뒀으면 다시 열지 않는다. 다시 열면 붙어 있던 기기가
        // 떨어져서 처음부터 다시 붙어야 한다.
        if (netsrv::mode() != netsrv::Mode::AP) gWifiWant = 2;
        return;
    }
    if (line == "wifi off") { controlSay("ok wifi off"); gWifiWant = 3; return; }

    // wifi idle <초> — 아무도 안 쓰면 저절로 끄기까지의 시간. 0 이면 안 끈다
    if (line.startsWith("wifi idle ")) {
        const uint32_t sec = (uint32_t)line.substring(10).toInt();
        netsrv::setIdleOff(sec);
        snprintf(out, sizeof(out), "ok wifi idle %lu", (unsigned long)sec);
        controlSay(out);
        return;
    }
    if (line == "wifi status" || line == "status") {
        snprintf(out, sizeof(out),
                 "status name %s mode %s ip %s rec %s idle %lus left %lus",
                 gFullName,
                 netsrv::mode() == netsrv::Mode::Off ? "off"
                   : netsrv::mode() == netsrv::Mode::AP ? "ap" : "join",
                 netsrv::ipText(), hlog::recording() ? "on" : "off",
                 (unsigned long)netsrv::idleOffSec(),
                 (unsigned long)(netsrv::idleLeftMs() / 1000));
        controlSay(out);
        return;
    }
    if (line == "help") {
        controlSay("cmds: wifi ssid|pass|scan|on|ap|off|status");
        return;
    }
    snprintf(out, sizeof(out), "err unknown %s", line.c_str());
    controlSay(out);
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        (void)info;
        const std::string v = chr->getValue();
        // 한 번에 여러 줄이 올 수도 있다. 줄 단위로 끊어 처리한다.
        String buf(v.c_str());
        int at = 0;
        while (at < (int)buf.length()) {
            int nl = buf.indexOf('\n', at);
            if (nl < 0) nl = buf.length();
            controlLine(buf.substring(at, nl).c_str());
            at = nl + 1;
        }
    }
};

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
    Serial.println("  boat <0~32>   로라 배 번호. 0 은 번호 없음. 예) boat 7");
    Serial.println("  lora          로라 상태 (주파수·전파시간·받은 개수)");
    Serial.println("  lora on       로라 켜기");
    Serial.println("  lora regs     칩 버그 세 개가 실제로 걸렸는지 레지스터로 확인");
    Serial.println("  lora rssi     이 주파수의 바닥 잡음. 보드 한 대로 하는 확인");
    Serial.println("  lora tx       시험 삼아 하나 보내기");
    Serial.println("  lora watch    받을 때마다 한 줄씩 뱉기 (두 대로 시험할 때)");
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
    Serial.println("  oledw         화면에 쓸 한글 줄의 폭을 잰다 (128px 안에 드나)");
    Serial.println("  hdgtilt       기울기 보정 방위 확인 — 손으로 기울이며 5초 본다");
    Serial.println("  off           보드를 끈다 (깊은잠). 버튼 5초 누르면 켜진다");
    Serial.println("  sleepstat     지난번에 정말 잤나 — 헛깬 횟수와 전압 낙차");
    Serial.println("  drain         잠자기 전류 재기 — 13시간 동안 15번 찍는다");
    Serial.println("  drain fast    같은 길을 초 단위로 — 13분이면 끝난다 (확인용)");
    Serial.println("  drainstat     그 표를 본다");
    Serial.println("  sd            SD카드 마운트 + 쓰기 시험");
    Serial.println("  sdbench [줄수] SD 쓰기 속도·최대 멈춤 실측 (기본 3600줄)");
    Serial.println("  rec           ★ 기록 상태. rec on / rec off / rec mark");
    Serial.println("  rec ls / rec check [번호]   파일 목록 / 되읽어 검사");
    Serial.println("  rec tail [번호] [줄수]      TXT 사본 끝줄 (전압·멈춤·버퍼)");
    Serial.println("  rec rm <번호>               그 세션 파일을 지운다 (못 되돌린다)");
    Serial.println("  pin <번호>    그 GPIO 를 5초 지켜본다 (버튼 달 자리 찾기)");
    Serial.println("  wifi          ★ 기록 파일을 WiFi 로 내보내기. wifi ap / join / off");
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
    Serial.println("  tz [분]       파일 이름에 쓸 시각 기울기 (기본 540 = 한국)");
    Serial.println("  sess [번호]   세션 번호 보기·고치기 (NVS 에 남는다)");
    Serial.println("  usbbench [KB] USB 시리얼 속도 실측 (기본 512 KB)");
    Serial.println("  level         ★ 지금 자세를 힐·피치 0° 로 삼기 (배가 평형일 때)");
    Serial.println("  heel [x|y|z]  힐을 어느 가속도 축에서 볼지 (앞에 - 로 뒤집기)");
    Serial.println("  hdg <A> <B>   방위를 만들 자력계 두 축. 예) hdg -z x");
    Serial.println("  hdg off <도>  방위 0점 보정 (자기 편각 + 보드 어긋남)");
    Serial.println("  pitch [x|y|z] 피치를 어느 가속도 축에서 볼지");
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
    if (line == "oledw")               { doOledWidth(); return; }
    if (line == "hdgtilt")             { doHeadingTilt(); return; }
    if (line == "sleepstat")           { doSleepStat(); return; }
    if (line == "drain")               { doDrainStart(false); return; }
    if (line == "drain fast")          { doDrainStart(true);  return; }
    if (line == "drainstat")           { doDrainStat();  return; }
    if (line.startsWith("probe ")) {
        gProbeOn = 1;
        for (int i = 0; i < 12; i++) { gProbeMv[i] = 0; gProbeRaw[i] = 0; }
        Serial.println("[PROBE] 자고 깨서 0.3초 간격으로 열두 번 찍습니다. probestat 으로 봅니다.");
        Serial.flush();
        goToSleep((uint32_t)line.substring(6).toInt());
        return;
    }
    if (line == "probestat") {
        Serial.println("──────────────────────────────────────────");
        Serial.println("  깬 뒤 배터리 ADC 가 제자리로 오는 모양 (0.3초 간격)");
        for (int i = 0; i < 12; i++) {
            if (!gProbeMv[i]) continue;
            Serial.printf("  %4.1f초   눈금 %4u   핀 %4u mV  →  배터리 %.3f V\n",
                          i * 0.3f, (unsigned)gProbeRaw[i], (unsigned)gProbeMv[i],
                          (gProbeMv[i] / 1000.0f) / rak::kBattDivider);
        }
        uint32_t now = 0; readBatteryVolts(&now);
        Serial.printf("  지금     눈금 %4u   핀 %4u mV  →  배터리 %.3f V\n",
                      (unsigned)analogRead(rak::kBattAdcPin),
                      (unsigned)now, (now / 1000.0f) / rak::kBattDivider);
        Serial.println("  ※ 눈금이 같은데 mV 만 다르면 감쇠 설정이 안 먹은 것이다.");
        Serial.println("  ※ 눈금부터 다르면 핀에 뭔가 매달린 것이다.");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    if (line == "battboot") {
        Serial.println("──────────────────────────────────────────");
        Serial.println("  켠 뒤 1초마다 담은 배터리 ADC 값");
        for (int i = 0; i < kBattBootN; i++) {
            if (!gBattBoot[i]) continue;
            Serial.printf("  %2d초  핀 %4u mV  →  배터리 %.3f V\n", i + 1,
                          (unsigned)gBattBoot[i],
                          (gBattBoot[i] / 1000.0f) / rak::kBattDivider);
        }
        Serial.printf("  지금     핀 %4u mV  →  배터리 %.3f V\n",
                      (unsigned)(gBattVolts * rak::kBattDivider * 1000.0f), gBattVolts);
        Serial.println("──────────────────────────────────────────");
        return;
    }
    if (line == "off")                 { goToSleep();   return; }
    if (line.startsWith("off ")) {       // `off 120` — 120초 뒤 스스로 깬다 (시험용)
        goToSleep((uint32_t)line.substring(4).toInt());
        return;
    }

    // sess — 세션 번호를 보거나 고친다.
    //
    // 번호는 NVS 에 남는다 (플래시의 따로 떼어 둔 자리). 전원을 빼도, 앱을
    // 다시 구워도 남는다. 손댈 일은 거의 없지만, 플래시를 통째로 지웠거나
    // 번호를 다시 매기고 싶을 때 쓴다.
    //
    // 낮춰도 카드에 있는 번호보다 작으면 기록을 시작할 때 저절로 올라간다 —
    // 파일을 덮어쓰지 않기 위해서다 (hlog.cpp 의 start 참조).
    if (line == "sess" || line.startsWith("sess ")) {
        Preferences p;
        p.begin("sail", false);
        if (line.length() > 5) {
            const uint32_t n = (uint32_t)line.substring(5).toInt();
            p.putUInt("sess_n", n);
            Serial.printf("[SESS] 다음 세션은 %u 번부터 (카드에 더 큰 번호가 "
                          "있으면 그 다음으로 올라갑니다)\n", (unsigned)(n + 1));
        }
        Serial.printf("[SESS] 마지막으로 쓴 번호 %u\n",
                      (unsigned)p.getUInt("sess_n", 0));
        p.end();
        return;
    }

    // tz — 파일 이름에 쓸 시각의 기울기 (분). 기본 한국 +9시간 = 540
    //
    // 머리글에는 UTC 가 그대로 들어간다. 이름만 사람 보기 좋게 그 고장
    // 시각으로 적는 것이다.
    if (line == "tz" || line.startsWith("tz ")) {
        Preferences p;
        p.begin("sail", false);
        if (line.length() > 3) {
            const int32_t m = (int32_t)line.substring(3).toInt();
            if (m < -720 || m > 840) {
                Serial.println("[TZ] -720 ~ 840 분 사이여야 합니다.");
            } else {
                p.putInt("tz_min", m);
                Serial.printf("[TZ] %+d분 (%+.1f시간) 으로 두었습니다.\n",
                              (int)m, m / 60.0f);
            }
        }
        const int32_t cur = p.getInt("tz_min", 540);
        p.end();
        Serial.printf("[TZ] 지금 %+d분 (%+.1f시간). 파일 이름에만 쓰입니다.\n",
                      (int)cur, cur / 60.0f);
        const time_t now = time(nullptr);
        if (now > 1600000000) {
            struct tm t; gmtime_r(&now, &t);
            Serial.printf("[TZ] 보드 시계 %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            Serial.println("[TZ] 보드 시계가 아직 안 맞았습니다 "
                           "— 위성을 잡으면 맞습니다.");
        }
        return;
    }
    if (line == "imu")                 { doImu();      return; }
    if (line == "sd")                  { doSd();       return; }
    // 어떤 핀에 버튼을 달 수 있나. 그 핀을 누가 이미 쓰고 있는지 재 본다.
    // 짐작하지 않는다 — GPS 가 슬롯 A 의 IO1 로 PPS 를 낼 수도 있다.
    if (line.startsWith("pin ")) {
        const int g = line.substring(4).toInt();

        // 먼저 내부 저항이 듣는지 본다.
        //
        // 풀업만 걸어 보고 HIGH 가 나오면 "풀업이 듣는다" 고 말할 수 없다.
        // 아무것도 안 물린 핀이 그냥 HIGH 로 앉아 있어도 같은 값이 나온다.
        // 풀다운을 걸었을 때 LOW 로 내려가야 내부 저항이 진짜 듣는 것이다.
        //
        // 셋을 나란히 보면 이렇게 읽는다.
        //   풀업 HIGH · 풀다운 LOW   비어 있다. 버튼 달 수 있다
        //   풀업 LOW  · 풀다운 LOW   무언가 LOW 로 붙잡고 있다. 못 쓴다
        //   풀업 HIGH · 풀다운 HIGH  무언가 HIGH 로 붙잡고 있다. 못 쓴다
        pinMode(g, INPUT_PULLUP);   delay(20); const int up   = digitalRead(g);
        pinMode(g, INPUT_PULLDOWN); delay(20); const int down = digitalRead(g);
        pinMode(g, INPUT_PULLUP);   delay(20);
        Serial.printf("[PIN] GPIO%d  풀업 %s · 풀다운 %s  → %s\n", g,
            up == LOW ? "LOW" : "HIGH", down == LOW ? "LOW" : "HIGH",
            (up == HIGH && down == LOW) ? "비어 있음 (버튼 달 수 있다)"
            : (up == LOW && down == LOW) ? "무언가 LOW 로 잡고 있다"
            : (up == HIGH && down == HIGH) ? "무언가 HIGH 로 잡고 있다"
            : "이상한 값");
        Serial.printf("[PIN] GPIO%d 를 5초 봅니다 (내부 풀업). 눌러 보세요.\n", g);
        int last = digitalRead(g);
        uint32_t changes = 0, lowMs = 0, t0 = millis(), lastT = t0;
        while (millis() - t0 < 5000) {
            const int v = digitalRead(g);
            if (v != last) {
                const uint32_t now2 = millis();
                if (last == LOW) lowMs += now2 - lastT;
                lastT = now2;
                last = v;
                ++changes;
            }
            delayMicroseconds(200);
        }
        Serial.printf("[PIN] 바뀐 횟수 %u,  LOW 로 있던 시간 %u ms,  지금 %s\n",
                      (unsigned)changes, (unsigned)lowMs, last == LOW ? "LOW" : "HIGH");
        if (changes == 0 && last == LOW) {
            // 내부 풀업을 걸었는데도 LOW 면 누가 끌어내리고 있다는 뜻이다.
            Serial.println("      ★ 풀업을 걸었는데 LOW 입니다. 누가 이 선을 끌어내리고");
            Serial.println("        있습니다. 버튼을 달면 눌린 것과 구별이 안 됩니다.");
        } else if (changes == 0) {
            Serial.println("      아무도 안 건드립니다 — 버튼 달아도 됩니다.");
        } else if (changes >= 8) {
            Serial.println("      ★ 누가 이 선을 흔들고 있습니다 (PPS 같은 것). 다른 핀을 쓰세요.");
        } else {
            Serial.println("      몇 번 바뀌었습니다. 누른 게 아니라면 다른 핀을 쓰세요.");
        }
        return;
    }

    // USB 시리얼이 얼마나 빠른지 재본다.
    //
    // 파일을 WiFi 대신 USB 로 보낼 만한지 정하려면 숫자가 있어야 한다.
    // WiFi AP 직결이 574 KB/초 였다 (netsrv 의 /api/speed 로 잰 값).
    //
    // 이 보드의 USB 는 UART 다리가 아니라 칩에 붙은 USB 다
    // [확인: 맥에서 "USB JTAG/serial debug unit", VID 303a PID 1001].
    // 그래서 115200 같은 속도 설정은 뜻이 없다. USB 가 낼 수 있는 만큼 나온다.
    if (line == "usbbench" || line.startsWith("usbbench ")) {
        uint32_t kb = 512;
        if (line.length() > 9) kb = (uint32_t)line.substring(9).toInt();
        if (kb < 1) kb = 1;
        if (kb > 8192) kb = 8192;

        static uint8_t buf[1024];
        memset(buf, '.', sizeof(buf));
        Serial.printf("BENCH START %lu\n", (unsigned long)kb);
        Serial.flush();
        const uint32_t t0 = millis();
        for (uint32_t i = 0; i < kb; i++) Serial.write(buf, sizeof(buf));
        Serial.flush();
        const uint32_t dt = millis() - t0;
        Serial.printf("\nBENCH END %lu KB  %.2f초  %.0f KB/초\n",
                      (unsigned long)kb, dt / 1000.0f,
                      dt ? kb * 1000.0f / dt : 0.0f);
        return;
    }

    // WiFi 로 기록 파일 내보내기. 자세히는 netsrv.h.
    if (line == "wifi" || line.startsWith("wifi ")) {
        String arg = line.substring(4);
        arg.trim(); arg.toLowerCase();

        if (arg == "join") {
            if (hlog::recording()) {
                Serial.println("[NET] 기록 중입니다. rec off 먼저 하세요.");
                return;
            }
            netsrv::startJoin();
            return;
        }
        if (arg == "off") {
            netsrv::stop();
            Serial.println("[NET] WiFi 껐습니다.");
            return;
        }
        // 아래는 BLE 설정 통로와 **같은 말**을 쓴다. 앱 없이 여기서 시험한다.
        //   wifi ssid <이름> / wifi pass <비번> / wifi scan / wifi status
        // "wifi on <앱번호>" 처럼 뒤에 번호가 붙을 수 있다. 앱이 붙을 때
        // 쓰는 말과 똑같이 여기서도 시험할 수 있어야 한다.
        if (arg.startsWith("ssid ") || arg.startsWith("pass ") ||
            arg == "scan" || arg == "status" || arg == "on" || arg == "ap" ||
            arg.startsWith("on ") || arg.startsWith("ap ") ||
            arg.startsWith("idle ")) {
            // 대소문자를 이미 내렸다. 이름·비밀번호는 원문이 필요하다.
            String orig = line.substring(5);
            orig.trim();
            controlLine((String("wifi ") + orig).c_str());
            return;
        }

        Serial.println("──────────────────────────────────────────");
        switch (netsrv::mode()) {
            case netsrv::Mode::Off:
                Serial.println("  WiFi          꺼져 있음");
                break;
            case netsrv::Mode::AP:
                Serial.printf("  WiFi          내가 만든 망\n");
                Serial.printf("  이름          %s\n", netsrv::ssidText());
                Serial.printf("  주소          http://%s/\n", netsrv::ipText());
                break;
            case netsrv::Mode::Join:
                Serial.printf("  WiFi          %s 에 붙어 있음\n", netsrv::ssidText());
                Serial.printf("  주소          http://%s/\n", netsrv::ipText());
                break;
        }
        Serial.printf("  보낸 파일     %u개  %.2f MB\n",
                      (unsigned)netsrv::servedFiles(),
                      netsrv::servedBytes() / 1048576.0);
        Serial.println("──────────────────────────────────────────");
        Serial.println("  wifi ap    보드가 스스로 WiFi 를 만든다 (바닷가용)");
        Serial.println("  wifi join  저장된 WiFi 에 붙는다");
        Serial.println("  wifi off   끈다");
        Serial.println("  ─ 아래는 BLE 설정 통로와 같은 말이다 ─");
        Serial.println("  wifi ssid <이름>   붙을 WiFi 이름 (전원 빼도 남는다)");
        Serial.println("  wifi pass <비번>   비밀번호");
        Serial.println("  wifi scan          주변 WiFi 훑기 (BLE 안 내리고 된다)");
        Serial.println("  wifi idle <초>     아무도 안 쓰면 끄기까지 (0 이면 안 끔)");
        Serial.println("  wifi status        지금 상태");
        return;
    }

    if (line == "loopstat") {
        gLoopStat = !gLoopStat;
        Serial.printf("[STAT] 루프 시간 출력 %s\n", gLoopStat ? "켬" : "끔");
        return;
    }

    // 기록 시작·종료·표식. 명세의 "저장 버튼" 자리다 — 보드에 버튼이 없어서
    // 지금은 시리얼로, 다음은 앱으로 한다.
    if (line == "rec" || line.startsWith("rec ")) {
        String arg = line.substring(3);
        arg.trim();
        arg.toLowerCase();

        if (arg == "on" || arg == "start") {
            if (logStartNow()) {
                hlog::Status st; hlog::getStatus(&st);
                Serial.printf("[REC] 시작 — %s\n", st.path);
            } else {
                hlog::Status st; hlog::getStatus(&st);
                Serial.printf("[REC] 시작 못 함 — %s\n",
                              st.lastError ? st.lastError : "알 수 없음");
            }
            return;
        }
        if (arg == "off" || arg == "stop") { hlog::stop(); return; }
        if (arg == "mark")                 { hlog::mark(); return; }
        if (arg == "ls" || arg == "list")  { hlog::listFiles(); return; }
        if (arg.startsWith("rm ")) {
            const long n = arg.substring(3).toInt();
            hlog::removeSession((uint32_t)(n < 0 ? 0 : n));
            return;
        }
        if (arg.startsWith("tail") || arg.startsWith("head")) {
            const bool head = arg.startsWith("head");
            // rec tail            마지막 세션 20줄
            // rec tail 27         27번 세션 20줄
            // rec tail 27 60      27번 세션 60줄
            long sess = 0, n = 20;
            const int sp = arg.indexOf(' ');
            if (sp > 0) {
                const String rest = arg.substring(sp + 1);
                const int sp2 = rest.indexOf(' ');
                sess = (sp2 > 0 ? rest.substring(0, sp2) : rest).toInt();
                if (sp2 > 0) n = rest.substring(sp2 + 1).toInt();
            }
            if (n < 1) n = 1;
            if (n > 400) n = 400;
            hlog::tail((uint32_t)(sess < 0 ? 0 : sess), (uint16_t)n, head);
            return;
        }
        if (arg == "check" || arg.startsWith("check ")) {
            const long n = (arg.length() > 6) ? arg.substring(6).toInt() : 0;
            hlog::verify((uint32_t)(n < 0 ? 0 : n));
            return;
        }

        hlog::Status st;
        hlog::getStatus(&st);
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  카드          %s\n", st.cardPresent ? "있음" : "없음");
        if (!st.recording) {
            Serial.println("  기록          멈춰 있음   (rec on 으로 시작)");
            if (st.lastError) Serial.printf("  지난 오류     %s\n", st.lastError);
            if (st.session)   Serial.printf("  지난 세션     %s  %u+%u줄\n",
                                            st.path, (unsigned)st.navRows,
                                            (unsigned)st.imuRows);
        } else {
            const uint32_t sec = (millis() - st.startedMs) / 1000;
            Serial.printf("  기록 중       %s\n", st.path);
            Serial.printf("  지난 시간     %u분 %u초\n", sec / 60, sec % 60);
            Serial.printf("  NAV 줄        %u   (10 Hz 면 %u 쯤이어야 정상)\n",
                          (unsigned)st.navRows, (unsigned)(sec * 10));
            Serial.printf("  IMU 줄        %u   (100 Hz 면 %u 쯤)\n",
                          (unsigned)st.imuRows, (unsigned)(sec * 100));
            Serial.printf("  쓴 양         %.2f MB\n", st.bytes / 1048576.0);
        }
        Serial.println("  ─── 건강 상태 ───");
        Serial.printf("  버린 줄       %u   %s\n", (unsigned)st.dropped,
                      st.dropped ? "★ 구멍이 났습니다" : "(0 이어야 정상)");
        Serial.printf("  기다린 횟수   %u   (버퍼가 찰 뻔한 횟수)\n",
                      (unsigned)st.waited);
        Serial.printf("  최대 멈춤     %u ms  (카드가 제일 오래 안 놓아준 시간)\n",
                      (unsigned)st.maxStallMs);
        Serial.printf("  버퍼 최고     %u %%\n", (unsigned)st.maxFillPct);
        Serial.printf("  IMU FIFO      %s,  넘칠 뻔 %u번 %s\n",
                      gFifoOn ? "켜짐(칩이 100Hz 로 뜸)" : "꺼짐",
                      (unsigned)gFifoOverrun,
                      gFifoOverrun ? "★ 그때 값이 비었습니다" : "(0 이어야 정상)");
        Serial.println("──────────────────────────────────────────");
        Serial.println("  rec on / rec off / rec mark");
        Serial.println("  rec ls          카드에 있는 파일 목록");
        Serial.println("  rec check [번호]  보드가 직접 되읽어 검사 (기본: 마지막 세션)");
        Serial.println("  rec tail [번호] [줄수]  TXT 사본 끝 몇 줄 (rec head 는 앞부분)");
        Serial.println("  rec rm <번호>     그 세션의 HLG·TXT 를 지운다 (못 되돌린다)");
        return;
    }


    // SD 쓰기 속도 실측. 기본 3600줄 = 10 Hz 로 6분치.
    if (line == "sdbench" || line.startsWith("sdbench ")) {
        long n = (line.length() > 8) ? line.substring(8).toInt() : 3600;
        if (n < 100) n = 100;
        if (n > 2000000) n = 2000000;
        doSdBench((uint32_t)n);
        return;
    }
    if (line == "fix")                 { doFix();      return; }
    if (line == "calib")               { doCalib();    return; }
    if (line == "level")               { doLevel();    return; }

    // 힐과 피치를 어느 가속도 축에서 볼지. 보드를 다는 방법이 바뀌면 여기만 고친다.
    //   heel  y   힐을 Y 축에서
    //   pitch -z  피치를 Z 축에서, 앞뒤 뒤집어서
    // 방위를 만드는 두 축을 바꾼다.
    //
    //   hdg              지금 설정과 값 보기
    //   hdg <A> <B>      atan2(A, B) 로 만든다. 예) hdg -z x
    //   hdg off <도>     0 점 보정 (자기 편각 + 보드 방향 어긋남)
    //
    // 어느 둘이 수평인지는 **돌려 보면** 안다. 케이스를 평평하게 두고 제자리에서
    // 한 바퀴 돌리면, 수평인 두 축은 값이 크게 오르내리고 위아래 축은 거의
    // 그대로다. `mag` 명령이 그걸 보여준다.
    if (line == "hdg" || line.startsWith("hdg ")) {
        String arg = line.substring(3); arg.trim(); arg.toLowerCase();

        auto parseAxis = [](String t, uint8_t& axis, float& sign) -> bool {
            sign = 1.0f;
            if (t.startsWith("-")) { sign = -1.0f; t = t.substring(1); }
            else if (t.startsWith("+")) { t = t.substring(1); }
            if (t == "x") { axis = 0; return true; }
            if (t == "y") { axis = 1; return true; }
            if (t == "z") { axis = 2; return true; }
            return false;
        };

        if (arg.startsWith("off")) {
            String v = arg.substring(3); v.trim();
            gHdgOffsetDeg = v.length() ? v.toFloat() : 0.0f;
            gPrefs.begin("sail", false);
            gPrefs.putFloat("hdg_off", gHdgOffsetDeg);
            gPrefs.end();
            Serial.printf("[IMU] 방위 0점 보정 %+.1f°\n", gHdgOffsetDeg);
        } else if (arg.length() > 0) {
            const int sp = arg.indexOf(' ');
            if (sp <= 0) {
                Serial.println("  hdg <A> <B>   예) hdg -z x   (앞에 - 를 붙이면 뒤집기)");
                Serial.println("  hdg off <도>  0점 보정");
                return;
            }
            uint8_t a = 0, b = 0; float sa = 1.0f, sb = 1.0f;
            String ta = arg.substring(0, sp), tb = arg.substring(sp + 1);
            ta.trim(); tb.trim();
            if (!parseAxis(ta, a, sa) || !parseAxis(tb, b, sb)) {
                Serial.println("  축은 x y z 중에서 고르세요. 예) hdg -z x");
                return;
            }
            if (a == b) {
                Serial.println("  두 축이 같으면 방위가 안 나옵니다. 서로 다른 축이어야 합니다.");
                return;
            }
            gHdgAxisA = a; gHdgAxisB = b; gHdgSignA = sa; gHdgSignB = sb;
            gPrefs.begin("sail", false);
            gPrefs.putUChar("hdg_a", a);  gPrefs.putUChar("hdg_b", b);
            gPrefs.putChar("hdg_sa", sa < 0 ? -1 : 1);
            gPrefs.putChar("hdg_sb", sb < 0 ? -1 : 1);
            gPrefs.end();
        }

        imuUpdate();
        const AxisName aAx(gHdgAxisA, gHdgSignA), bAx(gHdgAxisB, gHdgSignB);
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  지금 자력  %+.1f %+.1f %+.1f µT\n", gMag.x, gMag.y, gMag.z);
        Serial.printf("  방위  atan2(자력 %s, 자력 %s) %+.1f°  →  %.1f°\n",
                      aAx.text, bAx.text, gHdgOffsetDeg, headingDeg());
        Serial.println("──────────────────────────────────────────");
        Serial.println("  케이스를 평평하게 두고 제자리에서 한 바퀴 돌려 보세요.");
        Serial.println("  수평인 두 축은 크게 오르내리고, 위아래 축은 거의 그대로입니다.");
        return;
    }

    if (line == "heel" || line.startsWith("heel ") ||
        line == "pitch" || line.startsWith("pitch ")) {
        const bool isHeel = line.startsWith("heel");
        const char* what  = isHeel ? "힐" : "피치";
        uint8_t& axisRef  = isHeel ? gHeelAxis : gPitchAxis;
        float&   signRef  = isHeel ? gHeelSign : gPitchSign;
        float&   offRef   = isHeel ? gHeelOffsetDeg : gPitchOffsetDeg;

        String arg = line.substring(isHeel ? 4 : 5);
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
                Serial.printf("[IMU] %s x | %s y | %s z (앞에 - 를 붙이면 뒤집기)\n",
                              isHeel ? "heel" : "pitch", isHeel ? "heel" : "pitch",
                              isHeel ? "heel" : "pitch");
                return;
            }

            axisRef = (uint8_t)axis;
            signRef = sign;
            // 축을 바꾸면 옛 기준각은 다른 축에서 잡은 값이라 뜻이 없다.
            offRef  = 0.0f;
            gPrefs.begin("sail", false);
            gPrefs.putUChar(isHeel ? "heel_axis" : "pitch_axis", axisRef);
            gPrefs.putChar(isHeel ? "heel_sgn" : "pitch_sgn", sign < 0.0f ? -1 : 1);
            gPrefs.putFloat(isHeel ? "heel_off2" : "pitch_off", 0.0f);
            gPrefs.end();
            Serial.println("  기준각은 0 으로 되돌렸습니다. 평형일 때 level 을 다시 치세요.");
        }

        imuUpdate();
        const AxisName hAx(gHeelAxis, gHeelSign), pAx(gPitchAxis, gPitchSign);
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  지금 가속   %+.2f %+.2f %+.2f g\n", gAcc.x, gAcc.y, gAcc.z);
        Serial.printf("  힐    가속 %s  기준각 %+.1f°  →  %+.1f° "
                      "(기준각 빼기 전 %+.1f°)\n",
                      hAx.text, gHeelOffsetDeg, currentHeelDeg(), rawHeelDeg());
        Serial.printf("  피치  가속 %s  기준각 %+.1f°  →  %+.1f° "
                      "(기준각 빼기 전 %+.1f°)\n",
                      pAx.text, gPitchOffsetDeg, currentPitchDeg(), rawPitchDeg());
        Serial.println("──────────────────────────────────────────");
        Serial.printf("  배가 평형일 때 %s 축이 0 g 에 가까워야 맞는 축입니다.\n", what);
        Serial.println("  남은 한 축이 위아래를 향하는 축이라 ±1 g 를 읽습니다.");
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
            // 후보 B(GPIO2)는 저장 버튼과 같은 핀이다. 실측으로 이미 탈락한
            // 후보이기도 하다. 여기로 바꾸면 버튼을 누를 때마다 센서 전원이
            // 흔들린다. 막는다.
            if (pin == kButtonPin) {
                Serial.printf("[PWR] GPIO%d 은 저장 버튼 자리입니다. 못 씁니다.\n", pin);
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

    if (line == "lora")      { lora::report();     return; }
    if (line == "lora regs") { lora::reportRegs(); return; }
    if (line == "lora tx")   { lora::txTest();     return; }
    if (line == "lora rssi")  { lora::reportNoise();  return; }
    if (line == "lora watch") { lora::watchToggle();  return; }
    if (line == "lora on")   { lora::begin();      return; }

    // 로라 배 번호. PROTOCOL.md §10.11
    if (line == "boat" || line.startsWith("boat ")) {
        if (line == "boat") {
            if (gBoatId == 0) Serial.println("[BOAT] 번호 없음 — 로라로 안 보낸다");
            else Serial.printf("[BOAT] %u번 (차례 %u)\n", gBoatId, gBoatId - 1);
            return;
        }
        // ★ 달리는 중에는 안 바꾼다. 번호가 바뀌면 말할 차례가 옮겨 가는데,
        //   물 위에서 옮기면 남의 차례에 떨어질 수 있다. 멈춰야 바뀐다.
        if (hlog::recording()) {
            Serial.println("[BOAT] 기록 중에는 못 바꿉니다. 먼저 stop 하세요");
            return;
        }
        String arg = line.substring(5); arg.trim();
        long n = arg.toInt();
        if (arg.length() == 0 || (n == 0 && arg != "0") || n < 0 || n > kBoatIdMax) {
            Serial.printf("[BOAT] 0~%u 로 입력하세요. 0 은 번호 없음. 예) boat 7\n",
                          kBoatIdMax);
            return;
        }
        gBoatId      = (uint8_t)n;
        gBoatIdSetAt = millis();
        gPrefs.begin("sail", false);
        gPrefs.putUChar("boat", gBoatId);
        gPrefs.end();
        if (gBoatId == 0) Serial.println("[BOAT] 번호 없음 — 로라로 안 보낸다");
        else Serial.printf("[BOAT] %u번 (차례 %u). 뱃머리 번호표와 같은지 보세요\n",
                           gBoatId, gBoatId - 1);
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

// netsrv 가 AP 이름으로 쓴다. 헤더를 서로 물게 하지 않으려고 함수 하나로 낸다.
const char* sailFullName() { return gFullName; }

// ── BLE 올리기 / 내리기 ──────────────────────────────────────────────────
//
// WiFi 를 켤 때 BLE 를 내려야 한다. 안 내리면 칩이 이렇게 말하고 죽는다.
//
//   E wifi: Error! Should enable WiFi modem sleep when both WiFi and
//           Bluetooth are enabled!!!!!!
//   abort()
//   [확인: 보드 시리얼 출력 2026-08-24]
//
// 둘이 안테나 하나를 나눠 쓰기 때문이다. 같이 켜면 칩이 번갈아 쓰느라
// WiFi 를 계속 재워야 하고, 그러면 파일 보내는 속도가 209 KB/초에서
// 58 KB/초로 떨어진다 [확인: 2026-08-27 이 보드에서 실측].
//
// 그래서 **파일을 보내는 동안에만** 내린다. 평소에는 WiFi 를 켜 둔 채로
// BLE 도 같이 켜 둔다. 그래야 한 기기가 붙어 있는 동안에도 다른 기기가
// 배 찾기로 이 보드를 찾는다. 자세한 것은 netsrv.cpp 의 fastOn 주석.
void sailBleStart() {
    if (gBleUp) return;

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
    // ★ new 하지 않는다. NimBLE 은 캐릭터리스틱을 지울 때 콜백을 안 지운다
    //   [확인: NimBLE-Arduino 2.5.1 의 ~NimBLECharacteristic 은 디스크립터만
    //    지운다]. WiFi 를 켤 때마다 BLE 를 내렸다 올리므로, new 로 두면
    //    켤 때마다 하나씩 쌓인다. 상태가 없는 물건이라 하나만 두면 된다.
    static TelemetryCallbacks telemetryCb;
    gTelemetryChr->setCallbacks(&telemetryCb);

    // 설정 통로. 써 넣으면 한 줄로 답한다 (PROTOCOL.md §9).
    gControlChr = svc->createCharacteristic(
        sail::kControlUUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    static ControlCallbacks controlCb;      // 위와 같은 이유
    gControlChr->setCallbacks(&controlCb);
    gControlChr->setValue("ready");

    uint8_t initial[sail::kTelemetryExtLen];
    sail::encodeTelemetryExt(gLatest, buildExtra(), initial);
    gTelemetryChr->setValue(initial, sizeof(initial));

    // NimBLE 2.x 에서는 서버가 시작될 때 서비스도 함께 시작된다(svc->start() 는 no-op).
    gServer->start();

    gBleUp = true;
    applyAdvertising();
    gAdvNeedsApply = false;
    // 남은 메모리를 같이 적는다. BLE 를 껐다 켰다 하면 조금씩 새는 일이
    // 있다고 알려져 있다. 세 자리가 계속 줄면 그게 보인다.
    Serial.printf("[BLE] 올렸습니다. 남은 메모리 %lu 바이트\n",
                  (unsigned long)ESP.getFreeHeap());
}

void sailBleStop() {
    if (!gBleUp) return;
    gBleUp = false;                       // 먼저 내려야 loop 가 안 건드린다
    // deinit(true) 는 서버·서비스·characteristic 을 다 지운다.
    // [확인: .pio/libdeps/rak3112/NimBLE-Arduino/src/NimBLEDevice.cpp:1029]
    NimBLEDevice::deinit(true);
    gServer       = nullptr;
    gTelemetryChr = nullptr;
    gControlChr   = nullptr;
    gConnected    = false;
    Serial.printf("[BLE] 내렸습니다 (WiFi 쓰는 동안). 남은 메모리 %lu 바이트\n",
                  (unsigned long)ESP.getFreeHeap());
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


// ── 끊긴 기록을 이어서 시작한다 ──────────────────────────────────────────
//
// 2026-08-30 세션 27 이 29분 만에 그 자리에서 끊겼다. 카드도 배터리도
// 멀쩡했고 코어덤프도 안 남았다 — 전원 쪽이 순간 끊긴 것이다. 그때 기록이
// 통째로 끝나 버렸고, 배 위에서는 아무도 그걸 모른다.
//
// 그래서 켜질 때 표시를 보고 스스로 다시 건다. 재는 데 걸리는 시간은
// 실측으로 켜짐 3.9초 + 기록 시작 0.6초 = **약 4.5초**. 29분 세션의 0.26% 다.
//
// ★ 같은 파일에 이어 붙이지 않는다. 레코드의 local_ms 가 millis() 라
//   다시 켜면 0 부터 시작해서 한 파일 안에서 시간이 거꾸로 간다.
//   새 파일을 만들고 머리글에 앞 세션 번호를 적어 둔다 (hlog.h kOffPrevSession).
//
// ── 되풀이 막이 ──
// 사람이 전원을 끊은 것과 접점이 튄 것을 보드는 구분하지 못한다. 그래서
// 무조건 이어 시작하되, 이어 시작한 횟수를 세어 둔다. 연속으로 이만큼
// 넘으면 뭔가 계속 잘못되고 있는 것이니 멈추고 알린다.
// 1분 넘게 잘 돌면 세던 것을 0 으로 되돌린다 (아래 loop 안).
static constexpr uint8_t  kResumeMax   = 5;
static constexpr uint32_t kResumeOkMs  = 60000;

static uint8_t gResumeTries = 0;   // 0 이면 이어 시작한 게 아니다

static void resumeRecordingIfCut() {
    if (!hlog::cutShort()) {
        // 지난번에 제대로 닫혔다. 세던 것도 지운다.
        gPrefs.begin("sail", false);
        if (gPrefs.getUChar("rec_try", 0)) gPrefs.putUChar("rec_try", 0);
        gPrefs.end();
        return;
    }

    gPrefs.begin("sail", false);
    const uint8_t tries = (uint8_t)(gPrefs.getUChar("rec_try", 0) + 1);
    gPrefs.putUChar("rec_try", tries);
    const uint32_t prev = gPrefs.getUInt("sess_n", 0);
    gPrefs.end();

    if (tries > kResumeMax) {
        Serial.printf("[REC] ★ 이어 시작을 %u번 했는데 계속 끊깁니다 — 멈춥니다.\n", tries - 1);
        Serial.println("      전원선·배터리 접점을 보세요. rec on 으로 직접 걸 수 있습니다.");
        hlog::clearCutFlag();
        gPrefs.begin("sail", false); gPrefs.putUChar("rec_try", 0); gPrefs.end();
        return;
    }

    Serial.printf("[REC] 지난 세션 %u 가 못 닫히고 끊겼습니다 — 이어서 시작합니다 (%u번째)\n",
                  (unsigned)prev, tries);
    if (logStartNow(prev)) {
        gResumeTries = tries;
    } else {
        hlog::Status st; hlog::getStatus(&st);
        Serial.printf("[REC] 이어 시작 못 함 — %s\n", st.lastError ? st.lastError : "알 수 없음");
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────
void setup() {
    // ★ 제일 첫 줄. Serial 보다도 먼저다.
    //   잠자기 전류를 재는 중이면 여기서 전압만 적고 도로 잠들어 아래로 안 온다.
    drainGate();
    probeGate();

    Serial.begin(115200);
    delay(300);

    // ★ 제일 먼저 한다. 깊은잠에서 깼으면 5초를 채웠는지 여기서 가른다.
    //   못 채웠으면 이 안에서 도로 잠들고 아래로 안 내려온다.
    //   플래시에 쓰기 전, 워치독을 걸기 전이어야 한다 (wakeGate 주석 참고).
    wakeGate();

    // 초기화 도중에 멈추는 경우까지 잡으려면 여기서 먼저 켜야 한다.
    // 실제로 화면 초기화에서 멈춰 아무 로그도 없이 죽은 적이 있다.
    reportResetReason();
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
    // ── 켤 때 전압이 낮게 시작해 올라가는 것 ────────────────────────────
    //
    // 깨자마자 재면 2193 mV 인데 몇 분 뒤에는 3550 mV 다. 1.3 V 나 차이 난다
    // [확인: 2026-09-08, off 300 뒤 sleepstat 과 상태줄 비교].
    //
    // 분압 저항이 1 MΩ + 1.5 MΩ 이라 ADC 가 보는 저항이 2.5 MΩ 이다. ADC 안에는
    // 값을 잡아 두는 작은 콘덴서가 있는데 그걸 2.5 MΩ 을 통해 채우려니 느리다.
    // 덜 채워진 상태로 읽으면 낮게 나온다.
    //
    // ── 깬 뒤에는 배터리 값이 낮게 나온다 ───────────────────────────────
    //
    // **짐작하지 않고 쟀다.**
    //
    //   보통 부팅       1회차부터 2133 mV. 처음부터 맞다
    //   깊은잠에서 깸   0.25초에 1178 mV. 몇 초에 걸쳐 2138 mV 로 올라간다
    //
    // 분압 마디에 콘덴서가 있고, 자는 동안 비었다가 메가옴을 통해 다시 차는
    // 것으로 보인다. 저항이 2.5 MΩ 이라 느리다.
    // [확인: 2026-09-08, `off 30` 뒤 `battboot`]
    //
    // 그냥 두면 3.55 V 짜리 배터리가 1.96 V 로 잡힌다. 방전 곡선으로는 0% 다.
    // 바다에서 멀쩡한 배터리를 다 됐다고 판단하게 된다.
    //
    // 얼마나 기다려야 하나. loop 가 1초마다 담은 곡선을 봤다.
    //   1초째 2145 mV,  2초째 2144,  20초째 2143 — 처음부터 평평하다
    //   [확인: 2026-09-08, `off 40` 뒤 `battboot`]
    // 즉 1초면 다 찬다. 그래서 깬 경우에만 1초를 기다린다.
    // 보통 부팅은 처음부터 맞으므로 기다리지 않는다.
    if (gWokeFromSleep) { delay(1000); feedWatchdog(); }
    gBattVolts = readBatteryVolts(nullptr);
    gBattPct   = batteryPercent(gBattVolts);

    // ※ 깬 뒤의 전압은 여기서 안 적는다. setup 안에서 재면 아직 낮게 나온다.
    //   loop 가 1초 뒤에 잰 값을 쓴다 (아래 1f).


    // 기록기. 쓰기 작업을 코어 0 에 띄운다 (SDLOG.md §4)
    gPrefs.begin("sail", false);
    gPrefs.putUInt("boot_n", gPrefs.getUInt("boot_n", 0) + 1);
    gPrefs.end();
    hlog::begin();

    // 깊은잠에서 깼으면 그 기록부터 남긴다. 사람이 명령을 안 쳐도 남게.
    if (gRtcMagic == kRtcMagic && gRtcSleptUs != 0) {
        doSleepStat();
        sleepLogToCard();
    }

    // ── 센서 붙이기 ─────────────────────────────────────────────────────
    gpsBegin();
    Serial.printf("[GPS] UART1 %lubps / %u Hz (RX GPIO%d / TX GPIO%d)\n",
                  (unsigned long)kGpsBaud, gGpsHz, rak::kUART1_RX, rak::kUART1_TX);

    buttonBegin();

    if (imuBegin()) {
        imuFifoBegin();
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

    // 무전기는 부팅 때 올린다. 배에서 명령을 칠 수가 없다.
    // **번호가 있든 없든 늘 받는다.** 보내는 것만 번호가 정한다 (PROTOCOL.md §10.11).
    // 무전기가 없거나 실패해도 보드는 그대로 돌아간다.
    lora::begin();

    Serial.println("[SRC] SOG/COG 는 GPS 가 위성을 잡으면 실측, 못 잡으면 시뮬레이터");
    Serial.println("      HEEL 은 IMU 가 붙어 있으면 언제나 실측");

    gLatest = buildTelemetry(millis());

    sailBleStart();

    digitalWrite(rak::kLedGreen, HIGH); // 초록 = 살아서 광고 중

    // ★ 제일 마지막에 본다. 지난번에 기록 중 전원이 끊겼으면 스스로 다시 건다.
    //   센서·카드가 다 올라온 뒤여야 한다.
    resumeRecordingIfCut();
}

void loop() {
    const uint32_t loopT0 = micros();
    const uint32_t now = millis();

    // 코어 0 의 받기 일꾼이 링버퍼에 넣어 둔 것을 꺼낸다. 여기서 안 꺼내 가면
    // 64개가 차고 나서 버리기 시작한다 (`lora` 의 "버림" 이 0 이 아니게 된다).
    lora::pump();

    static uint32_t lastNotify = 0;
    static uint32_t lastAdv    = 0;
    static uint32_t lastLog    = 0;
    static uint32_t lastImu    = 0;
    static uint32_t lastImuFast = 0;
    static uint32_t lastBatt   = 0;
    static uint32_t lastText   = 0;

    pollSerial();
    netsrv::poll();

    // GPS 는 쉬지 않고 읽는다. UART 버퍼가 넘치면 문장 중간이 잘려 나간다.
    { const uint32_t t = micros(); gpsPoll(); secDone(gStGps, micros() - t); }

    // 1a) 10 ms 마다 FIFO 를 퍼 온다. 값을 뜨는 건 칩이 100 Hz 로 한다.
    //     우리가 늦게 퍼 와도 값들 사이 간격은 칩이 만든 10 ms 그대로다.
    if (now - lastImuFast >= 10) {
        const uint32_t t = micros();
        lastImuFast = now;
        imuDrainFifo();
        secDone(gStImu, micros() - t);
    }

    // 1b) 10 Hz — 자력계까지. 화면과 BLE 가 쓰는 값이다.
    if (now - lastImu >= 100) {
        lastImu = now;
        imuUpdate();
    }

    // 2) 1 Hz — 배터리. ADC 를 16번 재느라 30 ms 쯤 걸려서 자주 하면 손해다.
    //    2.5 MΩ 분압이라 값이 몇 % 씩 흔들린다. 천천히 따라가게 눌러 준다.
    // 이어 시작한 뒤 1분 넘게 멀쩡히 돌면 「계속 끊긴다」가 아니다. 세던 것을 지운다.
    if (gResumeTries && hlog::recording() &&
        now - hlog::recStartedMs() >= kResumeOkMs) {
        gResumeTries = 0;
        gPrefs.begin("sail", false); gPrefs.putUChar("rec_try", 0); gPrefs.end();
        Serial.println("[REC] 이어 시작한 기록이 1분 넘게 멀쩡합니다 — 되풀이 세기를 지웠습니다");
    }

    if (now - lastBatt >= 1000) {
        lastBatt = now;
        const float freshV = readBatteryVolts(nullptr);
        gBattVolts  = gBattVolts * 0.8f + freshV * 0.2f;
        gBattPct    = gBattPct * 0.8f + batteryPercent(freshV) * 0.2f;

        // 같은 주기로 센서가 아직 붙어 있는지도 확인한다.
        // 사라졌으면 끄고 나머지로 계속 간다. 돌아오면 다시 붙는다.
        checkSensors();
        hlog::healthCheck();
    }

    // 1f) 켠 뒤 20초 동안 배터리 값을 1초마다 담아 둔다. `battboot` 가 꺼낸다.
    //     깬 직후에 값이 낮게 나오는 것이 언제 제자리로 오는지 보려는 것이다.
    {
        static int curveN = 0;
        static uint32_t curveAt = 0;
        if (curveN < kBattBootN && now - curveAt >= 1000) {
            curveAt = now;
            readBatteryVolts(&gBattBoot[curveN]);
            // 깼을 때 전압은 **이 값**을 쓴다. setup 안에서 잰 값은 낮게 나온다.
            // 켠 지 1초면 제자리다 [확인: 2026-09-08 battboot 곡선].
            if (curveN == 0 && gWokeFromSleep) {
                gRtcWakeMv = (uint32_t)(gBattBoot[0] / rak::kBattDivider);
                // drain 이 끝나고 처음 켜진 것이면 끝 앵커를 여기서 잡는다
                if (gDrainWantAnchor1 && !gDrainOn) {
                    gDrainWantAnchor1 = 0;
                    gDrainAnchor1 = (uint16_t)(gBattBoot[0] / rak::kBattDivider);
                }
            }
            curveN++;
        }
    }

    // 1e) 저장 버튼. 사람 손가락이라 자주 볼 필요 없다.
    buttonPoll(now);

    // 1d) 초록 LED — 기록 중이면 1초에 한 번 깜박인다 (기능명세 「조작」).
    //     화면이 없는 실제 모듈에서는 이게 유일한 표시가 된다.
    {
        static bool ledOn = false;
        const bool want = hlog::recording() && (now % 1000) < 80;
        if (want != ledOn) { ledOn = want; digitalWrite(rak::kLedGreen, want); }
    }

    // 1c) 10초에 한 번 — 텍스트 사본 한 줄.
    //     카드를 꽂자마자 파서 없이 눈으로 확인하는 용이다.
    if (hlog::recording() && now - lastText >= 10000) {
        lastText = now;
        const uint32_t t = micros();
        logWriteText(now);
        secDone(gStLog, micros() - t);
    }

    // 3) gNotifyPeriodMs 주기 — 값 조립 + characteristic 갱신 + notify
    // ★ 칸을 더해 나간다. `lastNotify = now` 로 두면 한 바퀴(2.8 ms)만큼씩
    //   밀려서 9.85 Hz 가 나왔다 (실측). 많이 밀렸으면 지금부터 다시 센다.
    if (now - lastNotify >= gNotifyPeriodMs) {
        const uint32_t tN = micros();
        lastNotify += gNotifyPeriodMs;
        if (now - lastNotify >= gNotifyPeriodMs * 5) lastNotify = now;
        gpsUpdateFix();
        if (hlog::recording()) logWriteNav(now);
        gLatest = buildTelemetry(now);

        // 12바이트 뒤에 9축과 GPS 상태를 덧붙여 보낸다. 옛 앱은 앞 12바이트만
        // 읽으므로 그대로 돈다 (PROTOCOL.md §7).
        if (gBleUp) {
            uint8_t packet[sail::kTelemetryExtLen];
            sail::encodeTelemetryExt(gLatest, buildExtra(), packet);
            gTelemetryChr->setValue(packet, sizeof(packet)); // Read 용 값도 항상 최신
            if (gConnected) {
                gTelemetryChr->notify(); // 구독자가 없으면 NimBLE 가 알아서 무시
            }
        }
        secDone(gStNotify, micros() - tN);
    }

    // 3b) BLE 로 시킨 WiFi 켜고 끄기.
    //
    // ★ 콜백 안에서 하면 안 된다. NimBLE 안쪽에서 부르는데 거기서 BLE 를
    //   내리면 자기 발등을 찍는다. 깃발만 세워 두고 여기서 처리한다.
    if (gWifiWant) {
        const uint8_t want = gWifiWant;
        gWifiWant = 0;
        // 답이 폰에 닿을 시간을 준다. 이 뒤로 BLE 가 내려간다.
        delay(150);
        if (want == 1) netsrv::startJoin();
        else if (want == 2) netsrv::startAP();
        else netsrv::stop();
    }

    // 4) 광고 모드 전환 (연결/해제 직후, 또는 이름 변경 직후 한 번)
    if (gBleUp && gAdvNeedsApply) {
        gAdvNeedsApply = false;
        applyAdvertising();
    }

    // 5) 1 Hz — 광고 페이로드 갱신
    if (gBleUp && now - lastAdv >= sail::kAdvRefreshMs) {
        lastAdv = now;
        refreshAdvPayload();
    }

    // 6) 화면. 사람 눈에는 4 Hz 로 충분하다.
    //
    // ★ 기록 중에는 1 Hz 로 떨어뜨린다. 한 장 보내는 데 **실측 33.6 ms** 가
    //   걸리고 그동안 I2C 가 묶여서 IMU 를 못 읽는다. 4 Hz 로 그리면 초당
    //   134 ms 를 화면에 쓰고 IMU 가 100 Hz 를 못 채운다 (78 Hz 로 떨어졌다).
    //   1 Hz 면 초당 34 ms 라 IMU 가 99 Hz 를 넘긴다.
    //
    //   실제 모듈에는 화면이 아예 없다 (기능명세 「내구성」: "화면이 없어
    //   대회 중에도 장착 출전 가능"). 이 문제는 개발 보드에만 있다.
    static uint32_t lastDraw = 0;
    const uint32_t drawPeriod = hlog::recording() ? 1000 : 250;
    if (now - lastDraw >= drawPeriod) {
        lastDraw = now;
        sail::DisplayState ds;
        ds.userName     = gUserName;
        ds.bleConnected = gConnected;
        ds.bleNotifying = gSubscribed;
        ds.battVolts    = gBattVolts;
        ds.boatId       = gBoatId;
        ds.recording    = hlog::recording();
        ds.recSeconds   = ds.recording ? (now - hlog::recStartedMs()) / 1000 : 0;

        ds.sogKn      = gLatest.sogKn; // 다듬고 잡음 바닥까지 적용된 값
        // ★ 정해 둔 모드와 다를 때만 화면에 띄운다. 같으면 자리를 안 뺏는다.
        //   모듈은 전원이 오르내리면 기본값 0(휴대)으로 돌아가는데, 그 모드는
        //   저속을 통째로 0 으로 뭉갠다 (세션 24·25·27 실측). 그때 이 낱말이
        //   보이면 물 위에서도 알아챈다.
        ds.gnssMode   = (gGpsDyModel == gGpsDynWant) ? 0 :
            gGpsDyModel == 0 ? 'h' : gGpsDyModel == 1 ? 's' :
            gGpsDyModel == 2 ? 'p' : gGpsDyModel == 3 ? 'c' :
            gGpsDyModel == 4 ? 'b' : '?';
        ds.cogDeg     = gLatest.cogDeg;
        ds.headingDeg = headingDeg();
        ds.heelDeg    = gLatest.heelDeg;
        ds.pitchDeg   = currentPitchDeg();

        ds.imuOk = gImuOk;
        ds.magOk = gMagOk;

        ds.sogValid   = gLatest.sogValid;
        ds.heelValid  = gLatest.heelValid;
        ds.gpsFix     = gGpsFix;
        ds.satellites = gGps.satellites.isValid() ? (int)gGps.satellites.value() : 0;
        ds.hdop       = gGps.hdop.isValid() ? (float)gGps.hdop.hdop() : -1.0f;

        // 버튼이 막대를 그리고 있으면 평소 계기 화면은 건너뛴다.
        // 안 그러면 4 Hz 로 막대를 지워 버린다.
        if (!gBtnOwnsScreen) {
            const uint32_t tD = micros();
            sail::displayUpdate(ds);
            secDone(gStDraw, micros() - tD);
        }
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

        // 움직임 종류를 한 글자로. **늘 찍는다.**
        //
        // 번갈아 재기가 켜졌을 때만 찍었더니 두 가지가 나빴다. 보드를 다시
        // 켜면 번갈아 재기가 꺼져서 글자가 사라졌고, 켜고 끌 때마다 뒤 칸이
        // 두 글자씩 밀려 줄이 안 맞았다. 자리를 고정하고 늘 있게 한다.
        //
        //   h 휴대 · s 정지 · p 보행 · c 자동차 · b 선박 · ? 모름
        const char modeCh =
            gGpsDyModel == 0 ? 'h' : gGpsDyModel == 1 ? 's' :
            gGpsDyModel == 2 ? 'p' : gGpsDyModel == 3 ? 'c' :
            gGpsDyModel == 4 ? 'b' : '?';

        Serial.printf(
            "[%7.1fs] %s | SOG %c %s kn | COG %s° | HEEL %s° | BATT %3d%% %.2fV | seq %3u | %s%s\n",
            now / 1000.0f, gFullName, modeCh, sogTxt,
            cogTxt, heelTxt,
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
        loopStatPrint(sail::kLogPeriodMs);
    }

    // 한 바퀴에 얼마나 걸렸나
    {
        const uint32_t dt = micros() - loopT0;
        if (dt > gLoopMaxUs) gLoopMaxUs = dt;
        ++gLoopCount;
    }

    feedWatchdog(); // 여기까지 왔으면 살아 있다는 뜻
    delay(2);
}
