// ─────────────────────────────────────────────────────────────────────────
//  펌웨어 순수 로직 회귀 시험 — 보드 없이 맥에서 돈다
//
//  빌드:  c++ -std=c++17 -I../include -o fw_logic_test fw_logic_test.cpp && ./fw_logic_test
//
//  여기서 보는 것 (2026-09-14 검토에서 재현된 것들)
//    1  SD 부분 쓰기 뒤 재시도 — 이미 쓴 앞부분이 겹쳐 쓰이지 않는가
//    2  종료 쓰기가 덜 들어가면 성공으로 안 치는가 · 상태 전환
//    3  CASIC 체크섬·길이·NaN·ACK 짝
//    4  자이로 0점 단위 (라이브러리는 ±250 원시 단위로 받는다)
//    5  설정 저장 — begin 없이 put 하지 않는가, 실패를 알리는가
//    6  HTTP Range 해석
//    7  방위 각도 접기·원형 폭·명령 숫자·설정 검사·오일러 방위 변화율
//    8  자력 표본 검사
//    9  자력 보정 수락 조건
// ─────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "casic.h"
#include "hlog_write.h"
#include "http_range.h"
#include "imu_math.h"
#include "prefs_util.h"
#include "heading_math.h"
#include "mag_sample.h"
#include "magcal.h"
#include "sog_policy.h"
#include "heading_tilt.h"
#include "lora_packet.h"

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

// ── 1·2 SD 쓰기 ──────────────────────────────────────────────────────────

// 가짜 카드. script 에 적힌 대로 한 번씩 "몇 바이트 썼다" 를 돌려준다.
// 다 쓰면 그 뒤로는 요청한 만큼 다 쓴다.
struct FakeCard {
    std::string file;
    std::vector<long> script;   // -1 이면 요청한 만큼
    size_t call = 0;
    size_t write(const uint8_t* p, size_t n) {
        long want = (call < script.size()) ? script[call] : -1;
        ++call;
        size_t got = (want < 0) ? n : (size_t)want;
        if (got > n) got = n;
        file.append((const char*)p, got);
        return got;
    }
};

// 옛 코드의 순서를 흉내 낸다: 쓴 만큼 꼬리를 안 넘기고, 닫을 때 버퍼 전체를 다시 쓴다.
static std::string legacyWriterSimulation() {
    const std::string buf = "ABCDEFGHIJKLMNOP";
    FakeCard card{ "", {5, 0, 0, 0, -1} };
    size_t tail = 0;
    size_t done = card.write((const uint8_t*)buf.data() + tail, buf.size() - tail);
    for (int i = 0; i < 3 && done < buf.size(); ++i)
        done += card.write((const uint8_t*)buf.data() + tail + done, buf.size() - tail - done);
    // 실패로 멈춤 → 꼬리(tail) 는 그대로 0 → 종료가 처음부터 다시 씀
    card.write((const uint8_t*)buf.data() + tail, buf.size() - tail);
    return card.file;
}

static void testSdWrites() {
    std::printf("\n[1] SD 부분 쓰기 — 겹쳐 쓰지 않기\n");
    check(legacyWriterSimulation() == "ABCDEABCDEFGHIJKLMNOP",
          "옛 순서를 흉내 내면 보고서의 중복이 재현된다 (ABCDE + 전체)");

    const std::string buf = "ABCDEFGHIJKLMNOP";
    {   // 5바이트 쓰고, 첫 시도 + 다시 3번이 모두 0 → 멈춤. 쓴 5바이트만 소비됐어야 한다
        FakeCard card{ "", {5, 0, 0, 0, 0} };
        size_t tail = 0; int waits = 0; uint8_t tries = 0;
        const size_t got = hlog::writeAll(
            (const uint8_t*)buf.data(), buf.size(),
            [&](const uint8_t* p, size_t n) { return card.write(p, n); },
            [&](size_t k) { tail += k; },
            [&](uint8_t) { ++waits; }, 3, &tries);
        check(got == 5, "writeAll 은 실제로 쓴 5바이트를 돌려준다");
        check(tail == 5, "꼬리는 쓴 5바이트만큼만 넘어간다");
        check(tries == 3 && waits == 3, "한 바이트도 못 쓴 시도를 3번까지 다시 한다");
        check(got != buf.size(), "기록 중 쓰기 실패 — 덜 들어갔음을 안다 (일꾼은 닫는 중으로 넘긴다)");

        // 그 뒤 닫기(남은 범위만) 가 성공하면 파일은 겹침 없이 한 벌이다
        card.script.clear();
        const size_t got2 = hlog::writeAll(
            (const uint8_t*)buf.data() + tail, buf.size() - tail,
            [&](const uint8_t* p, size_t n) { return card.write(p, n); },
            [&](size_t k) { tail += k; }, [&](uint8_t) {}, 3);
        check(got2 == 11 && tail == 16, "남은 11바이트만 다시 쓴다");
        check(card.file == "ABCDEFGHIJKLMNOP", "결과 파일 ABCDEFGHIJKLMNOP (중복 없음)");
    }
    {   // 중간에 한 번 실패했다가 살아나는 경우
        FakeCard card{ "", {5, 0, 4, -1} };
        size_t tail = 0;
        const size_t got = hlog::writeAll(
            (const uint8_t*)buf.data(), buf.size(),
            [&](const uint8_t* p, size_t n) { return card.write(p, n); },
            [&](size_t k) { tail += k; }, [&](uint8_t) {}, 3);
        check(got == 16 && card.file == buf, "실패 한 번 뒤 이어서 끝까지 — 겹침 없음");
    }

    std::printf("\n[2] 종료 쓰기와 상태\n");
    {   // 16바이트 중 5바이트만 들어간 종료 → 성공으로 치면 안 된다
        FakeCard card{ "", {5, 0, 0, 0, 0} };
        size_t tail = 0;
        const size_t got = hlog::writeAll(
            (const uint8_t*)buf.data(), buf.size(),
            [&](const uint8_t* p, size_t n) { return card.write(p, n); },
            [&](size_t k) { tail += k; }, [&](uint8_t) {}, 3);
        const size_t lost = buf.size() - tail;
        check(lost == 11, "못 쓴 11바이트가 남는다 (보고할 수 있다)");
        check(!(got == buf.size() && lost == 0), "덜 들어간 종료는 complete=false — 머리글 closed=0");
    }
    {   // 상태 전이 — 멈춤 요청과 쓰기 포기가 같은 전이, 완료는 일꾼만 (검토 2번의 겹침 순서)
        hlog::RecState st = hlog::RecState::Recording;
        check(hlog::toDraining(st) && st == hlog::RecState::Draining, "기록 중 → 닫는 중 (요청이든 쓰기 포기든)");
        check(!hlog::toDraining(st), "이미 닫는 중이면 두 번째 요청은 아무것도 안 바꾼다");
        check(hlog::toClosed(st) && st == hlog::RecState::Closed, "일꾼 완료 → 닫힘");
        check(!hlog::toDraining(st) && st == hlog::RecState::Closed,
              "완료 뒤 늦게 온 종료 요청이 닫힌 세션을 닫는 중으로 되돌리지 않는다");
        hlog::RecState r2 = hlog::RecState::Recording;
        check(!hlog::toClosed(r2) && r2 == hlog::RecState::Recording, "닫는 중을 거치지 않고 닫힘으로 가지 않는다");
    }
    check(hlog::canStart(hlog::RecState::Closed) && !hlog::canStart(hlog::RecState::Recording) &&
          !hlog::canStart(hlog::RecState::Draining), "닫혀 있을 때만 시작한다");
    check(hlog::sdBusy(hlog::RecState::Recording) && hlog::sdBusy(hlog::RecState::Draining) &&
          !hlog::sdBusy(hlog::RecState::Closed), "SD 를 쥐고 있는 상태는 Recording·Draining 뿐");
}

