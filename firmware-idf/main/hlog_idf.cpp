// 기록기 — firmware-rak/src/hlog.cpp (커밋 2b18b17) 를 ESP-IDF v6.1 로 줄 단위로 옮긴 것.
// 선언은 firmware-rak/include/hlog.h 그대로. 동작·바이트 형식·출력 글자는 원본과 같게 둔다.
//
// 바꿔 쓴 것 (근거는 설치된 소스)
//   SD.open / File           → fopen + setvbuf(_IOFBF, 4096) · fwrite · fread · fseek · ftell
//                              아두이노 File 이 바로 이렇게 되어 있다 (FS/src/vfs_api.cpp:305 setvbuf)
//   File.flush               → fflush + fsync (vfs_api.cpp:416-418 과 같다)
//   SD.mkdir/remove/rename   → mkdir · remove · rename  (경로 앞에 마운트 자리 "/sd")
//   SD.totalBytes-usedBytes  → esp_vfs_fat_info 의 남은 바이트
//   File.openNextFile/name   → opendir/readdir (d_name 은 이름만 — 아두이노 name() 과 같다) · stat
//   Preferences              → nvs_open/nvs_get_u32/nvs_get_i32/nvs_set_u32/nvs_commit (이름공간 "sail", 키 그대로)
//   millis/delay             → esp_timer_get_time()/1000 · vTaskDelay
//   Serial.*                 → printf / fwrite(stdout)
//   ESP.getPsramSize 등      → heap_caps_get_info(SPIRAM) · heap_caps_get_free_size (Esp.cpp:155 getPsramSize 와 같은 식)
//   ps_malloc                → heap_caps_malloc(SPIRAM | 8BIT)
//   mbedtls_sha256_*_ret     → PSA psa_hash_setup/update/finish (IDF 6.1 은 mbedtls/sha256.h 가 private)
//   esp_task_wdt_reset       → 이 작업이 워치독에 등록돼 있을 때만 (esp_task_wdt_status). 안 된 작업이 부르면 오류만 난다
//   pinMode/digitalRead      → gpio_set_direction / gpio_set_pull_mode / gpio_get_level

#include <errno.h>
#include "hlog.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "board_rak.h"
#include "sdcard.h"

namespace hlog {
namespace {

inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// 워치독에 등록된 작업만 반려한다 (등록 안 된 작업이 부르면 ESP_ERR_NOT_FOUND 오류 로그)
inline void kickWdt() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

// "/LOGS/..." → "/sd/LOGS/..." (카드는 /sd 에 붙는다. 출력에는 원본처럼 /LOGS/... 를 찍는다)
void sdPath(char* out, size_t cap, const char* logsPath) { snprintf(out, cap, "/sd%s", logsPath); }

// 아두이노 SD.open(path, mode) 와 같게 연다 — 4 KB 버퍼
FILE* sdOpen(const char* logsPath, const char* mode) {
    char p[96];
    sdPath(p, sizeof(p), logsPath);
    FILE* f = fopen(p, mode);
    if (f) setvbuf(f, nullptr, _IOFBF, 4096);
    return f;
}
// 참 = 카드까지 내려갔다. ★ 09-15 외부 검토 R1: 옛 코드(firmware-rak 도)는 fflush·fsync 결과를 버려서,
//   fwrite 가 메모리 버퍼에만 받고 카드 반영이 실패해도 정상 종료로 보고할 수 있었다.
volatile uint8_t gTestFlushFailN = 0;   // 시험: 다음 n 번의 flush 를 실패로 흉내 (rec failflush <n>)
volatile bool    gFlushFaked = false;   // 마지막 flush 실패가 흉내였나 — 실패 기록에 "(시험)" 을 붙인다
// hlg=true 인 부름에만 흉내 실패를 건다 — TXT 사본 flush 가 흉내를 먼저 써 버려 HLG 시험이 안 됐다 (09-15 보드 실측)
bool fileFlush(FILE* f, bool hlg = false) {
    if (!f) return true;
    if (hlg && gTestFlushFailN) { gTestFlushFailN = (uint8_t)(gTestFlushFailN - 1); gFlushFaked = true; errno = EIO; return false; }
    const bool a = fflush(f) == 0;
    const bool b = fsync(fileno(f)) == 0;
    return a && b;
}
long fileSize(FILE* f) {
    struct stat st;
    return (f && fstat(fileno(f), &st) == 0) ? (long)st.st_size : 0;
}
uint64_t cardFreeBytes() {
    uint64_t total = 0, freeB = 0;
    if (esp_vfs_fat_info("/sd", &total, &freeB) != ESP_OK) return 0;
    return freeB;
}
bool endsWith(const char* s, const char* suf) {
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}
bool startsWith(const char* s, const char* pre) { return strncmp(s, pre, strlen(pre)) == 0; }
// snprintf(out, cap, "/LOGS/%s", name) 과 같은 결과 (넘치면 똑같이 잘린다).
// d_name 이 255자까지라 snprintf 로 쓰면 컴파일러가 잘림 경고(-Werror)를 낸다.
void logsName(char* out, size_t cap, const char* name) {
    static const char kPre[] = "/LOGS/";
    size_t o = 0;
    for (size_t i = 0; kPre[i] && o + 1 < cap; ++i) out[o++] = kPre[i];
    for (size_t i = 0; name[i] && o + 1 < cap; ++i) out[o++] = name[i];
    out[o] = '\0';
}

uint32_t nvsU32(const char* key, uint32_t def) {
    nvs_handle_t h;
    uint32_t v = def;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_u32(h, key, &v) != ESP_OK) v = def; nvs_close(h); }
    return v;
}
int32_t nvsI32(const char* key, int32_t def) {
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_i32(h, key, &v) != ESP_OK) v = def; nvs_close(h); }
    return v;
}

// ── 링버퍼 ───────────────────────────────────────────────────────────────
//
// 만드는 쪽(메인 루프, 코어 1)과 쓰는 쪽(코어 0)을 나눈다. SD 카드는 속으로
// 정리하느라 가끔 오래 대답을 안 한다. 우리 카드로 실측한 최악이 1.9초였다
// (SDLOG.md §0). 그걸 메인 루프에서 기다리면 BLE 10 Hz 가 끊긴다.
//
// 크기를 64 KB 로 잡은 근거:
//   초당 만드는 양 3.1 KB  ×  버티고 싶은 시간
//   4 KB  → 1.3초   ← 실측 최악 1.9초를 못 견딘다
//   64 KB → 21초    ← 열 배 여유
constexpr size_t kBufSize  = 65536;
constexpr size_t kChunk    = 4096;  // 카드에 한 번에 내보내는 단위 (4KB 정렬)
constexpr uint32_t kFlushMs = 5000; // 못 박는 주기. 전원이 끊기면 이 뒤가 날아간다
constexpr uint64_t kMinFreeBytes = 90ULL * 1024 * 1024;  // 기록 시작에 필요한 남은 자리 — 8시간 분량 (TRANSFER.md §1)

char*  gBuf = nullptr;
volatile size_t gHead = 0;
volatile size_t gTail = 0;

// 링버퍼에 얼마나 차 있나. 머리가 꼬리를 앞지르면 한 바퀴 돌아온 것이라
// 뺄셈을 반대로 한다.
//
// bufFree 가 1 을 빼는 이유. 머리와 꼬리가 같은 자리면 "비었다" 로 보기로
// 정했다. 그래서 꽉 채우면 빈 것과 구분이 안 된다. 한 칸을 늘 비워 둔다.
size_t bufUsed() {
    const size_t h = gHead, t = gTail;
    return (h >= t) ? (h - t) : (kBufSize - t + h);
}
size_t bufFree() { return kBufSize - bufUsed() - 1; }

// ── 텍스트 사본 ──────────────────────────────────────────────────────────
//
// 10초에 한 줄. 카드를 꽂자마자 파서 없이 눈으로 확인하는 용이다.
// ★ 램에 모았다가 1분에 한 번 쏟는다. 10초마다 카드를 건드리면 바이너리
//   파일 자리가 조각나고, 조각이 나면 멈춤이 더 자주 온다.
constexpr size_t kTextBufSize = 4096;
char   gTextBuf[kTextBufSize];
size_t gTextUsed = 0;
uint32_t gTextLastFlush = 0;
uint32_t gTextLastRow   = 0;

// ── 상태 ─────────────────────────────────────────────────────────────────

// ★ 상태 하나 (hlog_write.h RecState — Closed / Recording / Draining).
//   전이는 gStateMux 안에서 toDraining / toClosed 로만.
volatile RecState gState = RecState::Closed;
bool isRec() { return gState == RecState::Recording; }

