// SD 카드 사용권. 카드를 붙이고 떼는 곳은 여기 하나다.
//
// ★ 기록기 · 파일 서버 · 진단이 같은 SD 객체를 쓴다. 누가 SD.end() 를 부르면 남의 파일이 끊긴다.
//   옛 코드는 47곳에서 각자 SPI.begin + SD.begin / SD.end 를 부르고, 서로 hlog::busy() 를
//   물어서 피했다. 한 곳이라도 안 물으면(실제로 check 명령 하나가 빠졌었다) 기록 중 카드가 내려갔다.
//
//   주인      Recorder   기록 시작부터 마무리(머리글·이름)까지. 코어 0 일꾼이 놓는다
//             Download   WiFi 파일 서버가 켜져 있는 동안
//             Diagnostic 시리얼 진단 명령 한 번 (sd · sdbench · rec ls/check/tail/dump/hash/rm)
//
//   acquire  주인이 없을 때만 쥔다. 쥐면 SPI 를 준비하고 마운트한다. 마운트가 안 되면 놓고 false.
//            이미 같은 주인이 쥐고 있으면 true (다시 마운트하지 않는다).
//   release  쥔 주인만 놓는다. 카드를 내린다.
#pragma once

#include <cstdint>

namespace sdcard {

enum class Owner : uint8_t { None = 0, Recorder = 1, Download = 2, Diagnostic = 3 };

enum class Refusal : uint8_t { None = 0, Busy = 1, MountFailed = 2 };

bool    acquire(Owner who);
void    release(Owner who);
Owner   owner();
Refusal lastRefusal();                 // 마지막 acquire 가 왜 거절됐나
const char* ownerName(Owner o);
// 잠들기 직전. 아무도 안 쥐었으면 카드를 내려 칩셀렉트를 놓는다 (쥔 주인이 있으면 안 건드린다)
void    endForSleep();
// 시험용 (firmware-idf 만 구현, 09-15 속도 조사): 다음 마운트부터 SD SPI 주파수를 kHz 로. 0 = board_rak kSdHz
void    setTestFreqKhz(int khz);
int     cardFreqKhz();                 // 붙어 있으면 카드가 실제로 쓰는 주파수(kHz), 아니면 0
} // namespace sdcard
