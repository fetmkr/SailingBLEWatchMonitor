// 경기정 모듈 바이너리 로그 — 포맷 v1.0
//
// 규격 원문: docs/spec/로그포맷_v1.0_draft_2026-08-24.md
// 우리 보드에서 어떻게 채우는지와 실측 근거: SDLOG.md
//
// 두 파일을 같이 남긴다.
//   /LOGS/SNNN.HLG   바이너리. Type A(NAV) 10 Hz + Type B(IMU) 100 Hz. 분석용 원본
//   /LOGS/SNNN.TXT   10초에 한 줄. 카드를 꽂자마자 눈으로 확인하는 용
//
// 전부 리틀엔디언. 정렬을 맞추지 않았으므로 **memcpy 로 읽을 것.**
// 구조체 포인터로 캐스팅하면 안 된다 (local_ms 가 오프셋 1에 있다).
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace hlog {

// ── 크기와 표식 ──────────────────────────────────────────────────────────
constexpr uint8_t kMagic0 = 'H', kMagic1 = 'H', kMagic2 = 'L', kMagic3 = 'G';
constexpr uint8_t kVerMajor = 1;
constexpr uint8_t kVerMinor = 0;

constexpr size_t kHeaderSize = 128;
constexpr size_t kNavSize    = 38;
constexpr size_t kImuSize    = 27;

constexpr uint8_t kTypeNav = 0xA1;
constexpr uint8_t kTypeImu = 0xB1;

constexpr uint8_t kRateNav = 10;
constexpr uint8_t kRateImu = 100;

// ── 값 없음 표식 ─────────────────────────────────────────────────────────
//
// ★ 규격 원문에는 아직 없다. 반드시 넣어야 한다.
//
// 0 을 쓰면 "정박 중 0 노트" 와 "위성 못 잡음" 이 구별되지 않는다. 배에서
// 그건 위험하고, 나중에 분석할 때도 없는 값이 0 으로 섞여 들어간다.
// 우리 BLE 프로토콜이 쓰는 규칙과 같다 (PROTOCOL.md §2.1).
//
// 물리적으로 나올 수 없는 값을 고른다.
constexpr int32_t  kLatLonInvalid = (int32_t)0x80000000; // -214.7도. 없는 좌표
constexpr uint16_t kSogInvalid    = 0xFFFF;              // 65.535 m/s = 127노트
constexpr uint16_t kCogInvalid    = 0xFFFF;              // 유효 범위 0~35999 밖
constexpr uint16_t kAccInvalid    = 0xFFFF;              // 655 m. 쓸모없는 정확도
constexpr uint32_t kItowInvalid   = 0xFFFFFFFF;
constexpr uint16_t kWeekInvalid   = 0xFFFF;
// 쿼터니언 네 칸이 모두 0 이면 자세 없음. 0 벡터는 회전이 아니라서 안전하다.

// ── 이벤트 비트 (Type A 오프셋 29) ───────────────────────────────────────
constexpr uint8_t kEvMark      = 0x01; // 마킹 버튼
constexpr uint8_t kEvFirst     = 0x02; // 세션 첫 레코드
constexpr uint8_t kEvUtcResync = 0x04; // UTC 재동기

// ── 헤더 reserved 에 우리가 채우는 것 ────────────────────────────────────
//
// 규격이 "헤더 reserved 영역" 을 확장용으로 열어 뒀다. 나중에 이 파일을
// 읽는 사람이 "이 데이터가 어떤 장비에서 어떤 설정으로 나왔나" 를 알아야
// 해석할 수 있다. 우리가 dyModel 하나 때문에 저속 데이터를 통째로 날린 적이
// 있다 (README 의 GPS 항목). 그 설정이 파일에 안 남아 있으면 나중에 원인을
// 못 찾는다.
//
//  오프셋  크기  이름          뜻
//  ------  ----  ------------  ------------------------------------------
//  41       1    imu_type      0=BNO085, 1=MPU-9250(우리 시제품)
//  42       1    time_ref      0=GPS 시각, 1=UTC 에서 환산 (윤초만큼 다름)
//  43       1    mag_scale     0=raw LSB, 1=0.1 µT/LSB (우리)
//  44       1    gnss_dyn      GNSS 동역학 모델 (CASIC dyModel)
//  45       1    gnss_hz       GNSS 갱신율
//  46       1    sog_src       0=도플러 원본, 1=다듬은 값  ★항상 0 이어야 한다
//  47       1    quat_src      0=센서 융합, 1=없음(가속·자이로 원본만)
//  48      78    (미사용)      0x00
constexpr size_t kOffImuType  = 41;
constexpr size_t kOffTimeRef  = 42;
constexpr size_t kOffMagScale = 43;
constexpr size_t kOffGnssDyn  = 44;
constexpr size_t kOffGnssHz   = 45;
constexpr size_t kOffSogSrc   = 46;
constexpr size_t kOffQuatSrc  = 47;
// 세션을 닫을 때 채운다. 목록만 보고 뭘 받을지 정하려면 이게 있어야 한다
// (TRANSFER.md §1). 못 채운 파일은 전부 0 이다 — 전원이 갑자기 끊긴 경우다.
constexpr size_t kOffDurationS = 48;  // U4  세션 길이 (초)
constexpr size_t kOffNavRows   = 52;  // U4
constexpr size_t kOffImuRows   = 56;  // U4
constexpr size_t kOffDropped   = 60;  // U4  0 이 아니면 구멍 난 세션이다
constexpr size_t kOffClosed    = 64;  // U1  1 이면 제대로 닫힌 파일

constexpr uint8_t kImuBNO085  = 0;
constexpr uint8_t kImuMPU9250 = 1;

// ── CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) ────────────────────────
uint16_t crc16(const uint8_t* p, size_t n);

// ── 한 시점의 값 ─────────────────────────────────────────────────────────

struct NavSample {
    uint32_t localMs = 0;
    uint32_t itow    = kItowInvalid;
    uint16_t week    = kWeekInvalid;
    int32_t  lat     = kLatLonInvalid;   // 1e-7 도
    int32_t  lon     = kLatLonInvalid;
    uint16_t sog     = kSogInvalid;      // mm/s. ★다듬기 전 도플러 원본
    uint16_t cog     = kCogInvalid;      // 0.01 도
    uint8_t  numSv   = 0;
    uint8_t  fix     = 0;                // 0 없음 1 단독 2 DGNSS 4 RTKfix 5 RTKfloat
    uint16_t hAcc    = kAccInvalid;      // cm
    uint16_t battMv  = 0;                // ★전압 원시값. 퍼센트는 저장하지 않는다
    uint8_t  event   = 0;
    int16_t  mag[3]  = {0, 0, 0};        // 0.1 µT/LSB (헤더 mag_scale=1)
};

struct ImuSample {
    uint32_t localMs = 0;
    int16_t  acc[3]  = {0, 0, 0};        // 1 mg/LSB
    int16_t  gyr[3]  = {0, 0, 0};        // 1/32 °/s/LSB
    int16_t  quat[4] = {0, 0, 0, 0};     // Q14. 넷 다 0 이면 자세 없음
};

// 파일 머리글에 박아 둘 것
struct Header {
    uint8_t  mac[6]     = {0};
    uint16_t fwVersion  = 0x0100;
    uint8_t  hwRev      = 0;
    uint8_t  gnssType   = 1;   // 1 = SE868SY-D 자리. 지금은 L76K 라 예약값
    uint32_t sessionId  = 0;
    uint16_t bootCount  = 0;
    int16_t  mountQuat[4] = {0, 0, 0, 0};
    uint8_t  imuCalStatus = 0;
    // reserved 에 채우는 것
    uint8_t  imuType    = kImuMPU9250;
    uint8_t  timeRef    = 1;
    uint8_t  magScale   = 1;
    uint8_t  gnssDyn    = 255;
    uint8_t  gnssHz     = 0;
    uint8_t  sogSrc     = 0;
    uint8_t  quatSrc    = 1;
};

// 한 시점을 눈으로 볼 값 (10초에 한 줄 나가는 텍스트용).
// 쿼터니언이 없는 보드에서는 가속도에서 뽑은 참고값이 들어온다.
struct TextSample {
    float heelDeg  = 0;
    float pitchDeg = 0;
    float hdgDeg   = -1;
    bool  attOk    = false;
};

// ── 기록기 ───────────────────────────────────────────────────────────────

struct Status {
    bool     recording   = false;
    bool     cardPresent = false;
    uint32_t session     = 0;
    char     path[32]    = {0};
    uint32_t navRows     = 0;
    uint32_t imuRows     = 0;
    uint64_t bytes       = 0;
    uint32_t startedMs   = 0;
    uint32_t dropped     = 0;  // ★0 이어야 한다
    uint32_t waited      = 0;
    uint32_t maxStallMs  = 0;
    uint32_t maxFillPct  = 0;
    uint64_t freeBytes   = 0;
    const char* lastError = nullptr;
};

void begin();                        // setup() 에서 한 번. 쓰기 작업을 띄운다
bool start(const Header& h);
void stop();
void writeNav(const NavSample& s);   // 10 Hz
void writeImu(const ImuSample& s);   // 100 Hz
void writeText(const NavSample& s, const TextSample& t); // 10초에 한 번
void mark();                         // 다음 NAV 줄에 마킹 표식

// 첫 fix 때 한 번 부른다. 세션을 닫을 때 머리글에 박는다.
void noteUtcStart(uint32_t epochSec, uint16_t ms);
bool recording();
void getStatus(Status* out);
void healthCheck();                  // 1 Hz. 카드가 빠졌는지 본다

// 카드에 쓴 파일을 보드가 직접 되읽어 검사한다.
// 카드를 뽑아 컴퓨터로 옮길 수 없을 때 쓴다. 파이썬 파서와 같은 것을 본다.
//   session 0 이면 마지막 세션
void verify(uint32_t session);
void listFiles();
uint32_t sinceTextMs();
uint32_t recStartedMs();       // 기록 시작 시각 (화면이 지난 시간을 뽑는다)              // 마지막 텍스트 줄로부터 지난 시간

} // namespace hlog
