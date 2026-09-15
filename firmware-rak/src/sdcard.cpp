#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "board_rak.h"

namespace sdcard {
namespace {
// 주인 칸은 코어 0(기록 일꾼이 놓음)과 코어 1(나머지)이 같이 본다. 짧게 잠근다.
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Owner   gOwner = Owner::None;
Refusal gRefusal = Refusal::None;
} // namespace

bool acquire(Owner who) {
    if (who == Owner::None) return false;
    portENTER_CRITICAL(&gMux);
    const Owner cur = gOwner;
    if (cur == Owner::None) gOwner = who;          // 먼저 자리를 잡고, 마운트는 잠금 밖에서
    portEXIT_CRITICAL(&gMux);
    if (cur == who) return true;
    if (cur != Owner::None) { gRefusal = Refusal::Busy; return false; }

    SPI.begin(rak::kSPI_CLK, rak::kSPI_MISO, rak::kSPI_MOSI, rak::kSPI_CS);
    if (!SD.begin(rak::kSPI_CS, SPI, rak::kSdHz, "/sd", 5)) {
        portENTER_CRITICAL(&gMux);
        gOwner = Owner::None;
        portEXIT_CRITICAL(&gMux);
        gRefusal = Refusal::MountFailed;
        return false;
    }
    gRefusal = Refusal::None;
    return true;
}

void release(Owner who) {
    portENTER_CRITICAL(&gMux);
    const bool mine = (gOwner == who && who != Owner::None);
    portEXIT_CRITICAL(&gMux);
    if (!mine) return;
    SD.end();
    portENTER_CRITICAL(&gMux);
    gOwner = Owner::None;
    portEXIT_CRITICAL(&gMux);
}

Owner owner() {
    portENTER_CRITICAL(&gMux);
    const Owner o = gOwner;
    portEXIT_CRITICAL(&gMux);
    return o;
}

Refusal lastRefusal() { return gRefusal; }

const char* ownerName(Owner o) {
    switch (o) {
    case Owner::Recorder:   return "기록";
    case Owner::Download:   return "파일 전송";
    case Owner::Diagnostic: return "진단";
    default:                return "없음";
    }
}

void endForSleep() {
    if (owner() == Owner::None) SD.end();
}

} // namespace sdcard
