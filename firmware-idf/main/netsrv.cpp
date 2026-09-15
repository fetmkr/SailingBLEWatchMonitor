// firmware-idf 5단계 — WiFi 로 기록 파일 내보내기. firmware-rak/src/netsrv.cpp (커밋 2b18b17) 를 ESP-IDF v6.1 로 옮겼다.
// API 는 firmware-rak/include/netsrv.h 를 그대로 같이 쓴다 (아두이노 흔적 없음, PORTING.md 규칙 4).
//
// ── firmware-rak → 여기 ──────────────────────────────────────────────────
//   WebServer gServer(80) · handleClient      esp_http_server (자기 작업에서 받음) + ★ 요청을 큐로 넘겨 poll() 이 처리
//   gServer.send / sendHeader / sendContent  Resp — WebServer.cpp _prepareHeader·send·sendContent 와 같은 바이트를 httpd_send 로 직접 쓴다
//   gServer.arg / hasArg                     queryArg — WebServer Parsing.cpp _parseArguments·urlDecode 와 같은 규칙
//   WiFiClient.connected / fd / stop         recv(MSG_PEEK|MSG_DONTWAIT) · httpd_req_to_sockfd · async complete + httpd_sess_trigger_close
//   WiFi.mode / softAP / begin / setSleep    esp_wifi_init·set_mode·set_config·start·set_ps (WiFiGeneric/WiFiAP/WiFiSTA.cpp 와 같은 값)
//   WiFi.onEvent(onWifiEvent)                esp_event 처리기는 표식만 세운다 → poll() 이 로그·used()·gGoneAt (CLAUDE.md "콜백에서 일하지 않는다")
//   ESPmDNS                                  espressif/mdns 1.13.0 (mdns_init·hostname_set·service_add·txt_item_set·free)
//   SD.open / openNextFile / remove          POSIX (카드는 /sd 에 붙는다) · opendir · stat · esp_vfs_fat_info
//   Preferences "wifi"                       nvs "wifi" — 키 ssid · pass · lastip (문자열) 그대로
//
// ── 달라진 점 (전부 [확인: 코드]) ──────────────────────────────────────────
//   1) HTTP 판 번호는 늘 "HTTP/1.1". WebServer 는 요청 줄의 판 번호를 되받아 적었다 (Parsing.cpp:97). HTTP/1.0 요청이면 다르다.
//      판 번호가 0 이면 WebServer 는 목록(chunked)을 안 쓰고 끝에서 연결을 끊었는데, 여기는 늘 chunked 다.
//   2) 요청은 esp_http_server 작업이 받아 async 사본을 큐에 넣고, 머리·몸통 쓰기는 루프(poll)가 한다.
//      WebServer 도 루프에서 handleClient 로 처리했으니 "루프가 일한다" 는 같다. 큐는 8칸, 차면 10초 기다린다.
//   3) 응답을 다 쓰면 async 요청을 끝내고 httpd_sess_trigger_close 로 연결을 닫는다 (WebServer 의 Connection: close 와 같은 뜻).
//      닫기는 httpd 작업이 조금 뒤에 한다 — 그 사이에 같은 연결로 온 다음 요청은 받을 수도 있다 [추측: 순서상 가능].
//   4) WiFi 이름(ssid) 값은 시리얼 로그에 안 찍는다 — 글자 수만 (사용자 규칙). HTTP /api/status "name" 과 mDNS TXT "net" 은
//      firmware-rak 과 같이 값을 준다 (앱이 쓰는 약속).
//   5) Range 머리는 256바이트까지 읽는다. 더 길면 잘린 값으로 해석한다 (WebServer 는 길이 제한 없음).
//   6) 요청 URI 는 CONFIG_HTTPD_MAX_URI_LEN(512)·머리 1024 바이트를 넘으면 esp_http_server 가 제 오류로 답한다.
//   7) STA 이벤트 로그는 사건이 난 순간이 아니라 다음 poll 에 찍힌다 (루프 한 바퀴 안).
#include "netsrv.h"
#include "sdcard.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

#include "board_rak.h"
#include "hlog.h"
#include "http_range.h"

// main 이 준다. 헤더를 서로 물게 하지 않으려고 함수 하나로 받는다 (firmware-rak 과 같은 이름).
extern const char* sailFullName();
// BLE 를 내렸다 올린다. WiFi 절전을 끈 채 같이 켜면 칩이 죽는다 (firmware-rak main.cpp sailBleStart 주석).
extern void sailBleStop();
extern void sailBleStart();

namespace netsrv {
namespace {

inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline uint32_t micros() { return (uint32_t)esp_timer_get_time(); }
inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

httpd_handle_t gHttpd = nullptr;
QueueHandle_t  gQueue = nullptr;
constexpr int  kQueueLen = 8;

Mode      gMode = Mode::Off;
char      gIp[20]   = {0};
char      gSsid[40] = {0};
uint32_t  gServedFiles = 0;
uint64_t  gServedBytes = 0;

// ── 언제 WiFi 를 끄나 (firmware-rak 과 같은 네 겹) ───────────────────────
//   1) 앱이 "다 받았다"  POST /api/wifi/off   2) 빌림(lease) 끊김   3) 쓰던 상대가 사라짐   4) 시간
uint32_t  gIdleOffMs = 5 * 60 * 1000;   // 0 이면 안 끈다
uint32_t  gLeaseMs   = 0;               // 0 이면 아무도 안 빌렸다
uint32_t  gLastUse = 0;
constexpr uint32_t kGoneGraceMs = 8000;
uint32_t  gGoneAt = 0;          // 0 이면 상대가 붙어 있다

void used() { gLastUse = millis(); gGoneAt = 0; }

// ── 지금 이 보드를 쓰는 기기들 ───────────────────────────────────────────
struct Seen { uint32_t ip; uint32_t at; char id[13]; };
constexpr int      kMaxSeen     = 4;
constexpr uint32_t kForgetMs    = 30000;
Seen gSeen[kMaxSeen] = {};

uint32_t gBusyIp = 0;
char     gBusyFile[40] = {0};
uint32_t gLastIpDone   = 0;
char     gLastFileDone[40] = {0};
uint32_t gLastDoneAt   = 0;
uint32_t gLastDoneMs   = 0;

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

// IP 번호를 사람이 읽는 글로. 낮은 바이트가 첫 자리 (Arduino IPAddress → uint32_t 와 같은 순서)
void ipText4(uint32_t ip, char* out, size_t n) {
    if (ip == 0) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%lu.%lu.%lu.%lu",
             (unsigned long)(ip & 0xFF), (unsigned long)((ip >> 8) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF), (unsigned long)((ip >> 24) & 0xFF));
}

int usersExcept(const char* id) {
    const uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < kMaxSeen; i++) {
        if (gSeen[i].ip == 0) continue;
        if (now - gSeen[i].at > kForgetMs) { gSeen[i].ip = 0; continue; }
        if (id && *id && strcmp(gSeen[i].id, id) == 0) continue;
        n++;
    }
    return n;
}

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

