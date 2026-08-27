#include "netsrv.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>

#include "board_rak.h"
#include "hlog.h"
#include "secrets.h"

// main.cpp 가 준다. 헤더를 서로 물게 하지 않으려고 함수 하나로 받는다.
extern const char* sailFullName();
// BLE 를 내렸다 올린다. WiFi 와 같이 켜면 칩이 죽는다 — main.cpp 의 주석 참조.
extern void sailBleStop();
extern void sailBleStart();

namespace netsrv {
namespace {

WebServer gServer(80);
Mode      gMode = Mode::Off;
char      gIp[20]   = {0};
char      gSsid[40] = {0};
uint32_t  gServedFiles = 0;
uint64_t  gServedBytes = 0;
bool      gSdUp = false;

// ── 언제 WiFi 를 끄나 ────────────────────────────────────────────────────
//
// BLE 로 "WiFi 켜" 를 시키면 그 순간 BLE 가 내려간다. 그래서 언제 다시
// 끌지가 중요하다. 켠 채로 남으면 전기를 먹고 BLE 도 안 돌아온다.
//
// **시간을 재서 끄는 건 마지막 수단이다.** 네 겹으로 둔다.
//
//   1) 앱이 "다 받았다" 고 말한다        POST /api/wifi/off
//   2) 앱한테서 연락이 끊긴다            아래 "빌림"      ← 평소엔 이게 끈다
//   3) 쓰던 상대가 사라진다              아래 onWifiEvent()
//   4) 아무것도 아니면 시간              gIdleOffMs
//
// ── 빌림(lease) ──
//
// 앱이 `GET /api/ping?lease=15` 로 "15초 동안 빌리겠다" 고 말한다. 그러고는
// 4초마다 다시 부른다. 부르기를 멈추면 15초 뒤에 꺼진다.
//
// 앱이 죽었는지, 노트북을 덮었는지, 배가 멀어졌는지 따질 필요가 없다.
// 요청이 안 오는 것 하나로 충분하다. 사람이 단추를 누를 일도 없다.
//
// 빌린 적이 없으면 이걸 안 본다. 브라우저로 보드 화면을 열어 놓고 읽는
// 사람까지 15초 만에 끊으면 안 되니까. 그때는 4번이 걸린다.
uint32_t  gIdleOffMs = 5 * 60 * 1000;   // 0 이면 안 끈다
uint32_t  gLeaseMs   = 0;               // 0 이면 아무도 안 빌렸다
uint32_t  gLastUse = 0;

// 상대가 떨어졌다. 곧 끈다.
//
// 바로 안 끄고 조금 기다리는 이유 — WiFi 는 잠깐 끊겼다 다시 붙는 일이 흔하다.
// 노트북이 채널을 옮기거나 잠깐 졸 때 그렇다. 그 사이에 꺼 버리면 사람은
// 아무것도 안 했는데 연결이 죽는다.
constexpr uint32_t kGoneGraceMs = 8000;
uint32_t  gGoneAt = 0;          // 0 이면 상대가 붙어 있다

void used() { gLastUse = millis(); gGoneAt = 0; }

// ── 지금 이 보드를 쓰는 기기들 ───────────────────────────────────────────
//
// 보드는 한 번에 한 대만 상대한다. 그래서 다른 기기가 파일을 받는 중이면
// 내 요청은 그냥 멈춰 있다. 실제로 4MB 를 받는 동안 다른 기기의 상태
// 물어보기가 21.7초 기다렸다 [확인: 2026-08-27 실측].
//
// 이유를 모르면 사람은 고장으로 본다. 그래서 누가 있는지 적어 두고 알려준다.
// id 는 앱이 스스로 만든 고유 번호다. 주소로 가리면 두 군데서 틀린다 —
// 공유기가 주소를 바꾸면 같은 앱을 남으로 보고, 한 기기에서 앱 두 개를
// 띄우면 서로 다른 앱을 나로 본다. 그래서 앱이 제 번호를 들고 다닌다.
// 연락(ping)에 ?id= 로 실려 온다. 없으면 빈 글자다 (브라우저로 직접 본 경우).
struct Seen { uint32_t ip; uint32_t at; char id[13]; };
constexpr int      kMaxSeen     = 4;
constexpr uint32_t kForgetMs    = 30000;   // 30초 안 오면 나간 것으로 본다
Seen gSeen[kMaxSeen] = {};

uint32_t gBusyIp = 0;                      // 지금 파일을 받아 가는 기기
char     gBusyFile[40] = {0};

// 방금 끝난 받기. **이게 실제로 쓰이는 값이다.**
//
// 받는 동안에는 보드가 다른 요청을 아예 못 읽는다. 그래서 다른 기기가
// "누가 받고 있나" 를 물어보면 그 답은 받기가 끝난 뒤에야 나간다. 그때는
// busy 가 이미 비어 있다. 대신 방금 누가 무엇을 받았는지를 남겨 두면,
// 오래 기다린 앱이 왜 기다렸는지 사람에게 말해 줄 수 있다.
uint32_t gLastIpDone   = 0;
char     gLastFileDone[40] = {0};
uint32_t gLastDoneAt   = 0;        // millis. 0 이면 아직 없다
uint32_t gLastDoneMs   = 0;        // 얼마나 걸렸나

void sawClient(uint32_t ip, const char* id = nullptr) {
    if (ip == 0) return;
    const uint32_t now = millis();
    int oldest = 0;
    for (int i = 0; i < kMaxSeen; i++) {
        if (gSeen[i].ip == ip) {
            gSeen[i].at = now;
            if (id && *id) snprintf(gSeen[i].id, sizeof(gSeen[i].id), "%s", id);
            return;
        }
        if (gSeen[i].ip == 0) {
            gSeen[i].ip = ip; gSeen[i].at = now;
            snprintf(gSeen[i].id, sizeof(gSeen[i].id), "%s", (id && *id) ? id : "");
            return;
        }
        if (gSeen[i].at < gSeen[oldest].at) oldest = i;
    }
    gSeen[oldest].ip = ip;
    gSeen[oldest].at = now;
    snprintf(gSeen[oldest].id, sizeof(gSeen[oldest].id), "%s", (id && *id) ? id : "");
}

int seenCount() {
    const uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < kMaxSeen; i++) {
        if (gSeen[i].ip == 0) continue;
        if (now - gSeen[i].at > kForgetMs) { gSeen[i].ip = 0; continue; }
        n++;
    }
    return n;
}

void ipText4(uint32_t ip, char* out, size_t n) {
    if (ip == 0) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%lu.%lu.%lu.%lu",
             (unsigned long)(ip & 0xFF), (unsigned long)((ip >> 8) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF), (unsigned long)((ip >> 24) & 0xFF));
}

