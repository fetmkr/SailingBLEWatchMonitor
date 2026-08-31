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
constexpr uint8_t kVerMinor = 1;

constexpr size_t kHeaderSize = 128;
constexpr size_t kNavSize    = 38;

// ── Type B 가 27 → 19 바이트로 줄었다 (v1.1) ─────────────────────────────
//
// 규격 초안은 여기에 쿼터니언 4칸(8바이트)을 두고 "힐·트림·헤딩은 저장하지
// 않는다, 후처리에서 쿼터니언으로 산출한다" 고 했다. **그걸 뺐다.**
//
// 이유
//   - 우리 시제품에는 융합이 없어서 채울 값이 아예 없었다 (늘 0 이었다)
//   - 자세는 가속·자이로 원본에서 후처리로 뽑을 수 있다. 원본이 남아 있으면
//     계산법을 나중에 고쳐도 예전 데이터까지 다시 계산된다
//   - 제일 큰 흐름이 26% 줄었다. 8시간 89 MB → 66 MB
//
// 옛 파일(v1.0)은 27바이트다. 읽는 쪽은 머리글의 ver_minor 를 보고 고른다.
constexpr size_t kImuSizeV0  = 27;   // v1.0 — 쿼터니언이 있던 시절
constexpr size_t kImuSize    = 19;   // v1.1 — 지금

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

// ── 힐·피치를 어느 가속도 축에서 봤나 ────────────────────────────────────
//
// ★ 이게 없으면 나중에 이 파일로 힐을 못 구한다.
//
// 규격은 자세를 쿼터니언으로 저장하기로 되어 있는데, 우리 시제품에는 융합이
// 없어서 가속도 원본만 남긴다. 그러면 읽는 쪽이 "어느 축이 힐이었나" 를
// 알아야 하는데, 보드는 그걸 NVS 에만 갖고 있었다. 파일에 안 남으면
// 데스크탑 앱이 짐작해야 한다.
//
// 실제로 우리는 힐 축을 X → Y 로, 부호도 한 번 뒤집었다. 그 전후 파일을
// 같은 규칙으로 읽으면 값이 틀린다.
//
//   힐   = asin(heel_sign  * 그 축의 g / 중력 크기) - heel_off
//   피치 = asin(pitch_sign * 그 축의 g / 중력 크기) - pitch_off
constexpr size_t kOffHeelAxis   = 65;  // U1  0=X 1=Y 2=Z
constexpr size_t kOffHeelSign   = 66;  // U1  0=+ 1=-
constexpr size_t kOffPitchAxis  = 67;
constexpr size_t kOffPitchSign  = 68;
constexpr size_t kOffHeelOff    = 69;  // R4  기준각 (도)
constexpr size_t kOffPitchOff   = 73;

// ── 끊긴 세션을 이어 붙일 실마리 ─────────────────────────────────────────
//
// 전원이 갑자기 끊기면 세션이 거기서 끝난다. 다시 켜지면 **새 파일**로
// 이어서 기록한다 (같은 파일에 붙이면 안 된다 — 레코드의 local_ms 가
// millis() 라서 다시 켜면 0 부터 시작한다. 한 파일 안에서 시간이 거꾸로 간다).
//
// 그래서 새 파일 머리글에 앞 세션 번호를 적는다. 읽는 쪽은 이걸 보고
// 두 파일이 한 번의 훈련이었다는 걸 안다.
//   0 이면 사람이 시작한 세션이다.
constexpr size_t kOffPrevSession = 77;  // U4  이어받은 앞 세션 번호. 0 = 아님  // R4

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
    uint8_t  heelAxis   = 1;    // 0=X 1=Y 2=Z
    uint8_t  heelSign   = 1;    // 0=+ 1=-
    uint8_t  pitchAxis  = 2;
    uint8_t  pitchSign  = 0;
    float    heelOff    = 0.0f;
    float    pitchOff   = 0.0f;
    uint32_t prevSession = 0;   // 끊긴 세션을 이어받은 경우 그 번호. 0 = 아님
};

// 한 시점을 눈으로 볼 값 (10초에 한 줄 나가는 텍스트용).
// 쿼터니언이 없는 보드에서는 가속도에서 뽑은 참고값이 들어온다.
struct TextSample {
    float heelDeg  = 0;
    float pitchDeg = 0;
    float hdgDeg   = -1;
    bool  attOk    = false;