// 소켓 건너편 주소. IPv6 소켓이면 IPv4 가 ::ffff:a.b.c.d 로 들어온다 (CONFIG_LWIP_IPV6=1).
uint32_t peerIp(int fd) {
    if (fd < 0) return 0;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername(fd, (struct sockaddr*)&ss, &len) != 0) return 0;
    if (ss.ss_family == AF_INET) return ((struct sockaddr_in*)&ss)->sin_addr.s_addr;
    if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (struct sockaddr_in6*)&ss;
        const uint8_t* b = s6->sin6_addr.s6_addr;
        static constexpr uint8_t kMapped[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
        if (memcmp(b, kMapped, 12) != 0) return 0;
        uint32_t v; memcpy(&v, b + 12, 4);
        return v;
    }
    return 0;
}

// ── 지금 처리하는 요청 (WebServer 의 _currentClient·_currentUri·_currentArgs 자리) ──
constexpr size_t kLenNotSet  = (size_t)-2;   // WebServer.h CONTENT_LENGTH_NOT_SET
constexpr size_t kLenUnknown = (size_t)-1;   // CONTENT_LENGTH_UNKNOWN

struct Arg { char key[24]; char val[64]; };
constexpr int kMaxArgs = 8;

struct Req {
    httpd_req_t* r = nullptr;
    int      fd = -1;
    char     uri[CONFIG_HTTPD_MAX_URI_LEN + 1] = {0};   // 물음표 앞까지 (WebServer uri())
    Arg      args[kMaxArgs] = {};
    int      argN = 0;
    size_t   contentLength = kLenNotSet;
    bool     chunked = false;
    char     hdrs[512] = {0};      // sendHeader 로 모인 줄 (_responseHeaders)
    bool     done = false;         // 이미 끝냈다 (async complete 했다)
};
Req gReq;   // ★ 스택에 안 올린다 (URI 칸이 513 바이트)

void urlDecodeInto(const char* s, size_t n, char* out, size_t cap) {   // Parsing.cpp urlDecode 와 같은 규칙
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < cap;) {
        char c = s[i++];
        if (c == '%' && i + 1 < n) {
            char t[5] = {'0', 'x', s[i], s[i + 1], 0};
            i += 2;
            c = (char)strtol(t, nullptr, 16);
        } else if (c == '+') {
            c = ' ';
        }
        out[j++] = c;
    }
    out[j] = 0;
}

// Parsing.cpp _parseArguments 와 같은 규칙: & 로 나누고, = 가 없는 조각은 버린다.
void parseArgs(const char* q) {
    gReq.argN = 0;
    if (!q || !*q) return;
    const char* p = q;
    while (*p && gReq.argN < kMaxArgs) {
        const char* amp = strchr(p, '&');
        const char* end = amp ? amp : p + strlen(p);
        const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));
        if (eq) {
            Arg& a = gReq.args[gReq.argN++];
            urlDecodeInto(p, (size_t)(eq - p), a.key, sizeof a.key);
            urlDecodeInto(eq + 1, (size_t)(end - eq - 1), a.val, sizeof a.val);
        }
        if (!amp) break;
        p = amp + 1;
    }
}

bool hasArg(const char* k) {
    for (int i = 0; i < gReq.argN; i++) if (strcmp(gReq.args[i].key, k) == 0) return true;
    return false;
}
const char* arg(const char* k) {
    for (int i = 0; i < gReq.argN; i++) if (strcmp(gReq.args[i].key, k) == 0) return gReq.args[i].val;
    return "";
}
bool uriStarts(const char* pre) { return strncmp(gReq.uri, pre, strlen(pre)) == 0; }
bool uriEnds(const char* suf) {
    const size_t a = strlen(gReq.uri), b = strlen(suf);
    return a >= b && strcmp(gReq.uri + a - b, suf) == 0;
}

// 소켓에 다 쓴다 (WebServer _currentClientWrite 자리). 실패하면 false.
bool rawWrite(const char* p, size_t n) {
    while (n) {
        const int w = httpd_send(gReq.r, p, n);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

const char* codeText(int code) {   // WebServer.cpp _responseCodeToString 에서 쓰는 것만 같은 글자로
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 416: return "Requested range not satisfiable";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "";
    }
}

void sendHeader(const char* name, const char* value) {
    const size_t used = strlen(gReq.hdrs);
    snprintf(gReq.hdrs + used, sizeof(gReq.hdrs) - used, "%s: %s\r\n", name, value);
}
void setContentLength(size_t n) { gReq.contentLength = n; }

void sendContent(const char* s, size_t n) {   // WebServer.cpp sendContent
    if (gReq.chunked) {
        char sz[12];
        snprintf(sz, sizeof sz, "%x\r\n", (unsigned)n);
        rawWrite(sz, strlen(sz));
    }
    rawWrite(s, n);
    if (gReq.chunked) {
        rawWrite("\r\n", 2);
        if (n == 0) gReq.chunked = false;
    }
}
void sendContent(const char* s) { sendContent(s, strlen(s)); }

// WebServer.cpp _prepareHeader + send 와 같은 순서:
//   상태 줄 · Content-Type · (먼저 모인 머리들) · Content-Length 또는 chunked · Connection: close · 빈 줄 · 몸통
void send(int code, const char* type, const char* body) {
    static char head[1024];
    const size_t blen = strlen(body);
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s", code, codeText(code), type, gReq.hdrs);
    if (gReq.contentLength == kLenNotSet) {
        n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", (unsigned)blen);
    } else if (gReq.contentLength != kLenUnknown) {
        n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", (unsigned)gReq.contentLength);
    } else {
        gReq.chunked = true;
        n += snprintf(head + n, sizeof(head) - n, "Accept-Ranges: none\r\nTransfer-Encoding: chunked\r\n");
    }
    n += snprintf(head + n, sizeof(head) - n, "Connection: close\r\n\r\n");
    gReq.hdrs[0] = 0;
    rawWrite(head, (size_t)n);
    if (blen) sendContent(body, blen);
}

// 요청을 끝낸다. async 사본을 돌려주고 연결을 닫는다 (WebServer 가 처리 뒤 클라이언트를 놓던 자리).
void endRequest(httpd_req_t* r, int fd) {
    if (!r) return;
    httpd_req_async_handler_complete(r);            // ★ 먼저 끝내고 (세션을 먼저 닫으면 사본이 가리키는 세션이 사라진다)
    if (gHttpd && fd >= 0) httpd_sess_trigger_close(gHttpd, fd);
}

// ── 파일 보낼 때만 빠르게 ────────────────────────────────────────────────
bool     gFast   = false;
uint32_t gFastAt = 0;
constexpr uint32_t kFastHoldMs = 5000;

// WiFiGeneric.cpp setSleep: 값만 바꾸고, STA 가 켜진 모드일 때만 esp_wifi_set_ps 를 부른다.
// ESP32-S3 아두이노 기본값은 WIFI_PS_MIN_MODEM (netsrv.cpp 주석 [확인: WiFiGeneric.cpp:769]).
// ★ 그래서 firmware-rak 의 AP 모드에서 fastOn 의 setSleep(false) 는 칩에 아무것도 안 불렀다. 같게 둔다.
wifi_ps_type_t gSleepType = WIFI_PS_MIN_MODEM;
void setSleep(bool enabled) {
    const wifi_ps_type_t want = enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
    if (want == gSleepType) return;
    gSleepType = want;
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && (m & WIFI_MODE_STA)) {
        if (esp_wifi_set_ps(gSleepType) != ESP_OK) printf("[NET] esp_wifi_set_ps 실패\n");
    }
}