portMUX_TYPE gTextMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool gTextFlushWanted = false;
char gTxtOut[kTextBufSize];                 // 일꾼이 잠금 밖에서 쓰는 사본
uint32_t gLostBytes = 0;            // 마지막으로 닫을 때 못 쓴 바이트
portMUX_TYPE gStateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gResMux = portMUX_INITIALIZER_UNLOCKED;
recctl::SessionResult gRes;
volatile uint32_t gSlowCloseMs = 0;  // 시험용: 닫기 직전에 한 번 쉰다 (rec slow)
int32_t  gTzMin = 540;              // 이름 지을 시간대. start() 가 NVS 에서 읽어 둔다 — 일꾼은 NVS 를 안 만진다
uint32_t gDurS = 0;                 // 닫기 시작 때 잰 세션 길이
void finishSession(bool complete);
const char* gLastErrorShort = nullptr;

FILE*    gBin = nullptr;
FILE*    gTxt = nullptr;
uint32_t gSession = 0;
// /LOGS/S00014_19700103-0043_nosat.HLG = 36자. 넉넉히 잡는다.
char     gPath[64]    = {0};
char     gTxtPath[64] = {0};
uint32_t gNavRows = 0, gImuRows = 0;
uint64_t gBytes = 0;
uint32_t gStartedMs = 0;
uint32_t gDropped = 0, gWaited = 0, gMaxStall = 0, gMaxFill = 0;
uint64_t gFreeBytes = 0;
const char* gLastError = nullptr;

volatile uint8_t gPendingEvent = 0;
bool gFirstNav = true;
uint32_t gUtcStart = 0;     // 첫 fix 의 UNIX 시각 (초). 0 이면 아직 못 잡음
uint16_t gUtcStartMs = 0;

TaskHandle_t gWriter = nullptr;
const char* gBootWhy = "?";
const char* gLastFailLine = nullptr;  // 다음 세션 TXT 머리에 적는다
const char* gSessionNote  = nullptr;  // 방위 설정 등
volatile uint8_t gTestFailN = 0;      // 시험용 가짜 실패 남은 횟수
uint32_t gWriteRetries = 0;           // 다시 써서 살린 횟수
constexpr uint8_t kWriteRetries = 3;

// 카드가 꽂혀 있나. RAK15002 가 IO 슬롯 38번으로 알려준다 (GPIO39, 꽂히면 LOW).
// 부를 때마다 방향을 다시 잡는다 — 남이 바꿔 놓고 갔을 수 있다 (board_rak.h).
bool cardPresent() {
    const auto pin = static_cast<gpio_num_t>(rak::kSdCardDetect);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    return gpio_get_level(pin) == 0;
}

// ── 버퍼에 밀어넣기 ──────────────────────────────────────────────────────
//
// 자리가 없으면 **기다린다. 버리지 않는다.** 기록이 이 장비의 본체다.
bool push(const uint8_t* p, size_t n) {
    if (bufFree() < n) {
        ++gWaited;
        const uint32_t t0 = millis();
        while (bufFree() < n && millis() - t0 < 5000) vTaskDelay(pdMS_TO_TICKS(2));
        if (bufFree() < n) { ++gDropped; return false; } // 카드가 죽은 경우
    }
    size_t h = gHead;
    const size_t first = (h + n <= kBufSize) ? n : (kBufSize - h);
    memcpy(gBuf + h, p, first);
    if (first < n) memcpy(gBuf, p + first, n - first);
    gHead = (h + n) % kBufSize;

    const size_t fill = bufUsed() * 100 / kBufSize;
    if (fill > gMaxFill) gMaxFill = (uint32_t)fill;
    return true;
}

// ── 리틀엔디언 쓰기 ──────────────────────────────────────────────────────
inline void put8 (uint8_t* p, size_t& o, uint8_t v)  { p[o++] = v; }
inline void put16(uint8_t* p, size_t& o, uint16_t v) { p[o++] = (uint8_t)v; p[o++] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, size_t& o, uint32_t v) {
    p[o++] = (uint8_t)v;        p[o++] = (uint8_t)(v >> 8);
    p[o++] = (uint8_t)(v >> 16); p[o++] = (uint8_t)(v >> 24);
}

// ── 쓰기 작업 (코어 0) ───────────────────────────────────────────────────

// 메인 루프가 모은 텍스트를 옮겨 쓴다. 잠금 안에서는 복사만 한다.
void writerFlushText(bool force) {
    if (!gTxt) return;
    size_t n = 0;
    portENTER_CRITICAL(&gTextMux);
    if (gTextUsed && (force || gTextFlushWanted)) {
        memcpy(gTxtOut, gTextBuf, gTextUsed);
        n = gTextUsed;
        gTextUsed = 0;
        gTextFlushWanted = false;
    }
    portEXIT_CRITICAL(&gTextMux);
    if (n) {
        fwrite(gTxtOut, 1, n, gTxt);
        fileFlush(gTxt);
    }
}

// 링버퍼 꼬리에서 최대 maxBytes 를 쓴다. 다 썼으면 true.
// 쓴 만큼 꼬리를 바로 넘긴다 — 다시 시도할 때 앞부분을 또 쓰지 않는다 (writeAll).
struct WriteReport { uint8_t tries = 0; int err = 0; bool faked = false; size_t wrote = 0; size_t asked = 0; };
bool writeFromRing(size_t maxBytes, WriteReport* r) {
    while (r->wrote < maxBytes) {
        const size_t used = bufUsed();
        if (!used) break;
        const size_t t = gTail;
        size_t n = used;
        if (n > maxBytes - r->wrote) n = maxBytes - r->wrote;
        if (t + n > kBufSize) n = kBufSize - t;
        r->asked += n;
        uint8_t tries = 0;
        const size_t done = writeAll(
            (const uint8_t*)(gBuf + t), n,
            [&](const uint8_t* p, size_t len) -> size_t {
                if (gTestFailN) { gTestFailN = (uint8_t)(gTestFailN - 1); r->faked = true; r->err = EIO; return 0; }
                errno = 0;
                const size_t got = fwrite(p, 1, len, gBin);
                if (got != len && errno) r->err = errno;
                return got;
            },
            [&](size_t k) { gTail = (gTail + k) % kBufSize; gBytes += k; },
            [&](uint8_t k) { vTaskDelay(pdMS_TO_TICKS(50 * k)); },
            kWriteRetries, &tries);
        r->tries += tries;
        r->wrote += done;
        if (done < n) return false;
    }
    return true;
}

// 이 세션의 첫 오류를 적는다. 이미 있으면 안 바꾼다 (recctl::setFirst).
void noteFirst(uint8_t kind, const WriteReport& r) {
    const bool card = cardPresent();              // 핀 읽기는 임계 구역 밖에서
    const uint32_t recSec = (millis() - gStartedMs) / 1000;
    portENTER_CRITICAL(&gResMux);
    if (recctl::setFirst(gRes, kind)) {
        gRes.recSec = recSec;
        gRes.want   = (uint32_t)r.asked;
        gRes.wrote  = (uint32_t)r.wrote;
        gRes.err    = r.err;
        gRes.tries  = r.tries;
        gRes.card   = card;
        gRes.bytes  = (uint32_t)gBytes;
        gRes.fake   = r.faked;
    }
    portEXIT_CRITICAL(&gResMux);
}

