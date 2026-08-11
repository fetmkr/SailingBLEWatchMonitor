# app — Sailing Monitor 수신 앱 (iOS + watchOS)

CoreBluetooth 만 쓰는 SwiftUI 앱. 서드파티 의존성 없음.
프로토콜은 [`../PROTOCOL.md`](../PROTOCOL.md) 를 따른다.

| | |
|---|---|
| iOS | 17.0+ |
| watchOS | 10.0+ |
| 프로젝트 생성 | XcodeGen (`project.yml`) |

---

## 파일 구성

```
app/
├── project.yml              ← 프로젝트 정의. .xcodeproj 는 여기서 생성된다
├── Shared/                  ← 두 타깃이 함께 컴파일
│   ├── Protocol.swift       ← 패킷 디코딩. firmware/include/protocol.h 와 쌍
│   ├── ModulePin.swift      ← "내 모듈" 고정 저장 + 발견 목록 모델
│   └── BLEManager.swift     ← 탐색/연결 모드, notify, 재연결, 모듈 검증
├── iOS/
│   ├── SailingMonitorApp.swift     ← @main, 탭 3개
│   ├── LiveView.swift       ← 라이브 탭
│   ├── ScannerView.swift    ← 스캐너 탭
│   ├── ScannerManager.swift ← 광고 전용 별도 CBCentralManager
│   ├── SettingsView.swift   ← 설정 탭 (모듈 선택/해제)
│   └── Info.plist
└── Watch/
    ├── WatchApp.swift
    ├── WatchLiveView.swift
    ├── Info.plist
```

---

## 1. 프로젝트 생성

`.xcodeproj` 는 저장소에 넣지 않는다. 아래로 만든다.

```bash
brew install xcodegen      # 처음 한 번만
cd app
xcodegen generate
open SailingMonitor.xcodeproj
```

> `project.yml` 을 고쳤으면 `xcodegen generate` 를 다시 돌려야 반영된다.
> 반대로 Xcode UI 에서 타깃 설정을 바꾸면 다음 generate 때 날아간다 —
> 설정 변경은 항상 `project.yml` 에서 할 것.

## 2. 서명 팀 설정

`project.yml` 의 `DEVELOPMENT_TEAM` 한 줄이면 두 타깃 모두에 적용된다.

```yaml
settings:
  base:
    DEVELOPMENT_TEAM: KC8YM8J64N   # FETM (Company)
```

### Team ID 를 정확히 얻는 법

⚠️ **인증서 이름 괄호 안의 값은 Team ID 가 아니다.** `OU` 필드가 Team ID 다.

```bash
security find-identity -v -p codesigning
#   "Apple Development: hojun song (QFS2AH4UR3)"   ← QFS2AH4UR3 는 Team ID 가 아님

security find-certificate -c "Apple Development: hojun song" -p \
  | openssl x509 -noout -subject
#   /UID=.../CN=.../OU=M38GX6QKSQ/O=SUPERLESS Inc.   ← OU 가 Team ID
```

계정에 팀이 여러 개면 Xcode 가 캐시한 목록에서 확인할 수 있다.
(Xcode 를 한 번 종료해야 디스크에 기록된다)

```bash
defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier \
  | grep -E "teamID|teamName|isFree"
```

번들 ID 를 바꾸려면 **세 곳**을 함께 고친다.

1. `project.yml` 의 `PRODUCT_BUNDLE_IDENTIFIER` (iOS)
2. `project.yml` 의 `PRODUCT_BUNDLE_IDENTIFIER` (watch, iOS 것 + `.watchkitapp`)
3. `Watch/Info.plist` 의 `WKCompanionAppBundleIdentifier` (iOS 것과 동일)

---

## 3. 실기기 배포 — 처음 한 번 (여기서 제일 많이 막힌다)

순서대로 하지 않으면 엉뚱한 에러가 난다.

### 3.1 기기를 개발자 포털에 등록

Xcode 자동 서명이 기기를 자동 등록해 주는 건 **특정 기기를 대상으로 빌드할 때**다.
`generic/platform=iOS` 로 빌드하면 등록되지 않고 이런 에러가 난다.

```
error: Communication with Apple failed: Your team has no devices from
       which to generate a provisioning profile.
```

팀에 등록된 기기가 0대면 개발용 프로파일 자체를 만들 수 없다. 확실한 방법은 수동 등록:

