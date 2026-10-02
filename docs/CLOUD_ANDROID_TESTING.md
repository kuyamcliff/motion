# Running and testing the APK from Claude Code cloud sessions

This guide covers how Claude Code, running in an Anthropic cloud container (claude.ai/code), can build MOTIONFORGE, boot an Android emulator, install the APK, run the end-to-end tests, then read the logs and screenshots, fix the code and re-run.

## What the container offers and what it doesn't

| Need | Cloud container | Notes |
|---|---|---|
| JDK, Gradle, CMake, NDK, Android SDK | yes | Install on demand with `scripts/android/setup-sdk.sh`, or from the environment's setup script so each new session starts ready. |
| Downloads (Google Maven, Maven Central, Hugging Face) | yes, through the network policy | Maven Central can return HTTP 429. `settings.gradle.kts` lists Google's Maven Central mirror first. |
| Emulator, x86_64 system image | yes | No `/dev/kvm`, so the emulator runs in **software (TCG)**. It's slow but works: boot takes 5–10 min; tests take minutes. |
| Hardware GPU, hardware codecs | no | The emulator uses SwiftShader (software GLES) and Android's software codecs. That's enough to verify MediaCodec, EGL and export correctness, but not phone performance. |
| A real phone | no | A real device needs your own computer. See "Real devices" below. |
| Persistence | per session | The container is reclaimed when idle. Commit and push results. A new session starts from a clean machine plus the setup script. |

## One-time environment setup

In the cloud environment settings (environment menu → Edit → **Setup script**), add:

```bash
# Android SDK + emulator image for MOTIONFORGE (takes a few minutes; runs when each session starts)
cd "$CLAUDE_PROJECT_DIR" 2>/dev/null || cd /home/user/motion
bash scripts/android/setup-sdk.sh 30
```

Optionally, pre-fetch the Whisper model so builds don't download it:

```bash
mkdir -p .cache/models && curl -fL -o .cache/models/ggml-tiny.en-q5_1.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin
```

## The loop Claude Code runs

```bash
# 1. Build the debug APK and the test APK. Build BEFORE starting the emulator: a 4-core container
#    can't run Gradle/NDK and a software emulator at once (system_server gets killed).
./gradlew :app:assembleDebug :app:assembleDebugAndroidTest -Pmf.abis=x86_64 && ./gradlew --stop

# 2. Boot the emulator and wait until it is genuinely usable (boot + package manager + settled load)
scripts/android/start-emulator.sh mf30

# 3. Install and run tests, keeping logs, logcat and a screenshot under build/device-tests/
scripts/android/run-tests.sh --no-build                                    # everything
scripts/android/run-tests.sh --no-build com.motionforge.app.ScreensE2ETest # one class

# 4. Read the failure (instrument output + logcat), fix the code, rebuild, re-run only what failed.
```

For quick manual checks, Claude can also drive the app directly:

```bash
adb shell am start -n com.motionforge.mobile/com.motionforge.app.MainActivity
adb shell screencap -p /sdcard/s.png && adb pull /sdcard/s.png   # Claude can view PNGs
adb shell uiautomator dump /sdcard/ui.xml && adb pull /sdcard/ui.xml
adb shell input tap 540 1200 ; adb shell input text "hello" ; adb shell input keyevent KEYCODE_SPACE
adb logcat -d -b crash                                            # crashes
```

## Pitfalls learned while doing this

1. **Never build while the emulator runs tests.** Load above about 12 makes the watchdog kill `system_server`. Symptoms are `Can't find service: package`, `DeadSystemException` and installs failing with "Broken pipe". Build first, stop the Gradle daemon, then test. If the system server does die, wait until `pm path android` answers repeatedly and the load is below 4; `start-emulator.sh` does this.
2. **Post-boot work** (dexopt) keeps load high for 10–20 minutes after the first boot. Tests that start too early flake.
3. **Compose idling never settles during playback.** UI tests can't use Compose finders while the player renders frames. Poll app state instead and use key events (Space toggles playback). See `UiE2ETest`.
4. **Timeouts:** the software emulator runs about 10–50× slower than a phone. Use generous waits (60–90 s per step) and run Whisper with 4 threads.
5. **Container restarts:** a reclaimed container loses the running emulator. Re-run `start-emulator.sh`. The AVD in `~/.android` survives within the same session.
6. **"Lost network stack" framework crash loop.** On a slow device the NetworkStack process can die. After 30 minutes of uptime, Android 11 then crashes `system_server` on purpose, so zygote and every app restart, over and over. The crash buffer shows `FATAL EXCEPTION IN SYSTEM PROCESS: main ... IllegalStateException: Lost network stack`. `start-emulator.sh` rate-limits that crash through DeviceConfig (`connectivity/always_ratelimit_networkstack_crash=true`). Re-apply it after every cold boot, since the data partition is temporary.
7. **Use `scripts/android/wait-stable.sh` before installing** after any disturbance. It waits for the same `system_server` PID for 2 minutes with core services registered.
8. **More vCPUs help.** Software emulation runs one thread per vCPU; `-cores 4` on a 4-core container noticeably shortens boot and test time.
9. **Don't edit a shell script while it's running.** Bash reads scripts as it executes them.
10. **`adb logcat -d` can take minutes** on a slow emulator. Filter with `-b crash` or `-t N`, and wrap commands in `timeout`.

## Device matrix

The cloud container runs one software emulator at a time. The full matrix runs in CI (`.github/workflows/device-matrix.yml`) on hosted runners **with KVM**. Each cell runs the full instrumented suite:

| Cell | API | Form factor | Orientation | RAM | Purpose |
|---|---|---|---|---|---|
| low-end | 24 (Android 7.0, minSdk) | phone 720p | portrait | 1.5 GB | Oldest supported OS; low memory |
| mid | 30 (Android 11) | phone 1080p | portrait | 3 GB | Baseline (also the cloud emulator) |
| high | 34 (Android 14) | phone 1440p | portrait | 6 GB | Current OS, new permission model |
| tablet | 33 (Android 13) | tablet 2560×1600 | landscape | 4 GB | Wide layout, side inspector |
| landscape phone | 30 | phone | landscape | 3 GB | Rotation and layout |

Emulators can't cover **hardware GPU and codec variation**: vendor H.264/HEVC encoders, Mali/Adreno GLES drivers, or thermal throttling. For that, use either:

* **Firebase Test Lab** (physical devices). The command is in the workflow file as an optional job; it needs a `GCP_SA_KEY` secret:
  `gcloud firebase test android run --type instrumentation --app app-debug.apk --test app-debug-androidTest.apk --device model=a10,version=29 --device model=oriole,version=33 ...`
* **Real devices on your own computer**, driven by a Claude Code session running locally (the Claude desktop app, or `claude remote-control` in a terminal in the repo). The same `scripts/android/run-tests.sh` works against a USB-connected phone.

## What each test layer proves

| Layer | Where it runs | What it proves |
|---|---|---|
| `mftests` (host) | any Linux / CI | The engine is correct, including thousands of feature combinations and stress cases |
| `EngineE2ETest` | emulator / device | JNI, persistence, recovery, Whisper, MediaCodec export |
| `WorkflowRegressionTest` | emulator / device | A full creator workflow with pixel-checked export |
| `CombinationsDeviceTest` | emulator / device | Feature combinations through JNI, plus save/reopen |
| `ScreensE2ETest` | emulator / device | Every screen and panel, clicked like a user, with results verified after reopen |
| `UiE2ETest` | emulator / device | The core editing loop through real taps and keys |