// 카드에 실제로 쓰는 일꾼. **코어 0 에서 혼자 돈다.**
//
//   Recording  4 KB 가 차거나 0.5초가 지나면 쓴다. 못 쓰면 닫는 중(Draining)으로 넘기고 마무리한다.
//   Draining   남은 것을 다 쓰고 텍스트도 쏟고 닫고 마무리한다.
//   그 밖      쉰다.
void writerTask(void*) {
    uint32_t lastWrite = millis(), lastFlush = millis();
    for (;;) {
        const RecState st = gState;

        if (st == RecState::Recording) {
            const size_t used = bufUsed();
            const bool due = (used >= kChunk) || (used > 0 && millis() - lastWrite >= 500);
            if (due) {
                const uint32_t a = millis();
                WriteReport r;
                const bool ok = writeFromRing(kChunk, &r);
                const uint32_t dt = millis() - a;
                if (dt > gMaxStall) gMaxStall = dt;
                lastWrite = millis();
                if (!ok) {
                    noteFirst(recctl::kErrWrite, r);
                    portENTER_CRITICAL(&gStateMux);
                    toDraining(gState);
                    portEXIT_CRITICAL(&gStateMux);
                    gLastError = "카드 쓰기 실패";
                    gLastErrorShort = "쓰기 실패";
                    gDurS = (millis() - gStartedMs) / 1000;
                    finishSession(false);       // 닫고 머리글(closed=0)·이름까지 고쳐 본다
                    continue;
                }
                if (r.tries) ++gWriteRetries;
            }
            writerFlushText(false);
            if (millis() - lastFlush >= kFlushMs) {
                const uint32_t b = millis();
                const bool flushed = fileFlush(gBin, true);
                const uint32_t df = millis() - b;
                if (df > gMaxStall) gMaxStall = df;
                lastFlush = millis();
                if (!flushed) {
                    // ★ 카드 반영 실패 = 쓰기 실패와 같은 길 (첫 오류 적기 → 닫기 → 루프가 새 파일로 다시 건다). R1
                    WriteReport fr;
                    fr.err = errno ? errno : EIO;
                    fr.faked = gFlushFaked;
                    gFlushFaked = false;
                    noteFirst(recctl::kErrWrite, fr);
                    portENTER_CRITICAL(&gStateMux);
                    toDraining(gState);
                    portEXIT_CRITICAL(&gStateMux);
                    gLastError = "카드 반영(flush) 실패";
                    gLastErrorShort = "쓰기 실패";
                    gDurS = (millis() - gStartedMs) / 1000;
                    finishSession(false);
                    continue;
                }
            }
            if (!due) vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (st == RecState::Draining) {
            WriteReport r;
            const bool ok = writeFromRing((size_t)-1, &r);
            gLostBytes = (uint32_t)bufUsed();
            const bool complete = ok && gLostBytes == 0;
            if (!complete) {
                noteFirst(recctl::kErrDrainShort, r);
                gLastError = "닫으면서 다 못 썼습니다";
                gLastErrorShort = "저장 실패";
            }
            writerFlushText(true);
            if (gSlowCloseMs) {                       // 시험: 닫기 직전 한 번 쉰다
                const uint32_t ms = gSlowCloseMs;
                gSlowCloseMs = 0;
                vTaskDelay(pdMS_TO_TICKS(ms));
            }
            finishSession(complete);
            lastWrite = lastFlush = millis();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// /sd/LOGS 에서 이름이 pre 로 시작하고 suf 로 끝나는 첫 파일을 찾아 "/LOGS/이름" 으로 적는다.
bool findLog(const char* pre, const char* suf, char* out, size_t cap) {
    out[0] = '\0';
    DIR* dir = opendir("/sd/LOGS");
    if (!dir) return false;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        if (startsWith(e->d_name, pre) && endsWith(e->d_name, suf)) {
            logsName(out, cap, e->d_name);
            break;
        }
    }
    closedir(dir);
    return out[0] != '\0';
}

} // namespace

// ── CRC-16/CCITT-FALSE ───────────────────────────────────────────────────
uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ── 바깥에서 부르는 것들 ─────────────────────────────────────────────────

void noteBootReason(const char* why) { if (why) gBootWhy = why; }

// 링버퍼를 잡고 코어 0 에 일꾼을 띄운다. 켤 때 한 번만 부른다.
// ★ PSRAM 을 먼저 쓴다. 내부 RAM 은 BLE 스택이 크게 쓴다.
void begin() {
    if (gBuf) return;
    multi_heap_info_t pinfo;
    heap_caps_get_info(&pinfo, MALLOC_CAP_SPIRAM);   // 아두이노 ESP.getPsramSize 와 같은 식
    printf("[LOG] PSRAM %u 바이트 (남은 %u) · 내부 힙 남은 %u\n",
           (unsigned)(pinfo.total_free_bytes + pinfo.total_allocated_bytes),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    gBuf = (char*)heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // PSRAM 먼저
    if (!gBuf) gBuf = (char*)malloc(kBufSize);
    if (!gBuf) {
        printf("[LOG] 버퍼를 못 잡았습니다 — 기록 기능이 꺼집니다\n");
        return;
    }
    if (xTaskCreatePinnedToCore(writerTask, "hlog", 4096, nullptr, 1, &gWriter, 0) != pdPASS) {
        printf("[LOG] ★ 쓰기 작업을 못 띄웠습니다 — 기록 기능이 꺼집니다\n");
        free(gBuf);
        gBuf = nullptr;
        gWriter = nullptr;
        return;
    }
    printf("[LOG] 쓰기 작업 코어 0 (버퍼 %u KB = 초당 3.1KB 기준 %.0f초치)\n",
           (unsigned)(kBufSize / 1024), kBufSize / 3080.0f);
}

/**
 * 이름을 짓는다. **모양은 늘 하나다.**
 *
 *     S00012_20260825-1432.HLG          위성을 잡은 세션
 *     S00014_19700103-0040_nosat.HLG    못 잡은 세션
 *
 * 시각은 1. 이 세션의 첫 fix  2. 보드 시계  3. 그래도 없으면 시계가 말하는 그대로(1970) + `_nosat`.
 * ★ 이름은 그 고장 시각 (NVS tz_min, 기본 540). 머리글에는 UTC 그대로.
 */
static void nameFor(char* out, size_t cap, uint32_t session,
                    uint32_t utc, const char* ext) {
    const bool sure = utc != 0;
    if (!utc) utc = (uint32_t)time(nullptr);      // 보드 시계
    const int32_t tzMin = gTzMin;
    const time_t local = (time_t)((int64_t)utc + (int64_t)tzMin * 60);
    struct tm tmv;
    gmtime_r(&local, &tmv);                 // 이미 더했으니 gmtime 으로 푼다
    snprintf(out, cap, "/LOGS/S%05u_%04d%02d%02d-%02d%02d%s.%s",
             (unsigned)session, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, sure ? "" : "_nosat", ext);
}

bool start(const Header& h) {
    if (!gBuf || !gWriter) { gLastError = "버퍼 없음"; gLastErrorShort = "기록기 없음"; return false; }
    if (!canStart(gState)) {
        gLastError = (gState == RecState::Draining) ? "앞 기록을 닫는 중" : "이미 기록 중";
        gLastErrorShort = (gState == RecState::Draining) ? "닫는 중" : "이미 기록 중";
        return false;
    }
    {
        portENTER_CRITICAL(&gResMux);
        const bool pending = !gRes.consumed;
        portEXIT_CRITICAL(&gResMux);
        if (pending) { gLastError = "앞 기록 결과 정리 중"; gLastErrorShort = "정리 중"; return false; }
    }
    if (!cardPresent()) { gLastError = "카드가 안 꽂혀 있습니다"; gLastErrorShort = "카드 없음"; return false; }

    const uint32_t tA = millis();
    if (!sdcard::acquire(sdcard::Owner::Recorder)) {
        const bool used = sdcard::lastRefusal() == sdcard::Refusal::Busy;
        gLastError = used ? "카드를 다른 곳(파일 전송·진단)이 쓰는 중입니다" : "마운트 실패 — sd 명령으로 이유를 보세요";
        gLastErrorShort = used ? "카드 사용 중" : "마운트 실패";
        return false;
    }
    mkdir("/sd/LOGS", 0777);                 // 이미 있으면 EEXIST — 아두이노 SD.mkdir 도 결과를 안 본다
    // ★ 남은 자리를 시작 전에 본다 (체크리스트 12). 기준은 8시간 분량 90 MB.
    {
        const uint64_t freeB = cardFreeBytes();
        gFreeBytes = freeB;
        if (freeB < kMinFreeBytes) {
            sdcard::release(sdcard::Owner::Recorder);
            gLastError = "카드 남은 자리가 8시간 분량(90 MB)보다 적습니다";
            gLastErrorShort = "카드 자리 부족";
            return false;
        }
    }
    const uint32_t tB = millis();

    // ── 파일 이름 — 번호가 순서를 맡는다 (NVS sess_n, 늘 올라간다) ──
    nvs_handle_t nh = 0;
    const bool nvsOk = (nvs_open("sail", NVS_READWRITE, &nh) == ESP_OK);
    uint32_t sessN = 0, bootN = 0;
    int32_t tz = 540;
    if (nvsOk) {
        if (nvs_get_u32(nh, "sess_n", &sessN) != ESP_OK) sessN = 0;
        if (nvs_get_i32(nh, "tz_min", &tz) != ESP_OK) tz = 540;
    }
    uint32_t next = sessN + 1;
    gTzMin = tz;   // 이름 고치기용 (일꾼이 NVS 를 안 만지게)

    // ★ 카드에 있는 제일 큰 번호보다도 커야 한다 (NVS 가 날아갔거나 다른 보드의 카드).
    {
        DIR* dir = opendir("/sd/LOGS");
        if (dir) {
            struct dirent* e;
            while ((e = readdir(dir)) != nullptr) {
                const char* nm = e->d_name;
                if (strlen(nm) < 6 || nm[0] != 'S') continue;
                // 아두이노 String::substring(1,6).toInt() — 앞의 숫자만 읽는다
                char num[6];
                memcpy(num, nm + 1, 5);
                num[5] = '\0';
                const uint32_t n = (uint32_t)atol(num);
                if (n >= next) next = n + 1;
            }
            closedir(dir);
        }
    }
    gSession = next;
    printf("[LOG] 시작 단계  마운트 %ums · 폴더 훑기 %ums\n",
           (unsigned)(tB - tA), (unsigned)(millis() - tB));

    nameFor(gPath, sizeof(gPath), gSession, 0, "HLG");
    nameFor(gTxtPath, sizeof(gTxtPath), gSession, 0, "TXT");
    if (nvsOk) {
        nvs_set_u32(nh, "sess_n", gSession);
        if (nvs_get_u32(nh, "boot_n", &bootN) != ESP_OK) bootN = 0;
        nvs_commit(nh);
        nvs_close(nh);
    }
    const uint16_t bootCount = (uint16_t)bootN;

    gBin = sdOpen(gPath, "w");
    if (!gBin) { gLastError = "파일을 못 열었습니다"; gLastErrorShort = "파일 못 엶"; sdcard::release(sdcard::Owner::Recorder); return false; }
    gTxt = sdOpen(gTxtPath, "w");

    // ── 128바이트 머리글 ────────────────────────────────────────────────
    uint8_t hdr[kHeaderSize];
    memset(hdr, 0, sizeof(hdr));
    size_t o = 0;
    put8(hdr, o, kMagic0); put8(hdr, o, kMagic1);
    put8(hdr, o, kMagic2); put8(hdr, o, kMagic3);
    put8(hdr, o, kVerMajor);
    put8(hdr, o, kVerMinor);
    put16(hdr, o, (uint16_t)kHeaderSize);
    memcpy(hdr + o, h.mac, 6); o += 6;
    put16(hdr, o, h.fwVersion);
    put8(hdr, o, h.hwRev);
    put8(hdr, o, h.gnssType);
    put32(hdr, o, gSession);
    put16(hdr, o, bootCount);
    put32(hdr, o, 0);                 // utc_start — 첫 fix 때 채운다
    put16(hdr, o, 0);                 // utc_start_ms
    for (int i = 0; i < 4; ++i) put16(hdr, o, (uint16_t)h.mountQuat[i]);
    put8(hdr, o, h.imuCalStatus);
    put8(hdr, o, kRateNav);
    put8(hdr, o, kRateImu);
    hdr[kOffImuType]  = h.imuType;
    hdr[kOffTimeRef]  = h.timeRef;
    hdr[kOffMagScale] = h.magScale;
    hdr[kOffGnssDyn]  = h.gnssDyn;
    hdr[kOffGnssHz]   = h.gnssHz;
    hdr[kOffSogSrc]   = h.sogSrc;
    hdr[kOffQuatSrc]  = h.quatSrc;
    hdr[kOffHeelAxis]  = h.heelAxis;
    hdr[kOffHeelSign]  = h.heelSign;
    hdr[kOffPitchAxis] = h.pitchAxis;
    hdr[kOffPitchSign] = h.pitchSign;
    memcpy(hdr + kOffHeelOff,  &h.heelOff,  4);
    memcpy(hdr + kOffPitchOff, &h.pitchOff, 4);
    memcpy(hdr + kOffPrevSession, &h.prevSession, 4);
    hdr[kOffHdgFormula] = h.hdgFormula;
    hdr[kOffHdgAxisA]   = h.hdgAxisA;
    hdr[kOffHdgAxisB]   = h.hdgAxisB;
    hdr[kOffHdgSignA]   = h.hdgSignA;
    hdr[kOffHdgSignB]   = h.hdgSignB;
    memcpy(hdr + kOffHdgOff,  &h.hdgOff,  4);
    memcpy(hdr + kOffHdgDecl, &h.hdgDecl, 4);
    memcpy(hdr + kOffMagHi,   h.magHi,   12);
    const uint8_t magSiIndex[6] = {0, 1, 2, 4, 5, 8};
    for (int i = 0; i < 6; ++i) {
        long q = lroundf(h.magSi[magSiIndex[i]] * kMagSiScale);
        if (q < -32768) q = -32768;
        if (q >  32767) q =  32767;
        const uint16_t bits = (uint16_t)(int16_t)q;
        hdr[kOffMagSi + i * 2] = (uint8_t)bits;
        hdr[kOffMagSi + i * 2 + 1] = (uint8_t)(bits >> 8);
    }
    hdr[kOffMagCalVer] = h.magCalVersion;
    const uint16_t hcrc = crc16(hdr, 126);
    hdr[126] = (uint8_t)hcrc; hdr[127] = (uint8_t)(hcrc >> 8);
    // ★ 머리글이 다 들어갔는지 본다. 안 들어간 파일은 나중에 못 읽는다.
    if (fwrite(hdr, 1, sizeof(hdr), gBin) != sizeof(hdr)) {
        fclose(gBin); gBin = nullptr;
        if (gTxt) { fclose(gTxt); gTxt = nullptr; }
        sdcard::release(sdcard::Owner::Recorder);
        gLastError = "머리글을 못 썼습니다";
        gLastErrorShort = "머리글 실패";
        return false;
    }
    if (!fileFlush(gBin, true)) {   // ★ R1: 머리글이 카드까지 안 내려갔으면 시작하지 않는다
        fclose(gBin); gBin = nullptr;
        if (gTxt) { fclose(gTxt); gTxt = nullptr; }
        sdcard::release(sdcard::Owner::Recorder);
        gLastError = "머리글을 카드에 못 내렸습니다 (flush 실패)";
        gLastErrorShort = "머리글 실패";
        return false;
    }

    if (gTxt) {
        fprintf(gTxt, "# sail 경기정 모듈 — 세션 %u\n", (unsigned)gSession);
        fprintf(gTxt, "# 바이너리 원본: %s\n", gPath);
        fprintf(gTxt, "# 이 파일은 10초에 한 줄짜리 사본입니다. 눈으로 보는 용입니다.\n");
        fprintf(gTxt, "# 힐·피치·헤딩은 참고값입니다 (가속도에서 뽑음).\n");
        fprintf(gTxt, "# 속도 세 가지: 앞의 kn=도플러(RMC, 파일에 들어가는 값) "
                      "· pv=NAV-PV · pos=위치차분\n");
        fprintf(gTxt, "# f=칩이 밝힌 속도 표식 (4 이상이면 방금 잰 값, 3 이면 "
                      "옛날 값을 들고 있음) · c=침로 오차(도)\n");
        fprintf(gTxt, "# GNSS 동역학모델 %u / %u Hz,  IMU %s,  속도는 도플러 원본\n",
                h.gnssDyn, h.gnssHz,
                h.imuType == kImuBNO085 ? "BNO085" : "MPU-9250");
        fprintf(gTxt, "# 이 보드가 지난번에 꺼진 이유: %s\n", gBootWhy);
        if (gLastFailLine) fprintf(gTxt, "# 지난 기록 실패: %s\n", gLastFailLine);
        if (gSessionNote) fputs(gSessionNote, gTxt);
        fprintf(gTxt, "#\n");
        fileFlush(gTxt);
    }

    gNavRows = gImuRows = 0;
    gBytes = kHeaderSize;
    gDropped = gWaited = gMaxStall = gMaxFill = 0;
    gLastError = nullptr;
    gLastErrorShort = nullptr;
    gLostBytes = 0;
    portENTER_CRITICAL(&gTextMux);
    gTextUsed = 0;
    gTextFlushWanted = false;
    portEXIT_CRITICAL(&gTextMux);
    gTextLastFlush = gTextLastRow = millis();
    gFirstNav = true;
    gUtcStart = 0; gUtcStartMs = 0;
    gHead = 0; gTail = 0;
    gStartedMs = millis();
    portENTER_CRITICAL(&gResMux);
    gRes = recctl::SessionResult();         // consumed=true — 아직 끝난 결과 없음
    gRes.session = gSession;
    portEXIT_CRITICAL(&gResMux);
    portENTER_CRITICAL(&gStateMux);
    gState = RecState::Recording;
    portEXIT_CRITICAL(&gStateMux);

    printf("[LOG] 기록 시작 — %s  (+ %s)\n", gPath, gTxtPath);
    if (h.prevSession) {
        printf("[LOG] 세션 %u 가 끊겨서 이어받았습니다\n", (unsigned)h.prevSession);
    }
    return true;
}

namespace {
// 일꾼이 부른다. 파일을 닫고 · 머리글과 이름을 고치고 · 카드를 놓고 · 결과를 하나 남긴다.
bool rewriteHeader(bool closed) {
    FILE* h = sdOpen(gPath, "r+");
    if (!h) return false;
    bool ok = false;
    uint8_t hdr[kHeaderSize];
    if (fread(hdr, 1, kHeaderSize, h) == kHeaderSize && memcmp(hdr, "HHLG", 4) == 0) {
        memcpy(hdr + 24, &gUtcStart, 4);
        memcpy(hdr + 28, &gUtcStartMs, 2);
        memcpy(hdr + kOffDurationS, &gDurS, 4);
        memcpy(hdr + kOffNavRows, (const void*)&gNavRows, 4);
        memcpy(hdr + kOffImuRows, (const void*)&gImuRows, 4);
        memcpy(hdr + kOffDropped, (const void*)&gDropped, 4);
        // ★ 다 쓰고 닫혔을 때만 1.
        hdr[kOffClosed] = closed ? 1 : 0;
        const uint16_t c = crc16(hdr, 126);
        hdr[126] = (uint8_t)c; hdr[127] = (uint8_t)(c >> 8);
        // ★ R1: 자리 옮기기 · 쓰기 · 카드 반영 · 닫기가 **모두** 돼야 고친 것이다
        ok = fseek(h, 0, SEEK_SET) == 0;
        ok = ok && fwrite(hdr, 1, kHeaderSize, h) == kHeaderSize;
        ok = fileFlush(h) && ok;
    }
    if (fclose(h) != 0) ok = false;
    return ok;
}

void finishSession(bool complete) {
    // ★ R1: 본문이 카드까지 내려가고 닫혀야 "다 썼다". 아니면 닫힘 표시(closed=1)도 안 하고 첫 오류로 남긴다.
    bool bodyOk = true;
    if (gBin) {
        bodyOk = fileFlush(gBin, true);
        if (fclose(gBin) != 0) bodyOk = false;
        gBin = nullptr;
    }
    if (gTxt) { fileFlush(gTxt); fclose(gTxt); gTxt = nullptr; }   // TXT 는 사본 — 실패해도 HLG 판정은 안 바꾼다
    if (complete && !bodyOk) {
        WriteReport fr;
        fr.err = errno ? errno : EIO;
        fr.faked = gFlushFaked;
        gFlushFaked = false;
        noteFirst(recctl::kErrDrainShort, fr);
        gLastError = "닫으면서 카드에 다 못 내렸습니다 (flush/close 실패)";
        gLastErrorShort = "닫기 실패";
        complete = false;
    }
    const bool hdrOk = rewriteHeader(complete);
    // 이름을 시각으로 바꾼다 — 이 세션에서 위성을 잡았으면 그 시각으로.
    if (gUtcStart) {
        char binNew[48], txtNew[48];
        nameFor(binNew, sizeof(binNew), gSession, gUtcStart, "HLG");
        nameFor(txtNew, sizeof(txtNew), gSession, gUtcStart, "TXT");
        char a[96], b[96];
        sdPath(a, sizeof(a), gPath); sdPath(b, sizeof(b), binNew);
        if (strcmp(gPath, binNew) != 0 && rename(a, b) == 0) snprintf(gPath, sizeof(gPath), "%s", binNew);
        sdPath(a, sizeof(a), gTxtPath); sdPath(b, sizeof(b), txtNew);
        if (strcmp(gTxtPath, txtNew) != 0 && rename(a, b) == 0) snprintf(gTxtPath, sizeof(gTxtPath), "%s", txtNew);
    }
    gFreeBytes = cardFreeBytes();
    sdcard::release(sdcard::Owner::Recorder);   // 마무리까지 끝났다. 이제 누구든 쥘 수 있다
    if (complete && !hdrOk) {
        gLastError = "머리글을 못 고쳤습니다 (내용은 들어 있음)";
        gLastErrorShort = "머리글 실패";
    }
    portENTER_CRITICAL(&gResMux);
    gRes.session   = gSession;
    gRes.drained   = complete;
    gRes.headerOk  = hdrOk;
    gLostBytes = complete ? 0 : (uint32_t)bufUsed();
    gRes.lostBytes = gLostBytes;
    gRes.durS      = gDurS;
    if (complete && !hdrOk) recctl::setFirst(gRes, recctl::kErrHeader);
    gRes.consumed  = false;
    portEXIT_CRITICAL(&gResMux);
    portENTER_CRITICAL(&gStateMux);
    toClosed(gState);                   // 이게 "끝났다" 는 답이다
    portEXIT_CRITICAL(&gStateMux);
}
} // namespace

bool requestStop() {
    if (gState != RecState::Recording) return false;       // 빠른 거절. 확정은 아래 잠금 안에서
    const uint32_t durS = (millis() - gStartedMs) / 1000;
    char foot[256];
    int n = snprintf(foot, sizeof(foot),
                     "#\n# 끝 — %u분 %u초,  NAV %u줄  IMU %u줄\n"
                     "# 버린 줄 %u  기다린 횟수 %u  최대 멈춤 %ums  버퍼 최고 %u%%\n%s",
                     (unsigned)(durS / 60), (unsigned)(durS % 60),
                     (unsigned)gNavRows, (unsigned)gImuRows,
                     (unsigned)gDropped, (unsigned)gWaited,
                     (unsigned)gMaxStall, (unsigned)gMaxFill,
                     gDropped ? "# ★ 버린 줄이 있습니다. 이 세션은 구멍이 있습니다.\n" : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(foot)) n = sizeof(foot) - 1;
    bool mine = false;
    portENTER_CRITICAL(&gStateMux);
    if (gState == RecState::Recording) {
        portENTER_CRITICAL(&gTextMux);
        if (gTextUsed + (size_t)n < kTextBufSize) {
            memcpy(gTextBuf + gTextUsed, foot, (size_t)n);
            gTextUsed += (size_t)n;
        }
        portEXIT_CRITICAL(&gTextMux);
        gDurS = durS;
        mine = toDraining(gState);
    }
    portEXIT_CRITICAL(&gStateMux);
    return mine;
}

bool poll(recctl::SessionResult* out) {
    bool got = false;
    portENTER_CRITICAL(&gResMux);
    if (!gRes.consumed && gState == RecState::Closed) {
        if (out) *out = gRes;
        gRes.consumed = true;
        got = true;
    }
    portEXIT_CRITICAL(&gResMux);
    return got;
}

recctl::Phase phase() {
    const RecState s = gState;
    if (s == RecState::Recording) return recctl::Phase::Recording;
    if (s == RecState::Draining)  return recctl::Phase::Closing;
    return recctl::Phase::Idle;
}

void testSlowClose(uint32_t ms) { gSlowCloseMs = ms; }

// 항법 한 줄(40바이트, v1.2)을 링버퍼에 넣는다. 10 Hz 로 부른다. 원본 그대로, 끝의 hdg 만 계산값.
void writeNav(const NavSample& s) {
    if (!isRec()) return;
    uint8_t r[kNavSize];
    size_t o = 0;
    put8 (r, o, kTypeNav);
    put32(r, o, s.localMs);
    put32(r, o, s.itow);
    put16(r, o, s.week);
    put32(r, o, (uint32_t)s.lat);
    put32(r, o, (uint32_t)s.lon);
    put16(r, o, s.sog);
    put16(r, o, s.cog);
    put8 (r, o, s.numSv);
    put8 (r, o, s.fix);
    put16(r, o, s.hAcc);
    put16(r, o, s.battMv);

    uint8_t ev = s.event;
    if (gPendingEvent) { ev |= gPendingEvent; gPendingEvent = 0; }
    if (gFirstNav)     { ev |= kEvFirst; gFirstNav = false; }
    put8(r, o, ev);

    for (int i = 0; i < 3; ++i) put16(r, o, (uint16_t)s.mag[i]);
    put16(r, o, s.hdg);
    const uint16_t c = crc16(r, kNavSize - 2);
    put16(r, o, c);

    if (push(r, kNavSize)) ++gNavRows;
}

// 9축 한 줄(19바이트)을 링버퍼에 넣는다. 100 Hz 로 부른다.
void writeImu(const ImuSample& s) {
    if (!isRec()) return;
    uint8_t r[kImuSize];
    size_t o = 0;
    put8 (r, o, kTypeImu);
    put32(r, o, s.localMs);
    for (int i = 0; i < 3; ++i) put16(r, o, (uint16_t)s.acc[i]);
    for (int i = 0; i < 3; ++i) put16(r, o, (uint16_t)s.gyr[i]);
    const uint16_t c = crc16(r, kImuSize - 2);
    put16(r, o, c);

    if (push(r, kImuSize)) ++gImuRows;
}

// 사람이 눈으로 읽는 사본 한 줄. 10초에 한 번 부른다. 램에 모았다가 1분에 한 번 쏟는다.
void writeText(const NavSample& s, const TextSample& t) {
    if (!isRec()) return;

    char line[320];
    int n = 0;
    const uint32_t sec = (millis() - gStartedMs) / 1000;
    n += snprintf(line + n, sizeof(line) - n, "%02u:%02u:%02u  ",
                  (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60), (unsigned)(sec % 60));

    if (s.fix && s.lat != kLatLonInvalid) {
        n += snprintf(line + n, sizeof(line) - n, "fix%u sat%2u  %+.6f %+.6f  ",
                      s.fix, s.numSv, s.lat / 1e7, s.lon / 1e7);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "fix0 sat%2u  %-22s", s.numSv, "위치없음");
    }

    if (s.sog != kSogInvalid) {
        n += snprintf(line + n, sizeof(line) - n, "%5.2fkn ", s.sog * 0.001f * 1.943844f);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "  ---kn ");
    }
    if (s.cog != kCogInvalid) n += snprintf(line + n, sizeof(line) - n, "%5.1f  ", s.cog * 0.01f);
    else                      n += snprintf(line + n, sizeof(line) - n, "  ---  ");

    if (t.attOk) n += snprintf(line + n, sizeof(line) - n, "힐%+6.1f 피치%+6.1f ",
                               t.heelDeg, t.pitchDeg);
    else         n += snprintf(line + n, sizeof(line) - n, "힐  ---  피치  ---  ");
    if (t.hdgDeg >= 0) n += snprintf(line + n, sizeof(line) - n, "방위%3.0f  ", t.hdgDeg);
    else               n += snprintf(line + n, sizeof(line) - n, "방위---  ");

    if (t.sogPvKn >= 0) n += snprintf(line + n, sizeof(line) - n, "pv%5.2f ", t.sogPvKn);
    else                n += snprintf(line + n, sizeof(line) - n, "pv --- ");
    if (t.sogPosKn >= 0) n += snprintf(line + n, sizeof(line) - n, "pos%5.2f ", t.sogPosKn);
    else                 n += snprintf(line + n, sizeof(line) - n, "pos --- ");
    if (t.pvFlag != 255) n += snprintf(line + n, sizeof(line) - n, "f%u ", t.pvFlag);
    else                 n += snprintf(line + n, sizeof(line) - n, "f- ");
    if (t.cogAccDeg >= 0) n += snprintf(line + n, sizeof(line) - n, "c%4.0f ", t.cogAccDeg);
    else                  n += snprintf(line + n, sizeof(line) - n, "c--- ");
    if (t.sogAccKn >= 0) n += snprintf(line + n, sizeof(line) - n, "a%4.2f ", t.sogAccKn);
    else                 n += snprintf(line + n, sizeof(line) - n, "a--- ");

    n += snprintf(line + n, sizeof(line) - n, "%umV  ", s.battMv);

    n += snprintf(line + n, sizeof(line) - n,
                  "| NAV%lu IMU%lu 버림%lu 멈춤%lums 버퍼%lu%%\n",
                  (unsigned long)gNavRows, (unsigned long)gImuRows,
                  (unsigned long)gDropped, (unsigned long)gMaxStall,
                  (unsigned long)gMaxFill);

    portENTER_CRITICAL(&gTextMux);
    if (n > 0 && gTextUsed + (size_t)n < kTextBufSize) {
        memcpy(gTextBuf + gTextUsed, line, (size_t)n);
        gTextUsed += (size_t)n;
    }
    gTextLastRow = millis();
    if (millis() - gTextLastFlush >= 60000 || gTextUsed > kTextBufSize - 384) {
        gTextFlushWanted = true;
        gTextLastFlush = millis();
    }
    portEXIT_CRITICAL(&gTextMux);
}

// 이 세션에서 위성을 **처음** 잡은 시각. 두 번째부터는 무시한다.
void noteUtcStart(uint32_t epochSec, uint16_t ms) {
    if (!isRec() || gUtcStart || !epochSec) return;
    gUtcStart = epochSec;
    gUtcStartMs = ms;
    printf("[LOG] 첫 fix — UTC %lu.%03u 를 머리글에 적습니다\n",
           (unsigned long)epochSec, ms);
}

// 이벤트 표식 — 다음 항법 줄에 붙는다 (writeNav 가 합친다).
void mark() {
    gPendingEvent |= kEvMark;
    printf("[LOG] 다음 줄에 표식을 붙입니다\n");
}

bool recording() { return isRec(); }
bool busy() { return sdBusy(gState); }
RecState state() { return gState; }
void noteDropped(uint32_t rows) { if (isRec()) gDropped += rows; }

uint32_t sinceTextMs() { return millis() - gTextLastRow; }

uint32_t recStartedMs() { return gStartedMs; }

void getStatus(Status* out) {
    if (!out) return;
    out->recording   = isRec();
    out->state       = (uint8_t)gState;
    out->lostBytes   = gLostBytes;
    out->lastErrorShort = gLastErrorShort;
    out->cardPresent = cardPresent();
    out->session     = gSession;
    strncpy(out->path, gPath, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    out->navRows = gNavRows;
    out->imuRows = gImuRows;
    out->bytes   = gBytes;
    out->startedMs = gStartedMs;
    out->dropped = gDropped;
    out->waited  = gWaited;
    out->maxStallMs = gMaxStall;
    out->maxFillPct = gMaxFill;
    out->freeBytes  = gFreeBytes;
    out->lastError  = gLastError;
}

// ── 되읽어 검사하기 — tools/hlog_parse.py 와 같은 것을 본다 ──────────────
void verify(uint32_t session) {
    if (!cardPresent()) { printf("[검사] 카드가 없습니다.\n"); return; }

    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("[검사] 마운트 실패 — sd 명령으로 이유를 보세요.\n");
        return;
    }

    char path[64];
    if (session == 0) session = nvsU32("sess_n", 0);
    char want[16];
    snprintf(want, sizeof(want), "S%05u_", (unsigned)session);
    findLog(want, ".HLG", path, sizeof(path));
    if (!path[0]) {
        printf("[검사] 세션 %u 파일을 못 찾았습니다.\n", (unsigned)session);
        sdcard::release(sdcard::Owner::Diagnostic);
        return;
    }

    FILE* f = sdOpen(path, "r");
    if (!f) { printf("[검사] %s 를 못 열었습니다.\n", path); sdcard::release(sdcard::Owner::Diagnostic); return; }

    const uint32_t total = (uint32_t)fileSize(f);
    printf("──────────────────────────────────────────\n");
    printf("  %s   %u 바이트\n", path, (unsigned)total);

    uint8_t hdr[kHeaderSize];
    if (fread(hdr, 1, kHeaderSize, f) != kHeaderSize) {
        printf("  헤더를 다 못 읽었습니다.\n"); fclose(f); sdcard::release(sdcard::Owner::Diagnostic); return;
    }
    const uint16_t hwant = (uint16_t)hdr[126] | ((uint16_t)hdr[127] << 8);
    const bool hok = (memcmp(hdr, "HHLG", 4) == 0) && (hwant == crc16(hdr, 126));
    printf("  헤더           %s\n", hok ? "CRC 맞음" : "★ 깨졌습니다");
    {
        uint32_t utc, durS, nr, ir, dr;
        memcpy(&utc,  hdr + 24, 4);
        memcpy(&durS, hdr + kOffDurationS, 4);
        memcpy(&nr,   hdr + kOffNavRows, 4);
        memcpy(&ir,   hdr + kOffImuRows, 4);
        memcpy(&dr,   hdr + kOffDropped, 4);
        printf("  닫힘 표시      %s\n",
               hdr[kOffClosed] == 1 ? "제대로 닫혔음" : "★ 전원이 끊긴 파일");
        printf("  세션 길이      %u분 %u초   NAV %u줄  IMU %u줄  버림 %u\n",
               (unsigned)(durS / 60), (unsigned)(durS % 60),
               (unsigned)nr, (unsigned)ir, (unsigned)dr);
        if (utc) printf("  첫 fix UTC     %lu\n", (unsigned long)utc);
        else     printf("  첫 fix UTC     없음 (위성을 못 잡은 세션)\n");
        uint32_t prev; memcpy(&prev, hdr + kOffPrevSession, 4);
        if (prev) printf("  이어받음       세션 %u 가 끊겨서 이어서 찍은 파일입니다\n",
                         (unsigned)prev);
    }
    printf("  세션 %u  IMU %s  움직임종류 %u  %u/%u Hz\n",
           (unsigned)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24)),
           hdr[kOffImuType] == kImuBNO085 ? "BNO085" : "MPU-9250",
           hdr[kOffGnssDyn], hdr[39], hdr[40]);

    uint32_t nav = 0, imu = 0, bad = 0;
    uint32_t navFirst = 0, navLast = 0, imuFirst = 0, imuLast = 0;
    uint32_t firstFixUtc = 0, lastFixUtc = 0;
    uint32_t fixRows = 0;
    uint32_t prevImuMs = 0;
    uint32_t gap10 = 0, gapOther = 0, gapMax = 0;
    uint8_t  rec[64];
    const size_t imuSize = (hdr[5] >= 1) ? kImuSize : kImuSizeV0;
    const size_t navSize = navSizeFor(hdr[4], hdr[5]);

    for (;;) {                                   // 아두이노 f.available() + f.read() 와 같은 한 바이트씩
        const int t = fgetc(f);
        if (t == EOF) break;
        const size_t size = (t == kTypeNav) ? navSize : ((t == kTypeImu) ? imuSize : 0);
        if (size == 0) { ++bad; continue; }

        rec[0] = (uint8_t)t;
        if (fread(rec + 1, 1, size - 1, f) != size - 1) { ++bad; break; }
        const uint16_t want2 = (uint16_t)rec[size - 2] | ((uint16_t)rec[size - 1] << 8);
        if (want2 != crc16(rec, size - 2)) {
            fseek(f, ftell(f) - (long)(size - 1), SEEK_SET);
            ++bad;
            continue;
        }
        uint32_t ms;
        memcpy(&ms, rec + 1, 4);
        if (t == kTypeNav) {
            if (!nav) navFirst = ms;
            navLast = ms; ++nav;
            uint32_t itow; uint16_t week;
            memcpy(&itow, rec + 5, 4);
            memcpy(&week, rec + 9, 2);
            if (rec[24] != 0 && itow != kItowInvalid && week != kWeekInvalid) {
                const uint32_t u = 315964800UL + (uint32_t)week * 604800UL + itow / 1000UL;
                if (!firstFixUtc) firstFixUtc = u;
                lastFixUtc = u;
                ++fixRows;
            }
        } else {
            if (!imu) imuFirst = ms;
            else {
                const uint32_t g = ms - prevImuMs;
                if (g == 10) ++gap10; else ++gapOther;
                if (g > gapMax) gapMax = g;
            }
            prevImuMs = ms; imuLast = ms; ++imu;
        }
        if (((nav + imu) & 0x3FF) == 0) kickWdt();
    }
    fclose(f);
    const uint64_t freeB = cardFreeBytes();
    sdcard::release(sdcard::Owner::Diagnostic);

    const uint32_t used = kHeaderSize + nav * navSize + imu * imuSize;
    printf("  ─────────────────────────────────────\n");
    printf("  NAV 레코드     %u\n", (unsigned)nav);
    printf("  IMU 레코드     %u\n", (unsigned)imu);
    printf("  못 읽은 바이트 %d  %s\n", (int)((int32_t)total - (int32_t)used),
           (total == used && bad == 0) ? "(깨끗함)" : "★");
    printf("  CRC 틀린 자리  %u\n", (unsigned)bad);

    if (nav > 1 && navLast > navFirst) {
        printf("  NAV 실제 주기  %.2f Hz  (규격 %u)\n",
               (nav - 1) * 1000.0f / (navLast - navFirst), hdr[39]);
    }
    if (imu > 1 && imuLast > imuFirst) {
        printf("  IMU 실제 주기  %.2f Hz  (규격 %u)\n",
               (imu - 1) * 1000.0f / (imuLast - imuFirst), hdr[40]);
        const uint32_t gaps = gap10 + gapOther;
        printf("  IMU 등간격     10 ms 가 %u/%u = %.2f%%  (제일 벌어진 것 %u ms)\n",
               (unsigned)gap10, (unsigned)gaps,
               gaps ? 100.0f * gap10 / gaps : 0.0f, (unsigned)gapMax);
    }
    if (fixRows) {
        const int32_t tzMin = nvsI32("tz_min", 540);
        struct tm a, b;
        const time_t la = (time_t)((int64_t)firstFixUtc + (int64_t)tzMin * 60);
        const time_t lb = (time_t)((int64_t)lastFixUtc  + (int64_t)tzMin * 60);
        gmtime_r(&la, &a); gmtime_r(&lb, &b);
        printf("  줄에서 찾은 시각 %04d-%02d-%02d %02d:%02d:%02d ~ %02d:%02d:%02d (그 고장 시각)\n",
               a.tm_year + 1900, a.tm_mon + 1, a.tm_mday,
               a.tm_hour, a.tm_min, a.tm_sec, b.tm_hour, b.tm_min, b.tm_sec);
        printf("  위성 잡은 줄   %u / %u\n", (unsigned)fixRows, (unsigned)nav);
        uint32_t hu; memcpy(&hu, hdr + 24, 4);
        if (!hu) {
            printf("  ※ 머리글은 비었는데 줄에는 시각이 있습니다.\n");
            printf("     끊긴 파일이라 이름이 _nosat 으로 굳은 것뿐입니다. 값은 멀쩡합니다.\n");
        }
    }
    printf("  카드 남은 자리 %llu MB\n", (unsigned long long)(freeB / 1048576ULL));
    printf("──────────────────────────────────────────\n");
    printf("%s\n", ((total == used) && bad == 0 && hok) ? "  ✅ 깨끗합니다" : "  ❌ 문제가 있습니다");
    fflush(stdout);
}