void fastOn() {
    gFastAt = millis();
    if (gFast) return;
    // ★ 순서를 바꾸면 칩이 죽는다 (firmware-rak: BLE 가 켜진 채 절전을 끄면 abort). BLE 를 먼저 내린다.
    ::sailBleStop();
    setSleep(false);
    gFast = true;
    printf("[NET] 파일 보내는 동안 BLE 를 내립니다.\n");
}

void fastOff() {
    if (!gFast) return;
    setSleep(true);           // 절전을 먼저 켜고
    ::sailBleStart();         // 그 다음에 BLE 를 올린다
    gFast = false;
}

// ── WiFi 이름·비밀번호 (NVS "wifi") ─────────────────────────────────────
char gStaSsid[33] = {0};
char gStaPass[65] = {0};
char gLastIp[20]  = {0};

void nvsGetStr(const char* key, char* out, size_t cap) {
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = 0;
    nvs_close(h);
}

void loadLastIp() { nvsGetStr("lastip", gLastIp, sizeof gLastIp); }

void saveLastIp(const char* ip) {
    if (!ip || !*ip) return;
    if (strcmp(gLastIp, ip) == 0) return;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "lastip", ip);
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(gLastIp, sizeof(gLastIp), "%s", ip);
}

// ★ 코드에 박힌 기본 WiFi 로 물러서지 않는다 (firmware-rak 2026-09-14). 사람이 저장했을 때만 붙는다.
void loadCreds() {
    nvsGetStr("ssid", gStaSsid, sizeof gStaSsid);
    nvsGetStr("pass", gStaPass, sizeof gStaPass);
}

// AP 비밀번호. firmware-rak 과 같다 (secrets.h 에 SAIL_AP_PASS 가 없어 이 기본값을 썼다).
#ifndef SAIL_AP_PASS
#define SAIL_AP_PASS "sailing1234"
#endif

bool sdUp()   { return sdcard::acquire(sdcard::Owner::Download); }
void sdDown() { sdcard::release(sdcard::Owner::Download); }

