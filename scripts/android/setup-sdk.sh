#!/usr/bin/env bash
# Installs everything needed to build MOTIONFORGE and run it in a headless emulator on a
# Linux container (works without KVM, e.g. Claude Code cloud sessions). Idempotent.
# Usage: scripts/android/setup-sdk.sh [api-level ...]   (default system image: 30)
set -euo pipefail
SDK="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
APIS=("${@:-30}")
mkdir -p "$SDK"
if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
  tmp=$(mktemp -d)
  curl -fsSL -o "$tmp/clt.zip" https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
  unzip -q "$tmp/clt.zip" -d "$tmp"
  mkdir -p "$SDK/cmdline-tools" && rm -rf "$SDK/cmdline-tools/latest" && mv "$tmp/cmdline-tools" "$SDK/cmdline-tools/latest"
  rm -rf "$tmp"
fi
SM="$SDK/cmdline-tools/latest/bin/sdkmanager"
yes | "$SM" --sdk_root="$SDK" --licenses >/dev/null || true
pkgs=("platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018" "cmake;3.22.1" "emulator")
for a in "${APIS[@]}"; do pkgs+=("system-images;android-$a;default;x86_64"); done
"$SM" --sdk_root="$SDK" --install "${pkgs[@]}" >/dev/null
echo "sdk.dir=$SDK" > "$(dirname "$0")/../../local.properties"
# One AVD per requested API level (phone profile). Tablet/landscape runs reuse it with a different
# display size/rotation (see run-matrix.sh).
for a in "${APIS[@]}"; do
  name="mf$a"
  if [ ! -d "$HOME/.android/avd/$name.avd" ]; then
    echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd -n "$name" -k "system-images;android-$a;default;x86_64" -d pixel_4 >/dev/null
    printf 'hw.ramSize=2048\nhw.cpu.ncore=2\ndisk.dataPartition.size=6G\nhw.keyboard=yes\n' >> "$HOME/.android/avd/$name.avd/config.ini"
  fi
done
echo "Android SDK ready in $SDK (AVDs: ${APIS[*]/#/mf})"
