// firmware-idf 0단계 — 뼈대. PORTING.md 의 통과 기준:
//   켜짐 로그 · NVS `sail` 설정이 firmware-rak 이 쓴 값 그대로 읽힘 · GPS 바이트가 들어옴.
//
// ★ NVS 는 **읽기만** 한다. 이 보드에는 firmware-rak 이 저장한 방위 축·자력 보정·세션 번호가 있다.
//   초기화가 실패해도 지우지 않는다 (IDF 예제는 지우고 다시 만드는데, 그러면 설정이 날아간다).

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include <dirent.h>
#include <sys/stat.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_rak.h"   // firmware-rak/include — 같은 핀 정의를 같이 쓴다

static const char* TAG = "sail";

static const char* resetWhy(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "전원 켬";
        case ESP_RST_SW:        return "소프트웨어 재시작";
        case ESP_RST_PANIC:     return "★ 패닉";
        case ESP_RST_INT_WDT:   return "★ 인터럽트 워치독";
        case ESP_RST_TASK_WDT:  return "★ 작업 워치독";
        case ESP_RST_WDT:       return "★ 워치독";
        case ESP_RST_DEEPSLEEP: return "깊은잠에서 깸";
        case ESP_RST_BROWNOUT:  return "★ 전압 떨어짐";
        case ESP_RST_USB:       return "USB (포트 열림)";
        default:                return "기타";
    }
}

static void logBoot() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    ESP_LOGI(TAG, "────────────────────────────────────────");
    ESP_LOGI(TAG, "firmware-idf 0단계 · ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "켜진 이유 %s", resetWhy(esp_reset_reason()));
    ESP_LOGI(TAG, "MAC %02X:%02X:%02X:%02X:%02X:%02X · 코어 %d · 칩 판 %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ci.cores, ci.revision);
    ESP_LOGI(TAG, "플래시 %" PRIu32 " MB · PSRAM %u 바이트", flash / (1024 * 1024),
             (unsigned)esp_psram_get_size());
}

// NVS 이름공간 하나의 키를 전부 적는다. 문자열 값은 길이만 — wifi 에는 비밀번호가 있다.
static void dumpNamespace(const char* ns) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    int n = 0;
    nvs_handle_t h = 0;
    const bool opened = (nvs_open(ns, NVS_READONLY, &h) == ESP_OK);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char val[48] = "?";
        if (opened) {
            switch (info.type) {
                case NVS_TYPE_U8:  { uint8_t v;  if (nvs_get_u8(h, info.key, &v) == ESP_OK)  snprintf(val, sizeof val, "u8 %u", v); break; }
                case NVS_TYPE_I8:  { int8_t v;   if (nvs_get_i8(h, info.key, &v) == ESP_OK)  snprintf(val, sizeof val, "i8 %d", v); break; }
                case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(h, info.key, &v) == ESP_OK) snprintf(val, sizeof val, "u16 %u", v); break; }
                case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(h, info.key, &v) == ESP_OK) snprintf(val, sizeof val, "u32 %" PRIu32, v); break; }
                case NVS_TYPE_I32: { int32_t v;  if (nvs_get_i32(h, info.key, &v) == ESP_OK) snprintf(val, sizeof val, "i32 %" PRIi32, v); break; }
                case NVS_TYPE_STR: { size_t len = 0; nvs_get_str(h, info.key, nullptr, &len); snprintf(val, sizeof val, "문자열 %u 바이트", (unsigned)len); break; }
                case NVS_TYPE_BLOB: {
                    size_t len = 0; nvs_get_blob(h, info.key, nullptr, &len);
                    // Preferences::putFloat 은 4바이트 blob 이다 (아두이노 Preferences.cpp). 이 보드의 방위 오프셋 등이 여기 있다.
                    if (len == 4) { float f; nvs_get_blob(h, info.key, &f, &len); snprintf(val, sizeof val, "blob4 (float %.3f)", f); }
                    else snprintf(val, sizeof val, "blob %u 바이트", (unsigned)len);
                    break;
                }
                default: snprintf(val, sizeof val, "형 %d", info.type);
            }
        }
        ESP_LOGI(TAG, "  NVS %s.%-12s %s", ns, info.key, val);
        ++n;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (opened) nvs_close(h);
    ESP_LOGI(TAG, "  NVS 이름공간 '%s' 키 %d개", ns, n);
}

static void checkNvs() {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // ★ 지우지 않는다. 설정이 날아간다.
        ESP_LOGE(TAG, "NVS 초기화 실패 %s — 지우지 않고 멈춘다", esp_err_to_name(err));
        return;
    }
    dumpNamespace("sail");
    dumpNamespace("wifi");
}

static void sensorPowerOn() {
    gpio_set_direction(static_cast<gpio_num_t>(rak::kSensorPowerA), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(rak::kSensorPowerA), 1);
    ESP_LOGI(TAG, "센서 전원 켬 — GPIO%d HIGH (3V3_S)", rak::kSensorPowerA);
}

