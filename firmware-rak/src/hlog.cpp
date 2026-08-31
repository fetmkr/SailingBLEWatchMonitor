#include "hlog.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <time.h>
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
// 쓰기 실패로 저절로 멈췄다. 메인 루프(healthCheck)가 보고 NVS 표시를 지운다.
// 코어 0 에서 NVS 를 만지지 않으려고 표식만 세운다.
volatile bool gFailedStop = false;

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
            gRecording = false; gStopWanted = true; gFailedStop = true;
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

namespace {
void setCutFlag(uint8_t v) {
    Preferences p;
    p.begin("sail", false);
    p.putUChar("rec_on", v);
    p.end();
}
} // namespace

bool cutShort() {
    Preferences p;
    p.begin("sail", true);
    const uint8_t v = p.getUChar("rec_on", 0);
    p.end();
    return v == 1;
}

void clearCutFlag() { setCutFlag(0); }

void noteBootReason(const char* why) { if (why) gBootWhy = why; }

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

/**
 * 이름을 짓는다. **모양은 늘 하나다.**
 *
 *     S00012_20260825-1432.HLG          위성을 잡은 세션
 *     S00014_19700103-0040_nosat.HLG    못 잡은 세션
 *     └─번호─┘└───시각────┘└표시┘
 *        ↑ 정렬을 맡는다     ↑ 읽는다   ↑ 못 믿는 시각이라는 표
 *
 * 시각은 이 순서로 고른다.
 *
 *   1. 이 세션의 첫 fix (utc)          제일 맞다
 *   2. 보드 시계                        이번에 켠 뒤 위성을 한 번이라도
 *                                       잡았으면 맞다 (main.cpp 의 clockFromGps)
 *   3. 그래도 없으면 시계가 말하는 그대로  1970년으로 나온다
 *
 * 3번은 지어낸 값이 아니라 **보드가 실제로 아는 시각**이다. 그래도 못 믿는
 * 값이니 뒤에 `_nosat` 을 붙여 눈에 띄게 한다. 모양은 그대로 유지된다 —
 * 앞의 번호가 정렬을 맡으니 뒤에 뭐가 붙어도 순서는 안 흔들린다.
 *
 * ★ 이름은 **그 고장 시각**으로 적는다. 코치가 카드를 뽑아 "아침 10시 것"
 *   을 찾는데 UTC 로 적혀 있으면 아홉 시간이 어긋나 보인다. 기울기(분)는
 *   NVS 의 tz_min 에 있고 기본은 한국(+9시간 = 540분)이다. `tz <분>` 으로 바꾼다.
 *
 * 머리글에는 UTC 그대로 들어간다. 이름만 사람 보기 좋게 바꾸는 것이다.
 */
static void nameFor(char* out, size_t cap, uint32_t session,
                    uint32_t utc, const char* ext) {
    // 위성으로 안 시각인가, 보드 시계가 말하는 것뿐인가
    const bool sure = utc != 0;
    if (!utc) utc = (uint32_t)time(nullptr);      // 보드 시계

    Preferences prefs;
    prefs.begin("sail", true);
    const int32_t tzMin = (int32_t)prefs.getInt("tz_min", 540);   // 기본 한국
    prefs.end();

    const time_t local = (time_t)((int64_t)utc + (int64_t)tzMin * 60);
    struct tm tmv;
    gmtime_r(&local, &tmv);                 // 이미 더했으니 gmtime 으로 푼다
    snprintf(out, cap, "/LOGS/S%05u_%04d%02d%02d-%02d%02d%s.%s",
             (unsigned)session, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, sure ? "" : "_nosat", ext);
}