namespace {
// 아두이노 Stream::readStringUntil('\n') — 줄바꿈은 빼고 돌려준다. 끝이면 false.
bool readLine(FILE* f, char* out, size_t cap) {
    size_t n = 0;
    int c;
    bool any = false;
    while ((c = fgetc(f)) != EOF) {
        any = true;
        if (c == '\n') break;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return any;
}
} // namespace

// 텍스트 사본의 앞이나 뒤 몇 줄을 뱉는다. `rec tail` / `rec head` 가 부른다.
void tail(uint32_t session, uint16_t lines, bool head) {
    if (!cardPresent()) { printf("[꼬리] 카드가 없습니다.\n"); return; }
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("[꼬리] 마운트 실패.\n"); return;
    }

    if (session == 0) session = nvsU32("sess_n", 0);
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    char path[80];
    findLog(want, ".TXT", path, sizeof(path));
    if (!path[0]) {
        printf("[꼬리] 세션 %u 의 TXT 를 못 찾았습니다.\n", (unsigned)session);
        sdcard::release(sdcard::Owner::Diagnostic); return;
    }

    FILE* f = sdOpen(path, "r");
    if (!f) { printf("[꼬리] %s 를 못 열었습니다.\n", path); sdcard::release(sdcard::Owner::Diagnostic); return; }

