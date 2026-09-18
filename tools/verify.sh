#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────
#  하드웨어 없이 돌릴 수 있는 전체 검증.
#
#    ./tools/verify.sh            전부
#    ./tools/verify.sh quick      펌웨어 컴파일/Xcode 빌드는 건너뛰고 로직만
#
#  검사 항목
#    1. 펌웨어 호스트 검증 (인코딩 / 범위 / SD·CASIC·자이로·설정·Range 로직)
#    2. C++ 인코더 → Swift 디코더 교차 검증  ★ 두 구현이 어긋나면 여기서 잡힘
#    3. 펌웨어 실제 컴파일 (PlatformIO, ESP32-S3)
#    4. iOS + watchOS 앱 빌드 (시뮬레이터)
# ─────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/.verify"
QUICK="${1:-}"

mkdir -p "$BUILD"

bar() { printf '\n\033[1m════ %s ════\033[0m\n' "$1"; }
ok()  { printf '\033[32m✅ %s\033[0m\n' "$1"; }

# ── 1. 펌웨어 호스트 검증 + 골든 벡터 생성 ───────────────────────────────
bar "1/4  펌웨어 로직 호스트 검증"
# ★ RAK 보드 것을 쓴다. 옛 firmware/ (Feather) 는 더 이상 검증하지 않는다.
#   두 폴더의 protocol.h 가 이미 갈라져 있었고 (저쪽에는 "값 없음 표식" 이
#   아예 없다), 그런데도 교차 검증이 저쪽을 쓰고 있었다. 실제로 쓰는 보드가
#   아닌 것과 앱을 맞춰보고 있었던 셈이다.
c++ -std=c++17 -Wall -Wextra -O2 -I"$ROOT/firmware-rak/include" \
    -o "$BUILD/proto_test" "$ROOT/firmware-rak/tools/proto_test.cpp"
"$BUILD/proto_test" "$BUILD/vectors.tsv"
ok "펌웨어 로직 통과 — 벡터 $(grep -vc '^#' "$BUILD/vectors.tsv")줄 생성"

# SD 부분 쓰기·CASIC 프레임·자이로 단위·설정 저장·HTTP Range 회귀 시험 (2026-09-14)
c++ -std=c++17 -Wall -Wextra -O2 -I"$ROOT/firmware-rak/include" \
    -o "$BUILD/fw_logic_test" "$ROOT/firmware-rak/tools/fw_logic_test.cpp"
"$BUILD/fw_logic_test" > "$BUILD/fw_logic_test.log"
ok "펌웨어 순수 로직 회귀 시험 통과 ($(grep -c '\[ OK \]' "$BUILD/fw_logic_test.log")개)"

# 3축 hard/soft-iron 타원체 보정: 중심·행렬 복원, 한쪽/평면 표본 거절.
c++ -std=c++17 -Wall -Wextra -Werror -O2 -I"$ROOT/firmware-idf/main" \
    "$ROOT/firmware-idf/tests/mag_calibration_test.cpp" "$ROOT/firmware-idf/main/mag_calibration.cpp" \
    -o "$BUILD/mag_calibration_test"
"$BUILD/mag_calibration_test"

# 방위 식 — 보드(C++ heading_tilt.h)와 데스크탑·아이패드 앱(TS heading.ts)이 같은 값을 내나 (체크리스트 05, 2026-09-15)
c++ -std=c++17 -Wall -Wextra -O2 -I"$ROOT/firmware-rak/include" \
    -o "$BUILD/heading_vectors" "$ROOT/firmware-rak/tools/heading_vectors.cpp"
"$BUILD/heading_vectors" "$BUILD/heading_vectors.tsv" > /dev/null
"$ROOT/desktop/node_modules/.bin/esbuild" "$ROOT/desktop/tools/heading_check.ts" \
    --bundle --platform=node --log-level=warning --outfile="$BUILD/heading_check.cjs"
node "$BUILD/heading_check.cjs" "$BUILD/heading_vectors.tsv"
ok "방위 식 보드 ↔ 앱 일치"

# IDF 실물과 같은 Fusion C + wrapper: 자세·운동 가속·회전·센서 끊김.
cc -std=c99 -Wall -Wextra -O2 -I"$ROOT/firmware-idf/components/fusion" \
    -c "$ROOT/firmware-idf/components/fusion/FusionAhrs.c" -o "$BUILD/FusionAhrs.o"
c++ -std=c++17 -Wall -Wextra -Werror -O2 \
    -I"$ROOT/firmware-idf/components/fusion" -I"$ROOT/firmware-idf/main" -I"$ROOT/firmware-rak/include" \
    "$ROOT/firmware-idf/tests/heading_test.cpp" "$ROOT/firmware-idf/main/heading_filter.cpp" \
    "$BUILD/FusionAhrs.o" -o "$BUILD/heading_filter_test"
