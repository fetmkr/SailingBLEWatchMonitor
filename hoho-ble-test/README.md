# hoho-ble-test

요트 텔레메트리 BLE 테스트 모노레포.

ESP32-S3 개발보드가 가상 GPS 속도 데이터를 만들어 BLE 로 뿌리고,
아이폰과 애플워치 네이티브 앱이 이를 받아 실시간으로 표시한다.

```
hoho-ble-test/
├── PROTOCOL.md     ← 패킷 규격 (단일 진실 공급원)
├── firmware/       ← ESP32-S3 펌웨어  (PlatformIO + Arduino + NimBLE)
├── app/            ← Xcode 프로젝트   (SwiftUI, iOS 앱 + 독립 실행 watchOS 앱)
└── tools/
    ├── verify.sh              ← 하드웨어 없이 돌리는 전체 검증
    └── swift_decode_check/    ← C++ 인코더 ↔ Swift 디코더 교차 검증기
```

---

## 빠른 시작

```bash
# 1) 펌웨어 굽기
cd firmware && pio run -t upload && pio device monitor

# 2) 앱 프로젝트 만들기
cd ../app && xcodegen generate && open HohoBLE.xcodeproj
#    project.yml 의 DEVELOPMENT_TEAM 을 채운 뒤 실기기로 ⌘R
```

자세한 건 [`firmware/README.md`](firmware/README.md) 와 [`app/README.md`](app/README.md).

---

## 프로토콜 요약

전체 규격은 [`PROTOCOL.md`](PROTOCOL.md).

| | |
|---|---|
| 디바이스 이름 | `HOHO-01` |
| Service UUID | `B0A70001-0000-4000-8000-000000000001` |
| Telemetry Characteristic | `B0A70002-0000-4000-8000-000000000001` (Read + Notify) |
| Notify | 12바이트 / 4 Hz |
| Advertising | legacy, 200 ms. Manufacturer Data(9바이트 + seq) 를 1 Hz 로 갱신 |

**규격은 세 곳에 구현되어 있고 항상 함께 고쳐야 한다.**

| 역할 | 파일 |
|---|---|
| 문서 | `PROTOCOL.md` |
| 펌웨어 인코더 | `firmware/include/protocol.h` |
| 앱 디코더 | `app/Shared/Protocol.swift` |

세 개가 어긋나는 걸 막으려고 교차 검증기를 붙여 뒀다 (아래).

---

## 하드웨어 없이 검증하기

```bash
./tools/verify.sh          # 전부 (펌웨어 컴파일 + 앱 빌드 포함, 수 분)
./tools/verify.sh quick    # 로직/프로토콜만 (수 초)
```

| 단계 | 내용 |
|---|---|
| 1 | 펌웨어 로직 호스트 검증 — 인코딩 바이트, 시뮬레이션 궤적, 값 범위, 배터리 감소 |
| 2 | **C++ 인코더 → Swift 디코더 교차 검증** (192개 골든 벡터) |
| 3 | 펌웨어 실제 컴파일 (ESP32-S3) |
| 4 | iOS / watchOS 앱 빌드 |

2단계가 이 저장소의 핵심 안전장치다. 펌웨어가 실제로 뱉는 바이트열을
앱 디코더에 그대로 먹여 값이 일치하는지 본다. 엔디안·오프셋·부호 실수는 여기서 잡힌다.

시뮬레이터에서는 CoreBluetooth 가 동작하지 않으므로(상태 `unsupported`)
**BLE 동작 자체는 반드시 실기기에서 확인**해야 한다.

---

## 수락 기준 체크리스트

실기기에서 확인할 항목.

| # | 항목 | 어디서 확인 |
|---|---|---|
| 1 | 아이폰 라이브 탭에서 4 Hz 로 속도가 부드럽게 갱신 | 라이브 탭 진단 패널의 **수신율**이 4.00 Hz 근처 |
| 2 | 스캐너 탭에서 연결 없이도 속도 변화가 1 Hz 로 보임 | 스캐너 탭의 **갱신** 값이 1.00 Hz 근처, seq 가 1씩 증가 |
| 3 | ESP32 리셋 시 양쪽 앱 모두 3초 내 자동 재연결 | 보드 `RST` 버튼 → 디버그 로그의 `재연결 완료 — N초 걸림` |
| 4 | 워치: 훈련 시작 → 손목 내리고 30초 → 들면 즉시 현재 속도 | 값이 회색이 아니고 uptime 이 이어져 있으면 성공 |
| 5 | 워치 앱 스와이프 종료 후 재실행 시 1~2초 내 재연결 | 상태 배지가 곧바로 "연결됨" |

4번을 판별하는 요령: 손목을 들었을 때 값이 **회색이면** 백그라운드에서 연결이
끊겼다가 방금 복구된 것이고, **처음부터 정상 색이고 ESP32 uptime 이 30초쯤
늘어나 있으면** 세션이 살아있었던 것이다.

---

## 알아두면 좋은 설계 포인트

- **연결 중에도 광고가 계속된다.** 연결되면 `ADV_IND` → `ADV_SCAN_IND` 로 바꿔
  non-connectable 이지만 scannable 인 상태를 유지한다. 그래서 아이폰이 연결해 있는
  동안에도 다른 기기(또는 같은 앱의 스캐너 탭)에서 브로드캐스트를 볼 수 있다.
- **재연결은 pending connect 로 한다.** CoreBluetooth 의 `connect()` 는 타임아웃이
  없어서, 끊긴 즉시 걸어두면 주변장치가 다시 광고하는 순간 시스템이 붙여준다.
  폴링 루프보다 빠르고 배터리에도 유리하다.
- **마지막 값을 지우지 않는다.** 끊겨도 값은 남기고 회색으로만 바꾼다.
  화면이 비어버리면 "지금 안 보이는 게 끊긴 건지 값이 0인 건지" 구분이 안 된다.
- **모든 화면에 실측 진단값이 있다.** 기대치(4 Hz / 1 Hz)와 실측치를 나란히 보여줘서
  "잘 되는 것 같다"가 아니라 숫자로 판정한다.
