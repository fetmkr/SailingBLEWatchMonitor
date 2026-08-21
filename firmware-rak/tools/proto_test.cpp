// ─────────────────────────────────────────────────────────────────────────
//  RAK3112 프로토콜 호스트 검증 + 교차검증용 골든 벡터 생성
//
//  보드 없이 맥에서 바로 돌린다. 여기서 뽑은 바이트열을 앱의 Swift 디코더가
//  그대로 해석하는지 `tools/verify.sh` 가 이어서 확인한다.
//
//  ★ 왜 firmware/ 것을 안 쓰고 새로 뒀나
//    옛 Feather 폴더의 protocol.h 와 여기 것이 이미 갈라져 있다. 저쪽에는
//    "값 없음 표식"(sog 0xFFFF 등)이 아예 없다. 그런데 교차 검증이 저쪽을
//    쓰고 있어서, **실제로 쓰는 보드가 아닌 것과 앱을 맞춰보고 있었다.**
//    지금 쓰는 보드는 RAK 이고 앱도 그 규약을 따르므로 이쪽으로 옮긴다.
//
//    시뮬레이터 궤적과 TFT 화면 배치 검사는 가져오지 않았다. 둘 다 없어진
//    것들이다 (시뮬레이터는 걷어냈고, TFT 는 옛 보드 것이다).
//
//  빌드:  c++ -std=c++17 -I../include -o proto_test proto_test.cpp
// ─────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board_rak.h"
#include "protocol.h"

using sail::Telemetry;
using sail::TelemetryExtra;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_failures++;
}