static void testLoraPacket() {
    std::printf("\n[2a] LoRa 함대 패킷과 차례\n");
    lora::Live v;
    v.boat = 7;
    v.lat = 375512345;
    v.lon = 1269887654;
    v.gpsFix = true;
    v.recording = true;
    v.timeValid = true;
    v.boatChanged = true;
    v.sogValid = true; v.sogKn = 1.23f;
    v.cogValid = true; v.cogDeg = 359.96f;
    v.attitudeValid = true; v.heelDeg = -12.4f; v.pitchDeg = 8.6f;
    v.battPct = 80;
    v.heard = 0x80000005u;
    v.tie = 0xA3F2;
    uint8_t b[lora::kPayloadLen];
    lora::encode(v, b);
    lora::Decoded d;
    check(lora::decode(b, &d), "22바이트 정상 패킷을 다시 읽는다");
    check(d.wireVersion == lora::kWireVersion && d.boat == 7 && d.lat == v.lat && d.lon == v.lon,
          "무선 버전·배 번호와 위·경도는 보존");
    check(d.sog == 123 && d.cog == 0, "SOG 1.23 kn, COG 359.96°는 0.01 kn·0.1° 단위와 360° wrap");
    check(d.heel == -12 && d.pitch == 9, "힐·피치는 1도 단위");
    check((d.flags & 0x0F) == 0x0F && (d.flags >> 4) == 12, "fix·REC·PPS·번호변경과 배터리 4비트");
    check(d.heard == 0x80000005u && d.tie == 0xA3F2, "들은 배 비트와 MAC tie 보존");

    lora::Live presence = v;
    lora::makePresence(&presence);
    lora::encode(presence, b);
    check(lora::decode(b, &d) && !(d.flags & lora::kFlagGpsFix) && !(d.flags & lora::kFlagTime) &&
          d.lat == lora::kLatLonInvalid && d.lon == lora::kLatLonInvalid &&
          d.sog == lora::kSogInvalid && d.cog == lora::kCogInvalid &&
          d.heel == -12 && d.pitch == 9 && (d.flags & lora::kFlagRecording),
          "PPS 전 확인 신호는 항해값만 비우고 자세·REC·배터리는 살린다");

    v.gpsFix = false; v.sogValid = false; v.cogValid = false; v.attitudeValid = false;
    lora::encode(v, b);
    check(lora::decode(b, &d) && d.lat == lora::kLatLonInvalid && d.lon == lora::kLatLonInvalid &&
          d.sog == lora::kSogInvalid && d.cog == lora::kCogInvalid &&
          d.heel == lora::kAttitudeInvalid && d.pitch == lora::kAttitudeInvalid,
          "fix·센서값이 없으면 0 대신 값 없음 표식");
    b[0] = 7; // 버전 필드를 넣기 전의 legacy 패킷
    check(lora::decode(b, &d) && d.wireVersion == 0 && d.boat == 7,
          "교체 기간에는 무버전 22바이트도 읽는다");
    b[0] = 0;
    check(!lora::decode(b, &d), "배 번호 0인 수신 패킷은 거절");
    b[0] = (uint8_t)(2u << lora::kVersionShift) | 7u;
    check(!lora::decode(b, &d), "모르는 미래 무선 버전은 조용히 거절");
    check(lora::slotOffsetUs(1) == 0 && lora::slotOffsetUs(7) == 187500 &&
          lora::slotOffsetUs(32) == 968750, "1초를 32개 31.25ms 차례로 나눈다");
    check(!lora::ppsWithinHoldover(false, 1000, 500, lora::kPpsHoldoverMs),
          "한 번도 못 본 PPS는 무효");
    check(lora::ppsWithinHoldover(true, 1200000000u, 0, lora::kPpsHoldoverMs),
          "PPS는 20분 경계까지 유효");
    check(!lora::ppsWithinHoldover(true, 1200000001u, 0, lora::kPpsHoldoverMs),
          "PPS는 20분을 1us 넘으면 만료");
    check(lora::ppsWithinHoldover(true, 0x00000100u, 0xFFFFFF00u, lora::kPpsHoldoverMs),
          "micros 한 바퀴를 지나도 PPS 나이를 맞게 계산");
}

