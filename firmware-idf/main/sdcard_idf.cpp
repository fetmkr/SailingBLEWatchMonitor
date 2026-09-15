// SD 카드 사용권 — firmware-rak/src/sdcard.cpp 를 ESP-IDF v6.1 로 옮긴 것. 선언은 firmware-rak/include/sdcard.h 그대로.
//
// 바뀐 곳
//   SPI.begin + SD.begin(CS, SPI, 20 MHz, "/sd", 5)  →  spi_bus_initialize(SPI2_HOST) + esp_vfs_fat_sdspi_mount("/sd")
//   SD.end()                                         →  esp_vfs_fat_sdcard_unmount + spi_bus_free
// ★ 포맷하지 않는다 (format_if_mount_failed = false). 카드에 항해 기록이 있다.
// ★ 마운트 경로는 아두이노와 같은 "/sd". 파일은 /sd/LOGS/... (출력에 찍는 경로는 /LOGS/...)
// ※ 내릴 때 IDF 드라이버가 "conflict found for GPIO[12]" 경고를 낸다 — 드라이버 안의 동작 (PORTING.md 0단계)

#include "sdcard.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"

#include "board_rak.h"

namespace sdcard {
namespace {
// 주인 칸은 코어 0(기록 일꾼이 놓음)과 코어 1(나머지)이 같이 본다. 짧게 잠근다.
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Owner   gOwner = Owner::None;
Refusal gRefusal = Refusal::None;
sdmmc_card_t* gCard = nullptr;          // 붙어 있으면 null 아님
int gTestFreqKhz = 0;                   // 0 이면 board_rak kSdHz (sdhz 시험 명령)
// SDMMC 1비트는 쓰지 않는다 (09-15 사용자 결정): 모듈의 CMD·DAT0 풀업이 "100K/NC" 로 IDF 요구 10 kΩ 에 못 미친다
//   [확인: rak15002 회로도 · sd_pullup_requirements.rst]. 기록 장비라 가끔 나는 CRC 오류도 안 된다. CHECKLIST 5장.

// SD.begin 과 같은 일. 붙으면 true.
bool mountCard() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = rak::kSPI_MOSI;
    bus.miso_io_num = rak::kSPI_MISO;
    bus.sclk_io_num = rak::kSPI_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    const esp_err_t be = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (be != ESP_OK && be != ESP_ERR_INVALID_STATE) return false;   // INVALID_STATE = 이미 만들어져 있음

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = gTestFreqKhz > 0 ? gTestFreqKhz : (int)(rak::kSdHz / 1000);
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(rak::kSPI_CS);
    slot.host_id = SPI2_HOST;

    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // ★ 절대 포맷하지 않는다
    mcfg.max_files = 5;                    // firmware-rak SD.begin(..., 5) 과 같다
    mcfg.allocation_unit_size = 0;

    sdmmc_card_t* card = nullptr;
    if (esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mcfg, &card) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return false;
    }
    gCard = card;
    return true;
}

// SD.end 와 같은 일.
void unmountCard() {
    if (!gCard) return;
    esp_vfs_fat_sdcard_unmount("/sd", gCard);
    gCard = nullptr;
    spi_bus_free(SPI2_HOST);
}
} // namespace

bool acquire(Owner who) {
    if (who == Owner::None) return false;
    portENTER_CRITICAL(&gMux);
    const Owner cur = gOwner;
    if (cur == Owner::None) gOwner = who;          // 먼저 자리를 잡고, 마운트는 잠금 밖에서
    portEXIT_CRITICAL(&gMux);
    if (cur == who) return true;
    if (cur != Owner::None) { gRefusal = Refusal::Busy; return false; }

    if (!mountCard()) {
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
    unmountCard();
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

// firmware-rak 은 SD.end() 를 그냥 부른다 (안 붙어 있어도 된다). 여기서는 붙어 있을 때만 내린다.
void endForSleep() {
    if (owner() == Owner::None) unmountCard();
}

void setTestFreqKhz(int khz) { gTestFreqKhz = khz > 0 ? khz : 0; }
int  cardFreqKhz() { return gCard ? gCard->real_freq_khz : 0; }

} // namespace sdcard

// diagnostics.cpp `sd` 가 카드 종류·크기를 찍는다 (약한 심볼로 받는다). 안 붙어 있으면 nullptr
sdmmc_card_t* sailSdCard() { return sdcard::gCard; }
