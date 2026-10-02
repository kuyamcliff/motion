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
  ./gradlew :app:assembleUitest :app:assembleUitestAndroidTest -Pmf.abis=x86_64 --console=plain -q || exit 1
  ./gradlew --stop >/dev/null 2>&1   # free memory for the emulator
fi
scripts/android/wait-stable.sh || true
install() { for i in 1 2 3 4 5; do adb install -r -t "$1" && return 0; sleep 20; done; return 1; }
install app/build/outputs/apk/uitest/app-uitest.apk || exit 1
install app/build/outputs/apk/androidTest/uitest/app-uitest-androidTest.apk || exit 1
# AOT-compile both APKs: removes runtime class verification/JIT, which on a software-emulated CPU can block
# the UI thread long enough for input-dispatch ANRs (keyDispatchingTimedOut) on first use of Compose screens.
aot() {  # speed-profile: hot paths from the baseline profiles in the APK. A full "speed" compile fails here
  # (dex2oat gives up on the very large dex from material-icons-extended).
  for i in 1 2 3; do
    adb shell cmd package compile -m speed-profile -f "$1" >/dev/null 2>&1
    adb shell dumpsys package dexopt 2>/dev/null | grep -A3 "\[$1\]" | grep -q "status=speed" && return 0
    sleep 15
  done
  echo "warning: $1 not AOT-compiled"
}
aot com.motionforge.mobile
aot com.motionforge.mobile.test
stamp=$(date +%Y%m%d-%H%M%S)
adb logcat -c || true
run_one() {  # $1 = -e key, $2 = value (optional)
  if [ -n "${2:-}" ]; then adb shell am instrument -w -r -e "$1" "$2" com.motionforge.mobile.test/androidx.test.runner.AndroidJUnitRunner
  else adb shell am instrument -w -r com.motionforge.mobile.test/androidx.test.runner.AndroidJUnitRunner; fi
}
if [ -n "$FILTER" ]; then
  if [[ "$FILTER" == *.*[A-Z]* ]]; then run_one class "$FILTER"; else run_one package "$FILTER"; fi | tee "$OUT/instrument-$stamp.txt"
else
  # One instrumentation per class: a crash/ANR in one class cannot abort the others.
  : > "$OUT/instrument-$stamp.txt"
  for c in EngineE2ETest UiE2ETest ScreensE2ETest WorkflowRegressionTest CombinationsDeviceTest VideoLayerDeviceTest CapsuleScriptDeviceTest; do
    echo "=== com.motionforge.app.$c" | tee -a "$OUT/instrument-$stamp.txt"
    run_one class "com.motionforge.app.$c" | tee -a "$OUT/instrument-$stamp.txt"
    scripts/android/wait-stable.sh >/dev/null || true
  done
  passed=$(grep -c "^OK (" "$OUT/instrument-$stamp.txt"); total=7
  echo "SUMMARY: $passed/$total classes fully passed" | tee -a "$OUT/instrument-$stamp.txt"
fi
adb logcat -d > "$OUT/logcat-$stamp.txt" 2>/dev/null || true
mkdir -p "$OUT/failures-$stamp" && adb pull /sdcard/Download/mf-test-failures "$OUT/failures-$stamp/" >/dev/null 2>&1; adb shell rm -rf /sdcard/Download/mf-test-failures >/dev/null 2>&1; adb exec-out run-as com.motionforge.mobile tar c files/test-failures 2>/dev/null | tar x -C "$OUT/failures-$stamp" 2>/dev/null; adb shell run-as com.motionforge.mobile rm -rf files/test-failures >/dev/null 2>&1
adb shell screencap -p /sdcard/last.png && adb pull /sdcard/last.png "$OUT/screen-$stamp.png" >/dev/null 2>&1 || true
! grep -qE "^FAILURES|shortMsg|Process crashed" "$OUT/instrument-$stamp.txt"