// ── 2b 기록 제어 — 원하는 상태와 실제 상태 (rec_control.h) ────────────────
//
// NEXT.md 1번의 여섯 시나리오 + 옛 버그 재현 (세션 58 이어받기, rec off 뒤 저절로 재시작, userStopped 옮겨붙음)
static void testRecControl() {
    using namespace recctl;
    std::printf("\n[2b] 기록 제어 — 원하는 상태와 실제 상태\n");

    {   // 첫 오류는 한 번 채우면 안 바뀐다
        SessionResult r;
        check(setFirst(r, kErrCardGone) && r.firstKind == kErrCardGone, "첫 오류 채움 — 카드 빠짐");
        check(!setFirst(r, kErrDrainShort) && r.firstKind == kErrCardGone,
              "닫다가 못 쓴 것이 뒤따라와도 첫 오류(카드 빠짐)는 그대로");
        check(r.consumed, "새 결과는 일꾼이 consumed=false 로 내놓기 전까지 꺼낼 게 없다");
    }

    // ① 실패 도중 종료 — 쓰기 실패 결과가 오기 전에 사람이 멈췄다 → 다시 걸지 않는다
    check(afterSession(false, kErrWrite, 0, 3) == Next::Nothing,
          "① 쓰기 실패가 늦게 와도 사람이 멈췄으면(want=0) 다시 안 건다 (옛 userStopped 버그)");
    check(afterSession(true, kErrWrite, 0, 3) == Next::RestartSoon, "사람이 원하면 쓰기 실패 뒤 다시 건다");
    check(afterSession(true, kErrCardGone, 2, 3) == Next::RestartSoon, "카드 빠짐도 다시 건다 (3번째)");
    check(afterSession(true, kErrWrite, 3, 3) == Next::GiveUp, "3번 다 쓰면 포기");
    check(afterRestartFailed(1, 3) == Next::RestartSoon && afterRestartFailed(3, 3) == Next::GiveUp,
          "다시 걸기 시작 실패도 같은 한도");

    // ② 15초 넘은 뒤 늦은 완료 — 끄기는 기다리고, 넘으면 사람에게 묻고, 닫히면 잔다
    check(powerOff(Phase::Closing, 3000, 15000) == Off::Wait, "② 닫는 중 3초 — 기다린다");
    check(powerOff(Phase::Closing, 16000, 15000) == Off::AskForce, "② 16초 — 강제로 끌지 묻는다 (저절로 안 끈다)");
    check(powerOff(Phase::Idle, 40000, 15000) == Off::SleepNow, "② 늦게라도 닫히면 그때 잔다");

    // ③ 그 사이 WiFi·진단·끄기 — 닫는 중에는 카드를 쥐고 있다
    check(hlog::sdBusy(hlog::RecState::Draining) && !hlog::canStart(hlog::RecState::Draining),
          "③ 닫는 중에는 SD 를 쥐고 있고 새 시작도 안 된다");

    // ④ 종료 반복 · 닫는 중 다시 시작
    check(!showFailed(false, Phase::Closing, false, false) && !showFailed(true, Phase::Closing, false, false),
          "④ 닫는 중에는 REC FAIL 을 안 띄운다 (사람이 다시 시작을 눌렀어도)");
    check(afterSession(true, kErrNone, 0, 3) == Next::Nothing &&
          afterSession(true, kErrDrainShort, 0, 3) == Next::Nothing,
          "④ 닫는 중 다시 시작(want=1) — 앞 세션 결과로 포기하지 않는다. 루프가 '닫힌 뒤 시작' 으로 건다");

    // ⑤ IMU 끊긴 채 종료 — 중복 없이 정산
    {
        const uint32_t recStart = 10000, lost = 20000;
        const uint32_t atStop = imuGapRows(lost, recStart, 25000);
        check(atStop == 500, "⑤ 끊긴 5초 → 500줄");
        check(imuGapRows(25000, recStart, 25000) == 0, "⑤ 정산한 뒤 같은 시각으로 다시 불러도 0 (두 번 안 센다)");
        check(imuGapRows(5000, recStart, 12000) == 200, "기록 시작 전 끊긴 시간은 뺀다 (시작부터 2초)");
        check(imuGapRows(0xFFFFF000u, 0xFFFFE000u, 0x00000800u) == (0x1800u / 10), "millis 한 바퀴 넘어도 맞다");
    }

    // ⑥ 닫기 실패 뒤 끄기 — 기록만 남기고 잔다
    check(afterSession(false, kErrHeader, 0, 3) == Next::Nothing && powerOff(Phase::Idle, 0, 15000) == Off::SleepNow,
          "⑥ 머리글 실패로 닫힌 뒤 끄기 — 다시 걸지 않고 잔다");

    // REC FAIL 표시
    check(showFailed(true, Phase::Idle, false, false), "원하는데 멈춰 있다 → REC FAIL");
    check(!showFailed(false, Phase::Idle, false, false), "사람이 멈췄다 → REC FAIL 아님");
    check(showFailed(false, Phase::Idle, true, false), "포기했으면 사람이 볼 때까지 REC FAIL");
    check(!showFailed(true, Phase::Recording, false, false), "돌고 있으면 REC FAIL 아님");
    check(showFailed(false, Phase::Idle, false, true),
          "사람이 멈춘 세션이라도 마지막 저장이 실패했으면 REC FAIL (검토 3번)");

    // NVS 옮기기 · 켤 때
    {
        Persist none;
        Persist m = migrate(false, none, true, 1, 57);
        check(m.want == 1 && m.open == 57, "옛 rec_on=1 → want=1, open=마지막 세션");
        m = migrate(false, none, true, 0, 57);
        check(m.want == 0 && m.open == 0, "옛 rec_on=0 → 둘 다 0");
        Persist cur; cur.want = 0; cur.open = 12;
        m = migrate(true, cur, true, 1, 57);
        check(m.want == 0 && m.open == 12, "새 키가 있으면 옛 키를 무시한다");
    }
    {
        Persist p; p.want = 0; p.open = 57;
        check(atBoot(p, 0, 5) == Boot::UnclosedOnly,
              "사람이 멈춘 뒤 닫히기 전에 꺼졌다 → 켤 때 이어받지 않는다 (세션 58 버그)");
        p.want = 1;
        check(atBoot(p, 0, 5) == Boot::Resume, "기록 중 전원 끊김 → 이어 시작");
        check(atBoot(p, 5, 5) == Boot::TooMany, "5번 연달아 끊기면 멈춘다");
        Persist z;
        check(atBoot(z, 0, 5) == Boot::Nothing, "아무 표시 없으면 아무것도 안 한다");
    }
}

// ── 2c 화면 속도 규칙 (sog_policy.h) — 2026-09-14 동작을 그대로 옮겼나 ─────
static void testSogPolicy() {
    std::printf("\n[2c] 화면 속도 규칙 — 동작 유지\n");
    const sog::Policy p;
    check(!sog::sampleOk(true, 1.2f, p) && sog::sampleOk(true, 0.35f, p) && !sog::sampleOk(false, 0.1f, p) &&
          !sog::sampleOk(true, -1.0f, p), "표식이 방금 잰 값이고 오차 1 kn 이하일 때만 쓴다 (오차 모름도 버림)");
    check(sog::pvStale(0, 5000, p) && sog::pvStale(1000, 3100, p) && !sog::pvStale(1000, 3000, p),
          "NAV-PV 가 2초 넘게 안 오면 RMC 길로 물러선다");
    check(sog::nextSlowMode(false, 0.49f, p) && !sog::nextSlowMode(false, 0.5f, p),
          "3초 평균 0.5 kn 밑이면 느린 모드로 들어간다");
    check(sog::nextSlowMode(true, 0.8f, p) && !sog::nextSlowMode(true, 0.81f, p),
          "0.8 kn 넘어야 나온다 (0.5~0.8 은 그대로)");
    check(!sog::nextSlowMode(false, -1.0f, p), "표본이 없으면 모드를 안 바꾼다");
    check(sog::slowShownKn(0.19f, p) == 0.0f && std::fabs(sog::slowShownKn(0.25f, p) - 0.25f) < 1e-6f,
          "느린 모드에서 0.2 kn 밑은 0");
    {   // 성분 평균: 동·서로 번갈아 0.3 m/s 흔들림 → 크기 평균은 0.58 kn, 성분 평균은 0
        sog::Sample ring[8];
        for (int i = 0; i < 8; ++i) ring[i] = sog::Sample{(uint32_t)(1000 + i * 100), 0.0f, (i % 2) ? 0.3f : -0.3f};
        const float kn = sog::vectorMeanKn(ring, 8, 8, 8, 1800, 3000, 0);
        check(kn >= 0.0f && kn < 1e-4f, "멈춰서 흔들리는 속도는 성분 평균으로 0 이 된다 (크기 평균이면 0.58)");
        const float none = sog::vectorMeanKn(ring, 8, 8, 8, 1800, 3000, 5000);
        check(none < 0.0f, "notBefore 이후 표본이 없으면 -1");
    }
}

// ── 2d 배의 방위 식 — 답을 아는 자세로 (heading_tilt.h) ─────────────────
//
// 앞·오른쪽·아래(FRD) 몸체 좌표에서 자세를 만들고, 설정의 역변환으로 센서 좌표로 옮겨 넣는다.
//   롤 φ   세계→몸체  g=(0, sφ, cφ)            자기장=(H, V·sφ, V·cφ)
//   피치 θ 세계→몸체  g=(−sθ, 0, cθ)           자기장=(H·cθ − V·sθ, 0, H·sθ + V·cθ)
static void frdToSensors(const hdg::HeadingCfg& c, const float gFrd[3], const float mFrd[3],
                         float acc[3], float mag[3]) {
    const float ds = hdg::downSign(c);
    const uint8_t dAx = (uint8_t)(3 - c.axisA - c.axisB);
    float am[3], mm[3];
    // f = v[B]·sB, r = −v[A]·sA, d = v[D]·ds 의 역
    am[c.axisB] = -gFrd[0] / c.signB; am[c.axisA] = gFrd[1] / c.signA; am[dAx] = -gFrd[2] / ds;   // 가속은 −g
    mm[c.axisB] =  mFrd[0] / c.signB; mm[c.axisA] = -mFrd[1] / c.signA; mm[dAx] = mFrd[2] / ds;
    // 자력 좌표 → 가속도계 좌표 (자력 X=가속 Y, Y=가속 X, Z=−가속 Z)
    acc[0] = am[1]; acc[1] = am[0]; acc[2] = -am[2];
    for (int i = 0; i < 3; ++i) mag[i] = mm[i];
}

