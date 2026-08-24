#include "hlog.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <string.h>

#include <esp_task_wdt.h>

#include "board_rak.h"

namespace hlog {
namespace {

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

char*  gBuf = nullptr;
volatile size_t gHead = 0;
volatile size_t gTail = 0;

size_t bufUsed() {
    const size_t h = gHead, t = gTail;
    return (h >= t) ? (h - t) : (kBufSize - t + h);
}
size_t bufFree() { return kBufSize - bufUsed() - 1; }

// ── 텍스트 사본 ──────────────────────────────────────────────────────────
//
// 10초에 한 줄. 카드를 꽂자마자 파서 없이 눈으로 확인하는 용이다.
// 초당 15 바이트라 전체의 0.5% 밖에 안 된다.
//
// ★ 램에 모았다가 1분에 한 번 쏟는다. 10초마다 카드를 건드리면 바이너리
//   파일 자리가 조각나고, 조각이 나면 멈춤이 더 자주 온다.
//   전원이 끊기면 텍스트 1분치를 잃지만 원본은 바이너리에 다 있다.
constexpr size_t kTextBufSize = 4096;
char   gTextBuf[kTextBufSize];
size_t gTextUsed = 0;
uint32_t gTextLastFlush = 0;
uint32_t gTextLastRow   = 0;

// ── 상태 ─────────────────────────────────────────────────────────────────

volatile bool gRecording  = false;
volatile bool gStopWanted = false;

File     gBin, gTxt;
uint32_t gSession = 0;
char     gPath[32]    = {0};
char     gTxtPath[32] = {0};
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

bool cardPresent() {
    pinMode(rak::kSdCardDetect, INPUT_PULLUP);
    return digitalRead(rak::kSdCardDetect) == LOW;
}

// ── 버퍼에 밀어넣기 ──────────────────────────────────────────────────────
//
// 자리가 없으면 **기다린다. 버리지 않는다.** 기록이 이 장비의 본체다.
// 화면이 잠깐 덜컹거리는 편이 데이터를 잃는 것보다 낫다.
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

void writerTask(void*) {
    uint32_t lastWrite = millis(), lastFlush = millis();
    for (;;) {
        if (!gRecording) {
            if (gStopWanted) {
                while (bufUsed() > 0 && gBin) {
                    size_t t = gTail, n = bufUsed();
                    if (t + n > kBufSize) n = kBufSize - t;
                    gBin.write((const uint8_t*)(gBuf + t), n);
                    gTail = (t + n) % kBufSize;
                }
                if (gBin) { gBin.flush(); gBin.close(); }
                if (gTxt) { gTxt.flush(); gTxt.close(); }
                gFreeBytes = SD.totalBytes() - SD.usedBytes();
                SD.end();
                gStopWanted = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const size_t used = bufUsed();
        const bool due = (used >= kChunk) || (used > 0 && millis() - lastWrite >= 500);
        if (!due) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        size_t t = gTail;
        size_t n = used > kChunk ? kChunk : used;
        if (t + n > kBufSize) n = kBufSize - t;

        const uint32_t a = millis();
        const size_t   w = gBin.write((const uint8_t*)(gBuf + t), n);
        const uint32_t dt = millis() - a;
        if (dt > gMaxStall) gMaxStall = dt;
        lastWrite = millis();

        if (w != n) {
            gLastError = "카드 쓰기 실패";
            gRecording = false; gStopWanted = true;
            continue;
        }
        gTail = (t + n) % kBufSize;
        gBytes += w;

        if (millis() - lastFlush >= kFlushMs) {
            const uint32_t b = millis();
            gBin.flush();
            const uint32_t df = millis() - b;
            if (df > gMaxStall) gMaxStall = df;
            lastFlush = millis();
        }
    }
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

void begin() {
    if (gBuf) return;
    gBuf = (char*)ps_malloc(kBufSize);          // PSRAM 먼저. 내부 RAM 은 BLE 가 쓴다
    if (!gBuf) gBuf = (char*)malloc(kBufSize);
    if (!gBuf) {
        Serial.println("[LOG] 버퍼를 못 잡았습니다 — 기록 기능이 꺼집니다");
        return;
    }
    xTaskCreatePinnedToCore(writerTask, "hlog", 4096, nullptr, 1, &gWriter, 0);
    Serial.printf("[LOG] 쓰기 작업 코어 0 (버퍼 %u KB = 초당 3.1KB 기준 %.0f초치)\n",
                  (unsigned)(kBufSize / 1024), kBufSize / 3080.0f);
}

bool start(const Header& h) {
    if (!gBuf)      { gLastError = "버퍼 없음"; return false; }
    if (gRecording) { gLastError = "이미 기록 중"; return false; }
    if (!cardPresent()) { gLastError = "카드가 안 꽂혀 있습니다"; return false; }

    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, 4000000, "/sd", 5)) {
        gLastError = "마운트 실패 — sd 명령으로 이유를 보세요";
        return false;
    }
    SD.mkdir("/LOGS");

    // 파일 이름은 세션 번호로만 짓는다.
    //
    // 규격 원문은 /LOGS/YYYYMMDD/SNNN.HLG 인데, 전원을 켠 직후에는 GPS 가
    // 아직 날짜를 모른다. 선수가 해변에서 저장 버튼을 누르면 그 순간 날짜가
    // 없다. 그래서 이름은 세션 번호로 하고 시각은 헤더에 넣는다.
    // 정렬은 헤더의 utc_start 를 보고 하면 된다.
    Preferences prefs;
    prefs.begin("sail", false);
    gSession = prefs.getUInt("sess_n", 0) + 1;
    for (int guard = 0; guard < 20000; ++guard) {
        snprintf(gPath, sizeof(gPath), "/LOGS/S%05u.HLG", (unsigned)gSession);
        if (!SD.exists(gPath)) break;
        ++gSession;                                   // 절대 덮어쓰지 않는다
    }
    snprintf(gTxtPath, sizeof(gTxtPath), "/LOGS/S%05u.TXT", (unsigned)gSession);
    prefs.putUInt("sess_n", gSession);
    const uint16_t bootCount = (uint16_t)(prefs.getUInt("boot_n", 0));
    prefs.end();

    gBin = SD.open(gPath, FILE_WRITE);
    if (!gBin) { gLastError = "파일을 못 열었습니다"; SD.end(); return false; }
    gTxt = SD.open(gTxtPath, FILE_WRITE);

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
    // reserved 에 우리가 채우는 것 (hlog.h 의 표 참고)
    hdr[kOffImuType]  = h.imuType;
    hdr[kOffTimeRef]  = h.timeRef;
    hdr[kOffMagScale] = h.magScale;
    hdr[kOffGnssDyn]  = h.gnssDyn;
    hdr[kOffGnssHz]   = h.gnssHz;
    hdr[kOffSogSrc]   = h.sogSrc;
    hdr[kOffQuatSrc]  = h.quatSrc;
    // 힐·피치를 어느 축에서 봤나. 없으면 나중에 이 파일로 힐을 못 구한다.
    hdr[kOffHeelAxis]  = h.heelAxis;
    hdr[kOffHeelSign]  = h.heelSign;
    hdr[kOffPitchAxis] = h.pitchAxis;
    hdr[kOffPitchSign] = h.pitchSign;
    memcpy(hdr + kOffHeelOff,  &h.heelOff,  4);
    memcpy(hdr + kOffPitchOff, &h.pitchOff, 4);
    const uint16_t hcrc = crc16(hdr, 126);
    hdr[126] = (uint8_t)hcrc; hdr[127] = (uint8_t)(hcrc >> 8);
    gBin.write(hdr, sizeof(hdr));
    gBin.flush();

    if (gTxt) {
        gTxt.printf("# sail 경기정 모듈 — 세션 %u\n", (unsigned)gSession);
        gTxt.printf("# 바이너리 원본: %s\n", gPath);
        gTxt.printf("# 이 파일은 10초에 한 줄짜리 사본입니다. 눈으로 보는 용입니다.\n");
        gTxt.printf("# 힐·피치·헤딩은 참고값입니다 (가속도에서 뽑음).\n");
        gTxt.printf("# GNSS 동역학모델 %u / %u Hz,  IMU %s,  속도는 도플러 원본\n",
                    h.gnssDyn, h.gnssHz,
                    h.imuType == kImuBNO085 ? "BNO085" : "MPU-9250");
        gTxt.printf("#\n");
        gTxt.flush();
    }

    gNavRows = gImuRows = 0;
    gBytes = kHeaderSize;
    gDropped = gWaited = gMaxStall = gMaxFill = 0;
    gLastError = nullptr;
    gTextUsed = 0;
    gTextLastFlush = gTextLastRow = millis();
    gFirstNav = true;
    gUtcStart = 0; gUtcStartMs = 0;
    gHead = gTail = 0;
    gStartedMs = millis();
    gRecording = true;

    Serial.printf("[LOG] 기록 시작 — %s  (+ %s)\n", gPath, gTxtPath);
    return true;
}

void stop() {
    if (!gRecording && !gBin) return;

    // 텍스트에 마지막 요약을 남기고 램에 남은 것도 쏟는다
    if (gTxt && gTextUsed) { gTxt.write((const uint8_t*)gTextBuf, gTextUsed); gTextUsed = 0; }
    if (gTxt) {
        const uint32_t sec = (millis() - gStartedMs) / 1000;
        gTxt.printf("#\n# 끝 — %u분 %u초,  NAV %u줄  IMU %u줄\n",
                    sec / 60, sec % 60, (unsigned)gNavRows, (unsigned)gImuRows);
        gTxt.printf("# 버린 줄 %u  기다린 횟수 %u  최대 멈춤 %ums  버퍼 최고 %u%%\n",
                    (unsigned)gDropped, (unsigned)gWaited,
                    (unsigned)gMaxStall, (unsigned)gMaxFill);
        if (gDropped) gTxt.printf("# ★ 버린 줄이 있습니다. 이 세션은 구멍이 있습니다.\n");
    }

    const uint32_t durS = (millis() - gStartedMs) / 1000;

    gRecording  = false;
    gStopWanted = true;
    const uint32_t t0 = millis();
    while (gStopWanted && millis() - t0 < 15000) delay(10);

    // ── 머리글을 다시 쓴다 ──────────────────────────────────────────────
    //
    // 기록을 시작할 때는 첫 fix 의 UTC 도, 세션 길이도 모른다. 전원을 켠
    // 직후에는 GPS 가 시각조차 모른다. 그런데 나중에 목록만 보고 "이게 오늘
    // 오전 훈련인가 5분짜리 시험인가" 를 정하려면 그 값이 있어야 한다
    // (TRANSFER.md §1).
    //
    // 그래서 세션을 닫으면서 파일 맨 앞 128바이트를 새로 쓴다. 그때는 다
    // 알고 있다. CRC 도 다시 계산한다.
    //
    // 전원이 그냥 끊겨서 여기까지 못 오면 closed 가 0 으로 남는다. 데스크탑
    // 앱은 그런 파일도 받을 수 있어야 한다 — 안에는 값이 다 들어 있다.
    if (SD.begin(rak::kSPI_CS, SPI, 4000000, "/sd", 5)) {
        File h = SD.open(gPath, "r+");
        if (h) {
            uint8_t hdr[kHeaderSize];
            if (h.read(hdr, kHeaderSize) == (int)kHeaderSize &&
                memcmp(hdr, "HHLG", 4) == 0) {
                memcpy(hdr + 24, &gUtcStart, 4);
                memcpy(hdr + 28, &gUtcStartMs, 2);
                memcpy(hdr + kOffDurationS, &durS, 4);
                memcpy(hdr + kOffNavRows, (const void*)&gNavRows, 4);
                memcpy(hdr + kOffImuRows, (const void*)&gImuRows, 4);
                memcpy(hdr + kOffDropped, (const void*)&gDropped, 4);
                hdr[kOffClosed] = 1;
                const uint16_t c = crc16(hdr, 126);
                hdr[126] = (uint8_t)c; hdr[127] = (uint8_t)(c >> 8);
                h.seek(0);
                h.write(hdr, kHeaderSize);
                h.flush();
            }
            h.close();
        }
        SD.end();
    }

    Serial.printf("[LOG] 기록 끝 — %s  NAV %u줄 / IMU %u줄\n",
                  gPath, (unsigned)gNavRows, (unsigned)gImuRows);
    if (gDropped) {
        Serial.printf("[LOG] ★ 버린 줄 %u개 — 카드가 못 따라왔습니다\n", (unsigned)gDropped);
    }
}

void writeNav(const NavSample& s) {
    if (!gRecording) return;
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
    const uint16_t c = crc16(r, 36);
    put16(r, o, c);

    if (push(r, kNavSize)) ++gNavRows;
}

void writeImu(const ImuSample& s) {
    if (!gRecording) return;
    uint8_t r[kImuSize];
    size_t o = 0;
    put8 (r, o, kTypeImu);
    put32(r, o, s.localMs);
    for (int i = 0; i < 3; ++i) put16(r, o, (uint16_t)s.acc[i]);
    for (int i = 0; i < 3; ++i) put16(r, o, (uint16_t)s.gyr[i]);
    for (int i = 0; i < 4; ++i) put16(r, o, (uint16_t)s.quat[i]);
    const uint16_t c = crc16(r, 25);
    put16(r, o, c);

    if (push(r, kImuSize)) ++gImuRows;
}

void writeText(const NavSample& s, const TextSample& t) {
    if (!gRecording || !gTxt) return;

    char line[320];
    int n = 0;
    const uint32_t sec = (millis() - gStartedMs) / 1000;
    n += snprintf(line + n, sizeof(line) - n, "%02u:%02u:%02u  ",
                  sec / 3600, (sec / 60) % 60, sec % 60);

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

    n += snprintf(line + n, sizeof(line) - n, "%umV  ", s.battMv);

    // 기록기 자신의 상태. 카드를 꽂자마자 "이 세션 멀쩡한가" 를 보는 자리다.
    n += snprintf(line + n, sizeof(line) - n,
                  "| NAV%lu IMU%lu 버림%lu 멈춤%lums 버퍼%lu%%\n",
                  (unsigned long)gNavRows, (unsigned long)gImuRows,
                  (unsigned long)gDropped, (unsigned long)gMaxStall,
                  (unsigned long)gMaxFill);

    if (n > 0 && gTextUsed + (size_t)n < kTextBufSize) {
        memcpy(gTextBuf + gTextUsed, line, (size_t)n);
        gTextUsed += (size_t)n;
    }
    gTextLastRow = millis();

    // 1분에 한 번만 카드를 건드린다 (조각남 방지)
    if (millis() - gTextLastFlush >= 60000 || gTextUsed > kTextBufSize - 384) {
        gTxt.write((const uint8_t*)gTextBuf, gTextUsed);
        gTxt.flush();
        gTextUsed = 0;
        gTextLastFlush = millis();
    }
}

void noteUtcStart(uint32_t epochSec, uint16_t ms) {
    if (!gRecording || gUtcStart || !epochSec) return;
    gUtcStart = epochSec;
    gUtcStartMs = ms;
    Serial.printf("[LOG] 첫 fix — UTC %lu.%03u 를 머리글에 적습니다\n",
                  (unsigned long)epochSec, ms);
}

void mark() {
    gPendingEvent |= kEvMark;
    Serial.println("[LOG] 다음 줄에 표식을 붙입니다");
}

bool recording() { return gRecording; }

uint32_t sinceTextMs() { return millis() - gTextLastRow; }

uint32_t recStartedMs() { return gStartedMs; }

void getStatus(Status* out) {
    if (!out) return;
    out->recording   = gRecording;
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

// ── 되읽어 검사하기 ──────────────────────────────────────────────────────
//
// 카드를 뽑아 컴퓨터에 꽂을 수 없을 때가 있다. 그러면 보드가 스스로 읽어서
// 검사한다. tools/hlog_parse.py 와 같은 것을 본다.
//
//   헤더 CRC / 레코드 CRC / 못 읽은 바이트 / 실제 주기 / IMU 이웃 간격
//
// 제일 중요한 건 마지막 것이다. IMU 가 정말 10 ms 등간격인지.
void verify(uint32_t session) {
    if (gRecording) { Serial.println("[검사] 기록 중에는 못 합니다. rec off 먼저."); return; }
    if (!cardPresent()) { Serial.println("[검사] 카드가 없습니다."); return; }

    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, 4000000, "/sd", 5)) {
        Serial.println("[검사] 마운트 실패 — sd 명령으로 이유를 보세요.");
        return;
    }

    char path[32];
    if (session == 0) {
        Preferences prefs;
        prefs.begin("sail", true);
        session = prefs.getUInt("sess_n", 0);
        prefs.end();
    }
    snprintf(path, sizeof(path), "/LOGS/S%05u.HLG", (unsigned)session);

    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("[검사] %s 를 못 열었습니다.\n", path); SD.end(); return; }