void cors() {
    used();
    sawClient(peerIp(gReq.fd));
    sendHeader("Access-Control-Allow-Origin", "*");
    sendHeader("Access-Control-Allow-Headers", "*");
    sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

void whoJson(char* out, size_t n) {
    char busy[20] = {0}, last[20] = {0}, you[20] = {0};
    ipText4(gBusyIp, busy, sizeof(busy));
    ipText4(gLastIpDone, last, sizeof(last));
    ipText4(peerIp(gReq.fd), you, sizeof(you));
    snprintf(out, n,
             "\"you\":\"%s\",\"users\":%d,\"busy\":\"%s\",\"busy_file\":\"%s\","
             "\"last_ip\":\"%s\",\"last_file\":\"%s\",\"last_ago_s\":%lu,"
             "\"last_took_ms\":%lu",
             you, seenCount(), busy, gBusyFile, last, gLastFileDone,
             gLastDoneAt ? (unsigned long)((millis() - gLastDoneAt) / 1000) : 99999UL,
             (unsigned long)gLastDoneMs);
}

uint64_t cardFreeMb() {
    uint64_t total = 0, freeB = 0;
    if (esp_vfs_fat_info("/sd", &total, &freeB) != ESP_OK) return 0;
    return freeB / 1048576ULL;
}

void handleStatus() {
    cors();
    hlog::Status st;
    hlog::getStatus(&st);
    char who[200];
    whoJson(who, sizeof(who));
    char body[768];
    snprintf(body, sizeof(body),
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
        (unsigned long long)(sdcard::owner() == sdcard::Owner::Download ? cardFreeMb() : 0),
        who);
    send(200, "application/json", body);
}

// 파일 목록 + 머리글 128바이트 요약 (TRANSFER.md §1)
void handleFiles() {
    cors();
    if (hlog::busy()) {
        send(409, "application/json", "{\"ok\":false,\"error\":\"기록 중에는 카드를 못 읽습니다\"}");
        return;
    }
    if (!sdUp()) {
        send(503, "application/json", "{\"ok\":false,\"error\":\"카드를 못 읽습니다\"}");
        return;
    }

    setContentLength(kLenUnknown);
    send(200, "application/json", "");
    sendContent("{\"ok\":true,\"files\":[");

    DIR* dir = opendir("/sd/LOGS");
    bool first = true;
    static char item[512];
    static char path[300];
    struct dirent* de;
    while (dir && (de = readdir(dir)) != nullptr) {
        const char* nm = de->d_name;
        const size_t nl = strlen(nm);
        if (nl < 4 || strcmp(nm + nl - 4, ".HLG") != 0) continue;   // TXT 는 사본이라 뺀다

        snprintf(path, sizeof path, "/sd/LOGS/%s", nm);
        struct stat stt;
        const uint32_t sz = (stat(path, &stt) == 0) ? (uint32_t)stt.st_size : 0;

        uint8_t h[hlog::kHeaderSize];
        bool okHdr = false;
        if (FILE* e = fopen(path, "rb")) {
            okHdr = fread(h, 1, hlog::kHeaderSize, e) == hlog::kHeaderSize && memcmp(h, "HHLG", 4) == 0;
            fclose(e);
        }
        if (!okHdr) {
            snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%lu,\"ok\":false}",
                     first ? "" : ",", nm, (unsigned long)sz);
            sendContent(item);
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

        const bool closed = (h[hlog::kOffClosed] == 1);
        if (!closed && sz > hlog::kHeaderSize) {
            const uint32_t imuSz = (h[5] >= 1) ? hlog::kImuSize : hlog::kImuSizeV0;
            const uint32_t navSz = hlog::navSizeFor(h[4], h[5]);
            const uint32_t perSec = navSz * hlog::kRateNav + imuSz * hlog::kRateImu;
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
                 first ? "" : ",", nm, (unsigned long)sz,
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
        sendContent(item);
        first = false;
    }
    if (dir) closedir(dir);
    sendContent("]}");
    sendContent("");
}

// ── 보내기는 루프를 붙잡지 않는다 (firmware-rak 과 같은 방식: poll 한 번에 20 ms, MSG_DONTWAIT) ──
struct Xfer {
    bool         active = false;
    bool         synthetic = false;   // /api/speed — 파일 없이 램에서 보낸다
    FILE*        f = nullptr;
    httpd_req_t* r = nullptr;         // 끝날 때 async 를 돌려준다
    int          fd = -1;
    uint32_t     pos = 0;
    uint32_t     sent = 0, len = 0;
    uint32_t     t0 = 0, lastProgress = 0;
    uint32_t     usRead = 0, usWrite = 0;
    char         what[64] = {0};
};
Xfer gX;
constexpr uint32_t kXferStallMs  = 10000;
constexpr uint32_t kXferBudgetMs = 20;
uint8_t gXBuf[4096];                  // ★ 스택에 안 올린다

// WiFiClient::connected 자리 — 한 바이트 엿보기. 0 이면 상대가 닫았다.
bool sockConnected(int fd) {
    if (fd < 0) return false;
    uint8_t b;
    const int n = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return true;
    if (n == 0) return false;
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

void xferFinish(bool ok, const char* why) {
    const uint32_t dt = millis() - gX.t0;
    if (!gX.synthetic && gX.f) fclose(gX.f);
    endRequest(gX.r, gX.fd);
    if (!gX.synthetic) {
        gServedBytes += gX.sent;
        if (ok) {
            gLastIpDone = gBusyIp;
            snprintf(gLastFileDone, sizeof(gLastFileDone), "%s", gBusyFile);
            gLastDoneAt = millis();
            gLastDoneMs = dt;
            ++gServedFiles;
        }
        gBusyIp = 0;
        gBusyFile[0] = 0;
    }
    printf("[NET] %s %s  %lu/%lu 바이트  %.1f초  %.0f KB/초"
           "  (읽기 %.2f초  WiFi 쓰기 %.2f초)%s%s\n",
           ok ? "보냄" : "★ 중단", gX.what,
           (unsigned long)gX.sent, (unsigned long)gX.len, dt / 1000.0f,
           dt ? gX.sent / 1.024f / dt : 0.0f,
           gX.usRead / 1e6f, gX.usWrite / 1e6f,
           why ? " — " : "", why ? why : "");
    gX = Xfer();
}

void xferPump() {
    if (!gX.active) return;
    const uint32_t start = millis();
    while (millis() - start < kXferBudgetMs) {
        if (gX.sent >= gX.len) { xferFinish(true, nullptr); return; }
        if (!sockConnected(gX.fd)) { xferFinish(false, "받는 기기가 끊었습니다"); return; }
        const uint32_t left = gX.len - gX.sent;
        const size_t want = left > sizeof(gXBuf) ? sizeof(gXBuf) : (size_t)left;
        uint32_t t = micros();
        const int got = gX.synthetic ? (int)want : (int)fread(gXBuf, 1, want, gX.f);
        gX.usRead += micros() - t;
        if (got <= 0) { xferFinish(false, "SD 읽기 실패"); return; }
        t = micros();
        size_t w = 0;
        if (gX.fd < 0) { xferFinish(false, "소켓이 없습니다"); return; }
        errno = 0;
        const int res = ::send(gX.fd, gXBuf, (size_t)got, MSG_DONTWAIT);
        if (res > 0) w = (size_t)res;
        else if (res < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            gX.usWrite += micros() - t;
            xferFinish(false, "소켓 쓰기 오류");
            return;
        }
        gX.usWrite += micros() - t;
        if (w > 0) {
            gX.pos += (uint32_t)w;
            gX.sent += (uint32_t)w;
            gX.lastProgress = millis();
            used();
            fastOn();
        }
        if (w < (size_t)got) {
            if (!gX.synthetic) fseek(gX.f, (long)gX.pos, SEEK_SET);
            if (millis() - gX.lastProgress > kXferStallMs) {
                xferFinish(false, "10초 동안 한 바이트도 못 보냈습니다");
            }
            return;
        }
    }
}

// ★ 달라진 점: 이름이 kMaxName 보다 길면 거절한다. firmware-rak 은 경로 64칸에서 잘라 열었다 (대개 404).
//   가장 긴 세션 이름이 36자라 (S00014_19700103-0043_nosat.HLG + /LOGS/) 영향이 없다.
constexpr size_t kMaxName = 56;
bool badName(const char* name) {
    return strstr(name, "..") != nullptr || strchr(name, '/') != nullptr || *name == 0 || strlen(name) > kMaxName;
}

// 파일 하나 보내기. Range 를 받는다.
void handleFile() {
    cors();
    if (gX.active) {
        send(409, "application/json", "{\"ok\":false,\"error\":\"이미 보내는 중입니다\"}");
        return;
    }
    if (!sdUp()) {
        if (sdcard::owner() == sdcard::Owner::Recorder)
            send(409, "application/json", "{\"ok\":false,\"error\":\"기록 중에는 못 보냅니다\"}");
        else
            send(503, "application/json", "{\"ok\":false,\"error\":\"카드를 못 읽습니다\"}");
        return;
    }
    const uint32_t meIp = peerIp(gReq.fd);

    const char* name = gReq.uri + 6;          // "/file/S00008.HLG"
    if (badName(name)) {
        send(400, "application/json", "{\"ok\":false,\"error\":\"이름이 이상합니다\"}");
        return;
    }
    // firmware-rak 과 같은 64칸. 이름이 너무 길면 badName 이 400 으로 돌려보낸다 (아래 kMaxName).
    char path[64];
    strlcpy(path, "/LOGS/", sizeof path);
    strlcat(path, name, sizeof path);
    char real[80];
    strlcpy(real, "/sd", sizeof real);
    strlcat(real, path, sizeof real);
    FILE* f = fopen(real, "rb");
    if (!f) {
        send(404, "application/json", "{\"ok\":false,\"error\":\"그런 파일이 없습니다\"}");
        return;
    }
    struct stat stt;
    const uint32_t total = (fstat(fileno(f), &stt) == 0) ? (uint32_t)stt.st_size : 0;

    uint64_t from64 = 0, to64 = 0;
    char rh[256];
    const bool haveRange = httpd_req_get_hdr_value_str(gReq.r, "Range", rh, sizeof rh) == ESP_OK ||
                           (httpd_req_get_hdr_value_len(gReq.r, "Range") > 0);
    const http::RangeResult rr = http::parseRange(haveRange && rh[0] ? rh : nullptr, total, &from64, &to64);
    if (rr == http::RangeResult::Malformed) {
        fclose(f);
        send(400, "application/json", "{\"ok\":false,\"error\":\"Range 모양이 틀립니다\"}");
        return;
    }
    if (rr == http::RangeResult::Unsatisfiable) {
        fclose(f);
        char cr[40];
        snprintf(cr, sizeof(cr), "bytes */%lu", (unsigned long)total);
        sendHeader("Content-Range", cr);
        send(416, "text/plain", "");
        return;
    }
    const bool partial = (rr == http::RangeResult::Ok);
    const uint32_t from = partial ? (uint32_t)from64 : 0;
    const uint32_t to   = partial ? (uint32_t)to64 : (total ? total - 1 : 0);
    const uint32_t len  = total ? (to - from + 1) : 0;

    sendHeader("Accept-Ranges", "bytes");
    if (partial) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu", (unsigned long)from, (unsigned long)to, (unsigned long)total);
        sendHeader("Content-Range", cr);
    }
    if (len == 0) {
        fclose(f);
        setContentLength(0);
        send(200, "application/octet-stream", "");
        return;
    }
    if (fseek(f, (long)from, SEEK_SET) != 0) {
        fclose(f);
        send(500, "application/json", "{\"ok\":false,\"error\":\"파일 자리를 못 옮겼습니다\"}");
        return;
    }

    fastOn();
    gBusyIp = meIp;
    strlcpy(gBusyFile, name, sizeof(gBusyFile));   // snprintf 로 자르던 것과 같은 결과
    setContentLength(len);
    send(partial ? 206 : 200, "application/octet-stream", "");

    gX = Xfer();
    gX.active = true;
    gX.f = f;
    gX.r = gReq.r;
    gX.fd = gReq.fd;
    gX.pos = from;
    gX.len = len;
    gX.t0 = gX.lastProgress = millis();
    strlcpy(gX.what, path, sizeof(gX.what));
}

// 지우기. 파일 안의 세션 번호를 확인 값으로 받는다.
void handleDelete() {
    cors();
    if (hlog::busy() || gX.active) {
        send(409, "application/json",
             gX.active ? "{\"ok\":false,\"error\":\"파일을 보내는 중에는 못 지웁니다\"}"
                       : "{\"ok\":false,\"error\":\"기록 중에는 못 지웁니다\"}");
        return;
    }
    if (!sdUp()) { send(503, "application/json", "{\"ok\":false}"); return; }
    if (!hasArg("confirm")) {
        send(400, "application/json", "{\"ok\":false,\"error\":\"confirm=<세션번호> 가 있어야 합니다\"}");
        return;
    }
    const char* name = gReq.uri + 6;
    if (badName(name)) { send(400, "application/json", "{\"ok\":false}"); return; }
    // firmware-rak 과 같은 64칸. 이름이 너무 길면 badName 이 400 으로 돌려보낸다 (아래 kMaxName).
    char path[64];
    strlcpy(path, "/LOGS/", sizeof path);
    strlcat(path, name, sizeof path);
    char real[80];
    strlcpy(real, "/sd", sizeof real);
    strlcat(real, path, sizeof real);

    FILE* f = fopen(real, "rb");
    if (!f) { send(404, "application/json", "{\"ok\":false}"); return; }
    uint8_t h[hlog::kHeaderSize];
    const bool okHdr = fread(h, 1, hlog::kHeaderSize, f) == hlog::kHeaderSize && memcmp(h, "HHLG", 4) == 0;
    uint32_t session = 0;
    if (okHdr) memcpy(&session, h + 18, 4);
    fclose(f);

    if (!okHdr || (uint32_t)atol(arg("confirm")) != session) {   // String::toInt 는 atol
        send(403, "application/json", "{\"ok\":false,\"error\":\"세션 번호가 안 맞습니다\"}");
        return;
    }
    const bool gone = remove(real) == 0;
    if (gone) {
        char txt[80];
        strlcpy(txt, "/sd/LOGS/", sizeof txt);
        strlcat(txt, name, sizeof txt);
        const int dot = (int)strlen(txt) - 4;
        if (dot > 0) { strcpy(txt + dot, ".TXT"); remove(txt); }
        printf("[NET] 지웠습니다 — %s (세션 %lu)\n", path, (unsigned long)session);
    }
    send(gone ? 200 : 500, "application/json", gone ? "{\"ok\":true}" : "{\"ok\":false}");
}

void handleRec() {
    cors();
    if (uriEnds("/mark")) {
        hlog::mark();
        send(200, "application/json", "{\"ok\":true}");
        return;
    }
    // 시작·종료는 시리얼·버튼과 같은 길(recWantOn / recWantOff)을 타야 해서 여기서 직접 하지 않는다 — 501 (firmware-rak 과 같음)
    send(501, "application/json", "{\"ok\":false,\"error\":\"아직 안 만들었습니다\"}");
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
    send(200, "text/html; charset=utf-8", body);
}

// mDNS 이름 — 배 이름에서. "SAIL-random()" → "sail-random"
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

// ESPmDNS begin·addService·addServiceTxt 와 같은 호출 (서비스 이름 앞에 '_' 를 붙인다)
void startMdns() {
    char host[32];
    mdnsHostInto(host, sizeof(host));
    if (mdns_init() != ESP_OK) return;
    if (mdns_hostname_set(host) != ESP_OK) return;
    mdns_service_add(nullptr, "_sail", "_tcp", 80, nullptr, 0);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    mdns_service_txt_item_set("_sail", "_tcp", "name", ::sailFullName());
    mdns_service_txt_item_set("_sail", "_tcp", "net", gSsid);
    printf("[NET] 이름으로도 됩니다 — http://%s.local/\n", host);
}

void handleSpeed() {
    cors();
    if (hlog::busy()) {
        send(409, "application/json", "{\"ok\":false,\"error\":\"기록 중에는 속도시험을 못 합니다\"}");
        return;
    }
    if (gX.active) {
        send(409, "application/json", "{\"ok\":false,\"error\":\"이미 보내는 중입니다\"}");
        return;
    }
    uint32_t mb = 2;
    if (hasArg("mb")) mb = (uint32_t)atol(arg("mb"));
    if (mb < 1) mb = 1;
    if (mb > 32) mb = 32;

    fastOn();
    memset(gXBuf, 0x5A, sizeof(gXBuf));
    const uint32_t total = mb * 1024UL * 1024UL;
    setContentLength(total);
    send(200, "application/octet-stream", "");

    gX = Xfer();
    gX.active = true;
    gX.synthetic = true;
    gX.r = gReq.r;
    gX.fd = gReq.fd;
    gX.len = total;
    gX.t0 = gX.lastProgress = millis();
    snprintf(gX.what, sizeof(gX.what), "속도시험 %lu MB", (unsigned long)mb);
}

void handlePing() {
    cors();
    if (hasArg("id")) sawClient(peerIp(gReq.fd), arg("id"));
    if (hasArg("lease")) {
        uint32_t sec = (uint32_t)atol(arg("lease"));
        if (sec > 300) sec = 300;
        gLeaseMs = sec * 1000UL;
    }
    char who[200];
    whoJson(who, sizeof(who));
    char body[320];
    snprintf(body, sizeof(body), "{\"ok\":true,\"lease_s\":%lu,\"idle_s\":%lu,%s}",
             (unsigned long)(gLeaseMs / 1000), (unsigned long)(gIdleOffMs / 1000), who);
    send(200, "application/json", body);
}

bool gStopAfterReq = false;

void handleWifiOff() {
    cors();
    if (gFast) {
        char who[200];
        whoJson(who, sizeof(who));
        char body[288];
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"다른 기기가 받는 중입니다\",%s}", who);
        send(409, "application/json", body);
        return;
    }
    send(200, "application/json", "{\"ok\":true,\"wifi\":\"off\"}");
    gStopAfterReq = true;   // 답을 다 보내고 요청을 끝낸 뒤 120 ms 쉬고 끈다 (firmware-rak flush + delay(120) + stop)
}

// WebServer 의 on(...) 목록과 onNotFound 를 같은 순서로
void dispatch() {
    const int m = gReq.r->method;
    const char* u = gReq.uri;
    if (!strcmp(u, "/") && m == HTTP_GET)                                   { handleRoot();   return; }
    if (!strcmp(u, "/api/ping") && m == HTTP_GET)                           { handlePing();   return; }
    if (!strcmp(u, "/api/wifi/off") && (m == HTTP_POST || m == HTTP_GET))   { handleWifiOff(); return; }
    if (!strcmp(u, "/api/status") && m == HTTP_GET)                         { handleStatus(); return; }
    if (!strcmp(u, "/api/files") && m == HTTP_GET)                          { handleFiles();  return; }
    if (!strcmp(u, "/api/speed") && m == HTTP_GET)                          { handleSpeed();  return; }
    if (uriStarts("/file/")) {
        if (m == HTTP_DELETE) handleDelete();
        else                  handleFile();
        return;
    }
    if (uriStarts("/api/rec/")) { handleRec(); return; }
    cors();
    send(404, "application/json", "{\"ok\":false}");
}

// 큐에서 요청 하나를 꺼내 처리한다 (WebServer::handleClient 한 번)
void serveOne(httpd_req_t* r) {
    gReq = Req();
    gReq.r = r;
    gReq.fd = httpd_req_to_sockfd(r);
    const char* q = strchr(r->uri, '?');
    const size_t ul = q ? (size_t)(q - r->uri) : strlen(r->uri);
    memcpy(gReq.uri, r->uri, ul < sizeof(gReq.uri) - 1 ? ul : sizeof(gReq.uri) - 1);
    parseArgs(q ? q + 1 : nullptr);

    dispatch();
    if (gReq.chunked) sendContent("");                       // WebServer _finalizeResponse
    if (!(gX.active && gX.r == r)) endRequest(r, gReq.fd);   // 파일 몸통은 xferPump 가 보내고 끝낸다
    gReq.r = nullptr;
}

// esp_http_server 작업에서 불린다. ★ 여기서는 일하지 않는다 — async 사본을 큐에 넣기만 한다.
esp_err_t onRequest(httpd_req_t* req) {
    httpd_req_t* copy = nullptr;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) return ESP_FAIL;
    if (xQueueSend(gQueue, &copy, pdMS_TO_TICKS(10000)) != pdTRUE) {
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool startHttpd() {
    if (!gQueue) gQueue = xQueueCreate(kQueueLen, sizeof(httpd_req_t*));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&gHttpd, &cfg) != ESP_OK) { gHttpd = nullptr; return false; }
    static const httpd_uri_t any = {
        .uri = "/*", .method = (httpd_method_t)HTTP_ANY, .handler = onRequest, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(gHttpd, &any);
    return true;
}

void stopHttpd() {
    if (!gHttpd) return;
    httpd_req_t* r = nullptr;
    while (gQueue && xQueueReceive(gQueue, &r, 0) == pdTRUE) httpd_req_async_handler_complete(r);
    httpd_stop(gHttpd);
    gHttpd = nullptr;
}

// ── WiFi 켜고 끄기 (WiFiGeneric.cpp wifiLowLevelInit · espWiFiStop 과 같은 순서) ──
esp_netif_t* gNetAp = nullptr;
esp_netif_t* gNetSta = nullptr;
bool gWifiInit = false;
bool gWifiStarted = false;

constexpr uint32_t kEvApConn = 1, kEvApDisc = 2, kEvStaDisc = 4, kEvStaGotIp = 8;
std::atomic<uint32_t> gEv{0};
std::atomic<uint8_t>  gStaDiscReason{0};   // 마지막 STA 끊김 이유 (wifi_err_reason_t)
esp_event_handler_instance_t gEvWifi = nullptr, gEvIp = nullptr;

// esp_event 작업에서 불린다 (스택 2304). ★ 표식만 세운다.
void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED)    gEv.fetch_or(kEvApConn);
        if (id == WIFI_EVENT_AP_STADISCONNECTED) gEv.fetch_or(kEvApDisc);
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (data) gStaDiscReason = ((wifi_event_sta_disconnected_t*)data)->reason;
            gEv.fetch_or(kEvStaDisc);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        gEv.fetch_or(kEvStaGotIp);
    }
}