```bash
# UDID 얻기
xcrun xctrace list devices          # 괄호 안 25자리
xcrun devicectl list devices
```

[developer.apple.com/account](https://developer.apple.com/account) → Certificates, Identifiers & Profiles
→ **Devices** → `+` → Platform / 이름 / UDID 입력. **아이폰과 워치를 각각** 등록한다.

> 팀이 약관(PLA)에 동의하지 않은 상태면 무슨 짓을 해도 프로파일이 안 나온다.
> `PLA Update available` 에러가 그것이고, **Account Holder 만** 동의할 수 있다.

### 3.2 두 기기 모두 개발자 모드 켜기

```
설정 → 개인정보 보호 및 보안 → 개발자 모드 → 켜기 → 재시동
```

안 켜면: `error: Developer Mode disabled`

### 3.3 아이폰 설치

```bash
xcodebuild -project SailingMonitor.xcodeproj -scheme "SailingMonitor" \
  -destination 'platform=iOS,id=<아이폰 UDID>' -configuration Debug build \
  -allowProvisioningUpdates

APP=$(find ~/Library/Developer/Xcode/DerivedData/SailingMonitor-*/Build/Products/Debug-iphoneos \
      -maxdepth 1 -name "SailingMonitor.app" | head -1)
xcrun devicectl device install app --device <아이폰 UDID> "$APP"
xcrun devicectl device process launch --device <아이폰 UDID> kr.fetm.sailingmonitor
```

아이폰이 잠겨 있으면 실행만 실패한다(`BSErrorCodeDescription = Locked`).

### 3.4 워치 설치 — 함정 셋

**함정 1. 워치 앱 임베드 위치는 `Watch/` 다.**

`XcodeGen` 이슈 #1613 은 Xcode 26 에서 `PlugIns/` 로 옮기라고 하지만,
그렇게 하면 **아이폰 Watch 앱의 "사용 가능한 앱" 목록에서 아예 사라진다.**
실기기(iOS 26.5 / watchOS 26.6)에서 양쪽 다 확인한 결과다.
그 이슈는 *빌드가 실패하는* 경우에 대한 것이고 이 구성에는 해당하지 않는다.

**함정 2. watchOS 26.5 는 아이폰 경유 설치가 깨져 있다.**

애플이 인정한 회귀다 (Feedback **FB22807635**, DTS 확인).
IDS peer transport 에서 소켓이 60초 타임아웃 나면서
Watch 앱의 "설치" 가 돌기만 하고 끝난다. 재부팅·초기화·인증서 재발급 전부 무효.
**아이폰과 워치를 둘 다 26.6 으로 올리면 해결된다.** 한쪽만 올리면 안 된다.

**함정 3. 아이폰 블루투스가 켜져 있으면 맥에서 워치로 직접 설치도 안 된다.**

워치는 아이폰이 블루투스 범위에 있으면 Wi-Fi 를 올리지 않는다. 그런데 Xcode·devicectl 은
워치에 **로컬 네트워크로만** 접근한다. 그래서 터널이 안 열린다.

```bash
xcrun devicectl device info details --device <워치 UDID> | grep -E "transportType|tunnelState"
#   transportType: localNetwork
#   tunnelState:   disconnected      ← 아이폰 블루투스가 켜져 있을 때
```

**아이폰 설정 → Bluetooth → 끄기.** 그러면 워치가 Wi-Fi 로 올라오고 터널이 붙는다.

```bash
xcodebuild -project SailingMonitor.xcodeproj -scheme "SailingMonitor Watch App" \
  -destination 'generic/platform=watchOS' -configuration Debug build -allowProvisioningUpdates

WAPP=$(find ~/Library/Developer/Xcode/DerivedData/SailingMonitor-*/Build/Products/Debug-watchos \
       -maxdepth 1 -name "SailingMonitor Watch App.app" | head -1)
xcrun devicectl device install app --device <워치 UDID> --timeout 240 "$WAPP"
xcrun devicectl device process launch --device <워치 UDID> kr.fetm.sailingmonitor.watchkitapp
```

> `-destination 'platform=watchOS,id=...'` 로 빌드하면 기기 대기에서 타임아웃 날 수 있다.
> `generic/platform=watchOS` 로 빌드하고 `devicectl` 로 따로 설치하는 편이 확실하다.
>
> `devicectl device info` 의 OS 버전·터널 상태는 **캐시된 값일 수 있다.**
> 워치가 연결되지 않은 동안에는 마지막 값을 그대로 보여준다. 설치 출력 쪽을 믿을 것.

### 3.5 시뮬레이터

CoreBluetooth 가 동작하지 않는다(상태 `unsupported`).
UI 레이아웃 확인용으로만 쓰고 BLE 테스트는 반드시 실기기에서 한다.

---

## 4. 화면

### 공통 — 설정 (모듈 선택)

첫 실행 시에는 고정된 모듈이 없어 **탐색 모드**로 뜬다. 라이브 화면 대신
"연결할 모듈을 고르세요" 안내가 나오고, 설정에서 주변 모듈 목록을 보여준다.

```
발견된 모듈          RSSI
─────────────────────────
hojun               −45 dBm  · 5.53 kn   ← 탭
KOR1234             −78 dBm  · 4.10 kn
```

- 가까운 순(RSSI 내림차순) 정렬. 연결 없이 광고만으로 속도까지 미리 보인다
- 하나를 고르면 `{ 이름, module_id, 주변장치 식별자 }` 를 저장하고 즉시 연결
- 다음 실행부터는 목록 없이 바로 그 모듈에 붙는다
- "다른 모듈 선택" 으로 해제하면 목록이 다시 나온다

아이폰과 워치는 각각 한 번씩 골라야 한다 (주변장치 식별자가 기기마다 다르므로).

### iOS — 라이브 탭

연결해서 4 Hz notify 를 받는다.

- 속도(kn) 큰 숫자, COG(+나침반 방위), 힐 각(+좌/우현), 배터리, RSSI
- 상단에 연결 상태 배지 — `대기 / 검색 중 / 연결 중 / 연결됨 / 재연결 중…`
- **진단 패널**: 실측 수신율(Hz), 상태 머신 값, 마지막 수신 경과, ESP32 uptime
  → 4 Hz 가 실제로 나오는지 여기서 숫자로 확인한다
- **디버그 로그**: 스캔/연결/해제/재연결까지 타임스탬프와 함께 전부 기록.
  재연결에 몇 초 걸렸는지도 로그에 찍힌다
- 연결이 끊기면 마지막 값을 **회색으로 유지**하고 배지가 "재연결 중… N초"

### iOS — 스캐너 탭

연결하지 않고 광고만 본다. `allowDuplicates` 켬.

- 모듈별 sog / cog / heel / seq / RSSI / 배터리
- **수신율(%)** — `seq` 점프로 유실을 추정
  (`유실 = Σ((seq_now − seq_prev + 256) mod 256 − 1)`)
- **갱신(Hz)** — 실측 광고 페이로드 갱신율. 펌웨어 기대치 1 Hz

광고 인터벌이 200 ms 라 콜백은 초당 ~5회 오지만 페이로드는 1 Hz 로만 바뀐다.
그래서 `seq` 가 실제로 바뀐 것만 새 패킷으로 센다.

### watchOS — 항해용 계기판으로 동작시키기

`HKWorkoutSession(.sailing)` 을 **앱 진입 시 버튼 없이 자동 시작**한다.
버튼을 없앤 이유는 화면을 최소로 두기 위해서고, 세션을 유지하는 이유는 이것이다.

| | 손목 내렸을 때 화면 갱신 |
|---|---|
| 세션 있음 | **초당 1회** |
| 세션 없음 | **분당 1회** (사실상 멈춘 숫자) |

세션이 있어야 시계 화면으로 넘어가지 않고 내 앱이 어둡게 남으며, 화면이 꺼져도
앱이 suspend 되지 않아 BLE 가 유지된다.

**건강 앱에 기록은 남기지 않는다.** `HKLiveWorkoutBuilder`(데이터 수집기)를 쓰지 않기
때문이다. Always On·백그라운드 권한은 세션 자체가 주고, 저장은 빌더가 하는 일이다.
앱을 열 때마다 "세일링 운동" 이 쌓이면 지저분하다.

**Always On 레이아웃** — `@Environment(\.isLuminanceReduced)` 로 감지해서
어두워지면 COG·힐을 숨기고 속도만 84pt 로 키운다. 정보를 줄이는 게 아니라
초당 1회로만 갱신되는 상황에 맞게 읽을 것만 남기는 것이다.

**물 잠금** — `WKInterfaceDevice.enableWaterLock()`. 설정 페이지에 버튼.
크라운을 돌려 해제하면 스피커로 물을 빼낸다.

### iOS — 화면 꺼짐 방지

iOS 에는 Always On 같은 개념이 없다. `HKWorkoutSession` 을 켜도 화면이
안 꺼지게 되지 않으므로 **아이폰은 운동 앱으로 만들지 않았다.**
대신 자동 잠금만 막는다.

```swift
UIApplication.shared.isIdleTimerDisabled = active && ble.isLive
```

세 조건을 모두 만족할 때만 켠다 — 라이브 탭에 있고, 값을 받고 있고, 앱이 foreground 일 때.
백그라운드로 나가면 반드시 되돌린다. 안 그러면 다른 앱을 쓸 때도 화면이 안 꺼진다.

### watchOS — 세로 2페이지

- **1페이지**: 속도(제일 크게) · COG · 힐. 상단에 점 + 모듈 이름뿐. 그 외 아무것도 없음
- **2페이지**: 설정 — 모듈 선택/해제 + 세션 상태 + 물 잠금 + 진단값 + 강제 재연결

---

## 5. 재연결 설계

`BLEManager` 의 핵심. 상태 머신은
`idle / choosing / scanning / connecting / connected / reconnecting`.

```
didDisconnectPeripheral
   ├─ isLive = false            → 뷰가 마지막 값을 회색으로 유지
   ├─ state  = .reconnecting    → "재연결 중…" 배지
   ├─ central.connect(p)        ★ 타임아웃 없는 pending connect.
   │                              주변장치가 다시 광고하는 순간 시스템이 붙여준다
   └─ scanForPeripherals()        주소가 바뀌는 경우까지 대비한 병행 스캔
```

- **앱 재활성화**: `scenePhase == .active` 에서 `appBecameActive()` 호출 →
  연결이 안 되어 있으면 즉시 재시도
- **앱 재실행**: 마지막 주변장치 UUID 를 `UserDefaults` 에 저장해 두고,
  시작 시 `retrievePeripherals(withIdentifiers:)` 로 되살려 **스캔을 기다리지 않고**
  바로 `connect()` 를 건다. 워치 앱을 스와이프 종료 후 재실행해도 1~2초에 붙는 이유
- **이미 연결된 경우**: `retrieveConnectedPeripherals(withServices:)` 로 먼저 확인
- **정체 감지**: 연결은 살아있는데 2초간 값이 안 오면 `isLive = false` 로 내려
  화면이 낡은 값을 라이브인 척 보여주지 않게 한다
- **오접속 차단**: 저장된 주변장치 식별자로 바로 붙는 지름길은 광고 이름 검사를
  건너뛴다. 그래서 연결 후 **매 패킷**의 `module_id` 를 확인하고, 다르면 즉시 끊고
  저장된 식별자를 버린 뒤 다시 찾는다 (`rejectWrongModule`)

## 6. 워치 백그라운드

§4 의 "항해용 계기판으로 동작시키기" 참고. 세션은 자동 시작되며 사용자가
설정 페이지에서 종료할 수 있다.

필요한 설정:

- `Watch/SailingMonitorWatch.entitlements` → `com.apple.developer.healthkit`
- `Watch/Info.plist` → `WKBackgroundModes = [workout-processing]`
- `Watch/Info.plist` → `NSHealthShare/UpdateUsageDescription`

권한 문구에 "건강 데이터를 읽거나 저장하지 않는다" 고 적어뒀다. 실제로 그렇다 —
세션만 쓰고 빌더를 쓰지 않으므로 읽지도 쓰지도 않는다.

## 7. 권한 키 정리

| 키 | iOS | watchOS |
|---|:---:|:---:|
| `NSBluetoothAlwaysUsageDescription` | ✅ | ✅ |
| `UIBackgroundModes = [bluetooth-central]` | ✅ | — |
| `WKBackgroundModes = [workout-processing]` | — | ✅ |
| `NSHealthShareUsageDescription` | — | ✅ |
| `NSHealthUpdateUsageDescription` | — | ✅ |
| HealthKit capability (entitlement) | — | ✅ |
