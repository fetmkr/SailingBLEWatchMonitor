// firmware-idf 3단계 — BLE. 설명과 이름 표는 ble.h.
// 옮긴 원본: firmware-rak/src/main.cpp (커밋 2b18b17)
//   defaultUserName·formatMac·sanitizeName·applyIdentity(327-375) · loadSettings name/notify_ms(380-460) · saveIdentity(463) ·
//   buildAdvData·buildScanData·connectedCount·applyAdvertising·refreshAdvPayload(3884-3953) · ServerCallbacks(3956) ·
//   controlSay(4007) · controlEnqueue·controlPump·ControlCallbacks·TelemetryCallbacks(4152-4221) ·
//   hz 명령 저장(5151) · sailBleStart·sailBleStop(5244-5303) · loop 3)·4)·5)(5772-5818)

#include "ble.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "NimBLEDevice.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "nvs.h"

#include "board_rak.h"   // firmware-rak/include — kLedBlue

using sail::Telemetry;

namespace ble {
namespace {

// ── 이름 ─────────────────────────────────────────────────────────────────
char     sUserName[sail::kMaxUserNameLen + 1] = {0};   // "hojun"
char     sFullName[sail::kMaxFullNameLen + 1] = {0};   // "SAIL-hojun"
uint8_t  sModuleID = 1;
uint32_t sNotifyPeriodMs = sail::kNotifyPeriodMs;

// ── BLE 상태 ─────────────────────────────────────────────────────────────
NimBLEServer*         sServer     = nullptr;
NimBLECharacteristic* sTelemetryChr = nullptr;
NimBLECharacteristic* sControlChr = nullptr;
NimBLECharacteristic* sFleetChr = nullptr;

volatile bool sConnected     = false;   // 중앙장치 연결 여부
volatile bool sAdvNeedsApply = true;    // 광고 모드 재적용 필요
bool          sBleUp         = false;   // BLE 가 올라와 있나 (WiFi 로 파일 보낼 때 내린다)
volatile bool sSubscribed    = false;   // notify 구독 여부(로그용)

uint8_t   sSeq = 0;                     // manufacturer data 시퀀스
Telemetry sLatest;
sail::TelemetryExtra sLatestExtra;

// 설정한 이름이 없을 때의 기본값. MAC 의 **뒤쪽** 바이트 (앞 3바이트는 Espressif OUI 라 모든 보드가 같다).
void defaultUserName(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);   // 폴백
    }
    snprintf(out, cap, "%02X%02X", mac[4], mac[5]);
}

// 광고에 실을 수 있는 문자만 남긴다. 괄호도 받는다 — 이 배 이름이 "random()" 이다.
void sanitizeName(const char* in, char* out, size_t cap) {
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

uint32_t freeHeap() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }   // = Arduino ESP.getFreeHeap()

// ── 광고 데이터 ──────────────────────────────────────────────────────────
// ADV 패킷: Flags + Complete 128-bit Service UUID  (3 + 18 = 21 바이트)
NimBLEAdvertisementData buildAdvData() {
    NimBLEAdvertisementData d;
    d.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);   // 0x06
    d.setCompleteServices(NimBLEUUID(sail::kServiceUUID));
    return d;
}

// Scan Response: Manufacturer Data + Complete Local Name  (14 + 2+N 바이트)
NimBLEAdvertisementData buildScanData(const Telemetry& tm, const sail::TelemetryExtra& extra,
                                      uint8_t seq) {
    uint8_t mfg[2 + sail::kMfgLen];
    sail::encodeManufacturerData(
        tm, seq, mfg, sail::manufacturerStatus(extra.recording, extra.recFailed, extra.boatId));
    NimBLEAdvertisementData d;
    d.setManufacturerData(mfg, sizeof(mfg));
    d.setName(sFullName, /*isComplete=*/true);
    return d;
}