bool start(const Header& h) {
    if (!gBuf)      { gLastError = "버퍼 없음"; return false; }
    if (gRecording) { gLastError = "이미 기록 중"; return false; }
    if (!cardPresent()) { gLastError = "카드가 안 꽂혀 있습니다"; return false; }

    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        gLastError = "마운트 실패 — sd 명령으로 이유를 보세요";
        return false;
    }
    SD.mkdir("/LOGS");

    // ── 파일 이름 ───────────────────────────────────────────────────────
    //
    //     S00012_20260825-1432.HLG
    //     └─번호─┘└───시각────┘
    //        ↑ 정렬       ↑ 사람이 읽는 것
    //
    // **번호가 순서를 맡는다.** NVS 에 저장되고 늘 올라간다 — 전원을 빼도,
    // 파일을 지워도 되돌아가지 않는다. 그래서 이름순으로 늘어놓으면 늘
    // 만든 순서다. 시각이 틀려도, 아예 없어도 순서는 안 뒤집힌다.
    //
    // **시작할 때는 보드 시계로 짓는다.** 이 세션의 첫 fix 는 아직 없다 —
    // 해변에서 버튼을 누르면 그 순간 위성이 없고 보통 1~2분 뒤에 잡힌다.
    // 위성을 잡으면 닫을 때 그 시각으로 이름을 고친다 (stop 참조).
    // 번호는 NVS 에 남는다 (플래시의 따로 떼어 둔 자리, 0x9000). 전원을 빼도,
    // 앱을 다시 구워도 남는다. 지워지는 건 esptool erase_flash 뿐이다.
    Preferences prefs;
    prefs.begin("sail", false);
    uint32_t next = prefs.getUInt("sess_n", 0) + 1;

    // ★ 카드에 있는 제일 큰 번호보다도 커야 한다.
    //
    //   NVS 가 날아가거나 (플래시를 통째로 지웠거나) 다른 보드에서 쓰던
    //   카드를 꽂으면 번호가 1 부터 다시 시작한다. 그러면 이름순이 만든
    //   순서와 어긋난다.
    //
    //   예전에는 "같은 이름이 있으면 하나 올린다" 로 막았는데, 이름에 시각이
    //   붙으면서 그게 안 통한다 — 번호가 같아도 시각이 다르면 다른 이름이라
    //   부딪치지 않는다. 그래서 번호만 보고 정한다.
    {
        File dir = SD.open("/LOGS");
        while (File e = dir.openNextFile()) {
            const String nm = e.name();
            e.close();
            if (nm.length() < 6 || nm[0] != 'S') continue;
            const uint32_t n = (uint32_t)nm.substring(1, 6).toInt();
            if (n >= next) next = n + 1;
        }
        dir.close();
    }
    gSession = next;

    nameFor(gPath, sizeof(gPath), gSession, 0, "HLG");
    nameFor(gTxtPath, sizeof(gTxtPath), gSession, 0, "TXT");
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
    memcpy(hdr + kOffPrevSession, &h.prevSession, 4);
    const uint16_t hcrc = crc16(hdr, 126);
    hdr[126] = (uint8_t)hcrc; hdr[127] = (uint8_t)(hcrc >> 8);
    gBin.write(hdr, sizeof(hdr));
    gBin.flush();

    if (gTxt) {
        gTxt.printf("# sail 경기정 모듈 — 세션 %u\n", (unsigned)gSession);
        gTxt.printf("# 바이너리 원본: %s\n", gPath);
        gTxt.printf("# 이 파일은 10초에 한 줄짜리 사본입니다. 눈으로 보는 용입니다.\n");
        gTxt.printf("# 힐·피치·헤딩은 참고값입니다 (가속도에서 뽑음).\n");
        gTxt.printf("# 속도 세 가지: 앞의 kn=도플러(RMC, 파일에 들어가는 값) "
                    "· pv=NAV-PV · pos=위치차분\n");
        gTxt.printf("# GNSS 동역학모델 %u / %u Hz,  IMU %s,  속도는 도플러 원본\n",
                    h.gnssDyn, h.gnssHz,
                    h.imuType == kImuBNO085 ? "BNO085" : "MPU-9250");
        gTxt.printf("# 이 보드가 지난번에 꺼진 이유: %s\n", gBootWhy);
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
    gFailedStop = false;
    gRecording = true;

    // ★ 여기서 표시를 세운다. 전원이 끊기면 이게 남아서 다음에 이어 시작한다.
    setCutFlag(1);

    Serial.printf("[LOG] 기록 시작 — %s  (+ %s)\n", gPath, gTxtPath);
    if (h.prevSession) {
        Serial.printf("[LOG] 세션 %u 가 끊겨서 이어받았습니다\n", (unsigned)h.prevSession);
    }
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
    // 사람이 끝낸 것이다. 다음에 켜질 때 이어 시작하면 안 된다.
    setCutFlag(0);
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
    if (SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
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

    // ── 이름을 시각으로 바꾼다 ──
    //
    // 시작할 때는 보드 시계로 지었다. 이 세션에서 위성을 잡았으면 그
    // 시각이 더 맞으니 그것으로 고친다. 못 잡았으면 그대로 둔다.
    if (gUtcStart) {
        char binNew[48], txtNew[48];
        nameFor(binNew, sizeof(binNew), gSession, gUtcStart, "HLG");
        nameFor(txtNew, sizeof(txtNew), gSession, gUtcStart, "TXT");
        if (SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
            if (strcmp(gPath, binNew) != 0 && SD.rename(gPath, binNew)) {
                snprintf(gPath, sizeof(gPath), "%s", binNew);
            }
            if (strcmp(gTxtPath, txtNew) != 0) SD.rename(gTxtPath, txtNew);
            SD.end();
        }
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
    const uint16_t c = crc16(r, kImuSize - 2);
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

    // 속도 세 가지를 나란히. 1노트 아래에서 어느 길이 살아남는지 보려는 것이다.
    //   dop = 도플러(RMC, 바이너리에 들어가는 값) · pv = NAV-PV · pos = 위치차분
    if (t.sogPvKn >= 0) n += snprintf(line + n, sizeof(line) - n, "pv%5.2f ", t.sogPvKn);
    else                n += snprintf(line + n, sizeof(line) - n, "pv --- ");
    if (t.sogPosKn >= 0) n += snprintf(line + n, sizeof(line) - n, "pos%5.2f ", t.sogPosKn);
    else                 n += snprintf(line + n, sizeof(line) - n, "pos --- ");

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
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        Serial.println("[검사] 마운트 실패 — sd 명령으로 이유를 보세요.");
        return;
    }

    char path[64];
    if (session == 0) {
        Preferences prefs;
        prefs.begin("sail", true);
        session = prefs.getUInt("sess_n", 0);
        prefs.end();
    }
    // 이름 뒤에 붙은 시각은 닫을 때 바뀔 수 있다. 번호로 시작하는 것을 찾는다.
    char want[16];
    snprintf(want, sizeof(want), "S%05u_", (unsigned)session);
    path[0] = '\0';
    {
        File dir = SD.open("/LOGS");
        while (File e = dir.openNextFile()) {
            const String nm = e.name();
            e.close();
            if (nm.startsWith(want) && nm.endsWith(".HLG")) {
                snprintf(path, sizeof(path), "/LOGS/%s", nm.c_str());
                break;
            }
        }
        dir.close();
    }
    if (!path[0]) {
        Serial.printf("[검사] 세션 %u 파일을 못 찾았습니다.\n", (unsigned)session);
        SD.end();
        return;
    }

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
        uint32_t prev; memcpy(&prev, hdr + kOffPrevSession, 4);
        if (prev) Serial.printf("  이어받음       세션 %u 가 끊겨서 이어서 찍은 파일입니다\n",
                                (unsigned)prev);
    }
    Serial.printf("  세션 %u  IMU %s  움직임종류 %u  %u/%u Hz\n",
                  (unsigned)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24)),
                  hdr[kOffImuType] == kImuBNO085 ? "BNO085" : "MPU-9250",
                  hdr[kOffGnssDyn], hdr[39], hdr[40]);

    // 레코드를 훑는다. 한 번에 조금씩 읽어서 램을 아낀다.
    uint32_t nav = 0, imu = 0, bad = 0;
    uint32_t navFirst = 0, navLast = 0, imuFirst = 0, imuLast = 0;
    // ★ 끊긴 파일에서 시각을 되찾는다.
    //
    // 머리글의 첫 fix UTC 는 세션을 닫을 때만 박힌다. 끊기면 0 으로 남고
    // 이름도 _nosat 인 채로 굳는다. 그런데 **시각은 파일 안에 이미 있다** —
    // NAV 줄마다 GPS 주차(week)와 주중시각(itow)이 들어 있다.
    // 실제로 세션 27 이 위성 29개를 잡고도 _nosat 으로 남았다 (2026-08-30).
    uint32_t firstFixUtc = 0, lastFixUtc = 0;
    uint32_t fixRows = 0;
    uint32_t prevImuMs = 0;
    uint32_t gap10 = 0, gapOther = 0, gapMax = 0;
    uint8_t  rec[64];
    // 옛 파일(v1.0)은 IMU 레코드가 27바이트다. 머리글을 보고 고른다.
    const size_t imuSize = (hdr[5] >= 1) ? kImuSize : kImuSizeV0;

    while (f.available()) {
        const int t = f.read();
        if (t < 0) break;
        const size_t size = (t == kTypeNav) ? kNavSize : ((t == kTypeImu) ? imuSize : 0);
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
            // ★ fix 가 있는 줄만 믿는다.
            //
            // 위성을 못 잡았는데도 수신기가 시각 칸을 채워 보내는 때가 있다.
            // 실측: 실내에서 찍은 세션이 16줄 전부 시각이 있다고 나왔고,
            // 풀어 보니 1999년이었다 (2026-08-31). 그 값을 쓰면 지어낸 시각을
            // 보여주게 된다. fix 가 선 줄의 시각만 쓴다.
            //   NAV 레코드에서 fix 는 오프셋 24 다 (hlog.h 의 표 순서 그대로).
            uint32_t itow; uint16_t week;
            memcpy(&itow, rec + 5, 4);
            memcpy(&week, rec + 9, 2);
            if (rec[24] != 0 && itow != kItowInvalid && week != kWeekInvalid) {
                // GPS 시각 → UNIX. 315964800 은 1980-01-06 (GPS 원점).
                // 윤초는 안 뺀다 — 머리글에 넣는 값과 같은 규칙이다 (time_ref=1).
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
        if (((nav + imu) & 0x3FF) == 0) esp_task_wdt_reset();
    }
    f.close();
    const uint64_t freeB = SD.totalBytes() - SD.usedBytes();
    SD.end();

    const uint32_t used = kHeaderSize + nav * kNavSize + imu * imuSize;
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
    // 머리글이 비어 있어도 줄 안의 GPS 시각으로 언제 찍은 파일인지 알 수 있다.
    if (fixRows) {
        Preferences pz;
        pz.begin("sail", true);
        const int32_t tzMin = (int32_t)pz.getInt("tz_min", 540);
        pz.end();
        struct tm a, b;
        const time_t la = (time_t)((int64_t)firstFixUtc + (int64_t)tzMin * 60);
        const time_t lb = (time_t)((int64_t)lastFixUtc  + (int64_t)tzMin * 60);
        gmtime_r(&la, &a); gmtime_r(&lb, &b);
        Serial.printf("  줄에서 찾은 시각 %04d-%02d-%02d %02d:%02d:%02d ~ %02d:%02d:%02d (그 고장 시각)\n",
                      a.tm_year + 1900, a.tm_mon + 1, a.tm_mday,
                      a.tm_hour, a.tm_min, a.tm_sec, b.tm_hour, b.tm_min, b.tm_sec);
        Serial.printf("  위성 잡은 줄   %u / %u\n", (unsigned)fixRows, (unsigned)nav);
        uint32_t hu; memcpy(&hu, hdr + 24, 4);
        if (!hu) {
            Serial.println("  ※ 머리글은 비었는데 줄에는 시각이 있습니다.");
            Serial.println("     끊긴 파일이라 이름이 _nosat 으로 굳은 것뿐입니다. 값은 멀쩡합니다.");
        }
    }
    Serial.printf("  카드 남은 자리 %llu MB\n", (unsigned long long)(freeB / 1048576ULL));
    Serial.println("──────────────────────────────────────────");
    Serial.println(((total == used) && bad == 0 && hok) ? "  ✅ 깨끗합니다" : "  ❌ 문제가 있습니다");
}

