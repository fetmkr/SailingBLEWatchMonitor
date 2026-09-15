// 몇 초씩 루프를 붙잡는 진단 명령. 운영 상태(기록·센서·BLE)를 바꾸지 않는 것만 모은다.
//
// ★ NEXT 6 번 첫 단계 (2026-09-15). main.cpp 가 6200줄이라 진단과 운영이 한 파일에 섞여 있었다.
//   카드만 쓰는 두 명령과, 입력이 좁은 batt · oledw · sleepstat 을 옮겼다. IMU·GPS 진단(imu · calib · level · gpscfg · navpv · hdgtilt)은
//   운영 전역(gImu · gGps · 자이로 0점 · NVS)을 직접 만져서, 그걸 함수로 감싸기 전에는 옮기지 않는다.
#include "diagnostics.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "board_rak.h"
#include "display_rak.h"
#include "sdcard.h"

extern void  sailFeedWatchdog();                 // main.cpp 가 준다
extern float sailReadBatteryVolts(uint32_t* mv);
extern float sailBatteryPercent(float volts);

namespace diag {

// ── SD카드 확인 (RAK15002, IO 슬롯) ──────────────────────────────────────
void sdCheck() {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  RAK15002 SD — SPI (CLK%d MISO%d MOSI%d CS%d)\n",
                  rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);

    // 카드 삽입 감지. LOW 일 때 카드가 들어 있다 (내부 풀업).
    pinMode(rak::kSdCardDetect, INPUT_PULLUP);
    delay(10);
    int cd = digitalRead(rak::kSdCardDetect);
    Serial.printf("  카드 감지 GPIO%d = %s\n", rak::kSdCardDetect,
                  cd == LOW ? "LOW (카드 있음)" : "HIGH (카드 없음?)");

    sailFeedWatchdog();
    // 4 MHz 로 시작한다. 붙고 나서 필요하면 올린다. 카드는 사용권으로 쥔다 (sdcard.h).
    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        if (sdcard::lastRefusal() == sdcard::Refusal::Busy) {
            Serial.printf("  카드를 지금 쓰는 곳: %s.\n", sdcard::ownerName(sdcard::owner()));
            Serial.println("──────────────────────────────────────────");
            return;
        }
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

    sdcard::release(sdcard::Owner::Diagnostic);
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
void sdBench(uint32_t rows) {
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  SD 쓰기 실측 — %u줄 (10 Hz 로 %.0f초치)\n",
                  (unsigned)rows, rows / 10.0f);

