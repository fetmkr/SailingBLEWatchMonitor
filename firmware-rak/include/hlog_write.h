// SD 기록기의 순수 로직. 보드 없이 맥에서 시험한다 (tools/fw_logic_test.cpp).
//
// 여기에는 SD·FreeRTOS 가 없다. hlog.cpp 가 실제 파일 쓰기를 함수로 넘겨 쓴다.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rec_control.h"

namespace hlog {

// 기록기 상태. 쓰는 쪽(코어 0 일꾼)이 끝을 알린다 — 부르는 쪽은 상태만 본다.
//
//   Closed     파일 없음. 시작할 수 있다
//   Recording  파일이 열려 있고 쓰는 중. 생산자(writeNav/Imu/Text)는 이때만 넣는다
//   Draining   닫는 중 — 사람이 멈추라고 했거나(requestStop) 일꾼이 쓰기를 포기했다.
//              일꾼이 남은 것을 쓰고 · 닫고 · 머리글·이름을 고친 뒤 Closed
//
// ★ 전이는 아래 두 함수로만, hlog.cpp 의 상태 잠금 안에서 한다 (2026-09-15 검토 2번).
//   옛 코드는 쓰기 실패 뒤 마무리하는 동안 Recording 에 머물러 생산자가 계속 넣었고,
//   requestStop 의 "Recording 인가" 확인과 Draining 쓰기 사이에 일꾼이 Closed 를 쓰면
//   닫힌 세션이 닫는 중으로 되돌아갔다. 따로 두던 Failed 는 실제로 머물지 않아서 지웠다.
enum class RecState : uint8_t { Closed = 0, Recording = 1, Draining = 2 };

// 기록 중 → 닫는 중. 멈춤 요청과 쓰기 포기가 같은 전이를 쓴다. 이미 닫는 중·닫힘이면 아무것도 안 바꾼다.
// (템플릿인 까닭: hlog.cpp 의 gState 는 두 코어가 보는 volatile 이라 RecState& 로 못 받는다)
template <class S>
inline bool toDraining(S& s) {
    if (s != RecState::Recording) return false;
    s = RecState::Draining;
    return true;
}
// 닫는 중 → 닫힘. 일꾼만 부른다 (마무리를 다 한 뒤).
template <class S>
inline bool toClosed(S& s) {
    if (s != RecState::Draining) return false;
    s = RecState::Closed;
    return true;
}

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


// 시작은 파일이 닫혀 있을 때만.
inline bool canStart(RecState s) { return s == RecState::Closed; }

// SD 를 만져도 되나 (진단·파일 목록·WiFi). 기록기가 파일을 쥐고 있으면 안 된다.
inline bool sdBusy(RecState s) { return s == RecState::Recording || s == RecState::Draining; }

} // namespace hlog