static void testHeadingTilt() {
    std::printf("\n[2d] 배의 방위 식 — 답을 아는 자세\n");
    const float H = 30.0f, V = 40.0f;            // 한국 비슷한 복각
    const float deg = (float)M_PI / 180.0f;
    auto near = [](float a, float b, float tol) { float d = std::fabs(a - b); d = std::fmod(d, 360.0f); return std::min(d, 360.0f - d) <= tol; };
    const hdg::HeadingCfg cfgs[] = {
        {1, 0, 1.0f, 1.0f, 0.0f, 0.0f},          // 기본 atan2(+Y, +X)
        {1, 2, 1.0f, 1.0f, 0.0f, 0.0f},          // 세워 단 배 atan2(+Y, +Z)
        {2, 0, -1.0f, 1.0f, 0.0f, 0.0f},         // 부호 뒤집힌 설정
    };
    int okLevel = 0, okRoll = 0, okPitch = 0, bigFlatErr = 0;
    for (const auto& c : cfgs) {
        for (int k = 0; k < 8; ++k) {
            const float psi = k * 45.0f * deg;
            // 수평, 방위 ψ: 몸체에서 자기장 = (H cosψ, −H sinψ, V)
            const float g0[3] = {0, 0, 1}, m0[3] = {H * std::cos(psi), -H * std::sin(psi), V};
            float acc[3], mag[3];
            frdToSensors(c, g0, m0, acc, mag);
            if (near(hdg::tiltHeadingDeg(acc, mag, c), k * 45.0f, 0.01f) &&
                near(hdg::flatHeadingDeg(mag, c), k * 45.0f, 0.01f)) ++okLevel;
        }
        // 롤 20° (북쪽 보고)
        {
            const float ph = 20.0f * deg;
            const float g[3] = {0, std::sin(ph), std::cos(ph)}, m[3] = {H, V * std::sin(ph), V * std::cos(ph)};
            float acc[3], mag[3];
            frdToSensors(c, g, m, acc, mag);
            if (near(hdg::tiltHeadingDeg(acc, mag, c), 0.0f, 0.01f)) ++okRoll;
            if (!near(hdg::flatHeadingDeg(mag, c), 0.0f, 20.0f)) ++bigFlatErr;
        }
        // 피치 15°
        {
            const float th = 15.0f * deg;
            const float g[3] = {-std::sin(th), 0, std::cos(th)};
            const float m[3] = {H * std::cos(th) - V * std::sin(th), 0, H * std::sin(th) + V * std::cos(th)};
            float acc[3], mag[3];
            frdToSensors(c, g, m, acc, mag);
            if (near(hdg::tiltHeadingDeg(acc, mag, c), 0.0f, 0.01f)) ++okPitch;
        }
    }
    check(okLevel == 24, "수평 8방위 × 설정 3 — 보정·평평 둘 다 정답 (0.01° 안)");
    check(okRoll == 3, "힐 20° — 기울기 보정은 북쪽 그대로 (0.01° 안)");
    check(bigFlatErr == 3, "힐 20° — 평평 식은 20° 넘게 틀린다 (그래서 화면 식을 바꿨다)");
    check(okPitch == 3, "피치 15° — 기울기 보정은 북쪽 그대로");
    {
        hdg::HeadingCfg c{1, 0, 1.0f, 1.0f, 350.0f, 20.0f};
        const float g0[3] = {0, 0, 1}, m0[3] = {H, 0, V};
        float acc[3], mag[3];
        frdToSensors(c, g0, m0, acc, mag);
        check(near(hdg::tiltHeadingDeg(acc, mag, c), 10.0f, 0.01f), "오프셋 350 + 편각 20 → 10° 로 접는다");
        for (int i = 0; i < 3; ++i) acc[i] *= 1.2f;
        check(hdg::tiltHeadingDeg(acc, mag, c) < 0.0f, "가속 크기 1.2 g — 기울기를 못 믿어 방위 없음(-1)");
        check(near(hdg::tiltHeadingDeg(acc, mag, c, false), 10.0f, 0.01f),
              "식3: 1.2 g여도 같은 계산값 표시, 식2의 거절은 보존");
        const float noAcc[3] = {0, 0, 0};
        check(hdg::tiltHeadingDeg(noAcc, mag, c, false) < 0.0f,
              "식3도 가속 입력 자체가 없으면 방위를 만들지 않음");
        hdg::HeadingCfg bad{1, 1, 1.0f, 1.0f, 0.0f, 0.0f};
        check(hdg::tiltHeadingDeg(acc, mag, bad) < 0.0f && hdg::flatHeadingDeg(mag, bad) < 0.0f,
              "두 축이 같은 잘못된 설정 → 방위 없음");
    }
}

// ── 3 CASIC ──────────────────────────────────────────────────────────────

static std::vector<uint8_t> frame(uint8_t cls, uint8_t id, const std::vector<uint8_t>& pl,
                                  bool corrupt = false, long lenOverride = -1) {
    std::vector<uint8_t> f = {0xBA, 0xCE};
    const uint16_t len = lenOverride >= 0 ? (uint16_t)lenOverride : (uint16_t)pl.size();
    f.push_back(len & 0xFF); f.push_back(len >> 8);
    f.push_back(cls); f.push_back(id);
    f.insert(f.end(), pl.begin(), pl.end());
    uint32_t ck = casic::checksum(cls, id, (uint16_t)pl.size(), pl.data());
    if (corrupt) ck ^= 0x1;
    for (int i = 0; i < 4; ++i) f.push_back((ck >> (8 * i)) & 0xFF);
    return f;
}
static std::vector<uint8_t> navPvPayload(float speed, float velN = 1.0f) {
    std::vector<uint8_t> p(80, 0);
    auto put = [&](size_t off, float v) { std::memcpy(p.data() + off, &v, 4); };
    p[4] = 7; p[5] = 7; p[7] = 20;
    put(12, 1.2f); put(40, 4.0f); put(48, velN); put(52, 0.5f); put(56, 0.0f);
    put(64, speed); put(68, 45.0f); put(72, 0.04f); put(76, 1.0f);
    return p;
}
struct Fed { std::vector<casic::Result> results; std::vector<casic::Frame> frames; std::string nmea; };
static Fed feedAll(casic::Parser& ps, const std::vector<uint8_t>& bytes, uint32_t t0 = 0, uint32_t stepMs = 0) {
    Fed out;
    uint32_t t = t0;
    for (uint8_t b : bytes) {
        bool pass = false;
        auto r = ps.feed(b, t, &pass);
        if (pass) out.nmea.push_back((char)b);
        if (r != casic::Result::None) {
            out.results.push_back(r);
            if (r == casic::Result::Frame) out.frames.push_back(ps.frame());
        }
        t += stepMs;
    }
    return out;
}