    const uint32_t total = (uint32_t)fileSize(f);
    printf("──────────────────────────────────────────\n");
    printf("  %s   %u 바이트\n", path, (unsigned)total);

    static char ln[512];                         // ★ 스택에 안 올린다
    uint32_t from = 0;
    if (!head) {
        const uint32_t back = (uint32_t)lines * 160;
        from = (total > back) ? (total - back) : 0;
        fseek(f, (long)from, SEEK_SET);
        if (from) readLine(f, ln, sizeof(ln));   // 잘린 첫 줄은 버린다
        printf("  ── 마지막 %u줄 ──\n", (unsigned)lines);
    } else {
        printf("  ── 처음 %u줄 ──\n", (unsigned)lines);
    }

    uint16_t shown = 0;
    while (shown < (head ? lines : (uint16_t)(lines * 2)) && readLine(f, ln, sizeof(ln))) {
        if (!ln[0]) continue;
        printf("  %s\n", ln);
        ++shown;
        if ((shown & 0x0F) == 0) kickWdt();
    }
    fclose(f);
    sdcard::release(sdcard::Owner::Diagnostic);
    printf("──────────────────────────────────────────\n");
    fflush(stdout);
}

// 파일 한 조각을 base64 로 뱉는다. `rec dump` 가 부른다 (tools/serial_dump.py).
void dump(uint32_t session, bool hlg, uint32_t offset, uint32_t len) {
    if (!cardPresent()) { printf("@DUMP X 카드 없음\n"); return; }
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("@DUMP X 마운트 실패\n"); return;
    }
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    const char* ext = hlg ? ".HLG" : ".TXT";
    char path[80];
    findLog(want, ext, path, sizeof(path));
    if (!path[0]) { printf("@DUMP X 파일 없음\n"); sdcard::release(sdcard::Owner::Diagnostic); return; }
    FILE* f = sdOpen(path, "r");
    if (!f) { printf("@DUMP X 열기 실패\n"); sdcard::release(sdcard::Owner::Diagnostic); return; }

    const uint32_t total = (uint32_t)fileSize(f);
    printf("@DUMP S %s %u\n", path, (unsigned)total);
    if (offset > total) offset = total;
    if (len > 262144) len = 262144;
    if (len > total - offset) len = total - offset;
    fseek(f, (long)offset, SEEK_SET);

    static uint8_t raw[3000];                 // 3 의 배수라야 줄마다 base64 가 끊기지 않는다
    static unsigned char b64[4004];
    uint32_t crc = 0xFFFFFFFFu, sent = 0;
    while (sent < len) {
        const size_t ask = (len - sent) < sizeof(raw) ? (len - sent) : sizeof(raw);
        const size_t got = fread(raw, 1, ask, f);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            crc ^= raw[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        size_t olen = 0;
        mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, got);
        fputs("@DUMP B ", stdout);
        fwrite(b64, 1, olen, stdout);
        fputc('\n', stdout);
        sent += (uint32_t)got;
        kickWdt();
    }
    fclose(f);
    sdcard::release(sdcard::Owner::Diagnostic);
    printf("@DUMP E %u %u %08x\n", (unsigned)offset, (unsigned)sent, (unsigned)(crc ^ 0xFFFFFFFFu));
    fflush(stdout);
}