    const uint32_t total = f.size();
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  %s   %u 바이트\n", path, (unsigned)total);

    uint8_t hdr[kHeaderSize];
    if (f.read(hdr, kHeaderSize) != (int)kHeaderSize) {
        Serial.println("  헤더를 다 못 읽었습니다."); f.close(); SD.end(); return;
    }
    const uint16_t hwant = (uint16_t)hdr[126] | ((uint16_t)hdr[127] << 8);
    const bool hok = (memcmp(hdr, "HHLG", 4) == 0) && (hwant == crc16(hdr, 126));
    Serial.printf("  헤더           %s\n", hok ? "CRC 맞음" : "★ 깨졌습니다");
    // 세션을 닫으면서 다시 쓴 값들. 목록만 보고 뭘 받을지 정하는 데 쓴다.
    {
        uint32_t utc, durS, nr, ir, dr;
        memcpy(&utc,  hdr + 24, 4);
        memcpy(&durS, hdr + kOffDurationS, 4);
        memcpy(&nr,   hdr + kOffNavRows, 4);
        memcpy(&ir,   hdr + kOffImuRows, 4);
        memcpy(&dr,   hdr + kOffDropped, 4);
        Serial.printf("  닫힘 표시      %s\n",
                      hdr[kOffClosed] == 1 ? "제대로 닫혔음" : "★ 전원이 끊긴 파일");
        Serial.printf("  세션 길이      %u분 %u초   NAV %u줄  IMU %u줄  버림 %u\n",
                      (unsigned)(durS / 60), (unsigned)(durS % 60),
                      (unsigned)nr, (unsigned)ir, (unsigned)dr);
        if (utc) Serial.printf("  첫 fix UTC     %lu\n", (unsigned long)utc);
        else     Serial.println("  첫 fix UTC     없음 (위성을 못 잡은 세션)");
    }
    Serial.printf("  세션 %u  IMU %s  움직임종류 %u  %u/%u Hz\n",
                  (unsigned)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24)),
                  hdr[kOffImuType] == kImuBNO085 ? "BNO085" : "MPU-9250",
                  hdr[kOffGnssDyn], hdr[39], hdr[40]);

    // 레코드를 훑는다. 한 번에 조금씩 읽어서 램을 아낀다.
    uint32_t nav = 0, imu = 0, bad = 0;
    uint32_t navFirst = 0, navLast = 0, imuFirst = 0, imuLast = 0;
    uint32_t prevImuMs = 0;
    uint32_t gap10 = 0, gapOther = 0, gapMax = 0;
    uint8_t  rec[64];

    while (f.available()) {
        const int t = f.read();
        if (t < 0) break;
        const size_t size = (t == kTypeNav) ? kNavSize : ((t == kTypeImu) ? kImuSize : 0);
        if (size == 0) { ++bad; continue; }   // 한 바이트 밀면서 다시 맞춘다

        rec[0] = (uint8_t)t;
        if (f.read(rec + 1, size - 1) != (int)(size - 1)) { ++bad; break; }
        const uint16_t want = (uint16_t)rec[size - 2] | ((uint16_t)rec[size - 1] << 8);
        if (want != crc16(rec, size - 2)) {
            // CRC 가 틀리면 가짜다. 한 바이트만 밀고 다시 본다.
            f.seek(f.position() - (size - 1));
            ++bad;
            continue;
        }
        uint32_t ms;
        memcpy(&ms, rec + 1, 4);
        if (t == kTypeNav) {
            if (!nav) navFirst = ms;
            navLast = ms; ++nav;
        } else {
            if (!imu) imuFirst = ms;
            else {
                const uint32_t g = ms - prevImuMs;
                if (g == 10) ++gap10; else ++gapOther;
                if (g > gapMax) gapMax = g;
            }
            prevImuMs = ms; imuLast = ms; ++imu;
        }
        if (((nav + imu) & 0x3FF) == 0) esp_task_wdt_reset();
    }
    f.close();
    const uint64_t freeB = SD.totalBytes() - SD.usedBytes();
    SD.end();

    const uint32_t used = kHeaderSize + nav * kNavSize + imu * kImuSize;
    Serial.println("  ─────────────────────────────────────");
    Serial.printf("  NAV 레코드     %u\n", (unsigned)nav);
    Serial.printf("  IMU 레코드     %u\n", (unsigned)imu);
    Serial.printf("  못 읽은 바이트 %d  %s\n", (int)((int32_t)total - (int32_t)used),
                  (total == used && bad == 0) ? "(깨끗함)" : "★");
    Serial.printf("  CRC 틀린 자리  %u\n", (unsigned)bad);

    if (nav > 1 && navLast > navFirst) {
        Serial.printf("  NAV 실제 주기  %.2f Hz  (규격 %u)\n",
                      (nav - 1) * 1000.0f / (navLast - navFirst), hdr[39]);
    }
    if (imu > 1 && imuLast > imuFirst) {
        Serial.printf("  IMU 실제 주기  %.2f Hz  (규격 %u)\n",
                      (imu - 1) * 1000.0f / (imuLast - imuFirst), hdr[40]);
        const uint32_t gaps = gap10 + gapOther;
        Serial.printf("  IMU 등간격     10 ms 가 %u/%u = %.2f%%  (제일 벌어진 것 %u ms)\n",
                      (unsigned)gap10, (unsigned)gaps,
                      gaps ? 100.0f * gap10 / gaps : 0.0f, (unsigned)gapMax);
    }
    Serial.printf("  카드 남은 자리 %llu MB\n", (unsigned long long)(freeB / 1048576ULL));
    Serial.println("──────────────────────────────────────────");
    Serial.println(((total == used) && bad == 0 && hok) ? "  ✅ 깨끗합니다" : "  ❌ 문제가 있습니다");
}