static void testCasic() {
    std::printf("\n[3] CASIC 프레임\n");
    {
        casic::Parser ps;
        auto f = frame(0x01, 0x03, navPvPayload(2.5f));
        auto r = feedAll(ps, f);
        casic::NavPv pv;
        check(r.frames.size() == 1 && casic::parseNavPv(r.frames[0], &pv) &&
                  std::fabs(pv.speed2D - 2.5f) < 1e-6f,
              "정상 NAV-PV 를 받는다");
    }
    {
        casic::Parser ps;
        auto r = feedAll(ps, frame(0x01, 0x03, navPvPayload(2.5f), /*corrupt=*/true));
        check(r.frames.empty() && r.results.size() == 1 &&
                  r.results[0] == casic::Result::BadChecksum,
              "체크섬이 틀린 NAV-PV 는 버린다 (옛 코드는 받았다)");
    }
    {   // 선언 길이가 버퍼보다 큰 프레임 뒤에 정상 프레임 — 경계를 지켜야 한다
        casic::Parser ps;
        std::vector<uint8_t> big(200, 0x11);
        auto bytes = frame(0x0A, 0x04, big);
        auto good = frame(0x01, 0x03, navPvPayload(1.0f));
        bytes.insert(bytes.end(), good.begin(), good.end());
        auto r = feedAll(ps, bytes);
        check(r.results.size() == 2 && r.results[0] == casic::Result::Oversize &&
                  r.frames.size() == 1,
              "넘치는 프레임은 끝까지 먹고 버린다 → 다음 정상 프레임을 잡는다");
    }
    {
        casic::Parser ps;
        std::vector<uint8_t> odd(6, 0);
        auto r = feedAll(ps, frame(0x06, 0x07, odd));
        check(r.results.size() == 1 && r.results[0] == casic::Result::BadLength,
              "4 의 배수가 아닌 길이는 버린다");
    }
    {
        casic::Parser ps;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        auto r = feedAll(ps, frame(0x01, 0x03, navPvPayload(nan)));
        check(r.frames.size() == 1 && !casic::parseNavPv(r.frames[0], nullptr),
              "체크섬이 맞아도 속도가 NaN 이면 NAV-PV 로 안 쓴다");
        casic::Parser ps2;
        auto r2 = feedAll(ps2, frame(0x01, 0x03, navPvPayload(500.0f)));
        check(r2.frames.size() == 1 && !casic::parseNavPv(r2.frames[0], nullptr),
              "속도 500 m/s 는 범위 밖이라 버린다");
        casic::Parser ps3;
        auto r3 = feedAll(ps3, frame(0x01, 0x03, std::vector<uint8_t>(76, 0)));
        check(r3.frames.size() == 1 && !casic::parseNavPv(r3.frames[0], nullptr),
              "NAV-PV 길이가 80 이 아니면 버린다");
    }
    {   // 잘린 프레임: 머리만 오고 끊긴 뒤 정상 프레임 둘 — 첫 정상 프레임은 잘린 것에 먹히고
        //   (len+4 바이트 안에서 끝난다), 둘째는 잡힌다. 시간 초과는 없다.
        casic::Parser ps;
        std::vector<uint8_t> head = {0xBA, 0xCE, 80, 0, 0x01, 0x03, 1, 2, 3};
        auto bytes = head;
        auto good = frame(0x01, 0x03, navPvPayload(3.0f));
        bytes.insert(bytes.end(), good.begin(), good.end());
        bytes.insert(bytes.end(), good.begin(), good.end());
        auto r = feedAll(ps, bytes);
        bool sawBad = false;
        for (auto x : r.results) if (x == casic::Result::BadChecksum) sawBad = true;
        check(sawBad && r.frames.size() == 1, "잘린 프레임은 체크섬으로 버리고 그다음 정상 프레임을 받는다");
    }
    {   // 선언 길이가 넘치면 그 자리에서 버린다 — 뒤의 정상 프레임을 안 삼킨다
        casic::Parser ps;
        std::vector<uint8_t> bytes = {0xBA, 0xCE, 0xFF, 0xFF, 0x0A, 0x04};
        auto good = frame(0x01, 0x03, navPvPayload(1.0f));
        bytes.insert(bytes.end(), good.begin(), good.end());
        auto r = feedAll(ps, bytes);
        check(r.results.size() == 2 && r.results[0] == casic::Result::Oversize && r.frames.size() == 1,
              "길이 65535 는 그 자리에서 버리고 바로 뒤 프레임을 받는다 (65 KB 를 안 삼킨다)");
    }
    {   // NMEA 는 프레임 밖에서만 흘려보낸다
        casic::Parser ps;
        std::string s = "$GNRMC,1*00\r\n";
        std::vector<uint8_t> bytes(s.begin(), s.end());
        auto good = frame(0x01, 0x03, navPvPayload(1.0f));
        bytes.insert(bytes.end(), good.begin(), good.end());
        bytes.push_back('$');
        auto r = feedAll(ps, bytes);
        check(r.nmea == s + "$" && r.frames.size() == 1, "NMEA 바이트만 파서 밖으로 흘린다");
    }
    {   // ACK 짝
        casic::Parser ps;
        auto r = feedAll(ps, frame(0x05, 0x01, {0x06, 0x07, 0, 0}));
        check(r.frames.size() == 1 &&
                  casic::ackFor(r.frames[0], 0x06, 0x07) == casic::Ack::Ack &&
                  casic::ackFor(r.frames[0], 0x06, 0x04) == casic::Ack::NotMine,
              "ACK 는 요청한 class·id 에 대한 것만 우리 것으로 본다");
        casic::Parser ps2;
        auto r2 = feedAll(ps2, frame(0x05, 0x00, {0x06, 0x07, 0, 0}));
        check(r2.frames.size() == 1 && casic::ackFor(r2.frames[0], 0x06, 0x07) == casic::Ack::Nack,
              "NACK 을 NACK 으로 읽는다");
    }
}

// ── 4 자이로 단위 ────────────────────────────────────────────────────────

static void testGyro() {
    std::printf("\n[4] 자이로 0점 단위\n");
    const int f = imu::kGyrRangeFactor1000;
    const float raw = 100.0f;                       // ±1000 범위에서 가만히 둔 원시값
    const float legacyLeft = imu::libCorrect(raw, raw, f);
    check(std::fabs(legacyLeft - 75.0f) < 1e-4f,
          "옛 방식(원시 평균을 그대로 넣음)은 100 중 25 만 빼고 75 가 남는다");
    const float off = imu::gyrOffsetForLib(raw, f);
    check(std::fabs(imu::libCorrect(raw, off, f)) < 1e-4f, "고친 방식은 0 이 남는다");
    check(std::fabs(imu::libOffsetToDps(off) - imu::rawToDps(raw, f)) < 1e-5f,
          "진단 출력 °/s 는 라이브러리 단위로 환산해도 같다");
    check(std::fabs(imu::rawToDps(raw, f) - 3.0518f) < 1e-3f, "±1000 에서 원시 100 은 약 3.05 °/s");
    check(std::fabs(imu::migrateLegacyOffset(raw) - off) < 1e-4f,
          "NVS 옛 값(±1000 원시) 을 새 단위로 옮기면 같은 오프셋이 된다");
    check(imu::gyrCalAcceptable(1.0f, 1.0f, 3.8f, 5.0f), "조용하고 영점이 작으면 받는다");
    check(!imu::gyrCalAcceptable(6.0f, 1.0f, 3.8f, 5.0f), "흔들렸으면 거절");
    check(!imu::gyrCalAcceptable(0.5f, 12.0f, 3.8f, 5.0f),
          "고르게 돌고 있었으면(평균이 큼) 영점으로 안 받는다");
}

// ── 5 설정 저장 ──────────────────────────────────────────────────────────

struct FakePrefs {
    bool open = false, beginOk = true, putOk = true;
    int begins = 0, ends = 0, putsWhileClosed = 0, puts = 0;
    bool begin(const char*, bool) { ++begins; open = beginOk; return beginOk; }
    void end() { ++ends; open = false; }
    size_t putFloat(const char*, float) {
        ++puts;
        if (!open) { ++putsWhileClosed; return 0; }
        return putOk ? 4 : 0;
    }
};