bool wifiLowLevelInit() {
    if (!gNetAp) {
        esp_netif_init();
        const esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
        gNetAp  = esp_netif_create_default_wifi_ap();
        gNetSta = esp_netif_create_default_wifi_sta();
    }
    if (!gWifiInit) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();   // ★ 버퍼 수는 sdkconfig 값 (아두이노는 코드에서 덮어썼다 — 보고 참고)
        if (esp_wifi_init(&cfg) != ESP_OK) return false;
        esp_wifi_set_storage(WIFI_STORAGE_RAM);                // WiFi.persistent(false) 기본
        gWifiInit = true;
    }
    return true;
}

// WiFi.mode(m) — 끄기면 멈추고 풀어 준다 (espWiFiStop → esp_wifi_deinit)
bool wifiMode(wifi_mode_t m) {
    if (m == WIFI_MODE_NULL) {
        if (gWifiStarted) { esp_wifi_stop(); gWifiStarted = false; }
        if (gWifiInit) { esp_wifi_deinit(); gWifiInit = false; }
        return true;
    }
    if (!wifiLowLevelInit()) return false;
    if (esp_wifi_set_mode(m) != ESP_OK) return false;
    if (!gWifiStarted) {
        if (esp_wifi_start() != ESP_OK) return false;
        gWifiStarted = true;
        if (m & WIFI_MODE_STA) esp_wifi_set_ps(gSleepType);    // 아두이노 STA_START 때 _sleepEnabled 적용 [확인: WiFiGeneric.cpp:1046]
    }
    return true;
}

