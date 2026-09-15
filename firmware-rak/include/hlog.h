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

#include "hlog_write.h"

namespace hlog {

// ── 크기와 표식 ──────────────────────────────────────────────────────────
constexpr uint8_t kMagic0 = 'H', kMagic1 = 'H', kMagic2 = 'L', kMagic3 = 'G';
constexpr uint8_t kVerMajor = 1;
constexpr uint8_t kVerMinor = 2;

constexpr size_t kHeaderSize = 128;

// ── Type A 가 38 → 40 바이트로 늘었다 (v1.2, 2026-09-15) ─────────────────
//
// 보드가 그 순간 화면·BLE·TXT 에 보여준 방위(HDG)를 줄마다 같이 적는다.
//   36~37  u16  hdg   0.01 도 (0~35999). 0xFFFF = 못 구함
//   38~39  u16  CRC   0~37 범위
//
// "카드에는 원본만" 을 어긴 것이 아니다. 원본(자력 mag)은 그대로 남기고,
// 보드가 그때 보여준 계산값을 **나란히** 남긴다. 사용자 결정이다.
//
// 이유. 세션 46(09-13)에는 방위 설정이 파일에 없었다. 앱이 같은 식을 갖고도
// 축·오프셋을 몰라서 방위를 못 되살렸다. 값이 줄에 있으면 설정이 틀려도
// 그때 사람이 본 값은 남는다. 나중에 고친 식으로 다시 계산한 값과 견줄 수도 있다.
// 계산에 쓴 설정은 머리글 81~105 에 있다.
//
// 옛 파일(v1.0·v1.1)은 38바이트다. 읽는 쪽은 머리글 판 번호가 1.2 이상이면 40.
constexpr size_t kNavSize    = 40;   // v1.2 — 지금
constexpr size_t kNavSizeV1  = 38;   // v1.0·v1.1 — hdg 칸이 없던 시절

/** 머리글 판 번호(바이트 4·5)로 NAV 줄 크기를 고른다. */
constexpr size_t navSizeFor(uint8_t verMajor, uint8_t verMinor) {
    return (verMajor > 1 || (verMajor == 1 && verMinor >= 2)) ? kNavSize : kNavSizeV1;
}

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
constexpr uint16_t kHdgInvalid    = 0xFFFF;              // 방위 못 구함 (0~35999 밖)

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

// ── 방위를 다시 구할 설정 (2026-09-15) ───────────────────────────────────
//
// ★ 이게 없으면 앱이 방위 식을 짐작한다. 실제로 앱이 보드와 다른 식을 써서 COG 대비
//   흩어짐이 49.5° 로 나왔다 (세션 46). 보드가 화면·BLE·TXT 에 쓴 식과 그 입력을 적는다.
//
//   hdg_formula  0 = 안 적힘(옛 파일)  1 = 평평 atan2  2 = 기울기 보정(INSLIB ahrs_mag_detilt,
//                중력은 그때 가속도에서, |a| 가 1 g ±0.15 를 벗어나면 방위 없음)
//   축 A·B·부호  방위 = atan2(A·sA, B·sB) 의 두 축 (0=X 1=Y 2=Z, 부호 0=+ 1=-). 자력계 좌표
//                앞 = B·sB, 오른쪽 = −A·sA, 아래 = 오른손 법칙
//   가속→자력 축은 고정: 자력 X = 가속 Y, 자력 Y = 가속 X, 자력 Z = −가속 Z
//   mag_hi       기록된 mag 에서 이미 뺀 하드아이언 오프셋 (µT). 원본 = 기록값 + 이 값
//   세션 도중 설정이 바뀌면 TXT 에 사건 줄로 남는다 (머리글은 시작 때 값)
constexpr size_t kOffHdgFormula = 81;  // U1
constexpr size_t kOffHdgAxisA   = 82;  // U1
constexpr size_t kOffHdgAxisB   = 83;  // U1
constexpr size_t kOffHdgSignA   = 84;  // U1  0=+ 1=-
constexpr size_t kOffHdgSignB   = 85;  // U1
constexpr size_t kOffHdgOff     = 86;  // R4  장착 오프셋 (도)
constexpr size_t kOffHdgDecl    = 90;  // R4  자기 편각 (도, 동편 +)
constexpr size_t kOffMagHi      = 94;  // R4×3  뺀 하드아이언 (µT) 94·98·102
constexpr uint8_t kHdgFormulaFlat = 1;
constexpr uint8_t kHdgFormulaTilt = 2;

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
    uint16_t hdg     = kHdgInvalid;      // 0.01 도. 보드가 이 순간 보여준 방위 (v1.2)
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
    // 방위 설정 (kOffHdgFormula 표)
    uint8_t  hdgFormula = 0;
    uint8_t  hdgAxisA   = 1, hdgAxisB = 0;
    uint8_t  hdgSignA   = 0, hdgSignB = 0;
    float    hdgOff     = 0.0f;
    float    hdgDecl    = 0.0f;
    float    magHi[3]   = {0.0f, 0.0f, 0.0f};
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
    float   sogAccKn  = -1;   // 칩이 밝힌 속도 오차. 1 kn 넘으면 화면에서 버린다
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
    const char* lastErrorShort = nullptr;  // 화면 한 줄용 ("카드 없음" 등)
    uint8_t  state       = 0;     // RecState
    uint32_t lostBytes   = 0;     // 마지막으로 닫을 때 못 쓰고 버린 바이트
};

// ── 이어 시작 표시 ───────────────────────────────────────────────────────
//
// 기록기는 NVS 를 만지지 않는다. 켜면 다시 걸 의도(rec_want)와 마감 안 된 세션(rec_open)은
// 루프가 적는다 (main.cpp, rec_control.h). 옛 rec_on 한 칸은 두 뜻이 섞여 있었다.

