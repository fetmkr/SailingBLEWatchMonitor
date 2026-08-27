// WiFi 로 기록 파일 내보내기.
//
// 카드를 뽑아 컴퓨터에 꽂는 대신, 보드가 HTTP 서버가 되어 파일을 준다.
// 데스크탑 앱(`desktop/`)이 이걸 받아 간다.
//
// 두 가지 방식이 있다.
//   AP   보드가 스스로 WiFi 를 만든다. 바닷가에는 공유기가 없으니 이게 기본
//   JOIN 아는 WiFi 에 붙는다. 사무실·집에서 편하다 (secrets.h 에 적어 둔다)
//
// ★ 기록 중에는 켜지 않는다.
//   파일을 보내는 동안 메인 루프가 묶여서 100 Hz 기록이 끊긴다. 그리고
//   WiFi 와 BLE 가 같은 무선을 나눠 쓴다. 회수는 훈련이 끝난 뒤에 한다.
//
// 주소 (기본 http://192.168.4.1)
//   GET  /api/status          지금 상태 (JSON)
//   GET  /api/files           파일 목록 (JSON)
//   GET  /file/S00008.HLG     파일 원본 (Range 지원)
//   POST /api/rec/start|stop|mark   기록 제어
#pragma once

#include <stdint.h>

namespace netsrv {

enum class Mode : uint8_t { Off, AP, Join };

// 보드가 스스로 WiFi 를 만든다. 비밀번호는 secrets.h 에 있다.
bool startAP();

// 아는 WiFi 에 붙는다. secrets.h 의 SAIL_WIFI_SSID / PASS 를 쓴다.
bool startJoin(uint32_t timeoutMs = 15000);

void stop();

// loop() 에서 자주 부른다. 안 켜져 있으면 바로 돌아온다.
void poll();

// 주변 WiFi 하나.
struct ScanEntry {
    char ssid[33];
    int8_t rssi;       // dBm. -50 이면 아주 가깝고 -85 면 겨우 잡힌다
    bool locked;       // 비밀번호가 걸려 있나
};

// 주변 WiFi 를 훑는다. **BLE 를 안 내리고 할 수 있다.**
// 돌려주는 값은 out 에 채운 개수.
int scan(ScanEntry* out, int max);

// 붙을 WiFi 를 정한다. NVS 에 남으니 전원을 빼도 그대로다.
// pass 가 nullptr 이면 비밀번호는 그대로 둔다.
void setCreds(const char* ssid, const char* pass);
const char* staSsid();

// 아무도 안 쓰면 저절로 끄기. 기본은 켜져 있다.
//
// BLE 로 WiFi 를 켜라고 시키면 그 순간 BLE 가 내려간다. 앱이 죽으면
// 보드가 WiFi 를 켠 채 남아서 BLE 도 안 돌아온다. 그걸 막는다.
void     setIdleOff(uint32_t seconds);   // 0 이면 안 끈다
uint32_t idleOffSec();
uint32_t idleLeftMs();      // 꺼지기까지 남은 시간. 0 이면 안 끈다

// 이 보드의 mDNS 이름 ("sail-random"). 뒤에 .local 을 붙이면 주소가 된다.
const char* mdnsHost();
// 보드가 스스로 여는 WiFi 의 비밀번호.
const char* apPass();
// 지난번에 공유기한테 받았던 주소. 없으면 빈 문자열.
// BLE 가 끊긴 뒤에 앱이 찾아올 실마리로 쓴다.
const char* lastIp();

// 지금 이 보드를 쓰고 있는 기기 수. 30초 안에 요청을 보낸 주소를 센다.
// 붙기 전에 "이미 누가 쓰고 있다" 를 알려주는 데 쓴다.
int         users();
// 이 앱 번호를 뺀 나머지 기기 수. 앱은 이것만 보고 붙을지 정한다.
// 번호가 빈 글자면 아무도 안 뺀다.
int         othersThan(const char* id);
// 그 나머지 중 한 대의 주소. 사람에게 보여주려는 것뿐이다.
const char* otherIpText(const char* id);

Mode        mode();
const char* ipText();     // "192.168.4.1". 안 켜져 있으면 빈 문자열
const char* ssidText();
uint32_t    servedFiles();
uint64_t    servedBytes();

} // namespace netsrv
