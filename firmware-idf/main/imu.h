// firmware-idf 2단계 — IMU (RAK1905 / MPU-9250 + AK8963, 슬롯 C, I2C 0x68)
//
// firmware-rak src/main.cpp 의 IMU 부분과 MPU9250_WE 1.2.17 을 **driver/i2c_master.h 로 직접** 옮겼다.
// 레지스터·값·순서는 firmware-rak/.pio/libdeps/rak3112/MPU9250_WE/src 에서 읽었다 (imu.cpp 에 줄 번호).
// 단위·무효 표식은 firmware-rak 과 같다: 가속 g · 자이로 °/s · 자력 µT.
// 루프가 부르는 모양이다. 콜백·ISR 에서 일하지 않는다.
//
//   켤 때        imu::loadSettings() (NVS 가 열린 뒤) → imu::attach("부팅") → imu::calibrateGyro(false)
//   10 ms 마다   n = imu::fifoBegin(); while (imu::fifoNext(&ms)) { 방위 적분 · 기록 }   (= imuDrainFifo)
//                imu::takeDroppedRows() 를 hlog::noteDropped 에 넘긴다
//   100 ms 마다  imu::update()          자력계까지 (= imuUpdate). 참이면 새 자력 표본 → magCalCollect
//   1 Hz         imu::checkPresence()   끊김·다시 붙이기 (= checkSensors 의 IMU 부분)
//
// ── firmware-rak → 여기 ──────────────────────────────────────────────────
//
//   firmware-rak (main.cpp)             여기
//   ──────────────────────────────      ───────────────────────────────────────
//   gImu (SailImu : MPU9250_WE)         (없음 — imu.cpp 안의 레지스터 함수)
//   Wire.begin(9, 40, 400000)           imu::begin(bus)  — 버스를 받거나 만든다. imu::bus() 로 화면과 나눠 쓴다
//   gImuOk · gMagOk                     imu::ok() · imu::magOk()
//   magFresh()                          imu::magFresh()
//   gAcc · gGyr · gMag · gMagRaw        imu::acc() · gyr() · mag() · magRaw()      (imu::Vec = xyzFloat 자리)
//   gImuTempC · noteImuTemp(getTemperature())  imu::tempC() · imu::readTempC()
//   gGyrOffX/Y/Z                        imu::gyrOffsets() · imu::setGyrOffsets()   (±250 원시 단위, imu_math.h)
//   gGyrNeedSave · saveGyrOffsets()     imu::gyrNeedSave() · imu::saveGyrOffsets()
//   loadSettings 의 gyr_* · mag_* 줄     imu::loadSettings()  (범위 밖으로 고친 개수를 돌려준다)
//   gMagOff[3] · gMagRadius · gMagResid imu::magOffset() · magRadius() · magResid() / imu::setMagOffset()
//   gMagCount[5]                        imu::magCount(i)     (mag::Check 순서)
//   gImu.asa(i) · gImu.asaRaw(i)        imu::asa(i) · imu::asaRaw(i)
//   gMagPrev · gMagHavePrev · gMagFreshness  (imu.cpp 안)
//   imuBegin() · imuFifoBegin() · imuAttach(why)  imu::attach(why)
//   applyGyrOffsets()                   (setGyrOffsets · attach 안에서)
//   calibrateGyro(persist)              imu::calibrateGyro(persist)
//   imuFast()                           (update 안에서 FIFO 가 꺼져 있을 때)
//   imuDrainFifo()                      imu::fifoBegin() + imu::fifoNext()  (기록 없이 비우기만: imu::drainFifo())
//   gFifoOn · gImuTickMs                imu::fifoOn() · imu::tickMs()
//   gFifoOverrun · gFifoSets            imu::fifoOverrun() · imu::fifoSets()
//   hlog::noteDropped(sets) (넘침)       imu::takeDroppedRows()  → 부르는 쪽이 hlog::noteDropped
//   기록 시작 때 resetFifo·tick·overrun=0  imu::resetFifoForRecording()
//   imuUpdate()                         imu::update()        (참 = 새 자력 표본. magCalCollect 는 부르는 쪽)
//   checkSensors() 의 IMU 부분 · i2cPing  imu::checkPresence(&gapFrom)
//   gImuLostAt · settleImuGap()         imu::checkPresence 가 돌려주는 gapFrom · imu::moveLostAt(until)
//   gImuPauseUntil (test imu <초>)      imu::testPause(sec)
//   gImu.sleep(on)                      imu::sleep(on)
//   accInMagFrame · gyrInMagFrame       imu::accInMagFrame · imu::gyrInMagFrame
//   doImu() 머리 · 꼬리                  imu::diagBegin() · imu::diagEnd()   (5초 루프와 printImuLine 은 부르는 쪽)
//   printImuLine() 앞 절반               imu::printRaw()      (힐·피치·방위 비교는 부르는 쪽)
//
// ── firmware-rak 과 달라진 점 ─────────────────────────────────────────────
//
//   1) FIFO 한 벌을 12바이트 한 번에 읽는다. 라이브러리는 가속 6 · 자이로 6 을 따로 두 번 읽었다.
//      같은 FIFO_R_W 레지스터를 이어 읽는 것이라 바이트 순서는 같다.
//   2) I2C 가 실패하면 50 ms 동안 drain·update 를 쉰다 (IDF 드라이버가 실패마다 오류 로그를 찍는다).
//      firmware-rak 은 실패하면 0 을 읽어 벌 수 0 으로 그냥 넘어갔다.
//   3) FIFO 한 벌 읽기가 실패하면 FIFO 를 비우고 남은 벌을 dropped 에 더한다 (벌 경계를 다시 맞춘다).
//      firmware-rak 은 초기화 안 된 배열을 값으로 썼다.
//   4) 레지스터 읽기가 실패하면 0 을 돌려준다 (Wire 와 같음). 실패 수는 imu::i2cErrors().
//   5) 출력은 printf. dropped 는 hlog 를 직접 부르지 않고 takeDroppedRows 로 넘긴다.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace imu {