void begin();                        // setup() 에서 한 번. 쓰기 작업을 띄운다
// 지난번에 왜 꺼졌는지. 새 세션의 TXT 머리에 적는다. 세션이 끊기면 이유가
// 램과 함께 날아가므로, **다음 세션 파일에 남겨서** 나중에 찾을 수 있게 한다.
void noteBootReason(const char* why);
bool start(const Header& h);
// 멈추라고 **요청만** 하고 바로 돌아온다. 기록 중이 아니면 false.
// 일꾼이 남은 것을 쓰고 · 닫고 · 머리글과 이름을 고친 뒤 결과를 하나 남긴다 → poll().
// ★ 옛 stop() 은 15초까지 루프를 붙잡았고, 늦게 닫히면 깃발 넷으로 뒷정리를 했다.
bool requestStop();
// 끝난 세션 결과를 한 번만 꺼낸다. 루프가 매 바퀴 부른다. 새 결과가 없으면 false.
bool poll(recctl::SessionResult* out);
recctl::Phase phase();
// 시험용: 다음 닫기 직전에 일꾼이 ms 만큼 한 번 쉰다 (체크리스트 12 "닫기 20초 지연")
void testSlowClose(uint32_t ms);
void writeNav(const NavSample& s);   // 10 Hz
void writeImu(const ImuSample& s);   // 100 Hz
void writeText(const NavSample& s, const TextSample& t); // 10초에 한 번
void mark();                         // 다음 NAV 줄에 마킹 표식

// 첫 fix 때 한 번 부른다. 세션을 닫을 때 머리글에 박는다.
void noteUtcStart(uint32_t epochSec, uint16_t ms);
bool recording();
// 기록기가 카드를 쥐고 있나 (쓰는 중이거나 닫는 중). 이때는 다른 누구도 SD 를 만지면 안 된다.
bool busy();
RecState state();
// 기록에 못 들어간 표본 수를 더한다 (IMU FIFO 넘침 등). 머리글 dropped 에 들어간다.
void noteDropped(uint32_t rows);
void getStatus(Status* out);
void healthCheck();                  // 1 Hz. 카드가 빠졌는지 본다

// ── 기록이 멈춘 이유 (2026-09-13) ────────────────────────────────────────
//
// 세션 27(8/30)과 46(9/13)이 "# 끝" 없이 끊겼다. 쓰기 한 번 실패에 그대로
// 멈추는 길이었는데, 이유를 램에만 들고 있다가 재부팅에 날렸다.
// 이제 세션 결과(recctl::SessionResult)의 첫 오류 칸에 담기고, 루프가 poll() 로 꺼내
// NVS 와 다음 세션 TXT 에 남긴다.
// 다음 세션 TXT 머리에 "지난 기록 실패" 줄로 적는다. 부팅 때 NVS 에서 읽어 넘긴다.
void noteLastFail(const char* line);
// TXT 머리에 그대로 적을 여러 줄 (방위 알고리즘·축·오프셋·자력 보정값). 시작 전에 부른다.
// HLG 형식은 안 바꾼다 — 재현에 필요한 설정을 사람이 읽는 사본에 남긴다.
void setSessionNote(const char* text);
// 기록 중 설정이 바뀌면 TXT 에 "# 시각 설정 바뀜 — ..." 한 줄을 넣는다. 기록 중이 아니면 무시.
void noteEvent(const char* line);
// 시험용. 다음 n 번의 카드 쓰기를 "0 바이트 씀, EIO" 로 흉내 낸다.
void testFailWrites(uint8_t n);
uint32_t writeRetries();             // 다시 써서 살린 횟수 (이 부팅)

// 카드에 쓴 파일을 보드가 직접 되읽어 검사한다.
// 카드를 뽑아 컴퓨터로 옮길 수 없을 때 쓴다. 파이썬 파서와 같은 것을 본다.
//   session 0 이면 마지막 세션
void verify(uint32_t session);
// 세션의 TXT 사본 끝부분을 시리얼로 찍는다. 카드를 못 뽑을 때
// 마지막 순간의 전압·멈춤·버퍼를 보는 유일한 길이다.
//   session 0 이면 마지막 세션.  head 를 켜면 앞부분을 본다
void tail(uint32_t session, uint16_t lines = 20, bool head = false);
// 세션 파일 한 조각을 시리얼로 보낸다 (WiFi 없이 USB 로 받기). tools/serial_dump.py 가 부른다.
//   @DUMP S <경로> <크기> · @DUMP B <base64> 여러 줄 · @DUMP E <시작> <바이트> <crc32>
//   실패는 @DUMP X <이유>. 조각 하나는 256 KB 까지.
void dump(uint32_t session, bool hlg, uint32_t offset, uint32_t len);
// 세션 파일 하나의 SHA-256. 받은 파일이 카드 원본과 같은지 맞춰 본다 (체크리스트 10).
//   @HASH <경로> <바이트> <sha256 16진수>  · 실패는 @HASH X <이유>
void hashFile(uint32_t session, bool hlg);
void listFiles();
// 한 세션의 파일 두 벌(.HLG/.TXT)을 지운다. **되돌릴 수 없다.**
// 번호를 하나만 받는다 — 한 번에 여러 개를 지우는 길은 일부러 안 만들었다.
bool removeSession(uint32_t session);
uint32_t sinceTextMs();
uint32_t recStartedMs();       // 기록 시작 시각 (화면이 지난 시간을 뽑는다)              // 마지막 텍스트 줄로부터 지난 시간

} // namespace hlog
