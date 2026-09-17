// 몇 초씩 루프를 붙잡는 진단 명령 — firmware-rak/src/diagnostics.cpp (커밋 2b18b17) 를 ESP-IDF v6.1 로 옮긴 것.
// 헤더는 firmware-rak/include/diagnostics.h 를 같이 쓴다 (아두이노 흔적 없음).
// oledWidths 는 main/display.cpp 에 있다 — 여기서 빼았다.
//
// 바뀐 곳 (출력 글자·재는 방법·순서는 그대로)
//   Serial.printf/println           → printf
//   millis()                        → esp_timer_get_time()/1000
//   pinMode(INPUT_PULLUP)·digitalRead → gpio_config · gpio_get_level
//   SD.open(path, FILE_WRITE)       → fopen(path, "w") + setvbuf 4096 (아두이노 FILE_WRITE="w", vfs_api.cpp DEFAULT_FILE_BUFFER_SIZE 4096)
//   File.write / flush / size       → fwrite / fflush+fsync / fstat   (hlog_idf.cpp 와 같은 방식)
//   SD.mkdir / remove               → mkdir / remove  (앞에 마운트 자리 "/sd")
//   SD.totalBytes / usedBytes       → esp_vfs_fat_info 의 total, total−free (둘 다 FatFs f_getfree 의 클러스터 수로 셈)
//   SD.cardType / cardSize          → sdmmc_card_t (is_mmc · OCR bit30 · csd.capacity×sector_size).
//                                     ★ sdcard_idf.cpp 가 카드 포인터를 안 내준다 → 약한 심볼 sailSdCard() 로 받는다.
//                                       메인(또는 sdcard_idf.cpp)이 안 주면 "모름" 으로 찍는다 (지어내지 않는다)
#include "diagnostics.h"

#include <errno.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sd_protocol_types.h"

#include "board_rak.h"
#include "sdcard.h"

extern void  sailFeedWatchdog();                 // main 이 준다
extern float sailReadBatteryVolts(uint32_t* mv);
extern float sailBatteryPercent(float volts);
// 붙어 있는 카드 정보. 안 주면 null (약한 심볼) — 카드 종류·크기를 "모름" 으로 찍는다
extern sdmmc_card_t* sailSdCard() __attribute__((weak));