/**
 * 이 번호를 뺀 나머지 기기 수. 번호가 빈 글자면 아무도 안 뺀다.
 *
 * 앱은 이 값만 보면 된다. 0 이면 나 혼자니 붙어도 되고, 1 이상이면 남이
 * 쓰고 있으니 안 붙는다. 견주는 일을 보드가 하므로 앱이 헷갈릴 자리가 없다.
 */
int usersExcept(const char* id) {
    const uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < kMaxSeen; i++) {
        if (gSeen[i].ip == 0) continue;
        if (now - gSeen[i].at > kForgetMs) { gSeen[i].ip = 0; continue; }
        if (id && *id && strcmp(gSeen[i].id, id) == 0) continue;   // 나다
        n++;
    }
    return n;
}

/**
 * 나 말고 쓰고 있는 기기 하나를 사람이 알아볼 이름으로. 사람에게 보여주려는
 * 것뿐이다. 앱 번호가 있으면 그걸 쓴다 ("ios-8c21"). 브라우저로 직접 본
 * 경우처럼 번호가 없으면 주소를 쓴다.
 */
const char* otherName(const char* id) {
    static char buf[24];
    buf[0] = 0;
    const uint32_t now = millis();
    for (int i = 0; i < kMaxSeen; i++) {
        if (gSeen[i].ip == 0) continue;
        if (now - gSeen[i].at > kForgetMs) { gSeen[i].ip = 0; continue; }
        if (id && *id && strcmp(gSeen[i].id, id) == 0) continue;
        if (gSeen[i].id[0]) snprintf(buf, sizeof(buf), "%s", gSeen[i].id);
        else                ipText4(gSeen[i].ip, buf, sizeof(buf));
        return buf;
    }
    return buf;
}


/** 상태와 연락 답에 같이 붙이는 조각. */
void whoJson(char* out, size_t n) {
    char busy[20] = {0}, last[20] = {0}, you[20] = {0};
    ipText4(gBusyIp, busy, sizeof(busy));
    ipText4(gLastIpDone, last, sizeof(last));
    // 묻는 쪽이 자기 주소를 알아야 "남이 받았다" 와 "내가 받았다" 를 가른다.
    ipText4((uint32_t)gServer.client().remoteIP(), you, sizeof(you));
    snprintf(out, n,
             "\"you\":\"%s\",\"users\":%d,\"busy\":\"%s\",\"busy_file\":\"%s\","
             "\"last_ip\":\"%s\",\"last_file\":\"%s\",\"last_ago_s\":%lu,"
             "\"last_took_ms\":%lu",
             you, seenCount(), busy, gBusyFile, last, gLastFileDone,
             gLastDoneAt ? (unsigned long)((millis() - gLastDoneAt) / 1000) : 99999UL,
             (unsigned long)gLastDoneMs);
}

// ── 파일 보낼 때만 빠르게 ────────────────────────────────────────────────
//
// 평소에는 BLE 를 켜 둔다. 그래야 아이패드가 붙어 있는 동안에도 데스크탑이
// 배 찾기로 이 보드를 찾는다. DJI 와 GoPro 도 이렇게 한다.
//
// 그런데 둘을 같이 켜면 WiFi 절전을 켜 둬야 한다. 그러면 파일 보내는 속도가
// 209 KB/초에서 58 KB/초로 떨어진다 [확인: 2026-08-27 이 보드에서 실측].
//
// 그래서 파일을 보내는 동안만 BLE 를 내리고 절전을 끈다. 다 보내고 5초가
// 지나면 되돌린다. 5초를 두는 이유는 파일을 여러 개 이어서 받을 때 매번
// 껐다 켜지 않게 하려는 것이다.
//
// BLE 를 껐다 켜도 메모리는 안 준다. 8번 돌려서 1바이트도 안 줄어드는 것을
// 확인했다 [확인: 2026-08-27, heap_caps_get_free_size 로 재봄].
bool     gFast   = false;
uint32_t gFastAt = 0;
constexpr uint32_t kFastHoldMs = 5000;

void fastOn() {
    gFastAt = millis();
    if (gFast) return;
    // ★ 순서를 바꾸면 칩이 죽는다. BLE 가 켜진 채로 절전을 끄면 이렇게 말하고
    //   abort() 한다 — "Should enable WiFi modem sleep when both WiFi and
    //   Bluetooth are enabled!!!!!!" (main.cpp 의 sailBleStop 주석 참조).
    ::sailBleStop();
    WiFi.setSleep(false);
    gFast = true;
    Serial.println("[NET] 파일 보내는 동안 BLE 를 내립니다.");
}

void fastOff() {
    if (!gFast) return;
    WiFi.setSleep(true);      // 절전을 먼저 켜고
    ::sailBleStart();         // 그 다음에 BLE 를 올린다
    gFast = false;
}

// ── WiFi 이름·비밀번호 ──────────────────────────────────────────────────
//
// 예전에는 secrets.h 에 박아 두고 다시 구웠다. 배가 30대면 대회장 WiFi 가
// 바뀔 때마다 30대를 노트북에 꽂아야 한다.
//
// 이제 NVS 에 둔다. BLE 로 넣을 수 있다 (PROTOCOL.md §9).
// NVS 가 비어 있으면 secrets.h 값을 쓴다 — 내 책상에서는 그게 편하다.
Preferences gWifiPrefs;
char gStaSsid[33] = {0};
char gStaPass[65] = {0};

