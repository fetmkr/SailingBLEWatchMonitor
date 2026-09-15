// 루프를 몇 초씩 붙잡는 진단 명령. 부르는 쪽(main.cpp)이 sdFreeFor 로 카드 주인을 먼저 본다.
#pragma once

#include <cstdint>

namespace diag {
// 잠들기 직전에 RTC 메모리에 적어 둔 것 (main.cpp 가 채워 넘긴다)
struct SleepStats {
    bool     everSlept = false;
    uint32_t sleeps = 0, falseWakes = 0, fullWakes = 0;
    uint64_t sleptUs = 0;
    uint32_t sleepMv = 0, gpsBytes = 0;
};
void batteryReport();            // `batt`
void oledWidths();               // `oledw` — 화면 한글 줄 폭
void sleepReport(const SleepStats& st);   // `sleepstat`
void sdCheck();                  // `sd` — 카드 마운트 + 쓰기 시험
void sdBench(uint32_t rows);     // `sdbench` — 쓰기 속도와 최대 멈춤 실측
}