static void testPrefs() {
    std::printf("\n[5] 설정 저장\n");
    {
        FakePrefs p;
        size_t got = p.putFloat("dead_kn", 0.2f);     // 옛 코드: begin 없이 put
        check(got == 0 && p.putsWhileClosed == 1, "begin 없는 put 은 조용히 0 을 돌려준다 (재현)");
    }
    {
        FakePrefs p;
        bool ok = prefs::writeWith(p, "sail", [](FakePrefs& q) {
            return prefs::wrote(q.putFloat("dead_kn", 0.2f), sizeof(float));
        });
        check(ok && p.begins == 1 && p.ends == 1 && p.putsWhileClosed == 0,
              "writeWith 는 열고, 쓰고, 닫는다");
    }
    {
        FakePrefs p; p.putOk = false;
        bool ok = prefs::writeWith(p, "sail", [](FakePrefs& q) {
            return prefs::wrote(q.putFloat("dead_kn", 0.2f), sizeof(float));
        });
        check(!ok && p.ends == 1, "put 이 실패하면 거짓을 돌려주고 그래도 닫는다");
    }
    {
        FakePrefs p; p.beginOk = false;
        bool ok = prefs::writeWith(p, "sail", [](FakePrefs& q) { q.putFloat("x", 1); return true; });
        check(!ok && p.puts == 0, "begin 이 실패하면 put 을 부르지 않는다");
    }
    check(prefs::inRange(0.0f, 0.0f, 2.0f) && !prefs::inRange(2.1f, 0.0f, 2.0f) &&
          !prefs::inRange(-0.1f, 0.0f, 2.0f), "범위 검사");
}

// ── 6 Range ──────────────────────────────────────────────────────────────

static void testRange() {
    std::printf("\n[6] HTTP Range\n");
    using R = http::RangeResult;
    uint64_t a = 0, b = 0;
    check(http::parseRange(nullptr, 100, &a, &b) == R::None, "머리 없음 → None");
    check(http::parseRange("bytes=10-19", 100, &a, &b) == R::Ok && a == 10 && b == 19, "a-b");
    check(http::parseRange("bytes=90-", 100, &a, &b) == R::Ok && a == 90 && b == 99, "a-");
    check(http::parseRange("bytes=-10", 100, &a, &b) == R::Ok && a == 90 && b == 99, "-n (끝 10바이트)");
    check(http::parseRange("bytes=-500", 100, &a, &b) == R::Ok && a == 0 && b == 99, "-n 이 파일보다 크면 전체");
    check(http::parseRange("bytes=50-5000", 100, &a, &b) == R::Ok && b == 99, "끝이 넘치면 파일 끝으로");
    check(http::parseRange("bytes=20-10", 100, &a, &b) == R::Malformed, "끝 < 시작 → 거절");
    check(http::parseRange("bytes=abc-10", 100, &a, &b) == R::Malformed, "숫자 아님 → 거절");
    check(http::parseRange("bytes=1-2,5-6", 100, &a, &b) == R::Malformed, "여러 범위 → 거절");
    check(http::parseRange("items=1-2", 100, &a, &b) == R::Malformed, "bytes 가 아님 → 거절");
    check(http::parseRange("bytes=-", 100, &a, &b) == R::Malformed, "숫자 없음 → 거절");
    check(http::parseRange("bytes=100-", 100, &a, &b) == R::Unsatisfiable, "시작이 끝 뒤 → 416");
    check(http::parseRange("bytes=0-", 0, &a, &b) == R::Unsatisfiable, "빈 파일에 범위 → 416");
    check(http::parseRange("bytes=-0", 100, &a, &b) == R::Unsatisfiable, "-0 → 416");
    check(http::parseRange("bytes=99999999999999999999-", 100, &a, &b) == R::Malformed, "넘치는 숫자 → 거절");
}


// ── 7 방위 수학 ──────────────────────────────────────────────────────────

static void testHeadingMath() {
    std::printf("\n[7] 방위 수학\n");
    check(std::isnan(hdg::wrap360(INFINITY)) && std::isnan(hdg::wrap360(-INFINITY)) &&
          std::isnan(hdg::wrap360(NAN)), "inf·nan 은 NaN (옛 while 은 inf 에서 끝나지 않았다)");
    const float big = hdg::wrap360(1e10f);
    check(std::isfinite(big) && big >= 0.0f && big < 360.0f, "1e10 도 한 번에 0~360 안으로 (옛 while 은 끝나지 않았다)");
    check(std::fabs(hdg::wrap360(-725.0f) - 355.0f) < 1e-3f, "-725 → 355");
    check(std::fabs(hdg::wrap360(360.0f)) < 1e-6f, "360 → 0");
    check(std::fabs(hdg::wrap180(190.0f) + 170.0f) < 1e-3f, "wrap180(190) → -170");
    const float hs[] = {358.0f, 359.5f, 0.5f, 1.0f, 2.0f};
    check(std::fabs(hdg::circularSpread(hs, 5) - 4.0f) < 1e-3f,
          "원형 폭: 358·359.5·0.5·1·2 → 4° (옛 max−min 은 359°)");
    const float hs2[] = {10.0f, 350.0f, 180.0f};
    check(std::fabs(hdg::circularSpread(hs2, 3) - 190.0f) < 1e-3f, "원형 폭: 10·350·180 → 190°");
    float v = 0;
    check(!hdg::parseNumber("abc", &v) && !hdg::parseNumber("inf", &v) &&
          !hdg::parseNumber("nan", &v) && !hdg::parseNumber("12abc", &v) &&
          !hdg::parseNumber("", &v) && !hdg::parseNumber("0x10", &v),
          "숫자가 아닌 입력은 거절 (옛 toFloat 은 abc 를 0 으로 저장)");
    check(hdg::parseNumber(" -8.5 ", &v) && std::fabs(v + 8.5f) < 1e-6f, "-8.5 는 받는다");
    check(hdg::parseNumber("1e10", &v) && !(v >= -360.0f && v <= 360.0f),
          "1e10 은 숫자지만 범위 검사에서 걸러야 한다");
    float f1 = NAN; check(hdg::sanitizeFloat(&f1, -360, 360, 0) && f1 == 0.0f, "NVS NaN → 기본값");
    float f2 = 1e10f; check(hdg::sanitizeFloat(&f2, -360, 360, 0) && f2 == 0.0f, "NVS 1e10 → 기본값");
    float f3 = 12.0f; check(!hdg::sanitizeFloat(&f3, -360, 360, 0) && f3 == 12.0f, "정상 값은 그대로");
    uint8_t a = 1, b = 1; check(hdg::sanitizeAxes(&a, &b) && a == 1 && b == 0, "같은 축 두 개 → 기본");
    uint8_t a2 = 7, b2 = 0; check(hdg::sanitizeAxes(&a2, &b2) && a2 == 1 && b2 == 0, "축 번호 7 → 기본");
    uint8_t a3 = 2, b3 = 0; check(!hdg::sanitizeAxes(&a3, &b3), "정상 축은 그대로");

    // 오일러 방위 변화율 — yaw 고정 · 피치 20° · 힐 10° 에서 힐만 30°/s
    const float th = 20.0f * M_PI / 180.0f, ph = 10.0f * M_PI / 180.0f;
    const float rollRate = 30.0f, yawRate = 20.0f;
    // 몸통 각속도 = [φ̇ − ψ̇ sinθ, ψ̇ cosθ sinφ, ψ̇ cosθ cosφ] (θ̇ = 0)
    float p = rollRate, q = 0, r = 0;
    check(std::fabs(hdg::eulerYawRate(p, q, r, ph, th)) < 1e-4f,
          "힐만 바뀔 때 방위 변화율 0 (옛 식은 −10.3°/s)");
    p = -yawRate * std::sin(th); q = yawRate * std::cos(th) * std::sin(ph); r = yawRate * std::cos(th) * std::cos(ph);
    check(std::fabs(hdg::eulerYawRate(p, q, r, ph, th) - yawRate) < 1e-3f, "yaw 만 20°/s → 20°/s");
    check(std::isnan(hdg::eulerYawRate(0, 1, 1, 0, 89.0f * M_PI / 180.0f)), "피치 89° 는 풀지 않는다 (NaN)");
}

