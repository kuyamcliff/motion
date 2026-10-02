#!/usr/bin/env bash
# Boots a headless emulator and waits until it is genuinely usable (boot completed, package manager
# answering, load settled). Without KVM the emulator runs in software (-accel off): boot takes
# several minutes and the system can be unstable while post-boot work (dexopt) runs.
# Usage: scripts/android/start-emulator.sh [avd-name=mf30] [extra emulator args...]
set -euo pipefail
SDK="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
export PATH="$SDK/platform-tools:$SDK/emulator:$PATH"
AVD="${1:-mf30}"; shift || true
ACCEL="-accel off"; [ -e /dev/kvm ] && [ -w /dev/kvm ] && ACCEL=""
rm -f "$HOME/.android/avd/$AVD.avd/"*.lock
LOG="${MF_EMU_LOG:-/tmp/mf-emulator.log}"
nohup emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect $ACCEL -memory 4096 -cores "${MF_EMU_CORES:-4}" "$@" >"$LOG" 2>&1 &
echo "emulator pid $! (log $LOG)"
adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 5; done
ok=0
while [ $ok -lt 6 ]; do
  if timeout 20 adb shell pm path android >/dev/null 2>&1; then ok=$((ok+1)); else ok=0; fi
  sleep 5
done
# Wait for post-boot background work to settle (otherwise system_server may be killed by its watchdog).
for i in $(seq 1 120); do
  l=$(timeout 15 adb shell cat /proc/loadavg 2>/dev/null | cut -d' ' -f1 || echo 99)
  awk "BEGIN{exit !($l < 4)}" && break
  sleep 10
done
# system_server can restart several times while a software-emulated device settles. Require the same
# system_server PID for two minutes with core services registered before declaring the device usable.
wait_stable() {
  local last="" same=0
  for i in $(seq 1 180); do
    pid=$(timeout 15 adb shell pidof system_server 2>/dev/null | tr -d '\r')
    svc=$(timeout 15 adb shell "service check package; service check mount; service check activity" 2>/dev/null | grep -c ": found")
    if [ -n "$pid" ] && [ "$pid" = "$last" ] && [ "$svc" = 3 ]; then same=$((same+1)); else same=0; fi
    last="$pid"
    [ $same -ge 12 ] && return 0
    sleep 10
  done
  return 1
}
wait_stable || echo "warning: system_server did not stabilise"
adb shell settings put global window_animation_scale 0
adb shell settings put global transition_animation_scale 0
adb shell settings put global animator_duration_scale 0
echo "emulator ready: $(adb devices | sed -n 2p)"
