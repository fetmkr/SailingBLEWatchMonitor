// 기록 제어의 판단만 모은 것. SD·FreeRTOS·NVS 가 없다 — 맥에서 시험한다 (tools/fw_logic_test.cpp).
//
// ★ 원하는 상태와 실제 상태를 나눈다 (ArduPilot logging_enabled / PX4 desired_state 와 같은 모양).
//   옛 코드는 gNeedFinalize · gFailedStop · userStopped · gRecRestartAt 네 깃발이 서로 얽혀서,
//   하나 막으면 옆에서 새 버그가 났다 (시험 세션 이어받기, rec off 뒤 저절로 재시작 등).
//
//   want   사람이 정한 것. 단추·rec on/off·앱·끄기만 바꾼다. 늦게 온 쓰기 실패는 못 바꾼다
//   phase  기록기가 실제로 어디 있나 — Idle / Recording / Closing
//   result 끝난 세션 하나의 결과. 일꾼(코어 0)만 채우고, 루프는 임계 구역 안에서 복사만 한다
#pragma once

#include <cstdint>

namespace recctl {

enum class Phase : uint8_t { Idle = 0, Recording = 1, Closing = 2 };

// 첫 오류 종류. 한 세션에서 한 번 채우면 안 바꾼다.
//   1 쓰기 실패 · 2 기록 중 카드 빠짐          → 원하는 상태가 기록이면 새 파일로 다시 건다
//   3 닫으면서 다 못 씀 · 5 머리글 못 고침     → 기록만 남긴다
enum : uint8_t { kErrNone = 0, kErrWrite = 1, kErrCardGone = 2, kErrDrainShort = 3, kErrHeader = 5 };

struct SessionResult {
    uint32_t session  = 0;
    bool     consumed = true;    // 루프가 꺼내 갔나. false 일 때만 새 결과다
    // 닫힘 — 첫 오류와 따로 둔다
    bool     drained  = false;   // 남은 것을 다 쓰고 닫았나
    bool     headerOk = false;   // 머리글(길이·줄 수·첫 fix·closed)을 고쳤나
    uint32_t lostBytes = 0;      // 버퍼에 남아 못 쓴 바이트
    uint32_t durS      = 0;
    // 첫 오류
    uint8_t  firstKind = kErrNone;
    uint32_t recSec = 0, want = 0, wrote = 0, bytes = 0;
    int      err    = 0;
    uint8_t  tries  = 0;
    bool     card   = false;
    bool     fake   = false;     // rec fail 시험으로 흉내 낸 실패
};

inline bool setFirst(SessionResult& r, uint8_t kind) {
    if (r.firstKind != kErrNone || kind == kErrNone) return false;
    r.firstKind = kind;
    return true;
}

// 세션이 끝났을 때 루프가 할 일.
enum class Next : uint8_t { Nothing, RestartSoon, GiveUp };
//   ★ 닫기 중 실패(3·5)나 정상 종료(0)는 다시 걸 이유가 아니다. 그 사이 사람이 다시 시작을 눌렀으면
//     want 가 참이지만, 그건 "닫힌 뒤 시작" 으로 루프가 건다 — 여기서 포기로 바꾸면 안 된다.
inline Next afterSession(bool want, uint8_t firstKind, uint8_t restarts, uint8_t maxRestarts) {
    if (!want) return Next::Nothing;                           // 사람이 멈췄다
    if (firstKind == kErrWrite || firstKind == kErrCardGone)
        return restarts < maxRestarts ? Next::RestartSoon : Next::GiveUp;
    return Next::Nothing;
}

// 다시 걸 차례에 시작이 실패했을 때
inline Next afterRestartFailed(uint8_t restarts, uint8_t maxRestarts) {
    return restarts < maxRestarts ? Next::RestartSoon : Next::GiveUp;
}

// 화면 REC FAIL · BLE bit5 — 사람이 원하는데 기록이 멈춰 있거나, 다시 걸기를 포기했거나,
// 마지막 세션이 끝까지 저장되지 않았다 (사람이 멈춘 세션이라도 — 검토 3번).
// 닫는 중(Closing)은 뜻대로 가는 중이라 안 띄운다.
inline bool showFailed(bool want, Phase p, bool gaveUp, bool lastSaveBad) {
    return gaveUp || lastSaveBad || (want && p == Phase::Idle);
}

// 끄기. 닫기가 안 끝났으면 기다리고, 한도를 넘으면 사람에게 강제로 끌지 묻는다 (저절로 안 넘어간다).
enum class Off : uint8_t { SleepNow, Wait, AskForce };
inline Off powerOff(Phase p, uint32_t waitedMs, uint32_t limitMs) {
    if (p == Phase::Idle) return Off::SleepNow;
    return waitedMs >= limitMs ? Off::AskForce : Off::Wait;
}

// IMU 가 끊겨 있던 구간을 100 Hz 줄 수로 센다. 기록 시작 전은 빼고, lostAt 은 부르는 쪽이
// 정산할 때마다 until 로 옮겨서 같은 구간을 두 번 세지 않는다. millis() 한 바퀴를 넘겨도 맞게 뺀다.
inline uint32_t imuGapRows(uint32_t lostAt, uint32_t recStartMs, uint32_t untilMs) {
    const uint32_t from = (int32_t)(lostAt - recStartMs) > 0 ? lostAt : recStartMs;
    const int32_t span = (int32_t)(untilMs - from);
    return span > 0 ? (uint32_t)span / 10 : 0;
}

// NVS 두 칸. rec_want = 켜면 다시 걸 의도, rec_open = 아직 마감 안 된 세션 번호.
struct Persist { uint8_t want = 0; uint32_t open = 0; };

// 옛 키 rec_on(1 = 기록 중에 끊김) 을 새 두 칸으로 옮긴다. 새 키가 있으면 그대로.
inline Persist migrate(bool haveNew, Persist cur, bool haveOld, uint8_t oldRecOn, uint32_t lastSession) {
    if (haveNew) return cur;
    Persist p;
    if (haveOld && oldRecOn == 1) { p.want = 1; p.open = lastSession; }
    return p;
}

enum class Boot : uint8_t { Nothing, Resume, TooMany, UnclosedOnly };
// triesSoFar = 앞서 이어 시작한 횟수 (1분 넘게 잘 돌면 0 으로 돌아간다)
inline Boot atBoot(const Persist& p, uint8_t triesSoFar, uint8_t maxTries) {
    if (p.want) return (triesSoFar >= maxTries) ? Boot::TooMany : Boot::Resume;
    return p.open ? Boot::UnclosedOnly : Boot::Nothing;
}

} // namespace recctl