namespace diag {
namespace {
inline uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline void flushFile(FILE* f) { fflush(f); fsync(fileno(f)); }
inline sdmmc_card_t* cardNow() { return sailSdCard ? sailSdCard() : nullptr; }
} // namespace

// ── SD카드 확인 (RAK15002, IO 슬롯) ──────────────────────────────────────
void sdCheck() {
    printf("──────────────────────────────────────────\n");
    printf("  RAK15002 SD — SPI (CLK%d MISO%d MOSI%d CS%d)\n",
           rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);

    // 카드 삽입 감지. LOW 일 때 카드가 들어 있다 (내부 풀업).
    {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << rak::kSdCardDetect;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    const int cd = gpio_get_level(static_cast<gpio_num_t>(rak::kSdCardDetect));
    printf("  카드 감지 GPIO%d = %s\n", rak::kSdCardDetect,
           cd == 0 ? "LOW (카드 있음)" : "HIGH (카드 없음?)");

    sailFeedWatchdog();
    // 카드는 사용권으로 쥔다 (sdcard.h).
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        if (sdcard::lastRefusal() == sdcard::Refusal::Busy) {
            printf("  카드를 지금 쓰는 곳: %s.\n", sdcard::ownerName(sdcard::owner()));
            printf("──────────────────────────────────────────\n");
            return;
        }
        printf("  마운트 실패.\n");
        printf("\n");
        printf("  ★ 진짜 이유는 바로 위의 [E] 로 시작하는 줄에 있습니다.\n");
        printf("    라이브러리가 FatFs 오류 번호를 그대로 찍어 줍니다.\n");
        printf("\n");
        printf("    (13) There is no valid FAT volume\n");
        printf("        → 카드는 읽히는데 FAT 가 아니다. 64GB 이상은 공장에서\n");
        printf("          exFAT 로 나오고 우리 빌드는 exFAT 를 안 읽는다.\n");
        printf("          맥에서 FAT32 로 다시 포맷하면 된다:\n");
        printf("            diskutil list\n");
        printf("            diskutil eraseDisk FAT32 SAIL MBRFormat /dev/diskN\n");
        printf("\n");
        printf("    (3) The physical drive cannot work / (1) hard error\n");
        printf("        → SPI 가 안 통한다. 카드가 덜 꽂혔거나 모듈이\n");
        printf("          IO 슬롯이 아닌 곳에 꽂혔다 (IO 슬롯 전용).\n");
        printf("──────────────────────────────────────────\n");
        return;
    }

    const sdmmc_card_t* card = cardNow();
    if (card) {
        // 아두이노 sd_diskio.cpp 와 같은 가름: MMC · OCR bit30(HCS) 이면 SDHC/SDXC · 아니면 SDSC
        const char* typeName = card->is_mmc ? "MMC"
                               : (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC"
                                                              : "SDSC";
        printf("  카드 종류  %s\n", typeName);
        printf("  크기       %llu MB\n",
               (unsigned long long)((uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size / (1024ULL * 1024ULL)));
    } else {
        printf("  카드 종류  모름 (카드 정보를 받는 sailSdCard 가 없다)\n");
        printf("  크기       모름\n");
    }

    // 쓰기까지 돼야 기록에 쓸 수 있다.
    static const char kTestPath[] = "/sd/sail_test.txt";
    static const char kTestLine[] = "sailing monitor write test\n";
    errno = 0;
    FILE* f = fopen(kTestPath, "w");
    if (f) {
        setvbuf(f, nullptr, _IOFBF, 4096);
        bool ok = fwrite(kTestLine, 1, sizeof(kTestLine) - 1, f) == sizeof(kTestLine) - 1;
        ok = fflush(f) == 0 && ok;
        ok = fsync(fileno(f)) == 0 && ok;
        ok = fclose(f) == 0 && ok;

        char got[sizeof(kTestLine)] = {};
        FILE* check = ok ? fopen(kTestPath, "r") : nullptr;
        if (check) {
            ok = fread(got, 1, sizeof(kTestLine) - 1, check) == sizeof(kTestLine) - 1 &&
                 memcmp(got, kTestLine, sizeof(kTestLine) - 1) == 0;
            ok = fclose(check) == 0 && ok;
        } else {
            ok = false;
        }
        if (ok) printf("  쓰기       OK (/sail_test.txt 되읽기까지 일치)\n");
        else    printf("  쓰기       실패 — 쓴 파일을 다시 읽지 못했습니다 (errno %d: %s)\n", errno, strerror(errno));
        remove(kTestPath);
    } else {
        printf("  쓰기       실패 — 파일을 열지 못했습니다 (errno %d: %s)\n", errno, strerror(errno));
    }

    sdcard::release(sdcard::Owner::Diagnostic);
    printf("──────────────────────────────────────────\n");
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
//
// ★ firmware-rak 과 같이 이 안에서는 워치독만 먹인다. IMU FIFO 는 안 비운다 — 오래 돌리면 FIFO 가 넘친다
//   (CLAUDE.md "명령이 루프를 붙잡으면 센서가 언다"). 부르는 쪽이 기록 중이 아닐 때만 부른다 (sdFreeFor).
void sdBench(uint32_t rows) {
    printf("──────────────────────────────────────────\n");
    printf("  SD 쓰기 실측 — %u줄 (10 Hz 로 %.0f초치)\n", (unsigned)rows, rows / 10.0f);

    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        printf("  마운트 실패 — 먼저 sd 로 확인하세요.\n");
        printf("──────────────────────────────────────────\n");
        return;
    }
    mkdir("/sd/SAIL", 0777);          // 이미 있으면 EEXIST — 아두이노 SD.mkdir 도 결과를 안 본다
    remove("/sd/SAIL/BENCH.CSV");

    FILE* f = fopen("/sd/SAIL/BENCH.CSV", "w");
    if (!f) {
        printf("  파일을 못 열었습니다.\n");
        sdcard::release(sdcard::Owner::Diagnostic);
        printf("──────────────────────────────────────────\n");
        return;
    }
    setvbuf(f, nullptr, _IOFBF, 4096);   // 아두이노 File 과 같은 4 KB 버퍼

    // 실제 기록 줄과 길이를 맞춘 본보기 (SDLOG.md §3 의 예시 그대로 153바이트)
    static const char kSample[] =
        "1234500,1787492994100,1,37.5123456,126.9123456,5.53,5.61,315.0,11,1.4,"
        "0.05,-12.3,2.1,344,-0.034,0.012,-1.005,0.2,0.4,-0.1,3.0,-21.2,-16.7,"
        "68,3.91,0,A3F2\n";
    const size_t lineLen = strlen(kSample);

    static char buf[1024];            // ★ 스택에 KB 를 안 올린다 (firmware-rak 은 지역 배열이었다)
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

    const uint32_t t0 = millis32();
    for (uint32_t i = 0; i < rows; ++i) {
        memcpy(buf + used, kSample, lineLen);
        used += lineLen;

        if (used >= 512) {
            const uint32_t a = millis32();
            fwrite(buf, 1, used, f);
            const uint32_t dt = millis32() - a;
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
                const uint32_t bb = millis32();
                flushFile(f);
                const uint32_t df = millis32() - bb;
                if (df > maxFlush) maxFlush = df;
                ++flushes;
            }
        }
        if ((i & 0xFF) == 0) sailFeedWatchdog();
    }
    if (used > 0) { fwrite(buf, 1, used, f); bytes += used; }
    flushFile(f);
    const uint32_t elapsed = millis32() - t0;
    uint32_t size = 0;
    {
        struct stat st;
        if (fstat(fileno(f), &st) == 0) size = (uint32_t)st.st_size;
    }
    fclose(f);

    uint64_t totalB = 0, freeB = 0;
    if (esp_vfs_fat_info("/sd", &totalB, &freeB) != ESP_OK) { totalB = 0; freeB = 0; }   // 아두이노도 실패면 0
    const uint64_t usedB = totalB - freeB;
    sdcard::release(sdcard::Owner::Diagnostic);

    const float sec  = elapsed / 1000.0f;
    const float kbps = (bytes / 1024.0f) / (sec > 0 ? sec : 1);

    printf("  ─────────────────────────────────────\n");
    printf("  쓴 양            %llu 바이트 (파일 %u)\n", (unsigned long long)bytes, (unsigned)size);
    printf("  걸린 시간        %.2f 초\n", sec);
    printf("  평균 속도        %.0f KB/초\n", kbps);
    printf("  우리가 쓸 양     1.5 KB/초  →  여유 %.0f배\n", kbps / 1.5f);
    printf("  카드 전체 %llu MB  쓴 자리 %llu MB\n",
           (unsigned long long)(totalB / 1048576ULL), (unsigned long long)(usedB / 1048576ULL));
    printf("  ─────────────────────────────────────\n");
    printf("  한 번 쓰기       %u번,  제일 오래 %u ms\n", (unsigned)writes, (unsigned)maxWrite);
    printf("  flush            %u번,  제일 오래 %u ms\n", (unsigned)flushes, (unsigned)maxFlush);

    printf("  ─── 얼마나 오래 멈췄나 ───\n");
    static const char* kLabel[9] = {
        "     ~1 ms", "  1~2 ms", "  2~5 ms", " 5~10 ms", "10~20 ms",
        "20~50 ms", "50~100 ms", "100~200 ms", "200 ms 위"};
    for (int k = 0; k < 9; ++k) {
        if (!hist[k]) continue;
        printf("  %-10s %7u번  (%.3f%%)\n", kLabel[k], (unsigned)hist[k], 100.0f * hist[k] / writes);
    }

    printf("  ─── 제일 오래 멈춘 자리 ───\n");
    printf("  일정한 간격이면 FAT 갱신·클러스터 경계, 들쭉날쭉하면 카드 사정이다.\n");
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
        printf("  %3u ms  파일 %8.2f MB 자리", (unsigned)worstMs[k], worstAt[k] / 1048576.0);
        if (prev) printf("   (앞것과 %.2f MB 차이)", (worstAt[k] - prev) / 1048576.0);
        printf("\n");
        prev = worstAt[k];
    }

    printf("  ─────────────────────────────────────\n");
    const uint32_t worst = maxWrite > maxFlush ? maxWrite : maxFlush;
    if (worst < 100) {
        printf("  제일 오래 멈춘 시간 %u ms — notify 주기 100 ms 안이다.\n", (unsigned)worst);
    } else {
        printf("  ★ 제일 오래 멈춘 시간 %u ms — notify 주기 100 ms 를 넘는다.\n", (unsigned)worst);
        printf("    메인 루프에서 직접 쓰면 안 된다. 쓰기 작업을 따로 띄운다.\n");
    }
    printf("──────────────────────────────────────────\n");
}

void batteryReport() {
    uint32_t mv = 0;
    float    v  = sailReadBatteryVolts(&mv);
    printf("──────────────────────────────────────────\n");
    printf("  GPIO%d 실측       %u mV\n", rak::kBattAdcPin, (unsigned)mv);
    printf("  분압 되짚기       ÷ %.2f\n", rak::kBattDivider);
    printf("  배터리 전압       %.3f V  (약 %.0f%%)\n", v, sailBatteryPercent(v));
    printf("──────────────────────────────────────────\n");
    printf("  잔량은 리튬폴리머 방전 곡선으로 환산합니다 (직선 아님).\n");
    printf("──────────────────────────────────────────\n");
    printf("  ★ USB 가 꽂혀 있으면 충전 중이라 실제보다 높게 나옵니다.\n");
    printf("    진짜 잔량은 USB 를 뽑고 재야 합니다.\n");
    printf("  멀티미터 값과 어긋나면 board_rak.h 의 kBattCorrection 조정.\n");
}

// `sleepstat` — 지난번에 정말 잤나.
//
// 자는 보드는 아무 말도 못 한다. 그래서 잠들기 직전에 RTC 메모리에 적어 둔 것을
// 깨어난 뒤에 읽는다. 여기서 갈리는 것은 두 가지다.
//
//   헛깬 횟수가 많다  → 자다 깨다를 반복한 것이다. 버튼 핀이 뜨고 있다
//   시간당 낙차가 크다 → 자는 동안 뭔가가 켜져 있다
void sleepReport(const SleepStats& st) {
    printf("──────────────────────────────────────────\n");
    if (!st.everSlept) {
        printf("  아직 한 번도 안 잤습니다 (off 명령이나 버튼 5초)\n");
        printf("──────────────────────────────────────────\n");
        return;
    }
    printf("  잠든 횟수         %u\n", (unsigned)st.sleeps);
    printf("  헛깸(5초 못 채움) %u   ← 크면 버튼 핀이 뜨는 것이다\n", (unsigned)st.falseWakes);
    printf("  5초 채워 켜짐     %u\n", (unsigned)st.fullWakes);

    if (st.sleptUs == 0) {
        printf("  아직 깨어난 적이 없습니다.\n");
        printf("──────────────────────────────────────────\n");
        return;
    }
    const double sec = (double)st.sleptUs / 1e6;
    printf("  마지막으로 잔 시간 %.1f 초 (%.2f 시간)\n", sec, sec / 3600.0);
    // ★ 깰 때 전압은 안 보여준다. 깬 직후 배터리 값은 틀리게 읽힌다 (5분 2198, 60분 3393 mV,
    //   2026-09-09~10). 숫자로 뱉어 뒀더니 그 위에 13시간 측정을 쌓았다 — 안 보여주는 것으로 막는다.
    printf("  잘 때 %u mV\n", (unsigned)st.sleepMv);
    printf("  깰 때  --   (깬 직후 값은 못 믿는다. 원인 미상)\n");
    printf("  깨자마자 GPS 가 뱉은 바이트  %u\n", (unsigned)st.gpsBytes);
    printf("%s\n", st.gpsBytes > 0
        ? "  ★ 0 이 아닙니다 — 자는 동안 3V3_S 가 안 꺼졌습니다. GPS 가 계속 돌았습니다."
        : "  0 입니다 — 3V3_S 는 제대로 꺼져 있었습니다.");

    printf("  ※ 시간당 낙차는 안 계산한다. 깰 때 값을 못 믿으므로\n");
    printf("    거기서 나오는 숫자도 못 믿는다. 잠자기 전류를 재려면\n");
    printf("    멀티미터를 배터리 선에 물려야 한다 (POWER.md).\n");
    printf("  ※ ADC 소스 임피던스가 2.5 MΩ 라 mV 단위는 흔들립니다.\n");
    printf("  ※ USB 를 꽂은 채로 재면 충전 때문에 값이 무의미합니다.\n");
    printf("──────────────────────────────────────────\n");
}

} // namespace diag