/**
 * TXT 사본의 끝(또는 앞) 몇 줄을 시리얼로 찍는다.
 *
 * 카드를 뽑아 컴퓨터에 꽂을 수 없을 때, **세션이 끊기기 직전에 무슨 일이
 * 있었는지** 를 볼 수 있는 유일한 길이다. 한 줄에 전압(mV), 그때까지의
 * 최대 멈춤(ms), 버퍼 최고 사용률(%) 이 다 들어 있다.
 *
 * 파일 전체를 램에 올리지 않는다. 끝에서 필요한 만큼만 되짚어 읽는다.
 */
void tail(uint32_t session, uint16_t lines, bool head) {
    if (gRecording) { Serial.println("[꼬리] 기록 중에는 못 합니다. rec off 먼저."); return; }
    if (!cardPresent()) { Serial.println("[꼬리] 카드가 없습니다."); return; }
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        Serial.println("[꼬리] 마운트 실패."); return;
    }

    if (session == 0) {
        Preferences prefs;
        prefs.begin("sail", true);
        session = prefs.getUInt("sess_n", 0);
        prefs.end();
    }
    // 이름 뒤 시각은 닫을 때 바뀔 수 있으니 번호로 찾는다. 옛 파일은 S00001.TXT
    // 처럼 밑줄이 없다. 둘 다 받는다.
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    char path[80];
    path[0] = '\0';
    {
        File dir = SD.open("/LOGS");
        while (File e = dir.openNextFile()) {
            const String nm = e.name();
            e.close();
            if (nm.startsWith(want) && nm.endsWith(".TXT")) {
                snprintf(path, sizeof(path), "/LOGS/%s", nm.c_str());
                break;
            }
        }
        dir.close();
    }
    if (!path[0]) {
        Serial.printf("[꼬리] 세션 %u 의 TXT 를 못 찾았습니다.\n", (unsigned)session);
        SD.end(); return;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("[꼬리] %s 를 못 열었습니다.\n", path); SD.end(); return; }

    const uint32_t total = f.size();
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  %s   %u 바이트\n", path, (unsigned)total);

    uint32_t from = 0;
    if (!head) {
        // 한 줄이 100바이트를 넘는 일은 없다. 넉넉히 160 으로 잡고 되짚는다.
        const uint32_t back = (uint32_t)lines * 160;
        from = (total > back) ? (total - back) : 0;
        f.seek(from);
        if (from) f.readStringUntil('\n');   // 잘린 첫 줄은 버린다
        Serial.printf("  ── 마지막 %u줄 ──\n", (unsigned)lines);
    } else {
        Serial.printf("  ── 처음 %u줄 ──\n", (unsigned)lines);
    }

    uint16_t shown = 0;
    while (f.available() && shown < (head ? lines : (uint16_t)(lines * 2))) {
        const String ln = f.readStringUntil('\n');
        if (!ln.length()) continue;
        Serial.print("  "); Serial.println(ln);
        ++shown;
        if ((shown & 0x0F) == 0) esp_task_wdt_reset();
    }
    f.close();
    SD.end();
    Serial.println("──────────────────────────────────────────");
}