    // ── 속도 세 가지를 나란히 적는다 (음수면 그때 값이 없었다) ────────────
    //
    // 바이너리에는 도플러(RMC) 하나만 들어간다. 그런데 2026-08-30 세션 27 에서
    // **1노트 아래가 통째로 0.00 으로 찍혔다.** 위로는 완벽했다.
    //
    //   위치로 잰 속도   보드가 0 으로 적은 비율
    //   0.0~0.3 kts        96%
    //   0.3~0.6 kts       100%
    //   0.6~1.0 kts        75%
    //   1.0~1.5 kts         0%     ← 여기부터 하나도 안 틀렸다
    //
    // 우리 문턱값 탓이 아니다. 파일에 적히는 값은 다듬기 전 원본이고
    // 모듈의 정지 문턱값도 0 으로 꺼져 있었다. 수신기가 스스로 0 을 냈다.
    //
    // 어느 길이 그 대역에서 살아남는지 아직 모른다. 그래서 셋을 같이 적는다.
    // 바이너리 형식은 안 건드린다 — 이건 눈으로 견주는 사본이다.
    float sogPvKn  = -1;   // NAV-PV. 모듈이 NMEA 로 만들기 전의 속도
    float sogPosKn = -1;   // 위치 차분. 필터를 아예 안 거친 값

    // ★ 칩이 스스로 밝히는 「이 속도가 방금 잰 값인가」.
    //   4 이상이면 잰 값, 3 이면 옛날 값을 들고 있는 것이다 (CASIC NAV-PV).
    //   이 숫자를 같이 남겨야 나중에 파일만 보고 갈린다.
    uint8_t pvFlag    = 255;  // 255 = NAV-PV 를 아예 못 받았다
    float   cogAccDeg = -1;   // 칩이 밝힌 침로 오차. 침로가 얼면 커진다
};

// ── 기록기 ───────────────────────────────────────────────────────────────

struct Status {
    bool     recording   = false;
    bool     cardPresent = false;
    uint32_t session     = 0;
    char     path[64]    = {0};   // /LOGS/S00014_19700103-0043_nosat.HLG 가 36자
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

// ── 끊겼는지 표시 ────────────────────────────────────────────────────────
//
// start() 가 NVS 에 1 을 적고 stop() 이 0 으로 지운다. 전원이 갑자기 끊기면
// 지울 틈이 없으므로 1 이 남는다. 다음에 켜질 때 이걸 보고 이어서 시작한다.
//
// 카드 쓰기가 실패해서 저절로 멈춘 경우도 stop() 을 안 거치므로, 그때는
// healthCheck() 가 대신 지운다 (카드가 죽었는데 다시 걸어봐야 또 실패한다).
bool cutShort();       // 지난번에 기록 중이었는데 못 닫고 끊겼나
void clearCutFlag();   // 이어시작을 포기할 때 지운다

void begin();                        // setup() 에서 한 번. 쓰기 작업을 띄운다
// 지난번에 왜 꺼졌는지. 새 세션의 TXT 머리에 적는다. 세션이 끊기면 이유가
// 램과 함께 날아가므로, **다음 세션 파일에 남겨서** 나중에 찾을 수 있게 한다.
void noteBootReason(const char* why);
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
// 세션의 TXT 사본 끝부분을 시리얼로 찍는다. 카드를 못 뽑을 때
// 마지막 순간의 전압·멈춤·버퍼를 보는 유일한 길이다.
//   session 0 이면 마지막 세션.  head 를 켜면 앞부분을 본다
void tail(uint32_t session, uint16_t lines = 20, bool head = false);
void listFiles();
// 한 세션의 파일 두 벌(.HLG/.TXT)을 지운다. **되돌릴 수 없다.**
// 번호를 하나만 받는다 — 한 번에 여러 개를 지우는 길은 일부러 안 만들었다.
bool removeSession(uint32_t session);
uint32_t sinceTextMs();
uint32_t recStartedMs();       // 기록 시작 시각 (화면이 지난 시간을 뽑는다)              // 마지막 텍스트 줄로부터 지난 시간

} // namespace hlog
