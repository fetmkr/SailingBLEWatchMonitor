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

Mode        mode();
const char* ipText();     // "192.168.4.1". 안 켜져 있으면 빈 문자열
const char* ssidText();
uint32_t    servedFiles();
uint64_t    servedBytes();

} // namespace netsrv