bool removeSession(uint32_t session) {
    if (gRecording) { Serial.println("[지움] 기록 중에는 못 합니다. rec off 먼저."); return false; }
    if (!session)   { Serial.println("[지움] 번호를 적으세요."); return false; }
    if (!cardPresent()) { Serial.println("[지움] 카드가 없습니다."); return false; }
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        Serial.println("[지움] 마운트 실패."); return false;
    }
    char want[16];
    snprintf(want, sizeof(want), "S%05u", (unsigned)session);
    // 이름 뒤 시각은 세션마다 다르므로 번호로 찾는다. 한 번에 다 모아 놓고
    // 지운다 — 훑는 도중에 지우면 다음 항목을 건너뛴다.
    char hit[4][80];
    int n = 0;
    {
        File dir = SD.open("/LOGS");
        while (File e = dir.openNextFile()) {
            const String nm = e.name();
            e.close();
            if (n < 4 && nm.startsWith(want) &&
                (nm.endsWith(".HLG") || nm.endsWith(".TXT"))) {
                snprintf(hit[n], sizeof(hit[n]), "/LOGS/%s", nm.c_str());
                ++n;
            }
        }
        dir.close();
    }
    if (!n) {
        Serial.printf("[지움] 세션 %u 파일이 없습니다.\n", (unsigned)session);
        SD.end(); return false;
    }
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (SD.remove(hit[i])) { Serial.printf("  지웠습니다  %s\n", hit[i]); ++ok; }
        else                   { Serial.printf("  못 지웠습니다 %s\n", hit[i]); }
    }
    SD.end();
    return ok == n;
}

void listFiles() {
    if (gRecording) { Serial.println("[목록] 기록 중에는 못 합니다."); return; }
    if (!cardPresent()) { Serial.println("[목록] 카드가 없습니다."); return; }
    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
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
    // 쓰기가 실패해서 코어 0 이 저절로 멈춘 경우. 카드가 죽은 것이니
    // 다시 켜서 또 걸어봐야 똑같이 실패한다. 이어시작 표시를 지운다.
    if (gFailedStop) {
        gFailedStop = false;
        setCutFlag(0);
        Serial.println("[LOG] ★ 카드 쓰기가 실패해 멈췄습니다 — 이어시작은 안 합니다");
    }
    if (!gRecording) return;
    if (!cardPresent()) {
        Serial.println("[LOG] ★ 기록 중에 카드가 빠졌습니다 — 멈춥니다");
        gLastError = "기록 중 카드가 빠졌습니다";
        stop();
    }
}

} // namespace hlog