struct Vec { float x = 0.0f, y = 0.0f, z = 0.0f; };

constexpr uint32_t kFifoOverrunSets = 38;   // 이만큼 쌓였으면 비우고 센다 (512바이트 = 42벌)

// 버스를 받거나(화면과 나눠 쓸 때) 새로 만든다. 칩은 안 건드린다. attach 가 알아서 부른다.
bool begin(i2c_master_bus_handle_t sharedBus = nullptr);
i2c_master_bus_handle_t bus();

// NVS "sail" 읽기 (firmware-rak loadSettings 의 gyr_x/y/z/gyr_u · mag_ox/oy/oz/mag_r/mag_res 줄과 범위 검사).
// nvs_flash_init 뒤에 부른다. 돌려주는 값 = 범위 밖이라 기본값으로 고친 개수 ([SET] 줄에 더한다).
int  loadSettings();
bool gyrNeedSave();               // 옛 단위로 적힌 0점을 옮겼다 → saveGyrOffsets() 로 다시 적어야 한다
bool saveGyrOffsets();            // gyr_x/y/z (float blob) · gyr_u = 2
void setGyrOffsets(float x, float y, float z);
void gyrOffsets(float* x, float* y, float* z);
void setMagOffset(float ox, float oy, float oz, float radius, float resid);   // magcal 이 부른다 (저장은 부르는 쪽)
const float* magOffset();
float magRadius();
float magResid();

bool attach(const char* why);     // 범위·필터 → 자이로 0점 → FIFO → 첫 표본 검사
bool calibrateGyro(bool persist);

int  fifoBegin();                 // 퍼 올 벌 수. 넘쳤으면 비우고 0
bool fifoNext(uint32_t* tickMs);  // 한 벌 읽어 acc()/gyr() 갱신. tickMs = 그 벌의 시각
int  drainFifo();                 // 기록 없이 비우기만 (fifoBegin + fifoNext 반복)
uint32_t takeDroppedRows();       // 넘침·읽기 실패로 버린 벌 수 (가져가면 0)
void resetFifoForRecording();

bool update();                    // 참 = 새 자력 표본 (magRaw 가 바뀜)

enum class Presence : uint8_t { NoChange, Lost, Reattached };
// 1 Hz. Reattached 면 *gapFromMs 에 끊겼던 시각(0 이면 셀 구간 없음)을 넣는다.
Presence checkPresence(uint32_t* gapFromMs);
uint32_t moveLostAt(uint32_t untilMs);   // settleImuGap: 끊겨 있으면 옛 시각을 돌려주고 untilMs 로 옮긴다. 붙어 있으면 0
void testPause(long sec);                // test imu <초> (0~120)

void  sleep(bool on);
float readTempC();

bool ok();
bool magOk();
bool magFresh();
bool fifoOn();
const Vec& acc();
const Vec& gyr();
const Vec& mag();
const Vec& magRaw();
float tempC();
void accInMagFrame(float out[3]);        // 자력 X = 가속 Y, 자력 Y = 가속 X, 자력 Z = -가속 Z
void gyrInMagFrame(float out[3]);
float   asa(int i);
uint8_t asaRaw(int i);
uint32_t magCount(int check);
uint32_t fifoOverrun();
uint32_t fifoSets();
uint32_t tickMs();
uint32_t i2cErrors();

bool diagBegin();                 // doImu 머리. false 면 붙이기 실패 (이미 줄을 닫았다)
void diagEnd();
bool printRaw();                  // "   9축  가속 … 자력 …" (줄바꿈 없음). IMU 없으면 줄을 끝내고 false

} // namespace imu
