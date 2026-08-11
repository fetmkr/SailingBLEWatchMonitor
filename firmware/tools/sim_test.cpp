// ─────────────────────────────────────────────────────────────────────────
//  호스트(macOS/Linux)에서 돌리는 검증 하네스.
//  펌웨어에 플래시하기 전에 "값이 실제로 스펙대로 나오는지" 눈으로 확인한다.
//
//    cd firmware && make -C tools        (또는)
//    c++ -std=c++17 -Iinclude -o /tmp/simtest tools/sim_test.cpp && /tmp/simtest
//
//  검증 항목
//    1. PROTOCOL.md §3 예시 바이트열과 encodeTelemetryPacket() 결과가 일치
//    2. manufacturer data 11바이트 레이아웃
//    3. 노이즈 없이 240초 시뮬레이션 — SOG/COG/HEEL/BATT 궤적과 택 전환
//    4. COG 가 45°↔315° 를 북쪽(0°) 경유로 최단 회전하는지
//    5. 노이즈 켠 상태에서 값 범위가 스펙 안에 있는지 (10분치 샘플링)
// ─────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "display_layout.h"
#include "protocol.h"
#include "simulator.h"

using sail::Telemetry;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_failures++;
}

static void hexdump(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) std::printf("%02X ", p[i]);
}

// 고정 시드 LCG — 매 실행 동일한 결과가 나오도록
static uint32_t g_lcg = 12345;
static float    seededRand01() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (float)((g_lcg >> 8) & 0xFFFFFF) / (float)0x1000000;
}