// WiFi 쪽에서 오는 사건. 상대가 붙고 떨어지는 것을 여기서 안다.
void onWifiEvent(arduino_event_id_t ev) {
    switch (ev) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.printf("[NET] 붙었습니다 (%u대)\n", WiFi.softAPgetStationNum());
            used();
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            if (WiFi.softAPgetStationNum() == 0) {
                Serial.println("[NET] 쓰던 기기가 떨어졌습니다. 곧 끕니다.");
                gGoneAt = millis();
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // 붙어 있던 공유기를 놓쳤다. 이 상태로는 아무것도 못 한다.
            if (gMode == Mode::Join) {
                Serial.println("[NET] WiFi 를 놓쳤습니다. 곧 끕니다.");
                gGoneAt = millis();
            }
            break;
        default: break;
    }
}

// 지난번에 받았던 주소.
//
// BLE 로 "WiFi 켜" 를 시키면 그 순간 BLE 가 끊겨서, 새로 받은 주소를 앱에
// 알려줄 길이 없다. 이름(mDNS)으로 찾으라고 하는데 그게 늘 빠르진 않다.
// 실측으로 2.6초 걸렸다. 맥이 로컬 네트워크 권한을 안 주면 아예 못 찾는다.
//
// 그래서 지난번 주소를 남겨 두고 미리 알려준다. 공유기는 대개 같은 주소를
// 다시 준다. 앱은 이름과 이 주소를 같이 두드려서 먼저 답하는 쪽을 쓴다.
char gLastIp[20] = {0};

void loadLastIp() {
    gWifiPrefs.begin("wifi", true);
    String v = gWifiPrefs.getString("lastip", "");
    gWifiPrefs.end();
    snprintf(gLastIp, sizeof(gLastIp), "%s", v.c_str());
}

void saveLastIp(const char* ip) {
    if (!ip || !*ip) return;
    if (strcmp(gLastIp, ip) == 0) return;      // 안 바뀌었으면 안 쓴다
    gWifiPrefs.begin("wifi", false);
    gWifiPrefs.putString("lastip", ip);
    gWifiPrefs.end();
    snprintf(gLastIp, sizeof(gLastIp), "%s", ip);
}

void loadCreds() {
    gWifiPrefs.begin("wifi", /*readOnly=*/true);
    String ss = gWifiPrefs.getString("ssid", "");
    String pw = gWifiPrefs.getString("pass", "");
    gWifiPrefs.end();
    if (ss.length() == 0) { ss = SAIL_WIFI_SSID; pw = SAIL_WIFI_PASS; }
    snprintf(gStaSsid, sizeof(gStaSsid), "%s", ss.c_str());
    snprintf(gStaPass, sizeof(gStaPass), "%s", pw.c_str());
}

// AP 비밀번호. secrets.h 에 없으면 여기 기본값을 쓴다.
// ★ 진짜 값은 secrets.h 에만 둔다. 이 파일은 커밋된다.
#ifndef SAIL_AP_PASS
#define SAIL_AP_PASS "sailing1234"
#endif

bool sdUp() {
    if (gSdUp) return true;
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    gSdUp = SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5);
    return gSdUp;
}

void sdDown() {
    if (gSdUp) { SD.end(); gSdUp = false; }
}

// 브라우저나 데스크탑 앱이 다른 출처에서 부를 수 있게 열어 둔다.
// 이 서버는 우리 보드가 만든 닫힌 망에만 있고 비밀도 없다.
void cors() {
    used();
    sawClient((uint32_t)gServer.client().remoteIP());
    gServer.sendHeader("Access-Control-Allow-Origin", "*");
    gServer.sendHeader("Access-Control-Allow-Headers", "*");
    gServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

void handleStatus() {
    cors();
    hlog::Status st;
    hlog::getStatus(&st);

    char who[200];
    whoJson(who, sizeof(who));

    char body[768];
    const int n = snprintf(body, sizeof(body),
        "{\"ok\":true,"
        "\"name\":\"%s\","
        "\"uptime_ms\":%lu,"
        "\"recording\":%s,"
        "\"session\":%lu,"
        "\"nav_rows\":%lu,"
        "\"imu_rows\":%lu,"
        "\"dropped\":%lu,"
        "\"max_stall_ms\":%lu,"
        "\"card\":%s,"
        "\"free_mb\":%llu,"
        "%s}",
        gSsid,
        (unsigned long)millis(),
        st.recording ? "true" : "false",
        (unsigned long)st.session,
        (unsigned long)st.navRows,
        (unsigned long)st.imuRows,
        (unsigned long)st.dropped,
        (unsigned long)st.maxStallMs,
        st.cardPresent ? "true" : "false",
        (unsigned long long)(gSdUp ? (SD.totalBytes() - SD.usedBytes()) / 1048576ULL : 0),
        who);
    gServer.send(200, "application/json", body);
    (void)n;
}

// 파일 목록 + 요약.
//
// ★ 이름과 크기만 주면 안 된다. 한 세션이 90 MB 인데 "이게 뭔지" 를 알려고
//   90 MB 를 받아 볼 수는 없다 (TRANSFER.md §1).
//
// 그래서 파일마다 **머리글 128바이트만** 읽어서 요약까지 준다. 128바이트면
// 순식간이고, 코치는 이것만 보고 뭘 받을지 정할 수 있다.
void handleFiles() {
    cors();
    if (!sdUp()) {
        gServer.send(503, "application/json",
                     "{\"ok\":false,\"error\":\"카드를 못 읽습니다\"}");
        return;
    }

    gServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    gServer.send(200, "application/json", "");
    gServer.sendContent("{\"ok\":true,\"files\":[");

    File dir = SD.open("/LOGS");
    bool first = true;
    char item[512];
    while (File e = dir.openNextFile()) {
        const String nm = e.name();
        const uint32_t sz = e.size();

        if (!nm.endsWith(".HLG")) { e.close(); continue; }  // TXT 는 사본이라 뺀다

        uint8_t h[hlog::kHeaderSize];
        const bool okHdr = (e.read(h, hlog::kHeaderSize) == (int)hlog::kHeaderSize) &&
                           (memcmp(h, "HHLG", 4) == 0);
        e.close();

        if (!okHdr) {
            snprintf(item, sizeof(item),
                     "%s{\"name\":\"%s\",\"size\":%lu,\"ok\":false}",
                     first ? "" : ",", nm.c_str(), (unsigned long)sz);
            gServer.sendContent(item);
            first = false;
            continue;
        }

        uint32_t session, utcStart, durS, navRows, imuRows, dropped;
        uint16_t utcMs;
        memcpy(&session,  h + 18, 4);
        memcpy(&utcStart, h + 24, 4);
        memcpy(&utcMs,    h + 28, 2);
        memcpy(&durS,     h + hlog::kOffDurationS, 4);
        memcpy(&navRows,  h + hlog::kOffNavRows, 4);
        memcpy(&imuRows,  h + hlog::kOffImuRows, 4);
        memcpy(&dropped,  h + hlog::kOffDropped, 4);

        // 제대로 닫힌 파일이 아니면 위 값들이 0 이다. 크기로 어림잡아 준다.
        // 데스크탑 앱이 "대충 이만한 세션" 이라도 알아야 고를 수 있다.
        const bool closed = (h[hlog::kOffClosed] == 1);
        if (!closed && sz > hlog::kHeaderSize) {
            const uint32_t imuSz = (h[5] >= 1) ? hlog::kImuSize : hlog::kImuSizeV0;
            const uint32_t perSec = hlog::kNavSize * hlog::kRateNav +
                                    imuSz * hlog::kRateImu;
            durS = (sz - hlog::kHeaderSize) / perSec;
        }

        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"size\":%lu,\"ok\":true,"
                 "\"closed\":%s,"
                 "\"session\":%lu,"
                 "\"module\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"utc_start\":%lu,\"utc_start_ms\":%u,"
                 "\"duration_s\":%lu,"
                 "\"nav_rows\":%lu,\"imu_rows\":%lu,\"dropped\":%lu,"
                 "\"imu_type\":%u,\"gnss_dyn\":%u,"
                 "\"nav_hz\":%u,\"imu_hz\":%u,"
                 "\"fixed\":%s}",
                 first ? "" : ",", nm.c_str(), (unsigned long)sz,
                 closed ? "true" : "false",
                 (unsigned long)session,
                 h[8], h[9], h[10], h[11], h[12], h[13],
                 (unsigned long)utcStart, utcMs,
                 (unsigned long)durS,
                 (unsigned long)navRows, (unsigned long)imuRows,
                 (unsigned long)dropped,
                 h[hlog::kOffImuType], h[hlog::kOffGnssDyn],
                 h[39], h[40],
                 utcStart ? "true" : "false");
        gServer.sendContent(item);
        first = false;
    }
    dir.close();
    gServer.sendContent("]}");
    gServer.sendContent("");
}