"$BUILD/heading_filter_test"

# LoRa 수신 보드 BLE v1/v2, 배 번호 충돌, PPS 프레임·BLE 알림 누락.
"$ROOT/desktop/node_modules/.bin/esbuild" "$ROOT/desktop/tools/fleet_check.ts" \
    --bundle --platform=node --log-level=warning --outfile="$BUILD/fleet_check.cjs"
node "$BUILD/fleet_check.cjs"

# ── 2. C++ ↔ Swift 교차 검증 ─────────────────────────────────────────────
bar "2/4  펌웨어 인코더 ↔ 앱 디코더 교차 검증"
swiftc -O -o "$BUILD/decode_check" \
    "$ROOT/app/Shared/Protocol.swift" \
    "$ROOT/tools/swift_decode_check/main.swift"
"$BUILD/decode_check" "$BUILD/vectors.tsv"
ok "프로토콜 구현 일치"

# 보정 저장 거절이 매초 진행 알림에 묻히지 않는지, 재시도·저장 상태를 구분하는지.
swiftc -o "$BUILD/magcal_state_check" \
    "$ROOT/app/Shared/MagCalibrationState.swift" \
    "$ROOT/tools/magcal_state_check/main.swift"
"$BUILD/magcal_state_check"
ok "워치·아이폰 보정 결과 유지 및 재시도 상태"

if [ "$QUICK" = "quick" ]; then
    bar "quick 모드 — 컴파일 단계는 건너뜀"
    exit 0
fi

# ── 3. 펌웨어 컴파일 ─────────────────────────────────────────────────────
bar "3/4  펌웨어 컴파일"

# ★ pio 결과를 파이프로 넘기면 종료 코드가 tail 것으로 바뀌어 실패가 묻힌다.
#   실제로 이것 때문에 feather_s3_tft 빌드가 깨진 채로 "전체 통과" 가 나온 적이
#   있다. 로그는 파일로 받고 pio 자체의 종료 코드로 판정한다.
#   (앱 빌드 쪽은 원래 이렇게 하고 있었는데 펌웨어만 빠져 있었다)
build_firmware() {
    local dir="$1" name="$2" logfile="$3"
    shift 3
    if ( cd "$dir" && pio run "$@" ) > "$logfile" 2>&1; then
        printf '   %s — OK\n' "$name"
    else
        printf '\033[31m   %s — 빌드 실패\033[0m\n' "$name"
        grep -E "error|Error|FAILED" "$logfile" | sort -u | head -20
        printf '   전체 로그: %s\n' "$logfile"
        return 1
    fi
}

# 보드마다 따로 빌드한다. 한 번에 두 env 를 돌리면 espressif32 플랫폼이
# 프레임워크 경로를 준비하는 도중에 서로 밟아서 간헐적으로 실패한다.
# 지금 쓰는 보드만 짓는다.
#
# ★ 옛 firmware/ (Feather TFT · DevKit) 는 일부러 뺐다. 안 쓰는 보드인데
#   툴체인이 어긋날 때마다 검증 전체를 멈춰 세워서 발목을 잡았다.
#   폴더와 코드는 그대로 두었다 — 필요하면 그 폴더에서 직접 `pio run`.
build_firmware "$ROOT/firmware-rak" "firmware-rak / rak3112"    "$BUILD/fw-rak.log"

ok "펌웨어 빌드 (RAK3112)"

# ── 4. 앱 빌드 ───────────────────────────────────────────────────────────
bar "4/4  iOS / watchOS 앱 빌드"
( cd "$ROOT/app" && xcodegen generate >/dev/null )

IOS_DEST='platform=iOS Simulator,name=iPhone 17'
WATCH_DEST='platform=watchOS Simulator,name=Apple Watch Series 11 (46mm)'

# xcodebuild 결과를 파이프로 넘기면 종료 코드가 grep 것으로 바뀌어 실패가 묻힌다.
# 로그를 파일로 받고 xcodebuild 자체의 종료 코드로 판정한다.
build_target() {
    local scheme="$1" dest="$2" logfile="$3"
    if xcodebuild -project "$ROOT/app/SailingMonitor.xcodeproj" -scheme "$scheme" \
        -destination "$dest" -configuration Debug \
        CODE_SIGNING_ALLOWED=NO build > "$logfile" 2>&1; then
        printf '   %s — OK\n' "$scheme"
    else
        printf '\033[31m   %s — 빌드 실패\033[0m\n' "$scheme"
        grep -E "error:" "$logfile" | sort -u | head -20
        printf '   전체 로그: %s\n' "$logfile"
        return 1
    fi
}

build_target "SailingMonitor"           "$IOS_DEST"   "$BUILD/ios-build.log"
build_target "SailingMonitor Watch App" "$WATCH_DEST" "$BUILD/watch-build.log"

ok "앱 빌드"

bar "전체 통과"
