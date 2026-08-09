#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────
#  하드웨어 없이 돌릴 수 있는 전체 검증.
#
#    ./tools/verify.sh            전부
#    ./tools/verify.sh quick      펌웨어 컴파일/Xcode 빌드는 건너뛰고 로직만
#
#  검사 항목
#    1. 펌웨어 호스트 검증 (인코딩 / 시뮬레이션 궤적 / 범위)
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
c++ -std=c++17 -Wall -Wextra -O2 -I"$ROOT/firmware/include" \
    -o "$BUILD/sim_test" "$ROOT/firmware/tools/sim_test.cpp"
"$BUILD/sim_test" "$BUILD/vectors.tsv"
ok "펌웨어 로직 통과 — 벡터 $(grep -vc '^#' "$BUILD/vectors.tsv")줄 생성"

# ── 2. C++ ↔ Swift 교차 검증 ─────────────────────────────────────────────
bar "2/4  펌웨어 인코더 ↔ 앱 디코더 교차 검증"
swiftc -O -o "$BUILD/decode_check" \
    "$ROOT/app/Shared/Protocol.swift" \
    "$ROOT/tools/swift_decode_check/main.swift"
"$BUILD/decode_check" "$BUILD/vectors.tsv"
ok "프로토콜 구현 일치"

if [ "$QUICK" = "quick" ]; then
    bar "quick 모드 — 컴파일 단계는 건너뜀"
    exit 0
fi

# ── 3. 펌웨어 컴파일 ─────────────────────────────────────────────────────
bar "3/4  펌웨어 컴파일 (ESP32-S3)"
( cd "$ROOT/firmware" && pio run ) | tail -5
ok "펌웨어 빌드"

# ── 4. 앱 빌드 ───────────────────────────────────────────────────────────
bar "4/4  iOS / watchOS 앱 빌드"
( cd "$ROOT/app" && xcodegen generate >/dev/null )

IOS_DEST='platform=iOS Simulator,name=iPhone 17'
WATCH_DEST='platform=watchOS Simulator,name=Apple Watch Series 11 (46mm)'

xcodebuild -project "$ROOT/app/HohoBLE.xcodeproj" -scheme "HohoBLE" \
    -destination "$IOS_DEST" -configuration Debug \
    CODE_SIGNING_ALLOWED=NO build 2>&1 | grep -E "error:|BUILD" | grep -v "Metadata" || true

xcodebuild -project "$ROOT/app/HohoBLE.xcodeproj" -scheme "HohoBLE Watch App" \
    -destination "$WATCH_DEST" -configuration Debug \
    CODE_SIGNING_ALLOWED=NO build 2>&1 | grep -E "error:|BUILD" | grep -v "Metadata" || true

ok "앱 빌드"

bar "전체 통과"