// 연결 상태에 맞춰 광고를 (재)시작한다.
//   자리 남음 → ADV_IND       (connectable + scannable)
//   정원 참   → ADV_SCAN_IND  (non-connectable + scannable, scan response 유지)
// ★ 한 대만 붙어도 연결 불가로 바꾸면 워치가 먼저 붙었을 때 아이폰이 광고밖에 못 읽는다 (실제로 겪었다).
void applyAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();

    // 주의: setConnectableMode/setDiscoverableMode 는 Flags 를 건드리므로 setAdvertisementData() 보다 먼저.
    const bool full = connectedCount() >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    adv->setConnectableMode(full ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND);
    adv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);   // NON 이 아니어야 ADV_SCAN_IND 가 된다
    adv->enableScanResponse(true);
    adv->setMinInterval(sail::kAdvIntervalUnits);
    adv->setMaxInterval(sail::kAdvIntervalUnits);

    adv->setAdvertisementData(buildAdvData());
    adv->setScanResponseData(buildScanData(sLatest, sLatestExtra, sSeq));

    if (!adv->start()) {
        printf("[BLE] !! advertising start 실패\n");
        return;
    }
    printf("[BLE] advertising 시작 — %s (%s, 연결 %u/%d, interval %ums)\n",
           sFullName,
           full ? "ADV_SCAN_IND / non-connectable" : "ADV_IND / connectable",
           connectedCount(), CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
           sail::kAdvIntervalMs);
}

// ── 서버 콜백 ────────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        sConnected     = true;
        sAdvNeedsApply = true;
        gpio_set_level(static_cast<gpio_num_t>(rak::kLedBlue), 1);
        printf("[BLE] 연결됨 ← %s (conn=%u)\n",
               info.getAddress().toString().c_str(), server->getConnectedCount());
        // 높은 notify 주기를 소화하게 연결 파라미터를 조인다. 15~30 ms interval, latency 0, supervision 4 s
        server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    }

    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
        sConnected = server->getConnectedCount() > 0;
        // 남은 연결이 없을 때만 구독을 지운다. 한 대가 나갔다고 나머지 구독까지 지우면 notify 가 멎는다.
        if (!sConnected) sSubscribed = false;
        sAdvNeedsApply = true;   // 루프가 즉시 connectable 광고로 되돌린다
        gpio_set_level(static_cast<gpio_num_t>(rak::kLedBlue), 0);
        printf("[BLE] 연결 끊김 → %s (reason=%d, 남은 연결 %u) — 재광고 준비\n",
               info.getAddress().toString().c_str(), reason, server->getConnectedCount());
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        (void)info;
        printf("[BLE] MTU = %u\n", mtu);
    }
};

// ── 설정 통로로 들어온 줄을 담아 두는 자리 ───────────────────────────────
// ★ BLE 콜백에서 명령을 처리하면 안 된다. 2026-09-11 `magcal stop` 을 BLE 로 받는 순간
//   'nimble_host' 에서 죽었다 [확인: 그날 코어덤프]. 콜백은 줄을 베껴 두기만 한다.
constexpr int kCtlQueueMax = 4;
constexpr int kCtlLineMax  = 192;
char sCtlQueue[kCtlQueueMax][kCtlLineMax];
volatile uint8_t sCtlHead = 0, sCtlTail = 0;

// 콜백에서 부른다. 베끼기만 한다. 꽉 차면 버리고 그 사실을 남긴다.
void controlEnqueue(const char* line, size_t len) {
    const uint8_t next = (uint8_t)((sCtlHead + 1) % kCtlQueueMax);
    if (next == sCtlTail) { printf("[CTL] 줄이 밀렸습니다 — 버립니다\n"); return; }
    snprintf(sCtlQueue[sCtlHead], kCtlLineMax, "%.*s", (int)len, line);
    sCtlHead = next;
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        (void)info;
        const std::string v = chr->getValue();
        // firmware-rak 은 String(v.c_str()) 로 받아 첫 NUL 에서 끊겼다. 같은 뜻으로 첫 NUL 까지만 본다.
        const size_t total = strnlen(v.data(), v.size());
        const char* buf = v.data();
        // 한 번에 여러 줄이 올 수도 있다. '\n' 로 끊고 앞뒤 빈칸(Arduino String::trim = isspace)을 지워 담아만 둔다.
        size_t at = 0;
        while (at < total) {
            size_t nl = at;
            while (nl < total && buf[nl] != '\n') ++nl;
            size_t s = at, e = nl;
            while (s < e && isspace((unsigned char)buf[s])) ++s;
            while (e > s && isspace((unsigned char)buf[e - 1])) --e;
            if (e > s) controlEnqueue(buf + s, e - s);
            at = nl + 1;
        }
    }
};

class TelemetryCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        (void)chr;
        const bool on = (subValue & 0x0001) != 0;   // bit0 = notify
        // 여러 대가 붙을 수 있다. 한 대라도 구독 중이면 계속 내보낸다. 마지막 한 대는 onDisconnect 가 정리한다.
        if (on) sSubscribed = true;
        printf("[BLE] notify 구독 %s ← %s (연결 %u)\n",
               on ? "ON" : "OFF", info.getAddress().toString().c_str(),
               NimBLEDevice::getServer()->getConnectedCount());
    }
};

} // namespace

// ── 이름 ─────────────────────────────────────────────────────────────────
void applyIdentity(const char* name) {
    sanitizeName(name, sUserName, sizeof(sUserName));
    if (sUserName[0] == '\0') defaultUserName(sUserName, sizeof(sUserName));
    snprintf(sFullName, sizeof(sFullName), "%s%s", sail::kNamePrefix, sUserName);
    sModuleID        = sail::moduleIDFromName(sFullName);
    sLatest.moduleID = sModuleID;
}

void loadIdentity() {
    char saved[64] = "";
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
        // Preferences getString 은 한도 없는 String 이었다. 여기서는 64바이트 칸 — 넘으면 nvs_get_str 이 실패하고 기본 이름이 된다.
        //   saveIdentity 는 걸러진 10자까지만 적으므로 정상 보드에서는 안 넘는다.
        size_t len = sizeof saved;
        if (nvs_get_str(h, "name", saved, &len) != ESP_OK) saved[0] = '\0';
        uint32_t ms = sail::kNotifyPeriodMs;
        if (nvs_get_u32(h, "notify_ms", &ms) != ESP_OK) ms = sail::kNotifyPeriodMs;
        sNotifyPeriodMs = ms;
        nvs_close(h);
    }
    if (sNotifyPeriodMs < 10 || sNotifyPeriodMs > 2000) sNotifyPeriodMs = sail::kNotifyPeriodMs;
    if (saved[0]) {
        applyIdentity(saved);
    } else {
        char fallback[sail::kMaxUserNameLen + 1];
        defaultUserName(fallback, sizeof(fallback));
        applyIdentity(fallback);
    }
}

bool saveIdentity(const char* name) {
    applyIdentity(name);
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "name", sUserName) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

const char* userName() { return sUserName; }
const char* fullName() { return sFullName; }
uint8_t     moduleId() { return sModuleID; }

void formatMac(char* out, size_t cap) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

uint32_t notifyPeriodMs() { return sNotifyPeriodMs; }