void listFiles() {
    if (gRecording) { Serial.println("[목록] 기록 중에는 못 합니다."); return; }
    if (!cardPresent()) { Serial.println("[목록] 카드가 없습니다."); return; }
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, 4000000, "/sd", 5)) {
        Serial.println("[목록] 마운트 실패."); return;
    }
    File dir = SD.open("/LOGS");
    Serial.println("──────────────────────────────────────────");
    uint32_t n = 0;
    uint64_t sum = 0;
    while (File e = dir.openNextFile()) {
        Serial.printf("  %-16s %8u 바이트\n", e.name(), (unsigned)e.size());
        sum += e.size();
        ++n;
        e.close();
        if ((n & 0x1F) == 0) esp_task_wdt_reset();
    }
    dir.close();
    Serial.printf("  파일 %u개, 합쳐서 %.2f MB\n", (unsigned)n, sum / 1048576.0);
    Serial.printf("  카드 남은 자리 %llu MB\n",
                  (unsigned long long)((SD.totalBytes() - SD.usedBytes()) / 1048576ULL));
    SD.end();
    Serial.println("──────────────────────────────────────────");
}

void healthCheck() {
    if (!gRecording) return;
    if (!cardPresent()) {
        Serial.println("[LOG] ★ 기록 중에 카드가 빠졌습니다 — 멈춥니다");
        gLastError = "기록 중 카드가 빠졌습니다";
        stop();
    }
}

} // namespace hlog
