// firmware-idf 2단계 — IMU. firmware-rak src/main.cpp (커밋 2b18b17) 에서 옮겼다.
//   SailImu 129-187 · imuBegin 1794 · 자이로 0점 1862-1951 · imuFast 1957 · FIFO 1981-2066
//   imuUpdate 2127 · accInMagFrame 2245 · doImu 2387 · settleImuGap 3590 · checkSensors 3795 · test imu 4362
// MPU9250_WE 1.2.17 대신 레지스터를 직접 쓴다. 출처는 firmware-rak/.pio/libdeps/rak3112/MPU9250_WE/src:
//   MPU6500_WE.h 226-284 (레지스터·값) · MPU9250_WE.h 121-126, 136-160 (AK8963)
//   MPU6500_WE.cpp: init 122 · setGyrDLPF 232 · setSampleRateDivider 239 · setGyrRange 243
//     enableGyrDLPF 251 · setAccRange 264 · getGValues 329 · getTemperature 357 · getGyrValues 378
//     sleep 391 · startFifo 597 · enableFifo 606 · resetFifo 617 · setFifoMode 628
//     getNumberOfFifoDataSets 640 · correctGyrRawValues 682 · reset 688 · enableI2CMaster 693
//   MPU9250_WE.cpp: initMagnetometer 118 · setMagOpMode 142 · enableMagDataRead 160
//     resetMagnetometer 166 · getAsaVals 171 · writeAK8963Register 181 · readAK8963Register8 187
//     setMagnetometer16Bit 218
// 달라진 점은 imu.h 머리.
#include "imu.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "board_rak.h"
#include "heading_math.h"   // hdg::sanitizeFloat
#include "imu_math.h"
#include "mag_sample.h"