void onEvents(bool on) {
    if (on && !gEvWifi) {
        // ★ 고침 (09-15 보드 실측): 부팅 뒤 WiFi 를 처음 켤 때는 기본 이벤트 루프가 아직 없다 (wifiMode 안의
        //   wifiLowLevelInit 가 만든다). 루프 없이 등록하면 실패하고, STA 가 IP 를 받아도 kEvStaGotIp 가 안 서서
        //   `wifi on` 이 15초 기다린 뒤 "못 붙었습니다" 로 끝났다. 등록 전에 루프를 만들고, 실패하면 크게 찍는다.
        esp_netif_init();
        const esp_err_t le = esp_event_loop_create_default();
        if (le != ESP_OK && le != ESP_ERR_INVALID_STATE)
            printf("[NET] ★ 이벤트 루프를 못 만들었습니다 (%s)\n", esp_err_to_name(le));
        const esp_err_t e1 = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr, &gEvWifi);
        const esp_err_t e2 = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr, &gEvIp);
        if (e1 != ESP_OK || e2 != ESP_OK)
            printf("[NET] ★ WiFi 사건 받기 등록 실패 (%s · %s) — 붙어도 모릅니다\n", esp_err_to_name(e1), esp_err_to_name(e2));
    } else if (!on && gEvWifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, gEvWifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, gEvIp);
        gEvWifi = gEvIp = nullptr;
    }
    gEv = 0;
}