// 파일 하나 보내기.
//
// Range 를 받는다. 90 MB 를 보내다가 끊기면 처음부터 다시 받는 건 낭비다.
// 데스크탑 앱이 받다 만 지점부터 이어받을 수 있어야 한다.
void handleFile() {
    cors();
    if (hlog::recording()) {
        gServer.send(409, "application/json",
                     "{\"ok\":false,\"error\":\"기록 중에는 못 보냅니다\"}");
        return;
    }
    if (!sdUp()) {
        gServer.send(503, "application/json",
                     "{\"ok\":false,\"error\":\"카드를 못 읽습니다\"}");
        return;
    }

    // ── 받기는 한 번에 한 대만 ────────────────────────────────────────
    //
    // 보드는 한 번에 한 대만 상대한다. 둘이 같이 받으면 둘 다 느려지고,
    // 나중 사람은 왜 느린지 모른다. 그래서 먼저 온 쪽이 끝낼 때까지
    // 나중 사람은 거절한다. **빼앗기는 없다.**
    //
    // 자물쇠는 "붙어 있는 동안" 이 아니라 **"실제로 보내는 동안"** 만 잡는다.
    // 앱을 켜 놓고 노는 사람이 배를 잠가 버리면 안 된다. 그래서 잠기는
    // 최대 시간은 파일 하나 보내는 시간이다. 받아 가던 기기가 사라지면
    // 아래 connected() 검사가 바로 푼다.
    const uint32_t meIp = (uint32_t)gServer.client().remoteIP();
    if (gBusyIp && gBusyIp != meIp) {
        char owner[20]; ipText4(gBusyIp, owner, sizeof(owner));
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"다른 기기가 받는 중입니다\","
                 "\"owner\":\"%s\",\"owner_file\":\"%s\"}", owner, gBusyFile);
        gServer.send(409, "application/json", body);
        return;
    }

    String uri = gServer.uri();          // "/file/S00008.HLG"
    String name = uri.substring(6);
    // 위로 올라가는 경로를 막는다. /LOGS 밖은 못 준다.
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.length() == 0) {
        gServer.send(400, "application/json",
                     "{\"ok\":false,\"error\":\"이름이 이상합니다\"}");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "/LOGS/%s", name.c_str());
    File f = SD.open(path, FILE_READ);
    if (!f) {
        gServer.send(404, "application/json",
                     "{\"ok\":false,\"error\":\"그런 파일이 없습니다\"}");
        return;
    }

    const uint32_t total = f.size();
    uint32_t from = 0, to = total - 1;

    if (gServer.hasHeader("Range")) {
        const String r = gServer.header("Range");   // "bytes=1000-"
        const int eq = r.indexOf('=');
        const int dash = r.indexOf('-');
        if (eq >= 0 && dash > eq) {
            from = (uint32_t)r.substring(eq + 1, dash).toInt();
            const String tail = r.substring(dash + 1);
            if (tail.length()) to = (uint32_t)tail.toInt();
        }
        if (from >= total) {
            f.close();
            gServer.send(416, "text/plain", "");
            return;
        }
        if (to >= total) to = total - 1;
    }

    const uint32_t len = to - from + 1;
    f.seek(from);

    char cr[64];
    snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu",
             (unsigned long)from, (unsigned long)to, (unsigned long)total);
    gServer.sendHeader("Accept-Ranges", "bytes");
    if (from != 0 || to != total - 1) gServer.sendHeader("Content-Range", cr);

    fastOn();          // 보내는 동안만 BLE 를 내리고 절전을 끈다
    gBusyIp = meIp;                      // 여기서 잠근다. 위 갈래로 빠지면 안 잠긴다
    snprintf(gBusyFile, sizeof(gBusyFile), "%s", name.c_str());
    const uint32_t t0 = millis();
    gServer.setContentLength(len);
    gServer.send((from == 0 && to == total - 1) ? 200 : 206,
                 "application/octet-stream", "");

    // 4 KB 씩 흘려보낸다. SD 읽기와 WiFi 보내기가 같은 크기라 편하다.
    //
    // ★ static 이어야 한다. 그냥 두면 보드가 죽는다.
    //   loop() 를 도는 자리의 스택이 8 KB 인데 여기서 4 KB 를 차지한다.
    //   컴파일러는 이 자리를 함수에 들어서는 순간 잡아 두므로, 아래 while 에
    //   닿기 전인 SD.open() 에서 이미 넘친다. 실제로 이렇게 죽었다.
    //     Stack canary watchpoint triggered (loopTask)
    //     netsrv.cpp:317  SD.open → vfs_fat_stat → snprintf
    //   [확인: 2026-08-27, addr2line 으로 위 주소를 풀어 봤다]
    //
    //   이 자리는 loop() 하나에서만 부르니 여럿이 같이 쓸 걱정은 없다.
    //   바로 아래 handleSpeed() 도 같은 이유로 static 이다.
    //
    //   한동안 멀쩡했던 이유도 적어 둔다. 처음 만들 때는 loop() 가 스택을
    //   320 바이트만 썼다. 그 뒤로 loop() 가 커져서 2032 바이트가 됐고,
    //   남아 있던 여유 1472 바이트를 다 먹고 240 바이트를 더 넘겼다.
    //   [확인: objdump 로 두 판의 entry 명령을 읽어 견줬다.
    //          a66ce0a 는 loop 0x140 / handleFile 0x1160,
    //          지금은  loop 0x7f0 / handleFile 0x170]
    //
    //   교훈은 하나다. **스택에 KB 단위를 올리지 않는다.** 남은 자리는
    //   내 함수가 아니라 부르는 쪽이 정하므로, 오늘 되는 것이 내일 죽는다.
    static uint8_t buf[4096];
    uint32_t left = len;
    uint32_t usRead = 0, usWrite = 0;    // 어디서 시간을 쓰는지 갈라 본다
    while (left > 0) {
        const size_t want = (left > sizeof(buf)) ? sizeof(buf) : left;
        uint32_t t = micros();
        const int got = f.read(buf, want);
        usRead += micros() - t;
        if (got <= 0) break;
        // 받아 가던 기기가 사라졌으면 그만둔다.
        //
        // 이게 없으면 90 MB 를 허공에 다 읽어 보낸다. write() 는 실패해도
        // left 는 줄어드니 언젠가 끝나긴 하지만, 그동안 자물쇠가 잡혀 있어서
        // 다음 사람이 그만큼 기다린다.
        if (!gServer.client().connected()) {
            Serial.println("[NET] 받아 가던 기기가 사라져서 그만둡니다.");
            break;
        }
        t = micros();
        gServer.client().write(buf, (size_t)got);
        usWrite += micros() - t;
        left -= (uint32_t)got;
        used();          // 90 MB 를 보내는 중에 저절로 꺼지면 안 된다
        fastOn();        // 보내는 내내 빠른 구간을 붙잡아 둔다
    }
    f.close();

    const uint32_t dt = millis() - t0;
    // 방금 누가 무엇을 받았는지 남긴다. 줄 서서 기다린 앱이 이걸 보고
    // 왜 기다렸는지 사람에게 말해 준다.
    gLastIpDone = gBusyIp;
    snprintf(gLastFileDone, sizeof(gLastFileDone), "%s", gBusyFile);
    gLastDoneAt = millis();
    gLastDoneMs = dt;
    gBusyIp = 0;
    gBusyFile[0] = 0;

    ++gServedFiles;
    gServedBytes += (len - left);
    Serial.printf("[NET] %s  %lu 바이트  %.1f초  %.0f KB/초"
                  "  (SD 읽기 %.2f초  WiFi 쓰기 %.2f초)\n",
                  path, (unsigned long)(len - left), dt / 1000.0f,
                  dt ? (len - left) / 1.024f / dt : 0.0f,
                  usRead / 1e6f, usWrite / 1e6f);
}