namespace imu {
namespace {

// ── MPU-9250 레지스터 (MPU6500_WE.h) ──────────────────────────────────────
constexpr uint8_t REG_SMPLRT_DIV            = 0x19;
constexpr uint8_t REG_CONFIG                = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG           = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG          = 0x1C;
constexpr uint8_t REG_FIFO_EN               = 0x23;
constexpr uint8_t REG_I2C_MST_CTRL          = 0x24;
constexpr uint8_t REG_I2C_SLV0_ADDR         = 0x25;
constexpr uint8_t REG_I2C_SLV0_REG          = 0x26;
constexpr uint8_t REG_I2C_SLV0_CTRL         = 0x27;
constexpr uint8_t REG_INT_PIN_CFG           = 0x37;
constexpr uint8_t REG_ACCEL_OUT             = 0x3B;
constexpr uint8_t REG_TEMP_OUT              = 0x41;
constexpr uint8_t REG_GYRO_OUT              = 0x43;
constexpr uint8_t REG_EXT_SLV_SENS_DATA_00  = 0x49;
constexpr uint8_t REG_I2C_SLV0_DO           = 0x63;
constexpr uint8_t REG_USER_CTRL             = 0x6A;
constexpr uint8_t REG_PWR_MGMT_1            = 0x6B;
constexpr uint8_t REG_FIFO_COUNT            = 0x72;   // COUNT_H
constexpr uint8_t REG_FIFO_R_W              = 0x74;
constexpr uint8_t REG_WHO_AM_I              = 0x75;

constexpr uint8_t VAL_RESET                 = 0x80;
constexpr uint8_t VAL_BYPASS_EN             = 0x02;
constexpr uint8_t VAL_I2C_MST_EN            = 0x20;
constexpr uint8_t WHO_AM_I_MPU9250          = 0x71;   // MPU9250_WE.h WHO_AM_I_CODE

constexpr float   TEMP_ROOM_OFFSET          = 0.0f;
constexpr float   TEMP_SENSITIVITY          = 333.87f;

// setAccRange / setGyrRange / setGyrDLPF / FIFO 종류 (MPU6500_WE.h enum)
constexpr uint8_t ACC_RANGE_16G             = 3;      // → accRangeFactor 1<<3 = 8
constexpr uint8_t GYRO_RANGE_1000           = 2;      // → gyrRangeFactor 1<<2 = 4
constexpr uint8_t DLPF_6                    = 6;
constexpr uint8_t FIFO_ACC_GYR              = 0x78;
constexpr int     kSetBytes                 = 12;     // FIFO_ACC_GYR 한 벌

// ── AK8963 (MPU9250_WE.h) ────────────────────────────────────────────────
constexpr uint8_t AK_ADDR                   = 0x0C;
constexpr uint8_t AK_WHO_AM_I               = 0x48;
constexpr uint8_t AK_WIA                    = 0x00;
constexpr uint8_t AK_HXL                    = 0x03;
constexpr uint8_t AK_CNTL_1                 = 0x0A;
constexpr uint8_t AK_CNTL_2                 = 0x0B;
constexpr uint8_t AK_ASAX                   = 0x10;
constexpr uint8_t AK_VAL_16_BIT             = 0x10;
constexpr uint8_t AK_VAL_READ               = 0x80;
constexpr uint8_t AK_PWR_DOWN               = 0x00;
constexpr uint8_t AK_CONT_MODE_8HZ          = 0x02;
constexpr uint8_t AK_FUSE_ROM_ACC_MODE      = 0x0F;

constexpr int kI2cTimeoutMs = 50;   // firmware-rak Wire.setTimeOut(50)

// 자이로 0점 판정 (firmware-rak 1862-1864)
constexpr float kGyrCalMaxSpreadDps = 3.8f;
constexpr float kGyrCalMaxBiasDps   = 5.0f;
constexpr float kGyrBootMaxJumpDps  = 1.0f;

uint32_t millis32() { return (uint32_t)(esp_timer_get_time() / 1000); }
void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// ── 버스 ─────────────────────────────────────────────────────────────────
i2c_master_bus_handle_t sBus = nullptr;
i2c_master_dev_handle_t sDev = nullptr;
uint32_t sI2cErr    = 0;
bool     sResting   = false;   // I2C 가 방금 실패했다 → 50 ms 쉰다
uint32_t sRestAt    = 0;

void noteErr() { ++sI2cErr; sResting = true; sRestAt = millis32(); }
bool resting() {
    if (!sResting) return false;
    if (millis32() - sRestAt < 50) return true;
    sResting = false;
    return false;
}

bool wr(uint8_t reg, uint8_t val) {
    if (!sDev) return false;
    const uint8_t b[2] = {reg, val};
    if (i2c_master_transmit(sDev, b, 2, kI2cTimeoutMs) != ESP_OK) { noteErr(); return false; }
    return true;
}
bool rdN(uint8_t reg, uint8_t* out, size_t n) {
    if (!sDev) return false;
    if (i2c_master_transmit_receive(sDev, &reg, 1, out, n, kI2cTimeoutMs) != ESP_OK) { noteErr(); return false; }
    return true;
}
uint8_t rd8(uint8_t reg) {
    uint8_t v = 0;
    if (!rdN(reg, &v, 1)) return 0;
    return v;
}
int16_t rd16(uint8_t reg) {   // MSB 먼저 (readMPU9250Register16)
    uint8_t b[2] = {0, 0};
    if (!rdN(reg, b, 2)) return 0;
    return (int16_t)((b[0] << 8) + b[1]);
}

// ── 라이브러리 상태 (MPU6500_WE 멤버) ─────────────────────────────────────
int   sAccRangeFactor = 1;
int   sGyrRangeFactor = 1;
float sLibGyrOff[3]   = {0.0f, 0.0f, 0.0f};   // gyrOffsetVal (±250 원시 단위)

// ── firmware-rak 전역 ────────────────────────────────────────────────────
bool     sImuOk = false;
bool     sMagOk = false;
uint8_t  sMagPrev[6] = {0};
bool     sMagHavePrev = false;
mag::Freshness sMagFreshness;
uint32_t sMagCount[5] = {0};
Vec      sAcc, sGyr, sMag, sMagRaw;
float    sTempC = 0.0f;
float    sGyrOff[3] = {0.0f, 0.0f, 0.0f};
bool     sGyrNeedSave = false;
float    sMagOff[3] = {0.0f, 0.0f, 0.0f};
float    sMagRadius = 0.0f;
float    sMagResid  = 0.0f;
float    sAsa[3]    = {1.0f, 1.0f, 1.0f};
uint8_t  sAsaRaw[3] = {0, 0, 0};

bool     sFifoOn      = false;
uint32_t sImuTickMs   = 0;
uint32_t sFifoOverrun = 0;
uint32_t sFifoSets    = 0;
int      sPending     = 0;
uint32_t sDropped     = 0;

uint32_t sLostAt     = 0;   // 0 이면 붙어 있다
uint32_t sPauseUntil = 0;   // test imu

// ── MPU6500_WE / MPU9250_WE 함수 ─────────────────────────────────────────

void chipSleep(bool on) {
    uint8_t r = rd8(REG_PWR_MGMT_1);
    if (on) r |= 0x40; else r &= ~(0x40);
    wr(REG_PWR_MGMT_1, r);
}

bool mpuInit() {
    wr(REG_PWR_MGMT_1, VAL_RESET);          // reset_MPU9250
    delayMs(10);
    delayMs(10);
    wr(REG_INT_PIN_CFG, VAL_BYPASS_EN);
    delayMs(10);
    if (rd8(REG_WHO_AM_I) != WHO_AM_I_MPU9250) return false;
    sAccRangeFactor = 1;
    sGyrRangeFactor = 1;
    sLibGyrOff[0] = sLibGyrOff[1] = sLibGyrOff[2] = 0.0f;
    chipSleep(false);
    return true;
}

void setAccRange16() {
    uint8_t r = rd8(REG_ACCEL_CONFIG);
    r &= 0xE7;
    r |= (ACC_RANGE_16G << 3);
    wr(REG_ACCEL_CONFIG, r);
    sAccRangeFactor = 1 << ACC_RANGE_16G;
}

void setGyrRange1000() {
    uint8_t r = rd8(REG_GYRO_CONFIG);
    r &= 0xE7;
    r |= (GYRO_RANGE_1000 << 3);
    wr(REG_GYRO_CONFIG, r);
    sGyrRangeFactor = 1 << GYRO_RANGE_1000;
}

void enableGyrDLPF() {
    uint8_t r = rd8(REG_GYRO_CONFIG);
    r &= 0xFC;
    wr(REG_GYRO_CONFIG, r);
}

void setGyrDLPF(uint8_t dlpf) {
    uint8_t r = rd8(REG_CONFIG);
    r &= 0xF8;
    r |= dlpf;
    wr(REG_CONFIG, r);
}

void setSampleRateDivider(uint8_t div) { wr(REG_SMPLRT_DIV, div); }

void enableI2CMaster() {
    uint8_t r = rd8(REG_USER_CTRL);
    r |= VAL_I2C_MST_EN;
    wr(REG_USER_CTRL, r);
    wr(REG_I2C_MST_CTRL, 0x00);             // 400 kHz
    delayMs(10);
}

void enableMagDataRead(uint8_t reg, uint8_t bytes) {
    wr(REG_I2C_SLV0_ADDR, AK_ADDR | AK_VAL_READ);
    wr(REG_I2C_SLV0_REG, reg);
    wr(REG_I2C_SLV0_CTRL, 0x80 | bytes);
    delayMs(10);
}

void writeAK(uint8_t reg, uint8_t val) {
    wr(REG_I2C_SLV0_ADDR, AK_ADDR);
    wr(REG_I2C_SLV0_REG, reg);
    wr(REG_I2C_SLV0_DO, val);
}

uint8_t readAK8(uint8_t reg) {
    enableMagDataRead(reg, 0x01);
    const uint8_t v = rd8(REG_EXT_SLV_SENS_DATA_00);
    enableMagDataRead(AK_HXL, 0x08);
    return v;
}

void setMagOpMode(uint8_t mode) {
    uint8_t r = readAK8(AK_CNTL_1);
    r &= 0xF0;
    r |= mode;
    writeAK(AK_CNTL_1, r);
    delayMs(10);
    if (mode != AK_PWR_DOWN) enableMagDataRead(AK_HXL, 0x08);
}

bool initMagnetometer() {
    enableI2CMaster();
    writeAK(AK_CNTL_2, 0x01);               // resetMagnetometer
    delayMs(100);
    if (readAK8(AK_WIA) != AK_WHO_AM_I) return false;
    setMagOpMode(AK_FUSE_ROM_ACC_MODE);
    delayMs(10);
    // getAsaVals — 라이브러리는 여기서 ASA 를 읽어 getMagValues 에 쓴다. firmware-rak 은 그 값을 안 쓰고
    // loadAsa 로 다시 읽는다. 칩에 가는 순서를 같게 두려고 읽기만 한다.
    for (int i = 0; i < 3; ++i) (void)readAK8((uint8_t)(AK_ASAX + i));
    delayMs(10);
    {
        uint8_t r = readAK8(AK_CNTL_1);       // setMagnetometer16Bit
        r |= AK_VAL_16_BIT;
        writeAK(AK_CNTL_1, r);
    }
    delayMs(10);
    setMagOpMode(AK_CONT_MODE_8HZ);
    delayMs(10);
    return true;
}

// SailImu::loadAsa — Fuse ROM 모드로 들어가 읽고 연속 8 Hz 로 되돌린다 (mag_sample.h)
bool loadAsa() {
    uint8_t raw[3] = {0, 0, 0};
    const bool good = mag::readAsaFuseRom(
        [](uint8_t m) { setMagOpMode(m); },
        [](uint8_t r) { return readAK8(r); },
        AK_PWR_DOWN, AK_FUSE_ROM_ACC_MODE, AK_CONT_MODE_8HZ, AK_ASAX, raw);
    for (int i = 0; i < 3; ++i) {
        sAsaRaw[i] = raw[i];
        sAsa[i] = good ? mag::asaFactor(raw[i]) : 1.0f;
    }
    return good;
}

// SailImu::readMagMirror — HXL..CNTL1 8바이트. 읽은 바이트 수.
uint8_t readMagMirror(uint8_t* b) {
    return rdN(REG_EXT_SLV_SENS_DATA_00, b, 8) ? 8 : 0;
}

Vec be3(const uint8_t* b) {
    Vec v;
    v.x = (float)(int16_t)((b[0] << 8) | b[1]);
    v.y = (float)(int16_t)((b[2] << 8) | b[3]);
    v.z = (float)(int16_t)((b[4] << 8) | b[5]);
    return v;
}

// getGValues: 가속 오프셋은 firmware-rak 이 한 번도 안 건다 (0) → raw × factor / 16384
Vec accRawToG(const Vec& raw) {
    const float k = (float)sAccRangeFactor / 16384.0f;
    return Vec{raw.x * k, raw.y * k, raw.z * k};
}

// getGyrValues: (raw − offset/factor) × factor × 250 / 32768
Vec gyrRawToDps(const Vec& raw) {
    const float f = (float)sGyrRangeFactor;
    const float k = f * 250.0f / 32768.0f;
    return Vec{(raw.x - sLibGyrOff[0] / f) * k,
               (raw.y - sLibGyrOff[1] / f) * k,
               (raw.z - sLibGyrOff[2] / f) * k};
}

Vec getGValues() {
    uint8_t b[6] = {0};
    rdN(REG_ACCEL_OUT, b, 6);
    return accRawToG(be3(b));
}

Vec getGyrRawValues() {
    uint8_t b[6] = {0};
    rdN(REG_GYRO_OUT, b, 6);
    return be3(b);
}

void enableFifo(bool on) {
    uint8_t r = rd8(REG_USER_CTRL);
    if (on) r |= 0x40; else r &= ~(0x40);
    wr(REG_USER_CTRL, r);
}

void setFifoModeContinuous() {             // setFifoMode(MPU9250_CONTINUOUS = 0)
    uint8_t r = rd8(REG_CONFIG);
    r &= ~(0x40);
    wr(REG_CONFIG, r);
}

void resetFifo() {
    uint8_t r = rd8(REG_USER_CTRL);
    r |= 0x04;
    wr(REG_USER_CTRL, r);
}

void startFifoAccGyr() { wr(REG_FIFO_EN, FIFO_ACC_GYR); }

int16_t numberOfFifoSets() {
    int16_t n = (int16_t)(uint16_t)rd16(REG_FIFO_COUNT);
    return (int16_t)(n / kSetBytes);
}

// ── firmware-rak 함수 ────────────────────────────────────────────────────

void applyGyrOffsets() {
    if (!sImuOk) return;
    sLibGyrOff[0] = sGyrOff[0];
    sLibGyrOff[1] = sGyrOff[1];
    sLibGyrOff[2] = sGyrOff[2];
}

bool imuBegin() {
    if (!begin(nullptr)) return false;
    sImuOk = mpuInit();
    if (!sImuOk) return false;
    // ★ autoOffsets() 는 안 부른다 — 배 위에서 켜면 그때 기울기가 0 이 된다.
    // 범위: ±16 g · ±1000 °/s (저장 단위 1 mg · 1/32 °/s 에 맞춘다, firmware-rak 1805-1820)
    setAccRange16();
    setGyrRange1000();
    enableGyrDLPF();
    setGyrDLPF(DLPF_6);           // 가장 조용한 설정
    setSampleRateDivider(9);      // 1000/(1+9) = 100 Hz

    sMagOk = initMagnetometer();
    sMagHavePrev = false;
    sMagFreshness.reset();
    if (sMagOk && !loadAsa()) {
        printf("[IMU] ★ 자력계 공장 감도값(ASA)을 못 읽었습니다 (%02X %02X %02X) — 계수 1 로 씁니다\n",
               sAsaRaw[0], sAsaRaw[1], sAsaRaw[2]);
    }
    return true;
}

void imuFifoBegin() {
    if (!sImuOk) return;
    setSampleRateDivider(9);
    enableFifo(true);
    setFifoModeContinuous();
    resetFifo();
    startFifoAccGyr();
    sFifoOn = true;
    sPending = 0;
    sImuTickMs = millis32();
    printf("[IMU] FIFO 켜짐 — 칩이 100 Hz 로 떠서 쌓습니다 (등간격)\n");
}

void imuFast() {
    if (!sImuOk) return;
    sAcc = getGValues();
    sGyr = gyrRawToDps(getGyrRawValues());
}

float getF(nvs_handle_t h, const char* key, float def) {   // Preferences::getFloat (4바이트 blob)
    float v = def;
    size_t len = 0;
    if (nvs_get_blob(h, key, nullptr, &len) != ESP_OK || len != sizeof(float)) return def;
    if (nvs_get_blob(h, key, &v, &len) != ESP_OK) return def;
    return v;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

bool begin(i2c_master_bus_handle_t sharedBus) {
    if (sDev) return true;
    if (!sBus) {
        if (sharedBus) {
            sBus = sharedBus;
        } else {
            i2c_master_bus_config_t cfg = {};
            cfg.i2c_port = -1;
            cfg.sda_io_num = static_cast<gpio_num_t>(rak::kI2C1_SDA);
            cfg.scl_io_num = static_cast<gpio_num_t>(rak::kI2C1_SCL);
            cfg.clk_source = I2C_CLK_SRC_DEFAULT;
            cfg.glitch_ignore_cnt = 7;
            cfg.flags.enable_internal_pullup = 0;   // 베이스보드 4.7k 풀업 (board_rak.h)
            const esp_err_t e = i2c_new_master_bus(&cfg, &sBus);
            if (e != ESP_OK) {
                sBus = nullptr;
                printf("[IMU] ★ I2C 버스 못 만듦 %s\n", esp_err_to_name(e));
                return false;
            }
        }
    }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = rak::kAddrImu;
    dc.scl_speed_hz    = 400000;
    const esp_err_t e = i2c_master_bus_add_device(sBus, &dc, &sDev);
    if (e != ESP_OK) {
        sDev = nullptr;
        printf("[IMU] ★ I2C 장치 0x%02X 못 붙임 %s\n", rak::kAddrImu, esp_err_to_name(e));
        return false;
    }
    return true;
}

i2c_master_bus_handle_t bus() { return sBus; }

int loadSettings() {
    sGyrOff[0] = sGyrOff[1] = sGyrOff[2] = 0.0f;
    sMagOff[0] = sMagOff[1] = sMagOff[2] = 0.0f;
    sMagRadius = sMagResid = 0.0f;
    uint8_t gyrUnit = 0;
    nvs_handle_t h = 0;
    if (nvs_open("sail", NVS_READONLY, &h) == ESP_OK) {
        sMagOff[0] = getF(h, "mag_ox", 0.0f);
        sMagOff[1] = getF(h, "mag_oy", 0.0f);
        sMagOff[2] = getF(h, "mag_oz", 0.0f);
        sMagRadius = getF(h, "mag_r",  0.0f);
        sMagResid  = getF(h, "mag_res", 0.0f);
        sGyrOff[0] = getF(h, "gyr_x", 0.0f);
        sGyrOff[1] = getF(h, "gyr_y", 0.0f);
        sGyrOff[2] = getF(h, "gyr_z", 0.0f);
        if (nvs_get_u8(h, "gyr_u", &gyrUnit) != ESP_OK) gyrUnit = 0;
        nvs_close(h);
    }
    // 옛 단위(±1000 범위 원시값)로 적힌 0점을 ±250 원시 단위로 옮긴다. 저장은 켤 때 saveGyrOffsets.
    if (gyrUnit != 2 && (sGyrOff[0] != 0.0f || sGyrOff[1] != 0.0f || sGyrOff[2] != 0.0f)) {
        for (int i = 0; i < 3; ++i) sGyrOff[i] = imu::migrateLegacyOffset(sGyrOff[i]);
        sGyrNeedSave = true;
    }
    // 범위 검사 (firmware-rak 430-442 의 IMU 줄)
    int fixed = 0;
    bool magBad = false;
    for (int i = 0; i < 3; ++i) magBad |= hdg::sanitizeFloat(&sMagOff[i], -500.0f, 500.0f, 0.0f);
    if (magBad) { sMagOff[0] = sMagOff[1] = sMagOff[2] = 0.0f; sMagRadius = sMagResid = 0.0f; ++fixed; }
    fixed += hdg::sanitizeFloat(&sMagRadius, 0.0f, 500.0f, 0.0f);
    fixed += hdg::sanitizeFloat(&sMagResid,  0.0f, 500.0f, 0.0f);
    for (int i = 0; i < 3; ++i) fixed += hdg::sanitizeFloat(&sGyrOff[i], -32768.0f, 32768.0f, 0.0f);
    return fixed;
}

bool gyrNeedSave() { return sGyrNeedSave; }

bool saveGyrOffsets() {
    nvs_handle_t h = 0;
    if (nvs_open("sail", NVS_READWRITE, &h) != ESP_OK) return false;
    bool good = nvs_set_blob(h, "gyr_x", &sGyrOff[0], sizeof(float)) == ESP_OK &&
                nvs_set_blob(h, "gyr_y", &sGyrOff[1], sizeof(float)) == ESP_OK &&
                nvs_set_blob(h, "gyr_z", &sGyrOff[2], sizeof(float)) == ESP_OK &&
                nvs_set_u8(h, "gyr_u", 2) == ESP_OK;           // 2 = ±250 원시 단위
    if (good) good = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (good) sGyrNeedSave = false;
    return good;
}

void setGyrOffsets(float x, float y, float z) {
    sGyrOff[0] = x; sGyrOff[1] = y; sGyrOff[2] = z;
    applyGyrOffsets();
}
void gyrOffsets(float* x, float* y, float* z) {
    if (x) *x = sGyrOff[0];
    if (y) *y = sGyrOff[1];
    if (z) *z = sGyrOff[2];
}
void setMagOffset(float ox, float oy, float oz, float radius, float resid) {
    sMagOff[0] = ox; sMagOff[1] = oy; sMagOff[2] = oz;
    sMagRadius = radius; sMagResid = resid;
}
const float* magOffset() { return sMagOff; }
float magRadius() { return sMagRadius; }
float magResid() { return sMagResid; }

// ★ 옛 재연결은 imuBegin() 만 불러서 FIFO 가 꺼진 채 imuOk 만 참이었다. 전부 여기를 탄다.
bool attach(const char* why) {
    sFifoOn = false;
    if (!imuBegin()) return false;
    applyGyrOffsets();
    imuFifoBegin();
    delayMs(30);                                // 100 Hz 로 두세 벌 쌓일 시간
    const Vec a = getGValues();
    const float n = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    if (!std::isfinite(n) || n < 0.3f || n > 4.0f) {
        sImuOk = false; sMagOk = false; sFifoOn = false;
        printf("[IMU] ★ 붙었지만 첫 값이 이상합니다 (|a| = %.2f g) — 다음에 다시 봅니다\n", n);
        return false;
    }
    resetFifo();
    sPending = 0;
    sImuTickMs = millis32();
    printf("[IMU] 붙음 (%s) — 자력계 %s, |a| %.2f g\n", why, sMagOk ? "OK" : "응답 없음", n);
    return true;
}

// 지금 자이로 값을 0 으로 삼는다. 보드가 멈춰 있어야 한다.
//   persist = true   calib 명령. NVS 에 적는다
//   persist = false  부팅. 램에만. 저장값과 1 °/s 넘게 다르면 안 쓴다 (돌고 있었을 수 있음)
bool calibrateGyro(bool persist) {
    if (!sImuOk) return false;

    sLibGyrOff[0] = sLibGyrOff[1] = sLibGyrOff[2] = 0.0f;   // 보정을 지우고 날값을 본다

    const int kN = 64;
    float sx = 0, sy = 0, sz = 0;
    float mnx = 1e9f, mny = 1e9f, mnz = 1e9f;
    float mxx = -1e9f, mxy = -1e9f, mxz = -1e9f;

    for (int i = 0; i < kN; i++) {
        const Vec r = getGyrRawValues();
        sx += r.x; sy += r.y; sz += r.z;
        if (r.x < mnx) mnx = r.x;
        if (r.x > mxx) mxx = r.x;
        if (r.y < mny) mny = r.y;
        if (r.y > mxy) mxy = r.y;
        if (r.z < mnz) mnz = r.z;
        if (r.z > mxz) mxz = r.z;
        delayMs(5);
    }

    const int rf = imu::kGyrRangeFactor1000;
    float spread = mxx - mnx;
    if (mxy - mny > spread) spread = mxy - mny;
    if (mxz - mnz > spread) spread = mxz - mnz;
    const float spreadDps = imu::rawToDps(spread, rf);
    const float mx = sx / kN, my = sy / kN, mz = sz / kN;
    float biasDps = fabsf(imu::rawToDps(mx, rf));
    if (fabsf(imu::rawToDps(my, rf)) > biasDps) biasDps = fabsf(imu::rawToDps(my, rf));
    if (fabsf(imu::rawToDps(mz, rf)) > biasDps) biasDps = fabsf(imu::rawToDps(mz, rf));

    if (!imu::gyrCalAcceptable(spreadDps, biasDps, kGyrCalMaxSpreadDps, kGyrCalMaxBiasDps)) {
        applyGyrOffsets();   // 이전 보정을 되돌린다
        printf("[IMU] 자이로 0점 안 씀 — 흔들림 %.2f °/s (한계 %.1f) · 평균 %.2f °/s (한계 %.1f)\n",
               spreadDps, kGyrCalMaxSpreadDps, biasDps, kGyrCalMaxBiasDps);
        return false;
    }

    const float ox = imu::gyrOffsetForLib(mx, rf);
    const float oy = imu::gyrOffsetForLib(my, rf);
    const float oz = imu::gyrOffsetForLib(mz, rf);

    if (!persist && (sGyrOff[0] != 0.0f || sGyrOff[1] != 0.0f || sGyrOff[2] != 0.0f)) {
        float jump = fabsf(imu::libOffsetToDps(ox - sGyrOff[0]));
        if (fabsf(imu::libOffsetToDps(oy - sGyrOff[1])) > jump) jump = fabsf(imu::libOffsetToDps(oy - sGyrOff[1]));
        if (fabsf(imu::libOffsetToDps(oz - sGyrOff[2])) > jump) jump = fabsf(imu::libOffsetToDps(oz - sGyrOff[2]));
        if (jump > kGyrBootMaxJumpDps) {
            applyGyrOffsets();
            printf("[IMU] 부팅 때 잰 자이로 0점이 저장값과 %.2f °/s 다릅니다 — 저장값을 씁니다 "
                   "(돌고 있었을 수 있음. 멈춘 뒤 calib)\n", jump);
            return false;
        }
    }

    sGyrOff[0] = ox; sGyrOff[1] = oy; sGyrOff[2] = oz;
    applyGyrOffsets();

    bool saved = false;
    if (persist) {
        saved = saveGyrOffsets();
        if (!saved) printf("[IMU] ★ 자이로 0점을 보드에 못 적었습니다 — 껐다 켜면 옛 값\n");
    }
    printf("[IMU] 자이로 0점 %s — %.2f %.2f %.2f °/s 만큼 빼둡니다 (흔들림 %.2f °/s)\n",
           persist ? (saved ? "잡고 저장" : "잡음(저장 실패)") : "잡음(이번 부팅만)",
           imu::libOffsetToDps(ox), imu::libOffsetToDps(oy), imu::libOffsetToDps(oz),
           spreadDps);
    return true;
}

// ── 등간격 100 Hz — 칩 FIFO ──────────────────────────────────────────────
// 시각은 10 ms 씩 더해 나가되, 실제 시각과 200 ms 넘게 벌어지면 다시 맞춘다.
int fifoBegin() {
    sPending = 0;
    if (!sImuOk || !sFifoOn) return 0;
    if (resting()) return 0;
    const int16_t sets = numberOfFifoSets();
    if (sets <= 0) return 0;

    const uint32_t nowMs = millis32();

    // 42벌이 꽉 차면 오래된 것부터 덮어써서 벌 경계가 어긋난다. 그 전에 비우고 센다.
    if (sets >= (int16_t)kFifoOverrunSets) {
        resetFifo();
        sImuTickMs = nowMs;
        ++sFifoOverrun;
        sDropped += (uint32_t)sets;
        return 0;
    }

    const uint32_t oldest = nowMs - (uint32_t)(sets - 1) * 10u;
    const int32_t  gap    = (int32_t)(oldest - sImuTickMs);
    if (sImuTickMs == 0 || gap > 200 || gap < -200) sImuTickMs = oldest;

    sPending = sets;
    return sets;
}

bool fifoNext(uint32_t* tickMs) {
    if (sPending <= 0) return false;
    uint8_t b[kSetBytes];
    if (!rdN(REG_FIFO_R_W, b, sizeof b)) {
        // 몇 바이트를 꺼냈는지 모른다 → 비우고 경계를 다시 맞춘다. 남은 벌은 버린 것으로 센다.
        sDropped += (uint32_t)sPending;
        sPending = 0;
        resetFifo();
        sImuTickMs = millis32();
        return false;
    }
    --sPending;
    sAcc = accRawToG(be3(b));           // 순서 중요: 가속 6바이트 먼저
    sGyr = gyrRawToDps(be3(b + 6));     // 그다음 자이로 6바이트
    if (tickMs) *tickMs = sImuTickMs;
    sImuTickMs += 10;
    ++sFifoSets;
    return true;
}

int drainFifo() {
    int n = 0;
    fifoBegin();
    while (fifoNext(nullptr)) ++n;
    return n;
}

uint32_t takeDroppedRows() {
    const uint32_t d = sDropped;
    sDropped = 0;
    return d;
}

// 카드를 마운트하는 동안 FIFO 에 옛 값이 쌓인다. 비우고 기록을 시작한다.
void resetFifoForRecording() {
    if (!sFifoOn) return;
    resetFifo();
    sPending = 0;
    sImuTickMs = millis32();
    sFifoOverrun = 0;
}

// 자력계까지. 10 Hz. 자력계(AK8963)는 8 Hz 라 여기서만 읽는다.
bool update() {
    if (!sImuOk) return false;
    if (resting()) return false;
    if (!sFifoOn) imuFast();   // FIFO 가 켜져 있으면 가속·자이로는 거기서 온다
    bool fresh = false;
    if (sMagOk) {
        // 거울 8바이트를 한 번에 읽고 검사한다. 같은 값도 정상 읽기다.
        uint8_t b[8] = {0};
        const uint8_t got = readMagMirror(b);
        const mag::Check ck = mag::check(got, b, sMagPrev, sMagHavePrev);
        ++sMagCount[(int)ck];
        sMagFreshness.update(ck, millis32());
        if (ck == mag::Check::New) {
            memcpy(sMagPrev, b, 6);
            sMagHavePrev = true;
            sMagRaw.x = mag::toMicroTesla(mag::le16(b),     sAsa[0]);
            sMagRaw.y = mag::toMicroTesla(mag::le16(b + 2), sAsa[1]);
            sMagRaw.z = mag::toMicroTesla(mag::le16(b + 4), sAsa[2]);
            // 치우침을 여기서 뺀다. 아래로 가는 모든 것(방위·화면·기록)이 뺀 값을 쓴다.
            sMag.x = sMagRaw.x - sMagOff[0];
            sMag.y = sMagRaw.y - sMagOff[1];
            sMag.z = sMagRaw.z - sMagOff[2];
            fresh = true;
        }
    }
    return fresh;
}

// 1 Hz. 사라진 칩은 끄고, 돌아온 칩은 다시 붙인다.
Presence checkPresence(uint32_t* gapFromMs) {
    static uint32_t lastTry = 0;
    const bool present = sBus && i2c_master_probe(sBus, rak::kAddrImu, kI2cTimeoutMs) == ESP_OK;
    // 시험(test imu <초>) 중에는 칩이 대답해도 없는 것으로 본다
    const bool imuNow = present && (int32_t)(millis32() - sPauseUntil) >= 0;
    if (sImuOk && !imuNow) {
        sImuOk = false;
        sMagOk = false;
        sFifoOn = false;
        sPending = 0;
        sLostAt = millis32();
        printf("[IMU] 응답이 끊겼습니다 — 힐·9축은 무효로 내보냅니다\n");
        return Presence::Lost;
    }
    if (!sImuOk && imuNow && millis32() - lastTry >= 5000) {
        lastTry = millis32();      // 붙이기가 0.3초 붙잡으니 5초에 한 번만
        if (attach("다시 연결")) {
            if (gapFromMs) *gapFromMs = sLostAt;
            sLostAt = 0;
            return Presence::Reattached;
        }
    }
    return Presence::NoChange;
}

uint32_t moveLostAt(uint32_t untilMs) {
    const uint32_t from = sLostAt;
    if (from) sLostAt = untilMs;
    return from;
}

void testPause(long sec) {
    if (sec < 0) sec = 0;
    if (sec > 120) sec = 120;
    sPauseUntil = millis32() + (uint32_t)sec * 1000;
    printf("[TEST] IMU 가 %ld초 동안 없는 척합니다 (1초 안에 끊김으로 잡힙니다)\n", sec);
}

void sleep(bool on) { if (sImuOk) chipSleep(on); }

float readTempC() {
    const int16_t raw = rd16(REG_TEMP_OUT);
    sTempC = (float)((raw * 1.0 - TEMP_ROOM_OFFSET) / TEMP_SENSITIVITY + 21.0);
    return sTempC;
}

bool ok() { return sImuOk; }
bool magOk() { return sMagOk; }
bool magFresh() { return sMagOk && sMagFreshness.usable(millis32()); }
bool fifoOn() { return sFifoOn; }
const Vec& acc() { return sAcc; }
const Vec& gyr() { return sGyr; }
const Vec& mag() { return sMag; }
const Vec& magRaw() { return sMagRaw; }
float tempC() { return sTempC; }

// 한 칩인데 자력계만 따로 든 칩(AK8963)이라 축이 다르다 [확인: MPU-9250 데이터시트 + 세션 27 실측, firmware-rak 2240]
void accInMagFrame(float out[3]) {
    out[0] =  sAcc.y;
    out[1] =  sAcc.x;
    out[2] = -sAcc.z;
}
void gyrInMagFrame(float out[3]) {
    out[0] =  sGyr.y;
    out[1] =  sGyr.x;
    out[2] = -sGyr.z;
}

float   asa(int i)    { return (i >= 0 && i < 3) ? sAsa[i] : 1.0f; }
uint8_t asaRaw(int i) { return (i >= 0 && i < 3) ? sAsaRaw[i] : 0; }
uint32_t magCount(int check) { return (check >= 0 && check < 5) ? sMagCount[check] : 0; }
uint32_t fifoOverrun() { return sFifoOverrun; }
uint32_t fifoSets() { return sFifoSets; }
uint32_t tickMs() { return sImuTickMs; }
uint32_t i2cErrors() { return sI2cErr; }

bool diagBegin() {
    printf("──────────────────────────────────────────\n");
    printf("  RAK1905 IMU — I2C 0x%02X (슬롯 C)\n", rak::kAddrImu);
    if (!sImuOk) {
        printf("  아직 안 붙었습니다. 다시 붙여 봅니다...\n");
        if (!attach("imu 명령")) {
            printf("  여전히 응답 없음. 모듈이 덜 꽂혔는지 보세요.\n");
            printf("──────────────────────────────────────────\n");
            return false;
        }
    }
    printf("  가속도·자이로  OK\n");
    printf("  자력계         %s\n", sMagOk ? "OK" : "응답 없음");
    readTempC();
    printf("  칩 온도        %.1f °C\n", sTempC);
    printf("  5초 동안 값을 보여줍니다. 보드를 좌우로 기울여 보세요.\n");
    printf("  ─────────────────────────────────────\n");
    return true;
}

void diagEnd() {
    printf("──────────────────────────────────────────\n");
    printf("  가만히 뒀을 때 가속도 세 축을 합치면 1 g 이면 정상입니다.\n");
    printf("  기울여서 roll 이 따라 움직이면 힐 실측을 붙일 수 있습니다.\n");
}

bool printRaw() {
    if (!sImuOk) {
        printf("   9축  IMU 없음\n");
        return false;
    }
    printf("   9축  가속 %+5.2f %+5.2f %+5.2f g", sAcc.x, sAcc.y, sAcc.z);
    printf(" | 자이로 %+7.1f %+7.1f %+7.1f °/s", sGyr.x, sGyr.y, sGyr.z);
    if (sMagOk) printf(" | 자력 %+6.1f %+6.1f %+6.1f µT", sMag.x, sMag.y, sMag.z);
    else        printf(" | 자력계 없음");
    return true;
}

} // namespace imu