// ── 8 자력 표본 ──────────────────────────────────────────────────────────

static void testMagSample() {
    std::printf("\n[8] 자력 표본 검사\n");
    const uint8_t prev[6] = {0x10, 0x01, 0x20, 0x02, 0x30, 0x03};
    uint8_t b[8] = {0x10, 0x01, 0x20, 0x02, 0x30, 0x03, 0x10, 0x16};
    check(mag::check(8, b, prev, true) == mag::Check::Repeat, "같은 6바이트 → 반복 (새 표본 아님)");
    check(mag::check(8, b, prev, false) == mag::Check::New, "지난 표본이 없으면 새 표본");
    b[0] = 0x11;
    check(mag::check(8, b, prev, true) == mag::Check::New, "한 바이트라도 다르면 새 표본");
    check(mag::check(5, b, prev, true) == mag::Check::ShortRead, "8바이트를 다 못 읽으면 거절");
    b[6] = 0x18;
    check(mag::check(8, b, prev, true) == mag::Check::Overflow, "ST2 HOFL → 넘침");
    uint8_t z[8] = {0, 0, 0, 0, 0, 0, 0x10, 0x16};
    check(mag::check(8, z, prev, true) == mag::Check::Zero, "세 축 0 → 거절 (옛 코드는 방위 0° 로 냈다)");
    check(std::fabs(mag::asaFactor(128) - 1.0f) < 1e-6f && std::fabs(mag::asaFactor(176) - 1.1875f) < 1e-6f,
          "ASA 계수 = 라이브러리 식");
    check(std::fabs(mag::toMicroTesla(32760, 1.0f) - 4912.0f) < 0.01f, "원시 32760 → 4912 µT (16비트)");
    check(mag::le16(prev) == 0x0110 && mag::le16(b + 4) == 0x0330, "리틀엔디안");

    // 신선도: 같은 값이 계속 와도 정상 읽기면 무효가 아니다 (사용자 지적 3)
    {
        mag::Freshness fr;
        uint32_t t = 1000;
        fr.update(mag::Check::New, t);
        bool allUsable = true;
        for (int i = 0; i < 16; ++i) { t += 125; fr.update(mag::Check::Repeat, t); allUsable &= fr.usable(t); }
        check(allUsable, "같은 값이 2초 동안 8 Hz 로 계속 와도 신선하다 (첫 수정은 400 ms 뒤 무효)");
        for (int i = 0; i < 30; ++i) { t += 125; fr.update(mag::Check::Repeat, t); }
        check(!fr.usable(t), "세 축이 5초 넘게 한 번도 안 바뀌면 멈춘 것으로 본다");
        fr.update(mag::Check::New, t + 125);
        check(fr.usable(t + 125), "값이 바뀌면 다시 쓴다");
        mag::Freshness f2;
        f2.update(mag::Check::New, 1000);
        f2.update(mag::Check::ShortRead, 1300);
        check(f2.usable(1350) && !f2.usable(1450), "짧은 읽기만 이어지면 400 ms 뒤 무효");
        mag::Freshness f3;
        f3.update(mag::Check::New, 1000);
        check(!f3.usable(1900), "마지막 정상 읽기가 900 ms 전이면 무효 (두 방위 함수 공통 기준)");
    }

    // ASA 는 Fuse ROM 모드에서만 읽힌다 — 모드를 흉내 내는 가짜 칩 (사용자 지적 1)
    {
        struct FakeAk {
            uint8_t mode = 0x02;                 // 연속 8 Hz 로 켜져 있음
            std::vector<uint8_t> modes;
            bool badTransition = false;
            void set(uint8_t m) {
                if (mode != 0x00 && m != 0x00) badTransition = true;   // 전원 차단을 안 거침
                mode = m; modes.push_back(m);
            }
            uint8_t read(uint8_t reg) {
                if (reg >= 0x10 && reg <= 0x12) return mode == 0x0F ? (uint8_t)(0xA0 + reg - 0x10) : 0x00;
                return 0;
            }
        } ak;
        // 첫 수정 순서: 연속 모드에서 그대로 읽음
        const uint8_t wrong[3] = {ak.read(0x10), ak.read(0x11), ak.read(0x12)};
        check(wrong[0] == 0x00, "연속 모드에서 읽으면 ASA 가 안 나온다 (첫 수정의 오류 재현)");
        uint8_t out[3] = {0, 0, 0};
        const bool ok = mag::readAsaFuseRom([&](uint8_t m) { ak.set(m); }, [&](uint8_t r) { return ak.read(r); },
                                            0x00, 0x0F, 0x02, 0x10, out);
        check(ok && out[0] == 0xA0 && out[1] == 0xA1 && out[2] == 0xA2, "Fuse ROM 모드에서 세 값을 읽는다");
        check(!ak.badTransition, "모드를 바꿀 때마다 전원 차단을 거친다");
        check(ak.mode == 0x02, "끝나면 연속 8 Hz 로 되돌린다");
        struct Dead { uint8_t read(uint8_t) { return 0xFF; } } dead;
        uint8_t o2[3];
        check(!mag::readAsaFuseRom([](uint8_t) {}, [&](uint8_t r) { return dead.read(r); }, 0, 0x0F, 2, 0x10, o2),
              "0xFF 가 오면 못 읽은 것으로 본다");
    }
}

// ── 9 자력 보정 수락 ─────────────────────────────────────────────────────

static int makeSphere(int16_t (*out)[3], int n, float cx, float cy, float cz, float r, float noiseAmp) {
    const double golden = M_PI * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double y = 1.0 - 2.0 * (i + 0.5) / n, rad = std::sqrt(1 - y * y), th = golden * i;
        const float nz = (i % 2 ? 1.0f : -1.0f) * noiseAmp;
        out[i][0] = (int16_t)lroundf((cx + (r + nz) * std::cos(th) * rad) * 10);
        out[i][1] = (int16_t)lroundf((cy + (r + nz) * y) * 10);
        out[i][2] = (int16_t)lroundf((cz + (r + nz) * std::sin(th) * rad) * 10);
    }
    return n;
}