// UART1 을 GPS 핀에 한 번만 묶는다.
// ★ 속도를 바꿀 때마다 uart_set_pin 을 다시 부르면 "GPIO 43 is not usable, maybe used by others" 경고가 났다
//   (2026-09-15 0단계 첫 부팅 로그 — 두 번째 부를 때만). 핀은 한 번, 속도는 uart_set_baudrate 로 바꾼다.
static void gpsUartInit() {
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    uart_param_config(UART_NUM_1, &cfg);
    uart_set_pin(UART_NUM_1, rak::kUART1_TX, rak::kUART1_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_1, 4096, 0, 0, nullptr, 0);   // firmware-rak 과 같은 받는 버퍼 4096
}

// GPS 가 말하는지만 본다. 켤 때 모듈이 어느 속도에 있는지 모르니 둘 다 들어 본다 (firmware-rak 도 9600 → 115200).
static void listenGps(int baud, int ms) {
    uart_set_baudrate(UART_NUM_1, baud);
    uart_flush_input(UART_NUM_1);

    static uint8_t buf[512];          // ★ 스택에 안 올린다
    char first[100] = "";
    size_t firstN = 0, total = 0;
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        int got = uart_read_bytes(UART_NUM_1, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (got <= 0) continue;
        total += (size_t)got;
        for (int i = 0; i < got && firstN < sizeof first - 1; ++i) {
            if (firstN == 0 && buf[i] != '$') continue;
            if (buf[i] == '\r' || buf[i] == '\n') { if (firstN) firstN = sizeof first - 1; continue; }
            first[firstN++] = (char)buf[i];
        }
    }
    first[firstN < sizeof first ? strnlen(first, sizeof first - 1) : sizeof first - 1] = '\0';
    ESP_LOGI(TAG, "GPS %d bps %d ms 동안 %u 바이트 · 첫 문장 %s", baud, ms, (unsigned)total,
             first[0] ? first : "(없음)");
}

// ── I2C 훑기 — IMU(0x68)·자력계(0x0C)·화면(0x3C) 이 보이나. firmware-rak 과 같은 400 kHz, SDA 9 · SCL 40 ──
static void scanI2c() {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = -1;
    cfg.sda_io_num = static_cast<gpio_num_t>(rak::kI2C1_SDA);
    cfg.scl_io_num = static_cast<gpio_num_t>(rak::kI2C1_SCL);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = 0;    // 베이스보드에 4.7k 풀업이 있다 (board_rak.h)
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2C 버스 못 만듦 %s", esp_err_to_name(err)); return; }
    char found[200] = "";
    int n = 0;
    for (uint16_t a = 0x08; a < 0x78; ++a) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            size_t len = strlen(found);
            snprintf(found + len, sizeof found - len, " 0x%02X", a);
            ++n;
        }
    }
    ESP_LOGI(TAG, "I2C 훑기 — %d개:%s  (기대: 0x3C 화면 · 0x68 IMU)", n, n ? found : " 없음");
    i2c_del_master_bus(bus);
}

// ── 배터리 — firmware-rak readBatteryVolts 와 같은 조건: 16번 평균 · 2 ms 간격 · 11 dB(=IDF DB_12) · 분압 0.6 ──
//   아두이노 analogReadMilliVolts 는 칩 보정값으로 mV 를 낸다. 여기서는 IDF 보정 방식으로 같은 일을 한다.
static void readBattery() {
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(rak::kBattAdcPin, &unit, &ch) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 는 ADC 핀이 아니다", rak::kBattAdcPin);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = unit;
    adc_oneshot_unit_handle_t adc = nullptr;
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) { ESP_LOGE(TAG, "ADC 단위 못 만듦"); return; }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc, ch, &ccfg);

    adc_cali_handle_t cali = nullptr;
    const char* scheme = "보정 없음";
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {};
    cc.unit_id = unit; cc.chan = ch; cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali) == ESP_OK) scheme = "곡선 보정";
#endif
    if (!cali) {
        ESP_LOGE(TAG, "ADC 보정을 못 만듦 — mV 를 지어내지 않는다");
        adc_oneshot_del_unit(adc);
        return;
    }
    uint32_t sum = 0;
    int ok = 0;
    for (int i = 0; i < 16; ++i) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(adc, cali, ch, &mv) == ESP_OK) { sum += (uint32_t)mv; ++ok; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (ok == 0) { ESP_LOGE(TAG, "배터리 한 번도 못 읽음"); }
    else {
        const uint32_t mv = sum / ok;
        const float volts = (mv / 1000.0f) / rak::kBattDivider * rak::kBattCorrection;
        ESP_LOGI(TAG, "배터리 핀 GPIO%d (ADC%d 채널 %d, %s) — %u mV 평균 %d번 → 배터리 %.3f V",
                 rak::kBattAdcPin, (int)unit + 1, (int)ch, scheme, (unsigned)mv, ok, volts);
    }
    adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc);
}

