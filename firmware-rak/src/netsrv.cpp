#include "netsrv.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>

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
    gServer.sendHeader("Access-Control-Allow-Origin", "*");
    gServer.sendHeader("Access-Control-Allow-Headers", "*");
    gServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

void handleStatus() {
    cors();
    hlog::Status st;
    hlog::getStatus(&st);

    char body[512];
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
        "\"free_mb\":%llu}",
        gSsid,
        (unsigned long)millis(),
        st.recording ? "true" : "false",
        (unsigned long)st.session,
        (unsigned long)st.navRows,
        (unsigned long)st.imuRows,
        (unsigned long)st.dropped,
        (unsigned long)st.maxStallMs,
        st.cardPresent ? "true" : "false",
        (unsigned long long)(gSdUp ? (SD.totalBytes() - SD.usedBytes()) / 1048576ULL : 0));
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

    const uint32_t t0 = millis();
    gServer.setContentLength(len);
    gServer.send((from == 0 && to == total - 1) ? 200 : 206,
                 "application/octet-stream", "");

    // 4 KB 씩 흘려보낸다. SD 읽기와 WiFi 보내기가 같은 크기라 편하다.
    uint8_t buf[4096];
    uint32_t left = len;
    uint32_t usRead = 0, usWrite = 0;    // 어디서 시간을 쓰는지 갈라 본다
    while (left > 0) {
        const size_t want = (left > sizeof(buf)) ? sizeof(buf) : left;
        uint32_t t = micros();
        const int got = f.read(buf, want);
        usRead += micros() - t;
        if (got <= 0) break;
        t = micros();
        gServer.client().write(buf, (size_t)got);
        usWrite += micros() - t;
        left -= (uint32_t)got;
    }
    f.close();

    const uint32_t dt = millis() - t0;
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
    size_t j = 0;
    for (const char* p = ::sailFullName(); *p && j < sizeof(host) - 1; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') host[j++] = c;
        else if (c >= 'A' && c <= 'Z') host[j++] = (char)(c - 'A' + 'a');
    }
    host[j] = '\0';
    if (j == 0) snprintf(host, sizeof(host), "sail");

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

void routes() {
    gServer.on("/", HTTP_GET, handleRoot);
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
    ::sailBleStop();          // 이름을 읽은 뒤에 내린다

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
    // 파일을 내보내는 동안에는 전기보다 속도가 중요하다. 어차피 wifi 는
    // 훈련이 끝난 뒤 잠깐만 켠다 — 그때는 배가 부두에 있다.
    WiFi.setSleep(false);

    snprintf(gIp, sizeof(gIp), "%s", WiFi.softAPIP().toString().c_str());
    routes();
    gServer.begin();
    startMdns();
    gMode = Mode::AP;
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
    if (strlen(SAIL_WIFI_SSID) == 0) {
        Serial.println("[NET] secrets.h 에 WiFi 이름이 비어 있습니다.");
        return false;
    }
    stop();
    ::sailBleStop();
    WiFi.mode(WIFI_STA);
    WiFi.begin(SAIL_WIFI_SSID, SAIL_WIFI_PASS);
    Serial.printf("[NET] %s 에 붙는 중", SAIL_WIFI_SSID);

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
    // 라디오를 재우지 않는다. 자세히는 startAP 의 주석.
    WiFi.setSleep(false);

    snprintf(gSsid, sizeof(gSsid), "%s", SAIL_WIFI_SSID);
    snprintf(gIp, sizeof(gIp), "%s", WiFi.localIP().toString().c_str());
    routes();
    gServer.begin();
    startMdns();
    gMode = Mode::Join;
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
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    gMode = Mode::Off;
    gIp[0] = '\0';
    // WiFi 를 끄면 BLE 를 되살린다. 워치와 아이폰이 다시 붙는다.
    if (wasUp) ::sailBleStart();
}

void poll() {
    if (gMode == Mode::Off) return;
    gServer.handleClient();
}

Mode        mode()        { return gMode; }
const char* ipText()      { return gIp; }
const char* ssidText()    { return gSsid; }
uint32_t    servedFiles() { return gServedFiles; }
uint64_t    servedBytes() { return gServedBytes; }

} // namespace netsrv