static uint16_t u16At(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static int16_t i16At(const uint8_t* p) { return (int16_t)u16At(p); }
static uint32_t u32At(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ── 1. GATT 12바이트 ─────────────────────────────────────────────────────
static void testTelemetryEncoding() {
    std::printf("\n── 1. GATT 패킷 ──\n");

    Telemetry t;
    t.moduleID = 42;
    t.uptimeMs = 1234567;
    t.sogKn    = 6.38f;
    t.cogDeg   = 214.7f;
    t.heelDeg  = -12.0f;
    t.battPct  = 87.4f;

    uint8_t p[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(t, p);

    check(p[0] == sail::kVersion,        "버전 바이트");
    check(p[1] == 42,                    "module_id");
    check(u32At(&p[2]) == 1234567u,      "uptime (u32 LE)");
    check(u16At(&p[6]) == 638,           "sog = 6.38 kn → 638");
    check(u16At(&p[8]) == 2147,          "cog = 214.7° → 2147");
    check((int8_t)p[10] == -12,          "heel = -12°");
    check(p[11] == 87,                   "batt = 87%");
}

// ── 2. 값 없음 표식 ──────────────────────────────────────────────────────
//
// 여기가 이 파일에서 제일 중요하다. GPS 가 위성을 못 잡았을 때 0 을 보내면
// "정박 중" 과 구별되지 않고, 지어낸 값을 채우면 실측과 구별되지 않는다.
// 물리적으로 나올 수 없는 값을 예약해서 "모른다" 를 그대로 전한다.
static void testInvalidMarkers() {
    std::printf("\n── 2. 값 없음 표식 ──\n");

    Telemetry t;
    t.moduleID  = 7;
    t.sogKn     = 6.38f;   // 값은 들어 있지만
    t.cogDeg    = 214.7f;
    t.heelDeg   = -12.0f;
    t.battPct   = 50.0f;
    t.sogValid  = false;   // 유효하지 않다고 표시하면
    t.cogValid  = false;
    t.heelValid = false;

    uint8_t p[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(t, p);

    check(u16At(&p[6]) == sail::kSogInvalid,   "sog 무효 → 0xFFFF (숫자가 안 나간다)");
    check(u16At(&p[8]) == sail::kCogInvalid,   "cog 무효 → 0xFFFF");
    check((int8_t)p[10] == sail::kHeelInvalid, "heel 무효 → -128");
    check(p[11] == 50,                         "배터리는 GPS 와 무관하게 그대로");

    // 표식으로 쓰는 값이 정상 범위와 겹치면 안 된다.
    check(sail::encodeSog(655.0f) != sail::kSogInvalid,
          "655 kn 도 표식과 안 겹친다 (배가 낼 수 없는 속도)");
    check(sail::encodeCog(359.9f) != sail::kCogInvalid, "359.9° 는 표식과 안 겹친다");
    check(sail::encodeHeel(-90.0f) != sail::kHeelInvalid, "-90° 는 표식과 안 겹친다");
}

// ── 3. 광고 페이로드 ─────────────────────────────────────────────────────
static void testManufacturerEncoding() {
    std::printf("\n── 3. Manufacturer data ──\n");

    Telemetry t;
    t.moduleID = 3;
    t.sogKn    = 12.34f;
    t.cogDeg   = 45.6f;
    t.heelDeg  = 5.0f;
    t.battPct  = 61.0f;

    uint8_t p[2 + sail::kMfgLen];
    sail::encodeManufacturerData(t, 200, p);

    check(u16At(&p[0]) == sail::kCompanyID, "Company ID (LE)");
    check(p[2] == sail::kVersion,           "버전");
    check(p[3] == 3,                        "module_id");
    check(u16At(&p[4]) == 1234,             "sog = 12.34 kn");
    check(u16At(&p[6]) == 456,              "cog = 45.6°");
    check((int8_t)p[8] == 5,                "heel = +5°");
    check(p[9] == 61,                       "batt = 61%");
    check(p[10] == 200,                     "시퀀스는 맨 뒤");
}

// ── 4. 확장 37바이트 ─────────────────────────────────────────────────────
//
// 앞 12바이트가 한 글자도 안 달라야 한다. 옛 수신 측이 앞부분만 읽고 그대로
// 돌 수 있는 근거가 이것이다 (PROTOCOL.md §7).
static void testExtendedEncoding() {
    std::printf("\n── 4. 확장 페이로드 37바이트 ──\n");

    Telemetry t;
    t.moduleID = 9;
    t.uptimeMs = 555;
    t.sogKn    = 3.21f;
    t.cogDeg   = 100.0f;
    t.heelDeg  = -3.0f;
    t.battPct  = 42.0f;

    TelemetryExtra e;
    e.gpsFix     = true;
    e.imuOk      = true;
    e.magOk      = true;
    e.satellites = 11;
    e.hdop       = 1.4f;
    e.headingDeg = 344.5f;
    e.pitchDeg   = -2.5f;
    e.accX = 0.123f;  e.accY = -0.456f; e.accZ = 0.987f;
    e.gyrX = 12.3f;   e.gyrY = -45.6f;  e.gyrZ = 0.0f;
    e.magX = -31.8f;  e.magY = 6.1f;    e.magZ = -23.8f;

    uint8_t base[sail::kTelemetryLen];
    sail::encodeTelemetryPacket(t, base);

    uint8_t p[sail::kTelemetryExtLen];
    sail::encodeTelemetryExt(t, e, p);

    check(std::memcmp(base, p, sail::kTelemetryLen) == 0,
          "앞 12바이트가 기존 패킷과 완전히 같다");
    check((p[12] & 0x01) != 0,        "flags bit0 GPS fix");
    check((p[12] & 0x02) != 0,        "flags bit1 IMU");
    check((p[12] & 0x04) != 0,        "flags bit2 자력계");
    check((p[12] & 0x08) == 0,        "flags bit3 예약 — 항상 0");
    check(p[13] == 11,                "위성 수");
    check(p[14] == 14,                "HDOP 1.4 → 14");
    check(u16At(&p[15]) == 3445,      "방위 344.5° → 3445");
    check(i16At(&p[17]) == -25,       "피치 -2.5° → -25");
    check(i16At(&p[19]) == 123,       "가속 X 0.123 g → 123");
    check(i16At(&p[25]) == 123,       "자이로 X 12.3 °/s → 123");
    check(i16At(&p[31]) == -318,      "자력 X -31.8 µT → -318");

    // 자력계가 없으면 방위는 무효 표식이어야 한다.
    e.magOk = false;
    e.headingDeg = -1.0f;
    sail::encodeTelemetryExt(t, e, p);
    check(u16At(&p[15]) == 0xFFFF,    "자력계 없음 → 방위 0xFFFF");
    check(p[14] == 14,                "HDOP 는 그대로");

    // HDOP 를 모르면 255.
    e.hdop = -1.0f;
    sail::encodeTelemetryExt(t, e, p);
    check(p[14] == 255,               "HDOP 모름 → 255");
}

// ── 5. 이름 → module_id ──────────────────────────────────────────────────
static void testModuleIdentity() {
    std::printf("\n── 5. 모듈 신원 ──\n");

    const uint8_t a = sail::moduleIDFromName("SAIL-hojun");
    const uint8_t b = sail::moduleIDFromName("SAIL-hojun");
    const uint8_t c = sail::moduleIDFromName("SAIL-random()");

    check(a == b,      "같은 이름은 항상 같은 id");
    check(a != c,      "다른 이름은 다른 id");
    check(a != 0,      "id 0 은 안 나온다 (0 은 미지정 뜻)");
    check(c != 0,      "요트 이름 random() 도 id 0 이 아니다");
}

// ── 6. 배터리 곡선 ───────────────────────────────────────────────────────
//
// 곡선 자체가 헤더에 표로 들어 있다. 값을 읽는 코드는 main.cpp 에 있어서
// 여기서 못 부르지만, 표가 뒤죽박죽이면 무슨 코드를 써도 틀린다.
// 전압이 내려가면 퍼센트도 반드시 같이 내려가야 한다.
static void testBatteryCurve() {
    std::printf("\n── 6. 배터리 곡선 ──\n");

    bool voltsDescend = true, pctDescend = true;
    for (int i = 1; i < rak::kBattCurveLen; i++) {
        if (rak::kBattCurve[i].volts   >= rak::kBattCurve[i - 1].volts)   voltsDescend = false;
        if (rak::kBattCurve[i].percent >= rak::kBattCurve[i - 1].percent) pctDescend = false;
    }
    check(voltsDescend, "전압이 계속 내려간다 (순서가 안 뒤집혔다)");
    check(pctDescend,   "퍼센트도 계속 내려간다");
    check(rak::kBattCurve[0].percent == 100.0f, "만충이 100%");
    check(rak::kBattCurve[rak::kBattCurveLen - 1].percent == 0.0f, "바닥이 0%");
}

// ── 7. 교차검증용 골든 벡터 ──────────────────────────────────────────────
//
// 시뮬레이터가 없어졌으므로 값을 규칙적으로 훑는다. 난수 대신 정해진 수열을
// 쓰는 이유는 언제 돌려도 같은 결과가 나와야 하기 때문이다.
//
// 무효 표식 줄도 섞는다. 그 줄은 기대값 칸에 -1 을 적어 두고, Swift 쪽이
// "값이 없다" 로 읽어야 통과한다.
static void emitVectors(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        std::printf("\n  !! 벡터 파일을 열 수 없음: %s\n", path);
        g_failures++;
        return;
    }

    std::fprintf(f, "# kind\thex\tsog_x100\tcog_x10\theel\tbatt\tseq\tuptime_ms\tmodule_id\n");
    // ★ 숫자를 "값 없음" 표시로 쓰면 안 된다. 힐 -1도 는 실제로 나오는 값이라
    //   -1 을 표시로 쓰면 겹친다 (처음에 그렇게 했다가 검증기에 걸렸다).
    std::fprintf(f, "# sog/cog/heel 기대값이 nil 이면 \"값이 없다\" 로 읽혀야 한다는 뜻\n");

    const uint8_t moduleID = sail::moduleIDFromName("SAIL-random()");

    int count = 0;
    for (uint32_t ms = 0; ms <= 300000u; ms += 3137u) {
        Telemetry t;
        t.moduleID = moduleID;
        t.uptimeMs = ms;

        // 0~30 kn 을 훑는다. 실제로 배에서 보는 범위다.
        t.sogKn   = (float)((ms / 3137u) % 3001) / 100.0f;
        t.cogDeg  = (float)((ms / 3137u) * 7 % 3600) / 10.0f;
        t.heelDeg = (float)((int)((ms / 3137u) % 121) - 60);
        t.battPct = (float)(100 - (int)((ms / 3137u) % 101));

        // 열 줄에 한 번은 "GPS 를 놓친" 줄을 넣는다.
        const bool lost = (count % 10) == 9;
        t.sogValid = t.cogValid = !lost;
        t.heelValid = true;

        char sogCol[16], cogCol[16], heelCol[16];
        if (lost) {
            std::snprintf(sogCol, sizeof(sogCol), "nil");
            std::snprintf(cogCol, sizeof(cogCol), "nil");
        } else {
            std::snprintf(sogCol, sizeof(sogCol), "%u", sail::encodeSog(t.sogKn));
            std::snprintf(cogCol, sizeof(cogCol), "%u", sail::encodeCog(t.cogDeg));
        }
        std::snprintf(heelCol, sizeof(heelCol), "%d", (int)sail::encodeHeel(t.heelDeg));
        const long battCol = (long)sail::encodeBatt(t.battPct);
        const uint8_t seq  = (uint8_t)(count & 0xFF);

        uint8_t gatt[sail::kTelemetryLen];
        sail::encodeTelemetryPacket(t, gatt);
        std::fprintf(f, "gatt\t");
        for (size_t i = 0; i < sizeof(gatt); i++) std::fprintf(f, "%02X", gatt[i]);
        std::fprintf(f, "\t%s\t%s\t%s\t%ld\t-1\t%u\t%u\n",
                     sogCol, cogCol, heelCol, battCol, ms, moduleID);

        uint8_t mfg[2 + sail::kMfgLen];
        sail::encodeManufacturerData(t, seq, mfg);
        std::fprintf(f, "mfg\t");
        for (size_t i = 0; i < sizeof(mfg); i++) std::fprintf(f, "%02X", mfg[i]);
        std::fprintf(f, "\t%s\t%s\t%s\t%ld\t%u\t-1\t%u\n",
                     sogCol, cogCol, heelCol, battCol, seq, moduleID);

        count++;
    }
    std::fclose(f);
    std::printf("\n── 7. 교차검증 벡터 ──\n");
    std::printf("  %d개 시점 × 2종 = %d줄 → %s\n", count, count * 2, path);
    std::printf("  그중 %d개 시점은 GPS 를 놓친 줄이다\n", count / 10);
}

int main(int argc, char** argv) {
    std::printf("════════════════════════════════════════════════════════\n");
    std::printf("  RAK3112 프로토콜 호스트 검증\n");
    std::printf("════════════════════════════════════════════════════════\n");

    testTelemetryEncoding();
    testInvalidMarkers();
    testManufacturerEncoding();
    testExtendedEncoding();
    testModuleIdentity();
    testBatteryCurve();

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