// 지우기. **파일 안의 세션 번호를 확인 값으로 받는다.**
//
// URL 을 잘못 쳐서 남의 훈련이 날아가면 안 된다. 이름만으로는 못 지운다.
// 데스크탑 앱은 받아서 CRC 검사까지 끝난 뒤에만 이걸 부른다 (TRANSFER.md §4).
void handleDelete() {
    cors();
    if (hlog::recording()) {
        gServer.send(409, "application/json",
                     "{\"ok\":false,\"error\":\"기록 중에는 못 지웁니다\"}");
        return;
    }
    if (!sdUp()) { gServer.send(503, "application/json", "{\"ok\":false}"); return; }
    if (!gServer.hasArg("confirm")) {
        gServer.send(400, "application/json",
                     "{\"ok\":false,\"error\":\"confirm=<세션번호> 가 있어야 합니다\"}");
        return;
    }

    String name = gServer.uri().substring(6);
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.length() == 0) {
        gServer.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/LOGS/%s", name.c_str());

    File f = SD.open(path, FILE_READ);
    if (!f) { gServer.send(404, "application/json", "{\"ok\":false}"); return; }
    uint8_t h[hlog::kHeaderSize];
    const bool okHdr = (f.read(h, hlog::kHeaderSize) == (int)hlog::kHeaderSize) &&
                       (memcmp(h, "HHLG", 4) == 0);
    uint32_t session = 0;
    if (okHdr) memcpy(&session, h + 18, 4);
    f.close();

    if (!okHdr || (uint32_t)gServer.arg("confirm").toInt() != session) {
        gServer.send(403, "application/json",
                     "{\"ok\":false,\"error\":\"세션 번호가 안 맞습니다\"}");
        return;
    }

    const bool gone = SD.remove(path);
    // 같은 이름의 텍스트 사본도 같이 지운다
    if (gone) {
        char txt[64];
        snprintf(txt, sizeof(txt), "/LOGS/%s", name.c_str());
        const int dot = (int)strlen(txt) - 4;
        if (dot > 0) { strcpy(txt + dot, ".TXT"); SD.remove(txt); }
        Serial.printf("[NET] 지웠습니다 — %s (세션 %lu)\n",
                      path, (unsigned long)session);
    }
    gServer.send(gone ? 200 : 500, "application/json",
                 gone ? "{\"ok\":true}" : "{\"ok\":false}");
}