// 파일 하나를 끝까지 읽어 SHA-256 을 낸다. `rec hash` 가 부른다.
void hashFile(uint32_t session, bool hlg) {
    if (!cardPresent()) { printf("@HASH X 카드 없음\n"); return; }
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) { printf("@HASH X 마운트 실패\n"); return; }
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    const char* ext = hlg ? ".HLG" : ".TXT";
    char path[80];
    findLog(want, ext, path, sizeof(path));
    if (!path[0]) { printf("@HASH X 파일 없음\n"); sdcard::release(sdcard::Owner::Diagnostic); return; }
    FILE* f = sdOpen(path, "r");
    if (!f) { printf("@HASH X 열기 실패\n"); sdcard::release(sdcard::Owner::Diagnostic); return; }

    // IDF 6.1: PSA 해시. psa_crypto_init 은 켤 때 IDF 가 부른다 (mbedtls/port/esp_psa_crypto_init.c)
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool psaOk = (psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS);
    static uint8_t buf[4096];
    uint32_t total = 0, lastKick = millis();
    for (;;) {
        const size_t got = fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
        if (psaOk && psa_hash_update(&op, buf, got) != PSA_SUCCESS) psaOk = false;
        total += (uint32_t)got;
        if (millis() - lastKick > 500) { kickWdt(); lastKick = millis(); }
    }
    uint8_t out[32];
    size_t outLen = 0;
    if (psaOk && psa_hash_finish(&op, out, sizeof(out), &outLen) != PSA_SUCCESS) psaOk = false;
    if (!psaOk) psa_hash_abort(&op);
    const uint32_t size = (uint32_t)fileSize(f);
    fclose(f);
    sdcard::release(sdcard::Owner::Diagnostic);
    if (!psaOk || outLen != 32) { printf("@HASH X 해시 계산 실패\n"); return; }
    if (total != size) { printf("@HASH X 읽은 %u 바이트가 크기 %u 와 다름\n", (unsigned)total, (unsigned)size); return; }
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", out[i]);
    printf("@HASH %s %u %s\n", path, (unsigned)total, hex);
    fflush(stdout);
}

