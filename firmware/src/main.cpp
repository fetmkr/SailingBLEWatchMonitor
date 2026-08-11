// ─────────────────────────────────────────────────────────────────────────
//  Sailing Monitor — 요트 텔레메트리 BLE 테스트 펌웨어 (ESP32-S3 / NimBLE-Arduino 2.x)
//
//  하는 일
//    1. 가상 GPS/자세 데이터를 10 Hz 로 생성
//    2. GATT characteristic 으로 12바이트 패킷 Notify (기본 10 Hz, `hz` 명령으로 조절)
//    3. Advertising 의 Manufacturer Data 를 1 Hz 로 갱신 (연결 없이도 관측 가능)
//    4. 연결 중에는 non-connectable + scannable(ADV_SCAN_IND) 로 광고 유지
//       연결이 끊기면 즉시 connectable(ADV_IND) 로 복귀
//    5. 보드마다 고유한 이름을 갖고 그 이름을 광고에 실어 보낸다
//       → 앱이 여러 모듈 중 "내 것" 을 골라 붙을 수 있다
//
//  시리얼 명령 (115200)
//    name <이름>   보드 이름 설정 (최대 11자). 예) name hojun
//    info          현재 설정 출력
//    help          도움말
//
//  규격: ../../PROTOCOL.md
// ─────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "display.h"
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

// Notify 주기는 런타임에 바꿀 수 있다. 실제로 몇 Hz 까지 나오는지
// 앱의 실측 수신율로 확인하기 위해서다. (기본값은 프로토콜 규정치)
static uint32_t gNotifyPeriodMs = sail::kNotifyPeriodMs;
static Telemetry gLatest;  // 마지막으로 생성한 값

// ── 이름 관리 ────────────────────────────────────────────────────────────

// 설정된 이름이 없을 때의 기본값. MAC 의 **뒤쪽** 바이트를 쓴다.
//
// ★ 주의: MAC 앞 3바이트(예: f4:12:fa)는 Espressif OUI 라 모든 보드가 같다.
//   여기를 쓰면 기본 이름이 전부 겹쳐서 "보드마다 다르게" 라는 목적이 깨진다.
//   ESP.getEfuseMac() 은 MAC[0] 이 최하위 바이트인 uint64 를 돌려주므로
//   `mac & 0xFF` 가 바로 그 OUI 첫 바이트다. 그래서 쓰지 않고,
//   esp_read_mac() 으로 바이트 배열을 받아 뒤쪽 2바이트를 쓴다.
static void defaultUserName(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA); // 폴백
    }
    snprintf(out, cap, "%02X%02X", mac[4], mac[5]);
}