void handleRec() {
    cors();
    const String uri = gServer.uri();
    if (uri.endsWith("/mark")) {
        hlog::mark();
        gServer.send(200, "application/json", "{\"ok\":true}");
        return;
    }
    // 시작·종료는 시리얼·버튼과 같은 길을 타야 해서 여기서 직접 하지 않는다.
    // main.cpp 가 넘겨준 함수를 부른다 (아래 setRecControl).
    gServer.send(501, "application/json",
                 "{\"ok\":false,\"error\":\"아직 안 만들었습니다\"}");
}

void handleRoot() {
    cors();
    char body[640];
    snprintf(body, sizeof(body),
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:-apple-system,sans-serif;padding:16px;line-height:1.7}"
        "a{display:block;padding:6px 0}</style>"
        "<h2>%s</h2>"
        "<p>기록 파일 내려받기</p>"
        "<a href='/api/files'>파일 목록 (JSON)</a>"
        "<a href='/api/status'>지금 상태 (JSON)</a>"
        "<p style='color:#888'>파일은 <code>/file/이름</code> 으로 받습니다.<br>"
        "예) <code>/file/S00008.HLG</code></p>", gSsid);
    gServer.send(200, "text/html; charset=utf-8", body);
}

// mDNS 로 이름을 알린다.
//
// Join 모드에서는 공유기가 IP 를 주므로 미리 알 수 없다. 30대를 회수할 때
// 한 대씩 IP 를 찾아다닐 수는 없다. `_sail._tcp` 를 찾으면 켜져 있는 보드가
// 다 나온다 (TRANSFER.md §3).
// 이 보드의 mDNS 이름. "SAIL-random()" → "sail-random"
// BLE 로 "붙고 나면 이 이름으로 찾아와" 라고 알려줄 때도 쓴다.
void mdnsHostInto(char* host, size_t cap) {
    size_t j = 0;
    for (const char* p = ::sailFullName(); *p && j < cap - 1; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') host[j++] = c;
        else if (c >= 'A' && c <= 'Z') host[j++] = (char)(c - 'A' + 'a');
    }
    host[j] = '\0';
    if (j == 0) snprintf(host, cap, "sail");
}

void startMdns() {
    // ★ 이름은 **배 이름**으로 짓는다. 붙은 WiFi 이름으로 지으면 안 된다.
    //
    // 예전에는 gSsid 를 썼다. AP 모드에서는 gSsid 가 배 이름이라 맞았는데,
    // 접속 모드에서는 붙은 WiFi 이름이 들어간다. 그래서 공유기에 붙였더니
    // 보드가 자기를 fetm2g.local 이라고 불렀다 (실측).
    //
    // 배가 30대면 30대가 전부 fetm2g.local 을 자기 이름이라고 우긴다.
    // 이름으로 찾는 게 통째로 망가진다.
    //
    // 이름에 쓸 수 없는 글자는 뺀다. "SAIL-random()" → "sail-random"
    char host[32];
    mdnsHostInto(host, sizeof(host));

    if (MDNS.begin(host)) {
        MDNS.addService("sail", "tcp", 80);
        MDNS.addService("http", "tcp", 80);
        // 앱이 목록을 그릴 때 쓰는 값들. 붙어 보지 않고도 배를 고를 수 있게.
        MDNS.addServiceTxt("sail", "tcp", "name", String(::sailFullName()));
        MDNS.addServiceTxt("sail", "tcp", "net", String(gSsid));   // 붙은 WiFi
        Serial.printf("[NET] 이름으로도 됩니다 — http://%s.local/\n", host);
    }
}

// WiFi 만 얼마나 나오는지. SD 를 안 건드리고 램에서 바로 보낸다.
//
// 파일 받기가 느릴 때 어디가 느린지 갈라 보려고 둔다. 이것과 파일 받기가
// 비슷하면 WiFi 가 한계고, 이것만 빠르면 SD 나 파일 코드가 느린 것이다.
//
//   curl -o /dev/null http://<주소>/api/speed?mb=2
void handleSpeed() {
    cors();
    uint32_t mb = 2;
    if (gServer.hasArg("mb")) mb = (uint32_t)gServer.arg("mb").toInt();
    if (mb < 1) mb = 1;
    if (mb > 32) mb = 32;

    fastOn();
    static uint8_t buf[4096];
    memset(buf, 0x5A, sizeof(buf));

    const uint32_t total = mb * 1024UL * 1024UL;
    const uint32_t t0 = millis();
    gServer.setContentLength(total);
    gServer.send(200, "application/octet-stream", "");
    for (uint32_t left = total; left; ) {
        const size_t want = left > sizeof(buf) ? sizeof(buf) : left;
        gServer.client().write(buf, want);
        left -= want;
    }
    const uint32_t dt = millis() - t0;
    Serial.printf("[NET] 속도시험 %lu MB  %.1f초  %.0f KB/초\n",
                  (unsigned long)mb, dt / 1000.0f,
                  dt ? total / 1.024f / dt : 0.0f);
}