static void testMagcal() {
    std::printf("\n[9] 자력 보정 수락 조건\n");
    static int16_t pts[128][3];
    magcal::Fit fit;
    int n = makeSphere(pts, 96, 10.8f, 14.7f, -3.0f, 50.0f, 0.3f);
    auto v = magcal::fitAndJudge((const int16_t (*)[3])pts, n, &fit);
    const float ce = std::sqrt((fit.c[0]-10.8f)*(fit.c[0]-10.8f) + (fit.c[1]-14.7f)*(fit.c[1]-14.7f) + (fit.c[2]+3.0f)*(fit.c[2]+3.0f));
    check(v == magcal::Verdict::Ok && ce < 0.5f, "사방으로 고르게 → 통과, 중심 오차 0.5 µT 미만");

    // 수평으로 뱃머리만 돌림: 수평 30 µT, 수직 40 µT 고정 (복각 53°)
    for (int i = 0; i < 40; ++i) {
        const double a = i * 2 * M_PI / 40;
        pts[i][0] = (int16_t)lroundf((10.8f + 30.0f * std::cos(a)) * 10);
        pts[i][1] = (int16_t)lroundf((14.7f + 30.0f * std::sin(a)) * 10);
        pts[i][2] = (int16_t)lroundf((-3.0f + 40.0f + ((i % 3) - 1) * 0.3f) * 10);
    }
    v = magcal::fitAndJudge((const int16_t (*)[3])pts, 40, &fit);
    check(v != magcal::Verdict::Ok, "한 평면(뱃머리만 돌림) → 거절 (옛 코드는 z 38 µT 틀린 채 저장)");
    std::printf("         (거절 이유: %s)\n", magcal::verdictText(v));

    // 뱃머리 돌림 + 기울기 ±5°
    for (int i = 0; i < 60; ++i) {
        const double a = i * 2 * M_PI / 60, tilt = 5.0 * M_PI / 180 * std::sin(i * 0.7);
        const double hx = 30 * std::cos(a), hy = 30 * std::sin(a), vz = 40;
        pts[i][0] = (int16_t)lroundf((10.8 + hx * std::cos(tilt) + vz * std::sin(tilt)) * 10);
        pts[i][1] = (int16_t)lroundf((14.7 + hy) * 10);
        pts[i][2] = (int16_t)lroundf((-3.0 - hx * std::sin(tilt) + vz * std::cos(tilt)) * 10);
    }
    v = magcal::fitAndJudge((const int16_t (*)[3])pts, 60, &fit);
    check(v != magcal::Verdict::Ok, "기울기 ±5° 만 섞임 → 거절 (옛 코드는 3.3 µT 틀린 채 저장)");

    // 비스듬한 평면: 기울어진 축 둘레로만 돌림 → 점이 좌표축에 비스듬한 원 위에 놓인다 (사용자 지적 2)
    {
        const double nx = 1 / std::sqrt(3.0), ny = nx, nz = nx;      // 도는 축 (1,1,1)
        const double B[3] = {30.0, 0.0, 40.0};                       // 지구 자기장 50 µT
        for (int i = 0; i < 32; ++i) {
            const double a = i * 2 * M_PI / 32, c = std::cos(a), sn = std::sin(a);
            const double k[3] = {nx, ny, nz};
            const double kxB[3] = {k[1]*B[2]-k[2]*B[1], k[2]*B[0]-k[0]*B[2], k[0]*B[1]-k[1]*B[0]};
            const double kdB = k[0]*B[0] + k[1]*B[1] + k[2]*B[2];
            const double off[3] = {10.8, 14.7, -3.0};
            for (int ax = 0; ax < 3; ++ax) {
                const double rot = B[ax]*c + kxB[ax]*sn + k[ax]*kdB*(1 - c);   // 로드리게스 회전
                pts[i][ax] = (int16_t)lroundf((float)((off[ax] + rot + ((i + ax) % 3 - 1) * 0.3) * 10));
            }
        }
        v = magcal::fitAndJudge((const int16_t (*)[3])pts, 32, &fit);
        const bool oldRulePass = fit.solved && fit.r >= 25 && fit.r <= 70 && fit.resid <= fit.r * 0.1f &&
                                 fit.spread[0] >= fit.r && fit.spread[1] >= fit.r && fit.spread[2] >= fit.r;
        const float ce2 = fit.solved ? std::sqrt((fit.c[0]-10.8f)*(fit.c[0]-10.8f) + (fit.c[1]-14.7f)*(fit.c[1]-14.7f) + (fit.c[2]+3.0f)*(fit.c[2]+3.0f)) : -1;
        std::printf("         (비스듬한 평면 32점: 풀림 %d, 중심 오차 %.2f µT, 반지름 %.1f, 퍼짐 %.0f/%.0f/%.0f, 두께 %.2f → 첫 수정 조건 %s)\n",
                    fit.solved, ce2, fit.r, fit.spread[0], fit.spread[1], fit.spread[2], fit.thickness,
                    oldRulePass ? "통과" : "거절");
        check(v == magcal::Verdict::Flat, "비스듬한 평면 → 두께로 거절 (축별 퍼짐으로는 못 거른다)");
    }
    {   // 고른 공의 두께는 약 0.58r, 25% 캡은 통과, 12% 캡은 거절
        int n2 = makeSphere(pts, 96, 0, 0, 0, 50.0f, 0.0f);
        const float th = magcal::thicknessOf((const int16_t (*)[3])pts, n2);
        check(th > 0.5f * 50 && th < 0.65f * 50, "고른 공 전체의 두께 ≈ 0.58r");
        // 구의 캡(면적 비율 f): 높이 z 가 [r(1−2f), r] 에서 고르다 → 두께 ≈ 2f·r/√12
        auto cap = [&](double frac) {
            int m = 0;
            const double golden = M_PI * (3.0 - std::sqrt(5.0));
            for (int i = 0; i < 96; ++i) {
                const double z = 1.0 - 2.0 * frac * (i + 0.5) / 96, rad = std::sqrt(1 - z * z), a = golden * i;
                pts[m][0] = (int16_t)lroundf((float)(50 * rad * std::cos(a) * 10));
                pts[m][1] = (int16_t)lroundf((float)(50 * rad * std::sin(a) * 10));
                pts[m][2] = (int16_t)lroundf((float)(50 * z * 10));
                ++m;
            }
            return magcal::thicknessOf((const int16_t (*)[3])pts, m) / 50.0f;
        };
        const float c25 = cap(0.25), c12 = cap(0.12);
        std::printf("         (구의 25%% 캡 두께 %.3fr, 12%% 캡 두께 %.3fr, 문턱 %.2fr)\n", c25, c12, magcal::kThicknessRatio);
        check(c25 >= magcal::kThicknessRatio, "구의 25% 만 덮어도 두께 문턱은 통과 (검산 오차 0.5 µT 였던 범위)");
        check(c12 < magcal::kThicknessRatio, "구의 12% 만 덮으면 두께 문턱에서 거절 (검산 오차 3.7 µT 였던 범위)");
    }

    n = makeSphere(pts, 96, 10.8f, 14.7f, -3.0f, 15.0f, 0.3f);
    check(magcal::fitAndJudge((const int16_t (*)[3])pts, n, &fit) == magcal::Verdict::RadiusOut,
          "반지름 15 µT → 지구 자기장 범위 밖이라 거절");
    n = makeSphere(pts, 96, 10.8f, 14.7f, -3.0f, 50.0f, 8.0f);
    check(magcal::fitAndJudge((const int16_t (*)[3])pts, n, &fit) == magcal::Verdict::ResidHigh,
          "공에서 ±8 µT 벗어남 → 잔차로 거절");
    n = makeSphere(pts, 10, 0, 0, 0, 50.0f, 0.0f);
    check(magcal::fitAndJudge((const int16_t (*)[3])pts, n, &fit) == magcal::Verdict::TooFew, "점 10개 → 모자람");
}

int main() {
    testSdWrites();
    testLoraPacket();
    testRecControl();
    testSogPolicy();
    testHeadingTilt();
    testCasic();
    testGyro();
    testPrefs();
    testRange();
    testHeadingMath();
    testMagSample();
    testMagcal();
    std::printf("\n%s — 실패 %d개\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