// ── 1. GATT 패킷 인코딩 ──────────────────────────────────────────────────
static void testTelemetryEncoding() {
    std::printf("\n── 1. GATT characteristic 12바이트 인코딩 ──\n");

    Telemetry t;
    t.uptimeMs = 123456;
    t.sogKn    = 5.53f;
    t.cogDeg   = 315.0f;
    t.heelDeg  = -12.0f;
    t.battPct  = 87.0f;

    uint8_t buf[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(t, buf);

    std::printf("  실측: ");
    hexdump(buf, sizeof(buf));
    std::printf("\n  기대: 01 01 40 E2 01 00 29 02 4E 0C F4 57   (PROTOCOL.md §3)\n");

    const uint8_t expect[12] = {0x01, 0x01, 0x40, 0xE2, 0x01, 0x00,
                                0x29, 0x02, 0x4E, 0x0C, 0xF4, 0x57};
    check(std::memcmp(buf, expect, 12) == 0, "PROTOCOL.md §3 예시와 바이트 단위 일치");

    // 반올림 경계
    check(sail::encodeSog(5.535f) == 554, "encodeSog(5.535) == 554 (반올림)");
    check(sail::encodeCog(359.99f) == 3600 % 3600, "encodeCog(359.99) 이 3600 으로 넘치지 않음");
    check(sail::encodeCog(-1.0f) == 3590, "encodeCog(-1.0) == 3590 (음수 wrap)");
    check(sail::encodeHeel(-12.4f) == -12, "encodeHeel(-12.4) == -12");
    check(sail::encodeBatt(101.0f) == 100 && sail::encodeBatt(-3.0f) == 0, "batt 0…100 클램프");
}

// ── 2. Manufacturer data ─────────────────────────────────────────────────
static void testManufacturerEncoding() {
    std::printf("\n── 2. Manufacturer Specific Data (Company ID 포함 11바이트) ──\n");

    Telemetry t;
    t.moduleID = 1;
    t.sogKn   = 6.02f;
    t.cogDeg  = 45.0f;
    t.heelDeg = 14.0f;
    t.battPct = 99.0f;

    uint8_t mfg[2 + sail::kMfgLen];
    sail::encodeManufacturerData(t, /*seq=*/7, mfg);

    std::printf("  실측: ");
    hexdump(mfg, sizeof(mfg));
    std::printf("\n");

    check(sizeof(mfg) == 11, "전체 길이 11바이트 (Company ID 2 + 페이로드 9)");
    check(mfg[0] == 0xFF && mfg[1] == 0xFF, "Company ID 0xFFFF little-endian");
    check(mfg[2] == 0x01 && mfg[3] == 0x01, "ver / module_id");
    check((mfg[4] | (mfg[5] << 8)) == 602, "sog = 602 (6.02 kn)");
    check((mfg[6] | (mfg[7] << 8)) == 450, "cog = 450 (45.0°)");
    check((int8_t)mfg[8] == 14, "heel = 14");
    check(mfg[9] == 99, "batt = 99");
    check(mfg[10] == 7, "seq = 7");

    // AD 구조 총 길이 검증: [len][0xFF][11] = 13, 이름 [len][0x09]["SAIL-hojun"] = 12
    const size_t scanRspBytes = (1 + 1 + 11) + (1 + 1 + strlen("SAIL-hojun"));
    std::printf("  scan response 총 %zu 바이트 (한도 31)\n", scanRspBytes);
    check(scanRspBytes <= 31, "scan response 가 31바이트 한도 이내");

    const size_t advBytes = 3 /*flags*/ + (1 + 1 + 16) /*128-bit UUID*/;
    std::printf("  ADV 총 %zu 바이트 (한도 31)\n", advBytes);
    check(advBytes <= 31, "ADV 패킷이 31바이트 한도 이내");
}

// ── 2.5 모듈 신원 (이름 → module_id) ─────────────────────────────────────
static void testModuleIdentity() {
    std::printf("\n── 2.5 모듈 신원: 이름 → module_id ──\n");

    const char* names[] = {"SAIL-hojun", "SAIL-KOR1234", "SAIL-A3F2", "SAIL-x", "SAIL-boat2"};
    uint8_t ids[5];
    for (int i = 0; i < 5; i++) {
        ids[i] = sail::moduleIDFromName(names[i]);
        std::printf("   %-14s → module_id %3u (0x%02X)\n", names[i], ids[i], ids[i]);
    }

    // 결정적: 같은 이름은 항상 같은 값 (재부팅/재플래시해도 동일해야 함)
    check(sail::moduleIDFromName("SAIL-hojun") == ids[0], "같은 이름은 항상 같은 module_id");

    // 0 은 "미지정" 예약값이라 절대 나오면 안 된다
    bool anyZero = false;
    for (int i = 0; i < 5; i++) if (ids[i] == 0) anyZero = true;
    check(!anyZero, "module_id 가 0 이 아님 (0 은 미지정 예약)");

    // 서로 다른 이름은 (충돌 가능하지만) 이 샘플에서는 달라야 한다
    bool allDistinct = true;
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (ids[i] == ids[j]) allDistinct = false;
    check(allDistinct, "샘플 이름 5개의 module_id 가 모두 다름");

    // 이름 길이 예산: scan response 31 = mfg(13) + [len][type] + 이름
    const size_t maxName = sail::kMaxFullNameLen;
    const size_t scanRsp = 13 + 2 + maxName;
    std::printf("   최대 이름 %zu자 → scan response %zu 바이트\n", maxName, scanRsp);
    check(scanRsp <= 31, "최대 길이 이름에서도 scan response 가 31바이트 이내");
    check(sail::kMaxUserNameLen == sail::kMaxFullNameLen - 5, "사용자 이름 한도 = 전체 − 접두사(5)");

    // 실제로 쓸 이름으로 패킷을 만들어 module_id 가 실려 나가는지 확인
    Telemetry t;
    t.moduleID = sail::moduleIDFromName("SAIL-hojun");
    uint8_t gatt[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(t, gatt);
    check(gatt[1] == t.moduleID, "GATT 패킷 [1] 에 module_id 가 실림");

    uint8_t mfg[2 + sail::kMfgLen];
    sail::encodeManufacturerData(t, 0, mfg);
    check(mfg[3] == t.moduleID, "Manufacturer Data [3] 에 module_id 가 실림");
}

// ── 2.7 TFT 레이아웃 (240x135) ───────────────────────────────────────────
// 좌표 상수는 display_layout.h 의 static_assert 가 이미 컴파일 타임에 막지만,
// 런타임 문자열 길이(포맷 결과)는 값에 따라 달라지므로 여기서 실측한다.
static void testDisplayLayout() {
    namespace L = sail::layout;
    std::printf("\n── 2.7 TFT 레이아웃 (%dx%d) ──\n", L::kW, L::kH);

    char buf[64];

    // 극단값에서도 고정폭이 유지되는지 — 폭이 흔들리면 이전 글자가 화면에 남는다
    struct { float sog; const char* why; } sogCases[] = {
        {0.0f, "정지"}, {5.53f, "일반"}, {9.99f, "한 자리 최대"},
        {10.0f, "두 자리 시작"}, {99.99f, "두 자리 최대"},
    };
    size_t sogLen = 0;
    for (auto& c : sogCases) {
        L::formatSog(buf, sizeof(buf), c.sog);
        std::printf("   SOG %6.2f → \"%s\" (%zu글자, %s)\n",
                    c.sog, buf, strlen(buf), c.why);
        if (sogLen == 0) sogLen = strlen(buf);
        if (strlen(buf) != sogLen) {
            check(false, "SOG 문자열 폭이 값에 따라 달라짐");
            sogLen = strlen(buf);
        }
    }
    check(sogLen == (size_t)L::kSogChars, "SOG 실제 길이 == kSogChars");
    check(L::kSogX + L::textW((int16_t)sogLen, L::kSogSize) <= L::kW,
          "SOG 가 화면 폭 안");

    // COG — 음수/360 넘김에서도 3글자
    struct { float cog; const char* expect; } cogCases[] = {
        {0.0f, "000"}, {45.0f, "045"}, {315.0f, "315"},
        {359.9f, "000"}, {-1.0f, "359"}, {360.0f, "000"},
    };
    for (auto& c : cogCases) {
        L::formatCog(buf, sizeof(buf), c.cog);
        char msg[80];
        std::snprintf(msg, sizeof(msg), "COG %.1f° → \"%s\" (기대 \"%s\")",
                      c.cog, buf, c.expect);
        check(strcmp(buf, c.expect) == 0 && strlen(buf) == 3, msg);
    }

    // 나침반 방위는 항상 3글자
    bool pointsOk = true;
    for (int d = 0; d < 360; d += 5) {
        if (strlen(L::compassPoint((float)d)) != 3) pointsOk = false;
    }
    check(pointsOk, "나침반 방위가 전 각도에서 3글자 고정");

    // 하단바 — 극단값에서 가장 길어질 때를 본다
    L::formatBottom(buf, sizeof(buf), -128, 100, 255, 99999);
    std::printf("   하단바 최장 → \"%s\" (%zu글자, %dpx)\n",
                buf, strlen(buf), L::textW((int16_t)strlen(buf), L::kBotSize));
    check(strlen(buf) == (size_t)L::kBotChars, "하단바 실제 길이 == kBotChars");
    check(L::kBotX + L::textW((int16_t)strlen(buf), L::kBotSize) <= L::kW,
          "하단바가 화면 폭 안");

    // 이름 — 긴 이름이 잘려서 상태 표시를 침범하지 않는지
    L::formatName(buf, sizeof(buf), "abcdefghijklmnop"); // 16자 입력
    std::printf("   이름 16자 입력 → \"%s\" (%zu글자)\n", buf, strlen(buf));
    check(strlen(buf) == (size_t)L::kNameChars, "이름이 11자로 잘림");
    check(L::kNameX + L::textW((int16_t)strlen(buf), L::kNameSize) <= L::kStatusDotX - L::kStatusDotR,
          "긴 이름이 상태 표시를 침범하지 않음");

    // 세로: 각 영역이 겹치지 않는지 (컴파일 타임에도 걸리지만 수치를 눈으로 본다)
    std::printf("   세로 배치: 상단바 0..%d / SOG %d..%d / COG %d..%d / 하단바 %d..%d\n",
                L::kTopBarH, L::kSogY, L::kSogY + L::charH(L::kSogSize),
                L::kCogY, L::kCogY + L::charH(L::kCogSize), L::kBotBarY, L::kH);
    check(L::kSogY + L::charH(L::kSogSize) <= L::kCogY, "SOG 와 COG 세로 겹침 없음");
    check(L::kCogY + L::charH(L::kCogSize) <= L::kBotBarY, "COG 가 하단바 침범 안 함");
}

// ── 3. 궤적 (노이즈 OFF) ─────────────────────────────────────────────────
static void testTrajectoryNoNoise() {
    std::printf("\n── 3. 노이즈 없이 240초 궤적 ──\n");
    std::printf("     t(s)    SOG      COG     HEEL   BATT   비고\n");

    float sogMin = 1e9f, sogMax = -1e9f;
    float heelAbsMax = 0.0f;

    for (int s = 0; s <= 240; s += 5) {
        Telemetry t = sail::sim::simulate((uint32_t)s * 1000, sail::sim::noNoise);
        const char* note = "";
        if (s % 60 == 0 && s > 0) note = "← 택 시작";
        if (s % 60 == 5) note = "← 택 완료";
        std::printf("   %5d  %6.2f  %7.1f  %+7.1f  %4d%%   %s\n",
                    s, t.sogKn, t.cogDeg, t.heelDeg,
                    (int)sail::encodeBatt(t.battPct), note);

        if (t.sogKn < sogMin) sogMin = t.sogKn;
        if (t.sogKn > sogMax) sogMax = t.sogKn;
        if (std::fabs(t.heelDeg) > heelAbsMax) heelAbsMax = std::fabs(t.heelDeg);
    }

    std::printf("  SOG 범위 %.2f … %.2f kn / |HEEL| 최대 %.1f°\n", sogMin, sogMax, heelAbsMax);
    check(sogMin >= 3.9f && sogMax <= 7.1f, "SOG 가 5.5 ± 1.5 kn 범위 (노이즈 OFF)");
    check(heelAbsMax <= 18.1f, "|HEEL| 최대 18° 이내 (노이즈 OFF)");

    // 택 경계 전후
    Telemetry a = sail::sim::simulate(59'500, sail::sim::noNoise);  // 택 직전
    Telemetry b = sail::sim::simulate(65'000, sail::sim::noNoise);  // 택 완료 직후
    check(std::fabs(a.cogDeg - 45.0f) < 0.5f, "t=59.5s COG ≈ 45° (우현 택)");
    check(std::fabs(b.cogDeg - 315.0f) < 0.5f, "t=65s   COG ≈ 315° (좌현 택)");
    check(a.heelDeg > 0.0f && b.heelDeg < 0.0f, "택 전환으로 HEEL 부호 반전");

    Telemetry c = sail::sim::simulate(125'000, sail::sim::noNoise);
    check(std::fabs(c.cogDeg - 45.0f) < 0.5f, "t=125s  COG ≈ 45° (다시 우현 택)");
    check(c.heelDeg > 0.0f, "t=125s  HEEL 다시 양수");
}

// ── 4. COG 최단 회전 경로 ────────────────────────────────────────────────
static void testTackPath() {
    std::printf("\n── 4. 택 전환 시 COG 경로 (t=60.0 → 65.0s, 0.5s 간격) ──\n");

    float prev = -1.0f;
    bool  passedNorth = false;
    bool  monotonic   = true;

    for (int ms = 60'000; ms <= 65'000; ms += 500) {
        Telemetry t = sail::sim::simulate((uint32_t)ms, sail::sim::noNoise);
        std::printf("   t=%6.1fs  COG %6.1f°\n", ms / 1000.0f, t.cogDeg);

        if (prev >= 0.0f) {
            // 45 → 0/360 → 315 로 감소해야 한다 (wrap 지점 1회 허용)
            float delta = t.cogDeg - prev;
            if (delta > 180.0f) {           // 0° → 359° wrap
                passedNorth = true;
                delta -= 360.0f;
            }
            if (delta > 0.01f) monotonic = false;
        }
        prev = t.cogDeg;
    }

    check(passedNorth, "COG 가 북쪽(0°/360°)을 경유 — 최단 90° 회전");
    check(monotonic, "COG 가 단조 감소 (되돌아가지 않음)");
}

// ── 5. 노이즈 ON 범위 + 배터리 ───────────────────────────────────────────
static void testWithNoise() {
    std::printf("\n── 5. 노이즈 ON, 10분(4Hz 24000샘플) 범위 검사 ──\n");

    float sogMin = 1e9f, sogMax = -1e9f;
    float heelMin = 1e9f, heelMax = -1e9f;
    int   cogOutOfRange = 0;

    for (uint32_t ms = 0; ms <= 600'000; ms += 250) {
        Telemetry t = sail::sim::simulate(ms, seededRand01);

        if (t.sogKn < sogMin) sogMin = t.sogKn;
        if (t.sogKn > sogMax) sogMax = t.sogKn;
        if (t.heelDeg < heelMin) heelMin = t.heelDeg;
        if (t.heelDeg > heelMax) heelMax = t.heelDeg;

        uint16_t cogRaw = sail::encodeCog(t.cogDeg);
        if (cogRaw > 3599) cogOutOfRange++;

        // 인코딩 왕복 확인 (u16/i8 오버플로 없음)
        uint8_t buf[sail::kTelemetryLen];
        sail::encodeTelemetryPacket(t, buf);
        int8_t heelRaw = (int8_t)buf[10];
        if (std::fabs((float)heelRaw - t.heelDeg) > 1.0f) {
            std::printf("  !! heel 인코딩 손실: %.2f → %d (t=%ums)\n", t.heelDeg, heelRaw, ms);
            g_failures++;
            break;
        }
    }

    std::printf("  SOG  %.2f … %.2f kn\n", sogMin, sogMax);
    std::printf("  HEEL %+.1f … %+.1f°\n", heelMin, heelMax);
    check(sogMin >= 3.7f && sogMax <= 7.3f, "SOG 가 5.5 ±1.5 ±0.2 kn 범위 안");
    check(heelMin >= -18.6f && heelMax <= 18.6f, "HEEL 이 ±(18 + 0.5)° 범위 안");
    check(cogOutOfRange == 0, "COG raw 가 항상 0…3599");

    // 배터리
    struct { uint32_t ms; int pct; } battCases[] = {
        {0, 100}, {300'000, 100}, {599'000, 100}, {600'000, 99},
        {1'199'000, 99}, {1'200'000, 98}, {3'600'000, 94},
    };
    for (auto& c : battCases) {
        Telemetry t = sail::sim::simulate(c.ms, sail::sim::noNoise);
        int got = (int)sail::encodeBatt(t.battPct);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "t=%6.2f분 → batt %d%% (기대 %d%%)",
                      c.ms / 60000.0f, got, c.pct);
        check(got == c.pct, msg);
    }
}

// ── 6. 교차검증용 골든 벡터 출력 ─────────────────────────────────────────
// 여기서 뽑은 바이트열을 앱의 Swift 디코더가 그대로 해석하는지 확인한다.
// (../../tools/verify.sh 가 이 파일을 Swift 쪽 검증기에 물려준다)
static void emitVectors(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        std::printf("\n  !! 벡터 파일을 열 수 없음: %s\n", path);
        g_failures++;
        return;
    }

    std::fprintf(f, "# kind\thex\tsog_x100\tcog_x10\theel\tbatt\tseq\tuptime_ms\tmodule_id\n");

    // 실제로 쓸 법한 이름에서 나온 module_id 로 벡터를 만든다.
    const uint8_t moduleID = sail::moduleIDFromName("SAIL-hojun");

    int count = 0;
    g_lcg = 999; // 고정 시드
    for (uint32_t ms = 0; ms <= 300'000; ms += 3137) { // 소수 간격으로 택 구간까지 골고루
        Telemetry t = sail::sim::simulate(ms, seededRand01);
        t.moduleID  = moduleID;

        const uint16_t sogRaw  = sail::encodeSog(t.sogKn);
        const uint16_t cogRaw  = sail::encodeCog(t.cogDeg);
        const int      heelRaw = sail::encodeHeel(t.heelDeg);
        const int      battRaw = sail::encodeBatt(t.battPct);
        const uint8_t  seq     = (uint8_t)(count & 0xFF);

        uint8_t gatt[sail::kTelemetryLen];
        sail::encodeTelemetryPacket(t, gatt);
        std::fprintf(f, "gatt\t");
        for (size_t i = 0; i < sizeof(gatt); i++) std::fprintf(f, "%02X", gatt[i]);
        std::fprintf(f, "\t%u\t%u\t%d\t%d\t-1\t%u\t%u\n",
                     sogRaw, cogRaw, heelRaw, battRaw, ms, moduleID);

        uint8_t mfg[2 + sail::kMfgLen];
        sail::encodeManufacturerData(t, seq, mfg);
        std::fprintf(f, "mfg\t");
        for (size_t i = 0; i < sizeof(mfg); i++) std::fprintf(f, "%02X", mfg[i]);
        std::fprintf(f, "\t%u\t%u\t%d\t%d\t%u\t-1\t%u\n",
                     sogRaw, cogRaw, heelRaw, battRaw, seq, moduleID);

        count++;
    }
    std::fclose(f);
    std::printf("\n── 6. 교차검증 벡터 ──\n");
    std::printf("  %d개 시점 × 2종 = %d줄 → %s\n", count, count * 2, path);
}

int main(int argc, char** argv) {
    std::printf("════════════════════════════════════════════════════════\n");
    std::printf("  Sailing Monitor 프로토콜 / 시뮬레이터 호스트 검증\n");
    std::printf("════════════════════════════════════════════════════════\n");

    testTelemetryEncoding();
    testManufacturerEncoding();
    testModuleIdentity();
    testDisplayLayout();
    testTrajectoryNoNoise();
    testTackPath();
    testWithNoise();

    if (argc > 1) emitVectors(argv[1]);

    std::printf("\n════════════════════════════════════════════════════════\n");
    if (g_failures == 0) {
        std::printf("  ✅ 전부 통과\n");
    } else {
        std::printf("  ❌ 실패 %d 건\n", g_failures);
    }
    std::printf("════════════════════════════════════════════════════════\n");
    return g_failures == 0 ? 0 : 1;
}