// 앱이 살아 있다고 알리는 자리.
//
//   GET /api/ping?lease=15    "15초 동안 빌리겠다"
//   GET /api/ping             아직 있다는 표시만
//
// 앱은 4초마다 이걸 부른다. 멈추면 빌린 시간이 지나고 꺼진다.
void handlePing() {
    cors();                 // cors() 안에서 used() 가 불린다
    // 앱이 제 번호를 실어 보낸다. 이걸 적어 둬야 나중에 "나 말고 몇 대" 를 센다.
    if (gServer.hasArg("id")) {
        sawClient((uint32_t)gServer.client().remoteIP(),
                  gServer.arg("id").c_str());
    }
    if (gServer.hasArg("lease")) {
        uint32_t sec = (uint32_t)gServer.arg("lease").toInt();
        if (sec > 300) sec = 300;        // 너무 길게는 못 빌린다
        gLeaseMs = sec * 1000UL;
    }
    char who[200];
    whoJson(who, sizeof(who));
    char body[320];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"lease_s\":%lu,\"idle_s\":%lu,%s}",
             (unsigned long)(gLeaseMs / 1000), (unsigned long)(gIdleOffMs / 1000), who);
    gServer.send(200, "application/json", body);
}

// 다 받았으면 앱이 이걸 부른다. 보드가 WiFi 를 끄고 BLE 로 돌아온다.
//
// 왜 BLE 로 안 끄냐면 — WiFi 가 켜져 있는 동안은 BLE 가 내려가 있어서
// 보드가 BLE 말을 못 듣는다. 지금 붙어 있는 길로 시키는 게 맞다.
void handleWifiOff() {
    cors();
    // 다른 기기가 파일을 받는 중이면 안 끈다.
    //
    // 이제 WiFi 를 켠 동안에도 BLE 가 살아 있어서, 아이패드가 붙어 있는데
    // 데스크탑이 따로 붙는 일이 흔해진다. 한쪽이 "다 썼다" 를 눌렀다고
    // 다른 쪽이 받던 파일을 끊으면 안 된다.
    if (gFast) {
        char who[200];
        whoJson(who, sizeof(who));
        char body[288];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"다른 기기가 받는 중입니다\",%s}", who);
        gServer.send(409, "application/json", body);
        return;
    }
    gServer.send(200, "application/json", "{\"ok\":true,\"wifi\":\"off\"}");
    gServer.client().flush();
    delay(120);            // 답을 다 보내고 나서 끊는다
    stop();
}

// 길을 등록한다. **딱 한 번만 한다.**
//
// WebServer::stop() 은 소켓만 닫고 길 목록은 그대로 둔다. 목록을 지우는 건
// 소멸자뿐인데 gServer 는 전역이라 소멸자가 안 불린다.
// [확인: framework-arduinoespressif32 의 WebServer.cpp 91번 줄 ~WebServer,
//        351번 줄 stop() 은 close() 만 부른다]
//
// 그래서 켤 때마다 부르면 길 여덟 개가 그만큼 쌓인다. 실제로 WiFi 를 열 번
// 껐다 켜는 동안 800바이트씩 사라졌다. 길은 gServer 만큼 오래 살면 되므로
// 한 번만 등록하면 된다. begin() 과 close() 는 길 목록을 안 건드린다.
void routes() {
    static bool done = false;
    if (done) return;
    done = true;

    gServer.on("/", HTTP_GET, handleRoot);
    gServer.on("/api/ping", HTTP_GET, handlePing);
    gServer.on("/api/wifi/off", HTTP_POST, handleWifiOff);
    gServer.on("/api/wifi/off", HTTP_GET,  handleWifiOff);
    gServer.on("/api/status", HTTP_GET, handleStatus);
    gServer.on("/api/files", HTTP_GET, handleFiles);
    gServer.on("/api/speed", HTTP_GET, handleSpeed);
    gServer.onNotFound([]() {
        if (gServer.uri().startsWith("/file/")) {
            if (gServer.method() == HTTP_DELETE) handleDelete();
            else                                 handleFile();
            return;
        }
        if (gServer.uri().startsWith("/api/rec/")) { handleRec(); return; }
        cors();
        gServer.send(404, "application/json", "{\"ok\":false}");
    });
    const char* want[] = {"Range"};
    gServer.collectHeaders(want, 1);
}

} // namespace

bool startAP() {
    stop();
    snprintf(gSsid, sizeof(gSsid), "%s", ::sailFullName());

    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(gSsid, SAIL_AP_PASS)) {
        Serial.println("[NET] AP 를 못 열었습니다.");
        WiFi.mode(WIFI_OFF);
        ::sailBleStart();
        return false;
    }
    // 라디오를 재우지 않는다.
    //
    // ESP32-S3 는 기본이 WIFI_PS_MIN_MODEM 이다. 비컨과 비컨 사이에 라디오를
    // 꺼서 전기를 아끼는데, 그동안은 아무것도 못 보낸다.
    // [확인: framework-arduinoespressif32/libraries/WiFi/src/WiFiGeneric.cpp:769]
    //
    // 평소에는 절전을 켜 둔다. BLE 와 같이 켜려면 그래야 한다.
    // 파일을 보낼 때만 fastOn() 이 절전을 끄고 BLE 를 내린다.
    WiFi.setSleep(true);

    snprintf(gIp, sizeof(gIp), "%s", WiFi.softAPIP().toString().c_str());
    routes();
    gServer.begin();
    startMdns();
    gMode = Mode::AP;
    // ★ 시계를 여기서 돌린다. 안 돌리면 마지막으로 쓴 시각이 한참 전이라
    //   켜는 순간 이미 시간이 지나 있어서 그대로 꺼진다 (실제로 그랬다).
    used();
    sdUp();

    Serial.println("──────────────────────────────────────────");
    Serial.printf("  WiFi 를 열었습니다\n");
    Serial.printf("  이름       %s\n", gSsid);
    Serial.printf("  비밀번호   %s\n", SAIL_AP_PASS);
    Serial.printf("  주소       http://%s/\n", gIp);
    Serial.println("──────────────────────────────────────────");
    Serial.println("  ★ 기록 중에는 파일을 안 보냅니다. rec off 먼저.");
    return true;
}