bool setNotifyPeriodMs(uint32_t ms) {
    sNotifyPeriodMs = ms;
    nvs_handle_t h;
    if (nvs_open("sail", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_u32(h, "notify_ms", ms) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// ── 올리기 · 내리기 ──────────────────────────────────────────────────────
// WiFi 로 파일을 보내는 동안만 내린다 (둘이 안테나 하나를 나눠 써서 209 → 58 KB/초, 2026-08-27 실측).
void start(const Telemetry& t, const sail::TelemetryExtra& e) {
    if (sBleUp) return;
    sLatest = t;
    sLatest.moduleID = sModuleID;
    sLatestExtra = e;

    NimBLEDevice::init(sFullName);
    // firmware-rak 과 같은 인자. setPower 는 dBm 을 받는데 esp_power_level_t 값을 넘기고 있었다 — 뜻은 아래 보고 참고.
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    sServer = NimBLEDevice::createServer();
    sServer->setCallbacks(new ServerCallbacks());   // 서버가 지울 때 같이 지운다 (setCallbacks deleteCallbacks 기본 true)
    sServer->advertiseOnDisconnect(false);          // 광고 재개는 applyAdvertising 이 모드까지 맞춰 한다

    NimBLEService* svc = sServer->createService(sail::kServiceUUID);
    sTelemetryChr = svc->createCharacteristic(sail::kTelemetryUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    // ★ new 하지 않는다. 캐릭터리스틱을 지울 때 콜백은 안 지운다 — 올렸다 내릴 때마다 쌓인다.
    static TelemetryCallbacks telemetryCb;
    sTelemetryChr->setCallbacks(&telemetryCb);

    // 설정 통로. 써 넣으면 한 줄로 답한다 (PROTOCOL.md §9).
    sControlChr = svc->createCharacteristic(
        sail::kControlUUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    static ControlCallbacks controlCb;
    sControlChr->setCallbacks(&controlCb);
    sControlChr->setValue("ready");

    // 수신 전용 보드가 LoRa로 들은 배를 아이패드·맥에 그대로 넘긴다.
    // 앱이 이 특성을 구독하지 않으면 아무 알림도 나가지 않는다.
    sFleetChr = svc->createCharacteristic(sail::kFleetUUID, NIMBLE_PROPERTY::NOTIFY);

    uint8_t initial[sail::kTelemetryExtLen];
    sail::encodeTelemetryExt(sLatest, e, initial);
    sTelemetryChr->setValue(initial, sizeof(initial));

    sServer->start();   // 2.x 는 서버가 시작될 때 서비스도 시작된다

    sBleUp = true;
    applyAdvertising();
    sAdvNeedsApply = false;
    printf("[BLE] 올렸습니다. 남은 메모리 %lu 바이트\n", (unsigned long)freeHeap());
}

void stop() {
    if (!sBleUp) return;
    sBleUp = false;                  // 먼저 내려야 루프가 안 건드린다
    NimBLEDevice::deinit(true);      // 서버·서비스·특성을 다 지운다
    sServer       = nullptr;
    sTelemetryChr = nullptr;
    sControlChr   = nullptr;
    sFleetChr     = nullptr;
    sConnected    = false;
    printf("[BLE] 내렸습니다 (WiFi 쓰는 동안). 남은 메모리 %lu 바이트\n", (unsigned long)freeHeap());
}

bool up() { return sBleUp; }

// ── 루프에서 ─────────────────────────────────────────────────────────────
void pump() {
    if (sBleUp && sAdvNeedsApply) {
        sAdvNeedsApply = false;
        applyAdvertising();
    }
}

void refreshAdvPayload() {
    if (!sBleUp) return;
    sSeq++;
    NimBLEDevice::getAdvertising()->setScanResponseData(buildScanData(sLatest, sLatestExtra, sSeq));
}

void publish(const Telemetry& t, const sail::TelemetryExtra& e) {
    sLatest = t;
    sLatest.moduleID = sModuleID;
    sLatestExtra = e;
    if (!sBleUp) return;
    // 12바이트 뒤에 9축과 GPS 상태를 덧붙인다. 옛 앱은 앞 12바이트만 읽는다 (PROTOCOL.md §7).
    uint8_t packet[sail::kTelemetryExtLen];
    sail::encodeTelemetryExt(sLatest, e, packet);
    sTelemetryChr->setValue(packet, sizeof(packet));   // Read 용 값도 항상 최신
    if (sConnected) sTelemetryChr->notify();           // 구독자가 없으면 NimBLE 가 알아서 무시
}

void publishFleet(const uint8_t payload[22], uint32_t frame, int16_t rssi, int8_t snr) {
    if (!sBleUp || !sFleetChr || !sConnected) return;
    uint8_t out[sail::kFleetLen];
    out[0] = sail::kFleetVersion;
    memcpy(out + 1, payload, 22);
    out[23] = (uint8_t)frame;
    out[24] = (uint8_t)(frame >> 8);
    out[25] = (uint8_t)(frame >> 16);
    out[26] = (uint8_t)(frame >> 24);
    out[27] = (uint8_t)rssi;
    out[28] = (uint8_t)((uint16_t)rssi >> 8);
    out[29] = (uint8_t)snr;
    sFleetChr->setValue(out, sizeof(out));
    sFleetChr->notify();
}

const Telemetry& latest() { return sLatest; }

bool connected()  { return sConnected; }
bool subscribed() { return sSubscribed; }

uint8_t connectedCount() {
    NimBLEServer* srv = NimBLEDevice::getServer();
    return srv ? (uint8_t)srv->getConnectedCount() : 0;
}

void requestAdvApply() { sAdvNeedsApply = true; }

// ── 설정 통로 ────────────────────────────────────────────────────────────
bool takeControlLine(char* out, size_t cap) {
    if (sCtlTail == sCtlHead || cap == 0) return false;
    snprintf(out, cap, "%s", sCtlQueue[sCtlTail]);
    sCtlTail = (uint8_t)((sCtlTail + 1) % kCtlQueueMax);
    return true;
}

void controlSay(const char* line) {
    printf("[CTL] → %s\n", line);
    if (!sControlChr) return;
    sControlChr->setValue((const uint8_t*)line, strlen(line));
    sControlChr->notify();
}

// 1 Hz 시리얼 줄에 찍는 manufacturer data 시퀀스 (firmware-rak gSeq)
uint8_t seq() { return sSeq; }

} // namespace ble