// ── SD 카드 — 붙여서 /LOGS 목록만 읽는다. firmware-rak 과 같은 SPI 핀·20 MHz ──
//   ★ 쓰지 않는다. 붙이다 실패해도 포맷하지 않는다 (format_if_mount_failed = false) — 카드에 항해 기록이 있다.
//   대조 기준: firmware-rak `rec ls` (2026-09-15 15:1x) — 파일 89개, 49.69 MB, 남은 자리 122002 MB
static void listSd() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = rak::kSPI_MOSI;
    bus.miso_io_num = rak::kSPI_MISO;
    bus.sclk_io_num = rak::kSPI_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { ESP_LOGE(TAG, "SPI 버스 못 만듦 %s", esp_err_to_name(err)); return; }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = rak::kSdHz / 1000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(rak::kSPI_CS);
    slot.host_id = SPI2_HOST;

    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;     // ★ 절대 포맷하지 않는다
    mcfg.max_files = 5;
    mcfg.allocation_unit_size = 0;

    sdmmc_card_t* card = nullptr;
    const int64_t t0 = esp_timer_get_time();
    err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 못 붙임 %s — 포맷하지 않고 그만둔다", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return;
    }
    ESP_LOGI(TAG, "SD 붙음 %.0f ms · %s · %llu MB · %u kHz",
             (esp_timer_get_time() - t0) / 1000.0, card->cid.name,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)),
             (unsigned)card->real_freq_khz);

    DIR* d = opendir("/sd/LOGS");
    if (!d) {
        ESP_LOGE(TAG, "/sd/LOGS 를 못 연다");
    } else {
        int n = 0;
        uint64_t bytes = 0;
        struct dirent* e;
        static char path[300];               // ★ 스택에 안 올린다
        while ((e = readdir(d)) != nullptr) {
            snprintf(path, sizeof path, "/sd/LOGS/%s", e->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            ++n;
            bytes += (uint64_t)st.st_size;
            // 마지막 몇 개만 이름을 보인다 (firmware-rak rec ls 끝부분과 대 보려고)
            if (strstr(e->d_name, "S0009") == e->d_name || strstr(e->d_name, "S00046") == e->d_name) {
                ESP_LOGI(TAG, "  %-34s %8ld 바이트", e->d_name, (long)st.st_size);
            }
        }
        closedir(d);
        ESP_LOGI(TAG, "/LOGS 파일 %d개 · 합쳐서 %.2f MB", n, bytes / 1048576.0);
    }
    uint64_t total = 0, freeB = 0;
    if (esp_vfs_fat_info("/sd", &total, &freeB) == ESP_OK) {
        ESP_LOGI(TAG, "카드 전체 %llu MB · 남은 자리 %llu MB",
                 (unsigned long long)(total / 1048576), (unsigned long long)(freeB / 1048576));
    }
    esp_vfs_fat_sdcard_unmount("/sd", card);
    spi_bus_free(SPI2_HOST);
    ESP_LOGI(TAG, "SD 내림 (읽기만 했다)");
}

// ── 저장 버튼 — GPIO2(J11 1번, AIN1). 누르면 GND 로 떨어진다 ──
static void readButton() {
    const auto pin = static_cast<gpio_num_t>(rak::kAin1);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGI(TAG, "저장 버튼 GPIO%d = %d (1 = 안 누름)", rak::kAin1, gpio_get_level(pin));
}

// ── LED — firmware-rak 과 같게: 초록은 **기록 중일 때만** 1초에 80 ms 깜박인다 (main.cpp loop 1d) ──
//   ★ 0단계 첫 판은 "작업이 도나" 보려고 0.5초마다 늘 깜박였다. 사용자: "rec 중이 아닌데 연두색 불 깜박이네?"
//     기록 표시와 뜻이 섞이므로 지웠다. 기록기를 붙이면 hlog::recording() 으로 켠다.
static void ledsOff() {
    static const int kLeds[] = {rak::kLedGreen, rak::kLedBlue};
    for (int p : kLeds) {
        gpio_set_direction(static_cast<gpio_num_t>(p), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(p), 0);
    }
}

extern "C" void app_main(void) {
    ledsOff();
    logBoot();
    checkNvs();
    sensorPowerOn();
    gpsUartInit();
    vTaskDelay(pdMS_TO_TICKS(1000));                            // GPS 가 켜질 시간
    listenGps(115200, 2000);
    listenGps(9600, 2000);
    scanI2c();
    readBattery();
    readButton();
    listSd();
    ESP_LOGI(TAG, "0단계 끝 — LED 는 꺼 둔다 (기록 중일 때만 초록 깜박임, firmware-rak 과 같게)");
}
