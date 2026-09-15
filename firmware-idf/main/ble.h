// firmware-idf 3단계 — BLE (광고 · 텔레메트리 39바이트 · 설정 통로 · 이름)
//
// firmware-rak src/main.cpp 의 BLE 부분을 **뜻·글자·바이트 그대로** 옮겼다. 라이브러리는 NimBLE-Arduino 2.5.1 대신
// h2zero/esp-nimble-cpp 2.5.0 (main/idf_component.yml). 쓰는 함수의 모양은 두 판이 같다
// [확인: 두 판 NimBLEServer.h · NimBLECharacteristic.h · NimBLEAdvertising.h · NimBLEDevice.h grep 비교].
// 패킷은 firmware-rak/include/protocol.h 를 같이 쓴다 (복사 안 함).
//
// ★ 콜백에서 일하지 않는다 (CLAUDE.md). 콜백은 nimble_host 작업(스택 4096)에서 돈다.
//   콜백이 하는 일   표식 세우기 · 들어온 줄 베껴 담기 · 파랑 LED · 로그 한 줄
//   루프가 하는 일   ble::pump() (광고 다시 걸기) · ble::takeControlLine() 으로 줄 꺼내 처리
//
// ── firmware-rak → 여기 ──────────────────────────────────────────────────
//
//   firmware-rak (main.cpp)                 여기
//   ──────────────────────────────────────  ─────────────────────────────────────────────
//   gUserName · gFullName · gModuleID       ble::userName() · fullName() · moduleId()
//   sanitizeName · defaultUserName          (ble.cpp 안)
//   applyIdentity(name)                     ble::applyIdentity(name)
//   loadSettings 의 name · notify_ms 줄     ble::loadIdentity()   (NVS 가 열린 뒤)
//   saveIdentity(name)                      ble::saveIdentity(name)   (+ 광고 다시 걸기 표식은 부르는 쪽이 requestAdvApply)
//   formatMac                               ble::formatMac
//   gNotifyPeriodMs · hz 명령의 저장         ble::notifyPeriodMs() · ble::setNotifyPeriodMs(ms)  (출력 글자는 부르는 쪽)
//   gLatest                                 ble::latest()   (루프가 publish 로 넣은 마지막 값)
//   sailBleStart() · sailBleStop()          ble::start(t, e) · ble::stop()
//   gBleUp · gConnected · gSubscribed       ble::up() · connected() · subscribed()
//   connectedCount()                        ble::connectedCount()
//   gAdvNeedsApply = true                   ble::requestAdvApply()
//   loop 4) applyAdvertising                ble::pump()
//   loop 5) refreshAdvPayload (1 Hz)        ble::refreshAdvPayload()
//   loop 3) encodeTelemetryExt+setValue+notify  ble::publish(t, e)
//   controlEnqueue · controlPump 의 꺼내기   (콜백 안) · ble::takeControlLine(buf, cap)
//   controlSay(line)                        ble::controlSay(line)
//   controlLine (wifi·magcal·status 처리)    부르는 쪽 (netsrv·magcal 에 기댄다 — 5단계)
#pragma once

#include <cstddef>
#include <cstdint>

#include "protocol.h"   // firmware-rak/include

namespace ble {

// ── 이름 ─────────────────────────────────────────────────────────────────
void        loadIdentity();                 // NVS "sail" 의 name · notify_ms (firmware-rak loadSettings 와 같은 기본값·범위)
void        applyIdentity(const char* userName);
bool        saveIdentity(const char* userName);   // 적용 + NVS name. 적기 실패면 false
const char* userName();
const char* fullName();
uint8_t     moduleId();
void        formatMac(char* out, size_t cap);

uint32_t    notifyPeriodMs();
bool        setNotifyPeriodMs(uint32_t ms);  // 값 바꾸고 NVS notify_ms. 적기 실패면 false

// ── 올리기 · 내리기 ──────────────────────────────────────────────────────
// 올릴 때 텔레메트리 특성의 첫 값을 넣는다 (firmware-rak: buildTelemetry · buildExtra 로 만든 값).
void start(const sail::Telemetry& t, const sail::TelemetryExtra& e);
void stop();
bool up();

// ── 루프에서 ─────────────────────────────────────────────────────────────
void pump();                                // 연결·끊김·이름 바뀜 뒤 광고 다시 걸기
void refreshAdvPayload();                   // 1 Hz. 광고를 안 멈추고 scan response 만 바꾼다
void publish(const sail::Telemetry& t, const sail::TelemetryExtra& e);   // notify 주기마다
const sail::Telemetry& latest();

bool    connected();
bool    subscribed();
uint8_t connectedCount();
void    requestAdvApply();

// ── 설정 통로 (PROTOCOL.md §9) ───────────────────────────────────────────
// 콜백이 담아 둔 줄을 하나 꺼낸다. 없으면 false. 한 줄은 191자까지 (firmware-rak kCtlLineMax 192).
bool takeControlLine(char* out, size_t cap);
void controlSay(const char* line);          // "[CTL] → …" 로그 + 특성 값 + notify

} // namespace ble