uint8_t apStationNum() {
    wifi_sta_list_t l;
    wifi_mode_t m = WIFI_MODE_NULL;
    if (!gWifiInit || esp_wifi_get_mode(&m) != ESP_OK || m == WIFI_MODE_NULL) return 0;
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? (uint8_t)l.num : 0;
}

void netifIpText(esp_netif_t* nif, char* out, size_t cap) {
    esp_netif_ip_info_t ip;
    out[0] = 0;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK)
        snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip.ip.addr & 0xFF), (unsigned)((ip.ip.addr >> 8) & 0xFF),
                 (unsigned)((ip.ip.addr >> 16) & 0xFF), (unsigned)((ip.ip.addr >> 24) & 0xFF));
}

} // namespace

bool startAP() {
    if (hlog::busy()) {
        printf("[NET] 기록 중에는 WiFi 를 안 켭니다 — rec off 먼저\n");
        return false;
    }
    stop();
    snprintf(gSsid, sizeof(gSsid), "%s", ::sailFullName());

    onEvents(true);
    wifi_config_t conf = {};   // WiFiAP.cpp wifi_softap_config: 채널 1 · 4대 · 비컨 100 · 숨김 0 · WPA2 · CCMP
    const size_t apLen = strnlen(gSsid, sizeof(conf.ap.ssid));   // _wifi_strncpy(…, 32) 와 같다
    memcpy(conf.ap.ssid, gSsid, apLen);
    conf.ap.ssid_len = (uint8_t)apLen;
    snprintf((char*)conf.ap.password, sizeof(conf.ap.password), "%s", SAIL_AP_PASS);
    conf.ap.channel = 1;
    conf.ap.max_connection = 4;
    conf.ap.beacon_interval = 100;
    conf.ap.ssid_hidden = 0;
    conf.ap.authmode = WIFI_AUTH_WPA2_PSK;
    conf.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    conf.ap.ftm_responder = false;
    if (!wifiMode(WIFI_MODE_AP) || esp_wifi_set_config(WIFI_IF_AP, &conf) != ESP_OK) {
        printf("[NET] AP 를 못 열었습니다.\n");
        wifiMode(WIFI_MODE_NULL);
        onEvents(false);
        ::sailBleStart();
        return false;
    }
    setSleep(true);

    netifIpText(gNetAp, gIp, sizeof gIp);
    if (!startHttpd()) printf("[NET] ★ HTTP 서버를 못 열었습니다\n");
    startMdns();
    gMode = Mode::AP;
    used();
    sdUp();

    printf("──────────────────────────────────────────\n");
    printf("  WiFi 를 열었습니다\n");
    printf("  이름       %s\n", gSsid);
    printf("  비밀번호   %s\n", SAIL_AP_PASS);
    printf("  주소       http://%s/\n", gIp);
    printf("──────────────────────────────────────────\n");
    printf("  ★ 기록 중에는 파일을 안 보냅니다. rec off 먼저.\n");
    return true;
}

