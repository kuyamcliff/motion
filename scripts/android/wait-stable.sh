#!/usr/bin/env bash
# Waits until the running emulator's system_server is stable (same PID for 2 min, core services up).
SDK="${ANDROID_SDK_ROOT:-/opt/android-sdk}"; export PATH="$SDK/platform-tools:$PATH"
last=""; same=0
for i in $(seq 1 180); do
  pid=$(timeout 15 adb shell pidof system_server 2>/dev/null | tr -d '\r')
  svc=$(timeout 15 adb shell "service check package; service check mount; service check activity" 2>/dev/null | grep -c ": found")
  if [ -n "$pid" ] && [ "$pid" = "$last" ] && [ "$svc" = 3 ]; then same=$((same+1)); else same=0; fi
  last="$pid"; [ $same -ge 12 ] && { echo "stable (system_server $pid)"; exit 0; }
  sleep 10
done
echo "not stable"; exit 1
