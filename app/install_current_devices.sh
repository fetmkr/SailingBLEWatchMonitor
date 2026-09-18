#!/bin/zsh
set -euo pipefail

# Always build the current sources into a new directory.  Reusing DerivedData or
# searching it with `find | head` has installed an old Watch binary before.
APP_DIR=${0:A:h}
DEVICES_JSON=$(mktemp /tmp/sail-devices.XXXXXX)
trap 'rm -f "$DEVICES_JSON"' EXIT
xcrun devicectl list devices --json-output "$DEVICES_JSON" >/dev/null

device_id() {
  python3 - "$DEVICES_JSON" "$1" <<'PY'
import json, sys
devices = json.load(open(sys.argv[1]))["result"]["devices"]
kind = sys.argv[2]
for device in devices:
    if device.get("hardwareProperties", {}).get("deviceType") != kind:
        continue
    if device.get("connectionProperties", {}).get("tunnelState") == "unavailable":
        continue
    print(device["identifier"])
    break
else:
    raise SystemExit(f"사용 가능한 {kind} 기기를 찾지 못했습니다")
PY
}

WATCH_DEVICE=${WATCH_DEVICE:-$(device_id appleWatch)}
IPHONE_DEVICE=${IPHONE_DEVICE:-$(device_id iPhone)}
BUILD_NUMBER=$(date +%Y%m%d)
BUILD_STAMP=$(date +%Y%m%d-%H%M%S)
DERIVED_DATA="/tmp/sail-current-${BUILD_STAMP}"
IOS_APP="${DERIVED_DATA}/Build/Products/Debug-iphoneos/SailingMonitor.app"
WATCH_APP="${IOS_APP}/Watch/SailingMonitor Watch App.app"

cd "$APP_DIR"

xcodebuild \
  -project SailingMonitor.xcodeproj \
  -scheme SailingMonitor \
  -destination 'generic/platform=iOS' \
  -configuration Debug \
  -derivedDataPath "$DERIVED_DATA" \
  CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
  build -allowProvisioningUpdates

# Keep the phone's embedded Watch app and the directly installed Watch app on
# the same build, so the companion app cannot put an older binary back.
xcrun devicectl device install app --device "$IPHONE_DEVICE" "$IOS_APP"
xcrun devicectl device install app --device "$WATCH_DEVICE" --timeout 240 "$WATCH_APP"
xcrun devicectl device process launch \
  --device "$WATCH_DEVICE" --terminate-existing \
  kr.fetm.sailingmonitor.watchkitapp

xcrun devicectl device info apps --device "$IPHONE_DEVICE" \
  --filter 'bundleIdentifier == "kr.fetm.sailingmonitor"'
xcrun devicectl device info apps --device "$WATCH_DEVICE" \
  --filter 'bundleIdentifier == "kr.fetm.sailingmonitor.watchkitapp"'
