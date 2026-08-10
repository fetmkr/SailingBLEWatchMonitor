# app — HOHO 텔레메트리 수신 앱 (iOS + watchOS)

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
│   ├── HohoBLEApp.swift     ← @main, 탭 3개
│   ├── LiveView.swift       ← 라이브 탭
│   ├── ScannerView.swift    ← 스캐너 탭
│   ├── ScannerManager.swift ← 광고 전용 별도 CBCentralManager
│   ├── SettingsView.swift   ← 설정 탭 (모듈 선택/해제)
│   └── Info.plist
└── Watch/
    ├── WatchApp.swift
    ├── WatchLiveView.swift
    ├── WorkoutManager.swift ← HKWorkoutSession(.sailing)
    ├── Info.plist
    └── HohoBLEWatch.entitlements
```

---

## 1. 프로젝트 생성

`.xcodeproj` 는 저장소에 넣지 않는다. 아래로 만든다.

```bash
brew install xcodegen      # 처음 한 번만
cd app
xcodegen generate
open HohoBLE.xcodeproj
```

> `project.yml` 을 고쳤으면 `xcodegen generate` 를 다시 돌려야 반영된다.
> 반대로 Xcode UI 에서 타깃 설정을 바꾸면 다음 generate 때 날아간다 —
> 설정 변경은 항상 `project.yml` 에서 할 것.

## 2. 서명 팀 설정

`project.yml` 의 `DEVELOPMENT_TEAM` 한 줄만 채우면 두 타깃 모두에 적용된다.

```yaml
settings:
  base:
    DEVELOPMENT_TEAM: ABCDE12345   # ← 본인 Team ID
```

Team ID 는 Xcode → Settings → Accounts → 계정 선택 → Manage Certificates,
또는 [developer.apple.com/account](https://developer.apple.com/account) 의 Membership 에서 확인한다.

번들 ID 를 바꾸려면 **세 곳**을 함께 고친다.

1. `project.yml` 의 `PRODUCT_BUNDLE_IDENTIFIER` (iOS)
2. `project.yml` 의 `PRODUCT_BUNDLE_IDENTIFIER` (watch, 반드시 iOS 것 + `.watchkitapp`)
3. `Watch/Info.plist` 의 `WKCompanionAppBundleIdentifier` (iOS 것과 동일)

무료 개인 계정으로도 실기기 배포가 되지만 프로비저닝이 7일마다 만료된다.

## 3. 실기기 배포

### iPhone

1. 아이폰을 USB 로 연결 (또는 같은 Wi-Fi 에서 무선 디버깅 페어링)
2. Xcode 상단 스킴 → **HohoBLE**, 대상 → 본인 아이폰
3. `⌘R`
4. 첫 실행 시 아이폰에서 *설정 → 일반 → VPN 및 기기 관리* → 개발자 앱 신뢰
5. 앱이 뜨면 블루투스 권한 허용

### Apple Watch

워치 앱은 아이폰 앱 번들 안에 임베드되어 있으므로 **아이폰 앱을 설치하면
워치에도 자동으로 설치**된다 (Watch 앱 → 사용 가능한 앱 목록에서 설치).

직접 워치에 빌드해 넣으려면:

1. 워치가 아이폰과 페어링되어 있고, 워치 화면이 켜져 있고 잠금 해제 상태여야 한다
2. Xcode 스킴 → **HohoBLE Watch App**, 대상 → 본인 Apple Watch
3. `⌘R` (첫 설치는 몇 분 걸릴 수 있다)
4. 워치에서 블루투스·건강 권한 허용

> 워치가 대상 목록에 안 뜨면: 워치를 충전기에 올려두고, 아이폰과 워치 모두
> 잠금 해제한 뒤 Xcode → Window → Devices and Simulators 에서 페어링 상태를 확인한다.

### 시뮬레이터

CoreBluetooth 가 시뮬레이터에서는 동작하지 않는다 (상태가 `unsupported`).
UI 레이아웃 확인용으로만 쓰고, BLE 테스트는 반드시 실기기에서 한다.

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

### watchOS — 세로 2페이지

- **1페이지**: 속도 큰 숫자, COG/HEEL, 연결 상태, **훈련 시작/종료** 버튼
- **2페이지**: 설정 — 모듈 선택/해제 + 진단값 + 강제 재연결

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

## 6. 워치 백그라운드 유지

`HKWorkoutSession(activityType: .sailing, locationType: .outdoor)` 이 돌고 있으면
손목을 내려 화면이 꺼져도 앱이 suspend 되지 않아 BLE 연결과 notify 가 유지된다.
세션 없이는 화면이 꺼지고 몇 초 뒤 연결이 끊긴다.

필요한 설정 (이미 되어 있음):

- `Watch/HohoBLEWatch.entitlements` → `com.apple.developer.healthkit`
- `Watch/Info.plist` → `WKBackgroundModes = [workout-processing]`
- `Watch/Info.plist` → `NSHealthShareUsageDescription`, `NSHealthUpdateUsageDescription`

워크아웃이 실제로 살아있는지는 1페이지의 경과 시간과 심박수(bpm)로 확인할 수 있다.

## 7. 권한 키 정리

| 키 | iOS | watchOS |
|---|:---:|:---:|
| `NSBluetoothAlwaysUsageDescription` | ✅ | ✅ |
| `UIBackgroundModes = [bluetooth-central]` | ✅ | — |
| `WKBackgroundModes = [workout-processing]` | — | ✅ |
| `NSHealthShareUsageDescription` | — | ✅ |
| `NSHealthUpdateUsageDescription` | — | ✅ |
| HealthKit capability (entitlement) | — | ✅ |