// 세션 하나를 지운다. HLG 와 TXT 둘 다. `rec rm <번호>` 가 부른다. 못 되돌린다.
bool removeSession(uint32_t session) {
    if (!session)   { printf("[지움] 번호를 적으세요.\n"); return false; }
    if (!cardPresent()) { printf("[지움] 카드가 없습니다.\n"); return false; }
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("[지움] 마운트 실패.\n"); return false;
    }
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    // 한 번에 다 모아 놓고 지운다 — 훑는 도중에 지우면 다음 항목을 건너뛴다.
    char hit[4][80];
    int n = 0;
    {
        DIR* dir = opendir("/sd/LOGS");
        if (dir) {
            struct dirent* e;
            while ((e = readdir(dir)) != nullptr) {
                const char* nm = e->d_name;
                if (n < 4 && startsWith(nm, want) && (endsWith(nm, ".HLG") || endsWith(nm, ".TXT"))) {
                    logsName(hit[n], sizeof(hit[n]), nm);
                    ++n;
                }
            }
            closedir(dir);
        }
    }
    if (!n) {
        printf("[지움] 세션 %u 파일이 없습니다.\n", (unsigned)session);
        sdcard::release(sdcard::Owner::Diagnostic); return false;
    }
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        char p[96];
        sdPath(p, sizeof(p), hit[i]);
        if (remove(p) == 0) { printf("  지웠습니다  %s\n", hit[i]); ++ok; }
        else                { printf("  못 지웠습니다 %s\n", hit[i]); }
    }
    sdcard::release(sdcard::Owner::Diagnostic);
    return ok == n;
}