bool startJoin(uint32_t timeoutMs) {
    loadCreds();
    if (strlen(gStaSsid) == 0) {
        Serial.println("[NET] 붙을 WiFi 이름이 없습니다.");
        Serial.println("      BLE 로 넣거나 secrets.h 에 적으세요.");
        return false;
    }
    stop();
    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_STA);
    WiFi.begin(gStaSsid, gStaPass);
    Serial.printf("[NET] %s 에 붙는 중", gStaSsid);

    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NET] 못 붙었습니다. 이름·비밀번호를 보세요.");
        WiFi.mode(WIFI_OFF);
        ::sailBleStart();
        return false;
    }
    // 평소에는 절전을 켠 채로 둔다. 자세히는 fastOn 의 주석.
    WiFi.setSleep(true);

    snprintf(gSsid, sizeof(gSsid), "%s", gStaSsid);
    snprintf(gIp, sizeof(gIp), "%s", WiFi.localIP().toString().c_str());
    routes();
    gServer.begin();
    startMdns();
    gMode = Mode::Join;
    used();          // 위 startAP 의 주석 참조
    saveLastIp(gIp); // 다음에 앱이 이 주소부터 두드려 볼 수 있게
    sdUp();
    Serial.printf("[NET] 붙었습니다 — http://%s/\n", gIp);
    return true;
}

void stop() {
    const bool wasUp = (gMode != Mode::Off);
    if (wasUp) {
        gServer.stop();
        sdDown();
    }
    // mDNS 도 정리한다. 안 하면 켤 때마다 서비스 기록이 쌓인다.
    // end() 는 mdns_free() 를 부른다 [확인: ESPmDNS.cpp 78번 줄].
    if (wasUp) MDNS.end();
    WiFi.removeEvent(onWifiEvent);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    gMode = Mode::Off;
    gGoneAt = 0;
    gFast   = false;      // WiFi 가 없으면 빠른 구간도 뜻이 없다
    gBusyIp = 0;
    gBusyFile[0] = 0;
    gLastDoneAt = 0;
    gLastIpDone = 0;
    gLastFileDone[0] = 0;
    for (int i = 0; i < kMaxSeen; i++) gSeen[i] = {};
    gLeaseMs = 0;      // 다음에 켤 때 앱이 다시 빌린다
    gIp[0] = '\0';
    // WiFi 를 끄면 BLE 를 되살린다. 워치와 아이폰이 다시 붙는다.
    if (wasUp) ::sailBleStart();
}

void poll() {
    if (gMode == Mode::Off) return;
    gServer.handleClient();

    // 다 보내고 5초가 지났으면 BLE 를 되살린다.
    if (gFast && millis() - gFastAt > kFastHoldMs) fastOff();

    // 1) 쓰던 상대가 사라졌다 — 이게 제일 빠른 신호다
    if (gGoneAt && millis() - gGoneAt > kGoneGraceMs) {
        // AP 라면 그 사이에 누가 다시 붙었을 수도 있다. 한 번 더 본다.
        if (gMode == Mode::AP && WiFi.softAPgetStationNum() > 0) {
            gGoneAt = 0;
        } else {
            Serial.println("[NET] 쓰던 기기가 사라져서 WiFi 를 끕니다.");
            stop();
            return;
        }
    }

    // 2) 빌려 간 앱한테서 연락이 끊겼다
    if (gLeaseMs && millis() - gLastUse > gLeaseMs) {
        Serial.printf("[NET] 앱에서 %lu초 동안 연락이 없어 WiFi 를 끕니다.\n",
                      (unsigned long)(gLeaseMs / 1000));
        stop();
        return;
    }

    // 3) 아무 일도 없으면 시간으로. 켜 놓고 잊은 경우만 여기까지 온다.
    if (gIdleOffMs && millis() - gLastUse > gIdleOffMs) {
        Serial.printf("[NET] %lu초 동안 아무도 안 써서 WiFi 를 끕니다.\n",
                      (unsigned long)(gIdleOffMs / 1000));
        stop();
    }
}

void setIdleOff(uint32_t seconds) { gIdleOffMs = seconds * 1000UL; used(); }
uint32_t idleOffSec() { return gIdleOffMs / 1000; }

const char* mdnsHost() {
    static char host[32];
    mdnsHostInto(host, sizeof(host));
    return host;
}

const char* apPass() { return SAIL_AP_PASS; }

const char* lastIp() { loadLastIp(); return gLastIp; }

uint32_t idleLeftMs() {
    if (gMode == Mode::Off || !gIdleOffMs) return 0;
    const uint32_t gone = millis() - gLastUse;
    return gone >= gIdleOffMs ? 0 : gIdleOffMs - gone;
}

void setCreds(const char* ssid, const char* pass) {
    gWifiPrefs.begin("wifi", /*readOnly=*/false);
    gWifiPrefs.putString("ssid", ssid ? ssid : "");
    if (pass) gWifiPrefs.putString("pass", pass);
    gWifiPrefs.end();
    loadCreds();
}

const char* staSsid() { loadCreds(); return gStaSsid; }

// 주변 WiFi 훑기.
//
// ★ 이건 BLE 를 내리지 않고 할 수 있다. WiFi 절전을 끄지만 않으면 둘이
//   같이 돌아간다. 절전을 끄려 했을 때만 칩이 죽었다.
//   (main.cpp 의 sailBleStop 주석 참조)
int scan(ScanEntry* out, int max) {
    const bool wasOff = (gMode == Mode::Off);
    if (wasOff) WiFi.mode(WIFI_STA);
    const int n = WiFi.scanNetworks();
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const String nm = WiFi.SSID(i);
        if (nm.length() == 0) continue;          // 이름 숨긴 것은 고를 수가 없다
        snprintf(out[k].ssid, sizeof(out[k].ssid), "%s", nm.c_str());
        out[k].rssi = WiFi.RSSI(i);
        out[k].locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        k++;
    }
    WiFi.scanDelete();
    if (wasOff) WiFi.mode(WIFI_OFF);
    return k;
}

int         users()       { return seenCount(); }

// 이 번호를 뺀 나머지 기기 수. 앱은 이것만 보고 붙을지 정한다.
int othersThan(const char* id) { return usersExcept(id); }

// 그 나머지 중 하나를 사람이 알아볼 이름으로.
const char* otherIpText(const char* id) { return otherName(id); }
Mode        mode()        { return gMode; }
const char* ipText()      { return gIp; }
const char* ssidText()    { return gSsid; }
uint32_t    servedFiles() { return gServedFiles; }
uint64_t    servedBytes() { return gServedBytes; }

} // namespace netsrv
