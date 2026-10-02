#!/usr/bin/env bash
# Builds (unless --no-build), installs and runs the instrumented test suite, saving logs.
# Usage: scripts/android/run-tests.sh [--no-build] [test-class-or-package]
set -uo pipefail
cd "$(dirname "$0")/../.."
SDK="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
export PATH="$SDK/platform-tools:$PATH"
BUILD=1; [ "${1:-}" = "--no-build" ] && { BUILD=0; shift; }
FILTER="${1:-}"
OUT="${MF_TEST_OUT:-build/device-tests}"; mkdir -p "$OUT"
if [ $BUILD = 1 ]; then
  ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest -Pmf.abis=x86_64 --console=plain -q || exit 1
  ./gradlew --stop >/dev/null 2>&1   # free memory for the emulator
fi
scripts/android/wait-stable.sh || true
install() { for i in 1 2 3 4 5; do adb install -r -t "$1" && return 0; sleep 20; done; return 1; }
install app/build/outputs/apk/debug/app-debug.apk || exit 1
install app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk || exit 1
args=(-w -r)
if [ -n "$FILTER" ]; then
  if [[ "$FILTER" == *.*[A-Z]* ]]; then args+=(-e class "$FILTER"); else args+=(-e package "$FILTER"); fi
fi
stamp=$(date +%Y%m%d-%H%M%S)
adb logcat -c || true
adb shell am instrument "${args[@]}" com.motionforge.mobile.test/androidx.test.runner.AndroidJUnitRunner | tee "$OUT/instrument-$stamp.txt"
adb logcat -d > "$OUT/logcat-$stamp.txt" 2>/dev/null || true
adb shell screencap -p /sdcard/last.png && adb pull /sdcard/last.png "$OUT/screen-$stamp.png" >/dev/null 2>&1 || true
grep -q "^OK (" "$OUT/instrument-$stamp.txt"