// 카드에 있는 세션 목록. `rec ls` 가 부른다.
void listFiles() {
    if (!cardPresent()) { printf("[목록] 카드가 없습니다.\n"); return; }
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("[목록] 마운트 실패.\n"); return;
    }
    DIR* dir = opendir("/sd/LOGS");
    printf("──────────────────────────────────────────\n");
    uint32_t n = 0;
    uint64_t sum = 0;
    if (dir) {
        struct dirent* e;
        static char p[300];
        while ((e = readdir(dir)) != nullptr) {
            snprintf(p, sizeof(p), "/sd/LOGS/%s", e->d_name);
            struct stat st;
            const uint32_t sz = (stat(p, &st) == 0) ? (uint32_t)st.st_size : 0;
            printf("  %-16s %8u 바이트\n", e->d_name, (unsigned)sz);
            sum += sz;
            ++n;
            if ((n & 0x1F) == 0) kickWdt();
        }
        closedir(dir);
    }
    printf("  파일 %u개, 합쳐서 %.2f MB\n", (unsigned)n, sum / 1048576.0);
    printf("  카드 남은 자리 %llu MB\n", (unsigned long long)(cardFreeBytes() / 1048576ULL));
    sdcard::release(sdcard::Owner::Diagnostic);
    printf("──────────────────────────────────────────\n");
    fflush(stdout);
}

// 1 Hz. 기록 중에 카드가 빠졌나 본다.
void healthCheck() {
    if (!isRec()) return;
    if (!cardPresent()) {
        const uint32_t recSec = (millis() - gStartedMs) / 1000;
        portENTER_CRITICAL(&gResMux);
        if (recctl::setFirst(gRes, recctl::kErrCardGone)) {
            gRes.recSec = recSec; gRes.card = false; gRes.bytes = (uint32_t)gBytes;
        }
        portEXIT_CRITICAL(&gResMux);
        gLastError = "기록 중 카드가 빠졌습니다";
        gLastErrorShort = "카드 빠짐";
        printf("[LOG] ★ 기록 중에 카드가 빠졌습니다 — 닫습니다\n");
        requestStop();
    }
}

void noteLastFail(const char* line) { gLastFailLine = line; }
void setSessionNote(const char* text) { gSessionNote = text; }
void noteEvent(const char* text) {
    if (!isRec() || !text) return;
    const uint32_t sec = (millis() - gStartedMs) / 1000;
    char line[256];
    int n = snprintf(line, sizeof(line), "# %02u:%02u:%02u 설정 바뀜 — %s\n",
                     (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60), text);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    portENTER_CRITICAL(&gTextMux);
    if (gTextUsed + (size_t)n < kTextBufSize) {
        memcpy(gTextBuf + gTextUsed, line, (size_t)n);
        gTextUsed += (size_t)n;
    }
    portEXIT_CRITICAL(&gTextMux);
}
void testFailWrites(uint8_t n) { gTestFailN = n; }
void testFailFlush(uint8_t n) { gTestFlushFailN = n; }
uint32_t writeRetries() { return gWriteRetries; }

} // namespace hlog