// 진단용 — 전체 MAC 을 사람이 읽는 형식으로
static void formatMac(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 광고에 실을 수 있는 문자만 남긴다 (영숫자, '-', '_').
static void sanitizeName(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < cap; i++) {
        char c = in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
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

static void loadIdentity() {
    gPrefs.begin("sail", /*readOnly=*/true);
    String saved = gPrefs.getString("name", "");
    gNotifyPeriodMs = gPrefs.getUInt("notify_ms", sail::kNotifyPeriodMs);
    gPrefs.end();
    if (gNotifyPeriodMs < 10 || gNotifyPeriodMs > 2000) {
        gNotifyPeriodMs = sail::kNotifyPeriodMs;
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

static void printIdentity() {
    char mac[20];
    formatMac(mac, sizeof(mac));
    Serial.printf("[ID ] 이름 %s | module_id %u (0x%02X) | MAC %s\n",
                  gFullName, gModuleID, gModuleID, mac);
    Serial.printf("[ID ] notify %.1f Hz (%ums) | adv %.1f Hz\n",
                  1000.0f / gNotifyPeriodMs, (unsigned)gNotifyPeriodMs,
                  1000.0f / sail::kAdvRefreshMs);
}

// ── 가상 데이터 생성 ─────────────────────────────────────────────────────
// 생성 로직 본체는 include/simulator.h (호스트에서도 검증 가능한 순수 C++).
// 여기서는 Arduino 난수만 주입한다.
static float arduinoRand01() {
    return (float)random(0, 10001) / 10000.0f; // [0, 1]
}

static Telemetry simulate(uint32_t nowMs) {
    Telemetry t = sail::sim::simulate(nowMs, &arduinoRand01);
    t.moduleID  = gModuleID;
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

    // 주의: setConnectableMode/setDiscoverableMode 는 내부 m_advData 의 Flags 를 건드리므로
    //       반드시 setAdvertisementData() 보다 먼저 호출해야 한다.
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
static void printHelp() {
    Serial.println("──────────────────────────────────────────");
    Serial.println("  name <이름>   보드 이름 설정 (최대 11자, 영숫자/-/_)");
    Serial.println("                예) name hojun  →  SAIL-hojun");
    Serial.println("  hz <1~100>    notify 주기 설정. 예) hz 20  (기본 4)");
    Serial.println("  info          현재 설정 출력");
    Serial.println("  help          이 도움말");
    Serial.println("──────────────────────────────────────────");
}

static void handleCommand(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line == "help" || line == "?") {
        printHelp();
        return;
    }
    if (line == "info") {
        printIdentity();
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
        Serial.println("[ID ] 앱의 실측 수신율과 비교해 보세요.");
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
        // 광고 이름이 바뀌었으니 광고를 다시 올린다.
        gAdvNeedsApply = true;
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

    loadIdentity();

#if SAIL_HAS_TFT
    // 화면을 BLE 보다 먼저 켠다. BLE 초기화가 실패해도 보드가 살아있다는 걸
    // 눈으로 확인할 수 있어야 디버깅이 쉽다.
    sail::displayBegin();
    sail::displayBootMessage(gFullName, "BLE starting...");
#endif

    Serial.println();
    Serial.println("═══════════════════════════════════════════");
    Serial.printf("  %s — 요트 텔레메트리 BLE 테스트\n", gFullName);
    Serial.printf("  module_id %u (0x%02X)\n", gModuleID, gModuleID);
    Serial.printf("  service   %s\n", sail::kServiceUUID);
    Serial.printf("  telemetry %s\n", sail::kTelemetryUUID);
    Serial.printf("  notify %.1fHz / adv refresh %.1fHz\n",
                  1000.0f / gNotifyPeriodMs, 1000.0f / sail::kAdvRefreshMs);
    Serial.println("  이름을 바꾸려면:  name <이름>   (help 로 전체 명령)");
    Serial.println("═══════════════════════════════════════════");

    randomSeed(esp_random());
    gLatest = simulate(millis());

    NimBLEDevice::init(gFullName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // 최대 송신 출력

    gServer = NimBLEDevice::createServer();
    gServer->setCallbacks(new ServerCallbacks());
    // 광고 재개는 applyAdvertising() 이 모드까지 맞춰서 직접 처리한다.
    gServer->advertiseOnDisconnect(false);

    NimBLEService* svc = gServer->createService(sail::kServiceUUID);
    gTelemetryChr = svc->createCharacteristic(
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

#if SAIL_HAS_TFT
    sail::displayBeginMainScreen();
#endif
}

void loop() {
    const uint32_t now = millis();

    static uint32_t lastNotify = 0;
    static uint32_t lastAdv    = 0;
    static uint32_t lastLog    = 0;

    pollSerial();

    // 1) gNotifyPeriodMs 주기 — 데이터 생성 + characteristic 갱신 + notify
    if (now - lastNotify >= gNotifyPeriodMs) {
        lastNotify = now;
        gLatest    = simulate(now);

        uint8_t packet[sail::kTelemetryLen];
        sail::encodeTelemetryPacket(gLatest, packet);
        gTelemetryChr->setValue(packet, sizeof(packet)); // Read 용 값도 항상 최신
        if (gConnected) {
            gTelemetryChr->notify(); // 구독자가 없으면 NimBLE 가 알아서 무시
        }
    }

    // 2) 광고 모드 전환 (연결/해제 직후, 또는 이름 변경 직후 한 번)
    if (gAdvNeedsApply) {
        gAdvNeedsApply = false;
        applyAdvertising();
    }

    // 3) 1 Hz — 광고 페이로드 갱신
    if (now - lastAdv >= sail::kAdvRefreshMs) {
        lastAdv = now;
        refreshAdvPayload();
    }

#if SAIL_HAS_TFT
    // 3.5) 4 Hz — 내장 TFT 갱신. 사람 눈에는 이걸로 충분하고, 더 빨리 그려도 의미 없다.
    static uint32_t lastDraw = 0;
    if (now - lastDraw >= 250) { // 화면은 4 Hz 로 충분하다
        lastDraw = now;
        sail::DisplayState ds;
        ds.name      = gUserName;
        ds.moduleID  = gModuleID;
        ds.connected = gConnected;
        ds.notifying = gSubscribed;
        ds.seq       = gSeq;
        ds.telemetry = gLatest;
        sail::displayUpdate(ds);
    }
#endif

    // 4) 1 Hz — 시리얼 로그
    if (now - lastLog >= sail::kLogPeriodMs) {
        lastLog = now;
        Serial.printf(
            "[%7.1fs] %s | SOG %5.2f kn | COG %5.1f° | HEEL %+6.1f° | BATT %3d%% | seq %3u | %s%s\n",
            now / 1000.0f,
            gFullName,
            gLatest.sogKn,
            gLatest.cogDeg,
            gLatest.heelDeg,
            (int)sail::encodeBatt(gLatest.battPct),
            gSeq,
            gConnected ? "CONNECTED" : "ADVERTISING",
            gConnected ? (gSubscribed ? " (notify ON)" : " (notify OFF)") : "");
    }

    delay(2); // 워치독 여유
}
