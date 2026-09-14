// SD 기록기의 순수 로직. 보드 없이 맥에서 시험한다 (tools/fw_logic_test.cpp).
//
// 여기에는 SD·FreeRTOS 가 없다. hlog.cpp 가 실제 파일 쓰기를 함수로 넘겨 쓴다.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hlog {

// 기록기 상태. 쓰는 쪽(코어 0 일꾼)이 끝을 알린다 — 부르는 쪽은 상태만 본다.
//
//   Closed     파일 없음. 시작할 수 있다
//   Recording  파일이 열려 있고 쓰는 중
//   Draining   stop() 이 멈추라고 했다. 일꾼이 남은 것을 다 쓰고 닫는 중
//   Failed     쓰기가 실패해서 일꾼이 파일을 닫았다. 남은 것은 못 썼다
enum class RecState : uint8_t { Closed = 0, Recording = 1, Draining = 2, Failed = 3 };

// 한 범위를 끝까지 쓰려고 한다.
//
// ★ 쓴 만큼 **바로** consume(got) 을 부른다. 그래야 다시 시도할 때 이미 쓴 앞부분을
//   또 쓰지 않는다. 옛 코드는 5바이트를 쓰고 실패하면 꼬리를 그대로 두고 닫기로
//   넘어가서, 닫을 때 같은 버퍼를 처음부터 다시 썼다 (ABCDE + ABCDEFGH… 중복).
//
//   write(p, len)  → 실제로 쓴 바이트 (0 이면 이번엔 못 씀)
//   consume(n)     → 버퍼 꼬리를 n 만큼 넘긴다
//   wait(k)        → k 번째 다시 시도 전에 쉰다
//
// 한 바이트도 못 쓴 시도가 retries 번 이어지면 멈춘다. 반환은 실제로 쓴 합.
template <class WriteFn, class ConsumeFn, class WaitFn>
size_t writeAll(const uint8_t* p, size_t n, WriteFn&& write, ConsumeFn&& consume,
                WaitFn&& wait, uint8_t retries, uint8_t* triesOut = nullptr) {
    size_t done = 0;
    uint8_t fails = 0, tries = 0;
    while (done < n) {
        size_t got = write(p + done, n - done);
        if (got > n - done) got = n - done;       // 라이브러리가 더 썼다고 해도 믿지 않는다
        if (got > 0) {
            consume(got);
            done += got;
            fails = 0;
            continue;
        }
        if (fails >= retries) break;
        ++fails;
        ++tries;
        wait(fails);
    }
    if (triesOut) *triesOut = tries;
    return done;
}

// 쓰기 결과로 다음 상태를 정한다.
inline RecState afterWrite(RecState s, bool complete) {
    if (s == RecState::Recording) return complete ? RecState::Recording : RecState::Failed;
    if (s == RecState::Draining)  return complete ? RecState::Closed    : RecState::Failed;
    return s;
}

// 시작은 파일이 닫혀 있을 때만.
inline bool canStart(RecState s) { return s == RecState::Closed || s == RecState::Failed; }

// 정상 종료를 확정해도 되나 — 일꾼이 남은 것을 **다 쓰고** 닫았을 때만.
// Draining 에 머물러 있으면(시간 초과) 머리글을 고치거나 표시를 지우면 안 된다.
inline bool stopMayFinalize(RecState s) { return s == RecState::Closed; }

// SD 를 만져도 되나 (진단·파일 목록·WiFi). 기록기가 파일을 쥐고 있으면 안 된다.
inline bool sdBusy(RecState s) { return s == RecState::Recording || s == RecState::Draining; }

} // namespace hlog