bool startJoin(uint32_t timeoutMs) {
    if (hlog::busy()) {
        printf("[NET] 기록 중에는 WiFi 를 안 켭니다 — rec off 먼저\n");
        return false;
    }
    loadCreds();
    if (strlen(gStaSsid) == 0) {
        printf("[NET] 붙을 WiFi 이름이 없습니다.\n");
        printf("      BLE 로 넣거나 secrets.h 에 적으세요.\n");
        return false;
    }
    stop();
    onEvents(true);
    wifi_config_t conf = {};   // WiFiSTA.cpp wifi_sta_config: rssi −127 · PMF 가능 · 빠른 훑기 · 신호 순 · 비밀번호 있으면 WPA2 이상
    memcpy(conf.sta.ssid, gStaSsid, strnlen(gStaSsid, sizeof(conf.sta.ssid)));   // _wifi_strncpy(…, 32)
    if (gStaPass[0]) {
        memcpy(conf.sta.password, gStaPass, strnlen(gStaPass, sizeof(conf.sta.password)));
        conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        conf.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    conf.sta.threshold.rssi = -127;
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    conf.sta.scan_method = WIFI_FAST_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    const bool okStart = wifiMode(WIFI_MODE_STA) && esp_wifi_set_config(WIFI_IF_STA, &conf) == ESP_OK &&
                         esp_wifi_connect() == ESP_OK;
    memset(&conf, 0, sizeof conf);   // 비밀번호 사본을 스택에 남기지 않는다
    printf("[NET] WiFi(이름 %u자) 에 붙는 중", (unsigned)strlen(gStaSsid));   // ★ 이름 값은 안 찍는다

    // 아두이노 WiFiGeneric.cpp _eventCallback(STA_DISCONNECTED): 스스로 떠난 것(ASSOC_LEAVE) 말고는 다시 붙는다
    //   (첫 연결은 이유 가리지 않고 한 번 더, 그 뒤는 autoReconnect). 여기서는 기다리는 동안 끊기면 다시 esp_wifi_connect.
    const uint32_t t0 = millis();
    while (okStart && !(gEv.load() & kEvStaGotIp) && millis() - t0 < timeoutMs) {
        delay(300);
        printf(".");
        if ((gEv.fetch_and(~kEvStaDisc) & kEvStaDisc) && gStaDiscReason.load() != WIFI_REASON_ASSOC_LEAVE)
            esp_wifi_connect();
    }
    printf("\n");
    if (!okStart || !(gEv.load() & kEvStaGotIp)) {
        printf("[NET] 못 붙었습니다. 이름·비밀번호를 보세요.\n");
        wifiMode(WIFI_MODE_NULL);
        onEvents(false);
        ::sailBleStart();
        return false;
    }
    setSleep(true);

    snprintf(gSsid, sizeof(gSsid), "%s", gStaSsid);
    netifIpText(gNetSta, gIp, sizeof gIp);
    if (!startHttpd()) printf("[NET] ★ HTTP 서버를 못 열었습니다\n");
    startMdns();
    gEv = 0;            // 붙는 동안 난 끊김 표식은 Join 전이라 버린다 (firmware-rak 도 gMode 가 Join 일 때만 봤다)
    gMode = Mode::Join;
    used();
    saveLastIp(gIp);
    sdUp();
    printf("[NET] 붙었습니다 — http://%s/\n", gIp);
    return true;
}

void stop() {
    const bool wasUp = (gMode != Mode::Off);
    if (gX.active) xferFinish(false, "WiFi 를 끕니다");
    if (wasUp) {
        stopHttpd();
        sdDown();
    }
    if (wasUp) mdns_free();
    onEvents(false);
    if (gWifiStarted) esp_wifi_disconnect();
    wifiMode(WIFI_MODE_NULL);
    gMode = Mode::Off;
    gGoneAt = 0;
    gFast   = false;
    gBusyIp = 0;
    gBusyFile[0] = 0;
    gLastDoneAt = 0;
    gLastIpDone = 0;
    gLastFileDone[0] = 0;
    for (int i = 0; i < kMaxSeen; i++) gSeen[i] = {};
    gLeaseMs = 0;
    gIp[0] = '\0';
    if (wasUp) ::sailBleStart();
}

void poll() {
    if (gMode == Mode::Off) return;

    // WiFi 사건 (처리기가 세운 표식) — firmware-rak onWifiEvent 의 몸통
    const uint32_t ev = gEv.exchange(0);
    if (ev & kEvApConn) {
        printf("[NET] 붙었습니다 (%u대)\n", apStationNum());
        used();
    }
    if ((ev & kEvApDisc) && apStationNum() == 0) {
        printf("[NET] 쓰던 기기가 떨어졌습니다. 곧 끕니다.\n");
        gGoneAt = millis();
    }
    if ((ev & kEvStaDisc) && gMode == Mode::Join) {
        printf("[NET] WiFi 를 놓쳤습니다. 곧 끕니다.\n");
        gGoneAt = millis();
    }

    // 요청 하나 (handleClient)
    httpd_req_t* r = nullptr;
    if (gQueue && xQueueReceive(gQueue, &r, 0) == pdTRUE) {
        serveOne(r);
        if (gStopAfterReq) {
            gStopAfterReq = false;
            delay(120);
            stop();
            return;
        }
    }
    xferPump();

    if (gFast && millis() - gFastAt > kFastHoldMs) fastOff();

    if (gGoneAt && millis() - gGoneAt > kGoneGraceMs) {
        if (gMode == Mode::AP && apStationNum() > 0) {
            gGoneAt = 0;
        } else {
            printf("[NET] 쓰던 기기가 사라져서 WiFi 를 끕니다.\n");
            stop();
            return;
        }
    }
    if (gLeaseMs && millis() - gLastUse > gLeaseMs) {
        printf("[NET] 앱에서 %lu초 동안 연락이 없어 WiFi 를 끕니다.\n", (unsigned long)(gLeaseMs / 1000));
        stop();
        return;
    }
    if (gIdleOffMs && millis() - gLastUse > gIdleOffMs) {
        printf("[NET] %lu초 동안 아무도 안 써서 WiFi 를 끕니다.\n", (unsigned long)(gIdleOffMs / 1000));
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
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid ? ssid : "");
        if (pass) nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    loadCreds();
}

const char* staSsid() { loadCreds(); return gStaSsid; }

// 주변 WiFi 훑기 (WiFiScan.cpp scanNetworks: 능동 · 채널마다 100~300 ms · 숨긴 이름 안 봄). BLE 를 안 내린다.
// ★ WiFi.enableSTA(true) 와 같이, AP 로 켜져 있으면 AP+STA 로 바꾸고 되돌리지 않는다 (아두이노도 안 되돌렸다).
int scan(ScanEntry* out, int max) {
    const bool wasOff = (gMode == Mode::Off);
    if (wasOff) { if (!wifiMode(WIFI_MODE_STA)) { wifiMode(WIFI_MODE_NULL); return 0; } }
    else if (gMode == Mode::AP) wifiMode(WIFI_MODE_APSTA);
    esp_wifi_clear_ap_list();
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;
    int k = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 0) {
            wifi_ap_record_t* recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));   // ★ 스택에 안 올린다
            if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                for (int i = 0; i < n && k < max; i++) {
                    if (recs[i].ssid[0] == 0) continue;
                    snprintf(out[k].ssid, sizeof(out[k].ssid), "%s", (const char*)recs[i].ssid);
                    out[k].rssi = recs[i].rssi;
                    out[k].locked = (recs[i].authmode != WIFI_AUTH_OPEN);
                    k++;
                }
            }
            free(recs);
        }
    }
    esp_wifi_clear_ap_list();
    if (wasOff) wifiMode(WIFI_MODE_NULL);
    return k;
}

int         users()       { return seenCount(); }
int         othersThan(const char* id) { return usersExcept(id); }
const char* otherIpText(const char* id) { return otherName(id); }
Mode        mode()        { return gMode; }
bool        transferring(){ return gX.active; }
const char* ipText()      { return gIp; }
const char* ssidText()    { return gSsid; }
uint32_t    servedFiles() { return gServedFiles; }
uint64_t    servedBytes() { return gServedBytes; }

} // namespace netsrv
