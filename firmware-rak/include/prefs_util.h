// NVS 설정 저장 도우미. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// ★ Preferences 는 begin() 없이 put 해도 아무 말 없이 실패한다. 실행 중 값만 바뀌고
//   재부팅하면 옛 값으로 돌아간다. dead / smooth 가 그랬다 (2026-09-14 검토).
//   그래서 여닫기와 반환값 확인을 한 곳에 모은다.
#pragma once

#include <cstddef>

namespace prefs {

// P 는 Preferences 와 같은 모양이면 된다 (begin / end). fn(p) 이 참을 돌려야 성공.
template <class P, class Fn>
bool writeWith(P& p, const char* ns, Fn&& fn) {
    if (!p.begin(ns, /*readOnly=*/false)) return false;
    const bool ok = fn(p);
    p.end();
    return ok;
}

// put 계열이 돌려주는 "쓴 바이트" 가 기대한 크기인가
inline bool wrote(size_t got, size_t want) { return got == want; }

template <class T>
inline bool inRange(T v, T lo, T hi) { return !(v < lo) && !(hi < v); }

} // namespace prefs
