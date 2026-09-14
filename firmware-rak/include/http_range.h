// HTTP Range 머리 해석. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
//
// 받는 모양은 단일 범위 셋뿐이다 (RFC 9110 §14.1.2).
//   bytes=a-b     a 부터 b 까지 (b 포함)
//   bytes=a-      a 부터 끝까지
//   bytes=-n      끝에서 n 바이트
// 여러 범위(쉼표)는 안 받는다.
#pragma once

#include <cstdint>
#include <cstring>

namespace http {

enum class RangeResult : uint8_t {
    None,           // Range 머리 없음 → 파일 전체 200
    Ok,             // 206
    Unsatisfiable,  // 416 — 모양은 맞는데 파일 안에 없는 범위 (빈 파일 포함)
    Malformed,      // 400 — 모양이 틀림 (숫자 아님, 끝 < 시작, 여러 범위)
};

namespace detail {
inline bool parseU64(const char* s, const char* e, uint64_t* out) {
    if (s >= e) return false;
    uint64_t v = 0;
    for (const char* p = s; p < e; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return false;   // 넘침
        v = v * 10 + d;
    }
    *out = v;
    return true;
}
inline const char* trim(const char* s) { while (*s == ' ') ++s; return s; }
} // namespace detail

// hdr 가 nullptr 이거나 빈 글자면 None.
inline RangeResult parseRange(const char* hdr, uint64_t total, uint64_t* from, uint64_t* to) {
    if (!hdr) return RangeResult::None;
    hdr = detail::trim(hdr);
    if (!*hdr) return RangeResult::None;
    if (strncmp(hdr, "bytes=", 6) != 0) return RangeResult::Malformed;
    const char* s = detail::trim(hdr + 6);
    const char* end = s + strlen(s);
    while (end > s && end[-1] == ' ') --end;
    for (const char* p = s; p < end; ++p) if (*p == ',') return RangeResult::Malformed;
    const char* dash = nullptr;
    for (const char* p = s; p < end; ++p) if (*p == '-') { dash = p; break; }
    if (!dash) return RangeResult::Malformed;

    uint64_t a = 0, b = 0;
    const bool hasA = dash > s;
    const bool hasB = dash + 1 < end;
    if (hasA && !detail::parseU64(s, dash, &a)) return RangeResult::Malformed;
    if (hasB && !detail::parseU64(dash + 1, end, &b)) return RangeResult::Malformed;
    if (!hasA && !hasB) return RangeResult::Malformed;

    if (!hasA) {                                   // bytes=-n
        if (b == 0 || total == 0) return RangeResult::Unsatisfiable;
        *from = (b >= total) ? 0 : total - b;
        *to   = total - 1;
        return RangeResult::Ok;
    }
    if (hasB && b < a) return RangeResult::Malformed;
    if (total == 0 || a >= total) return RangeResult::Unsatisfiable;
    *from = a;
    *to   = (!hasB || b >= total) ? total - 1 : b;
    return RangeResult::Ok;
}

} // namespace http