    if (!sdcard::acquire(sdcard::Owner::Diagnostic)) {
        Serial.println("  마운트 실패 — 먼저 sd 로 확인하세요.");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    SD.mkdir("/SAIL");
    SD.remove("/SAIL/BENCH.CSV");

    File f = SD.open("/SAIL/BENCH.CSV", FILE_WRITE);
    if (!f) {
        Serial.println("  파일을 못 열었습니다.");
        sdcard::release(sdcard::Owner::Diagnostic);
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
        if ((i & 0xFF) == 0) sailFeedWatchdog();
    }
    if (used > 0) { f.write((const uint8_t*)buf, used); bytes += used; }
    f.flush();
    const uint32_t elapsed = millis() - t0;
    const uint32_t size    = f.size();
    f.close();

    const uint64_t totalB = SD.totalBytes();
    const uint64_t usedB  = SD.usedBytes();
    sdcard::release(sdcard::Owner::Diagnostic);

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

void batteryReport() {
    uint32_t mv = 0;
    float    v  = sailReadBatteryVolts(&mv);
    Serial.println("──────────────────────────────────────────");
    Serial.printf("  GPIO%d 실측       %u mV\n", rak::kBattAdcPin, (unsigned)mv);
    Serial.printf("  분압 되짚기       ÷ %.2f\n", rak::kBattDivider);
    Serial.printf("  배터리 전압       %.3f V  (약 %.0f%%)\n", v, sailBatteryPercent(v));
    Serial.println("──────────────────────────────────────────");
    Serial.println("  잔량은 리튬폴리머 방전 곡선으로 환산합니다 (직선 아님).");
    Serial.println("──────────────────────────────────────────");
    Serial.println("  ★ USB 가 꽂혀 있으면 충전 중이라 실제보다 높게 나옵니다.");
    Serial.println("    진짜 잔량은 USB 를 뽑고 재야 합니다.");
    Serial.println("  멀티미터 값과 어긋나면 board_rak.h 의 kBattCorrection 조정.");
}

// `oledw` — 화면에 쓸 한글 줄이 실제로 들어가나 재본다.
//
// 눈대중으로 자리를 잡지 않는다. 그리고 **글꼴에 없는 글자는 폭 0 으로 나온다.**
// 그래서 폭을 재면 빠진 글자까지 같이 잡아낸다. 한글 한 자는 16px, 빈칸과
// 숫자는 8px 여야 맞다.
void oledWidths() {
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
void sleepReport(const SleepStats& st) {
    Serial.println("──────────────────────────────────────────");
    if (!st.everSlept) {
        Serial.println("  아직 한 번도 안 잤습니다 (off 명령이나 버튼 5초)");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    Serial.printf("  잠든 횟수         %u\n", (unsigned)st.sleeps);
    Serial.printf("  헛깸(5초 못 채움) %u   ← 크면 버튼 핀이 뜨는 것이다\n",
                  (unsigned)st.falseWakes);
    Serial.printf("  5초 채워 켜짐     %u\n", (unsigned)st.fullWakes);

    if (st.sleptUs == 0) {
        Serial.println("  아직 깨어난 적이 없습니다.");
        Serial.println("──────────────────────────────────────────");
        return;
    }
    const double sec = (double)st.sleptUs / 1e6;
    Serial.printf("  마지막으로 잔 시간 %.1f 초 (%.2f 시간)\n", sec, sec / 3600.0);
    // ★ 깰 때 전압은 안 보여준다.
    //
    //   깬 직후에는 배터리 값이 틀리게 읽힌다. 얼마나 잤느냐에 따라 다르게
    //   틀린다 — 5분 자면 2198, 60분 자면 3393 mV 로 나왔다. 5분만 자도
    //   전압이 15 mV **올라가** 있다. 배터리가 저절로 충전될 리 없다.
    //   [확인: 2026-09-09~10, off 300 과 13시간짜리 두 판]
    //
    //   원인을 못 찾았다. 세 가지를 짚었는데 셋 다 아니었다 (핀 떼어놓기,
    //   디지털 입력 버퍼 끄기, 3V3_S 켜기).
    //
    //   ★ 그런데 이걸 화면에 숫자로 뱉어 뒀더니 **내가 그 위에 13시간짜리
    //     측정을 쌓았다.** 문서에 "못 믿는 값" 이라고 적어 뒀는데도 그랬다.
    //     적어 두는 걸로는 안 막힌다. 안 보여주는 것으로 막는다.
    //     이 저장소의 원칙 그대로다 — 값이 없으면 없다고 보여준다.
    Serial.printf("  잘 때 %u mV\n", (unsigned)st.sleepMv);
    Serial.println("  깰 때  --   (깬 직후 값은 못 믿는다. 원인 미상)");
    Serial.printf("  깨자마자 GPS 가 뱉은 바이트  %u\n", (unsigned)st.gpsBytes);
    Serial.println(st.gpsBytes > 0
        ? "  ★ 0 이 아닙니다 — 자는 동안 3V3_S 가 안 꺼졌습니다. GPS 가 계속 돌았습니다."
        : "  0 입니다 — 3V3_S 는 제대로 꺼져 있었습니다.");

    Serial.println("  ※ 시간당 낙차는 안 계산한다. 깰 때 값을 못 믿으므로");
    Serial.println("    거기서 나오는 숫자도 못 믿는다. 잠자기 전류를 재려면");
    Serial.println("    멀티미터를 배터리 선에 물려야 한다 (POWER.md).");
    Serial.println("  ※ ADC 소스 임피던스가 2.5 MΩ 라 mV 단위는 흔들립니다.");
    Serial.println("  ※ USB 를 꽂은 채로 재면 충전 때문에 값이 무의미합니다.");
    Serial.println("──────────────────────────────────────────");
}

} // namespace diag
