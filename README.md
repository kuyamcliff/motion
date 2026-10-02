# MOTIONFORGE Mobile

An offline-first, After-Effects-class motion graphics and video editor for Android. It has two parts:

* a portable **C++20 engine** (`engine/`), which owns the project model, undo, rendering, audio, captions, packages and scripting;
* a **Kotlin / Jetpack Compose** app shell (`app/`), which provides the UI, MediaCodec decode/encode and Android integration.

Every button is backed by real engine behavior. The app requests no INTERNET permission. Offline captions use **Whisper** (whisper.cpp) with a bundled model.

## Build

Requirements:

* JDK 17+
* Android SDK 35
* NDK 27.2.12479018
* CMake 3.22

```bash
# Fast emulator-only build
./gradlew :app:assembleDebug -Pmf.abis=x86_64

# Phone + emulator build (default ABIs: arm64-v8a, x86_64)
./gradlew :app:assembleDebug :app:assembleRelease
```

The output is in `app/build/outputs/apk/{debug,release}/`. Release builds are signed with the local debug key so they can be installed for testing. Sign with your own key for distribution.

The build places these files in the APK as assets:

* the Whisper model `ggml-tiny.en-q5_1.bin` (31 MB, SHA-256 checked). It comes from `.cache/models/` or is downloaded once at build time.
* the DejaVu fonts
* the JFK speech sample used for the captions demo and tests

## Tests

The tests run at three layers:

| Layer | Command | What it covers |
|---|---|---|
| Engine unit + integration (host) | `cmake -S engine -B build-whisper -DMF_WITH_WHISPER=ON -DMF_BUILD_TESTS=ON && cmake --build build-whisper -j && ./build-whisper/mftests` | Model ops, undo/redo, keyframes/easing, expressions, rasterizer, effects, text, 3D, audio, storage/recovery, packages (sign/encrypt), scripting sandbox, GIF, tracking, scenario runner. Also checks Whisper on `jfk.wav` against a word error rate limit. |
| Scenarios | `./build-whisper/mfcli scenario tests/scenarios/basic_edit.mftest /tmp/scn` and `./build-whisper/mfcli scenario tests/scenarios/whisper_captions.mftest $PWD` | Scripted editing flows that end in export and validation. |
| Android instrumented E2E | `./gradlew :app:connectedDebugAndroidTest -Pmf.abis=x86_64` | See the list below. |

The Android E2E tests run on a device or emulator.

* `EngineE2ETest` covers:
  * JNI and the command layer
  * move, undo and redo
  * keyframes
  * split
  * render pixels
  * save, reopen and persist
  * crash recovery from the journal
  * **Whisper captions** generated from the bundled speech sample through MediaCodec audio decode
  * MP4 export with MediaCodec/EGL/MediaMuxer, validated
  * GIF export, validated
  * a script run as one undo step, and the permission denial path
  * a round trip of an encrypted project package
* `UiE2ETest` drives the real UI:
  * new project
  * add a text layer
  * undo and redo buttons
  * frame step
  * play and pause
  * save
  * back to Home and reopen

More device suites cover the rest:

* `ScreensE2ETest` clicks through every screen and inspector panel and verifies each result after reopening the project.
* `WorkflowRegressionTest` runs a full creator workflow and pixel-inspects the exported MP4.
* `CombinationsDeviceTest` runs feature combinations plus a 500-layer stress test.
* `VideoLayerDeviceTest` covers decoding, compositing and re-exporting imported video.
* `CapsuleScriptDeviceTest` covers Capsule v2 and Script API v2.

On the host, `test_combo` runs hundreds of pair and random feature combinations (set `MF_COMBO_COUNT` for thousands) and the stress tests.

* Cloud and CI device testing: [docs/CLOUD_ANDROID_TESTING.md](docs/CLOUD_ANDROID_TESTING.md). Scripts are in `scripts/android/`.
* Per-feature status (WORKING_ANDROID / WORKING_ENGINE_ONLY / PARTIAL / UI_ONLY / NOT_IMPLEMENTED): [docs/FEATURE_STATUS.md](docs/FEATURE_STATUS.md).
* GPU renderer roadmap: [docs/GPU_RENDERER_PLAN.md](docs/GPU_RENDERER_PLAN.md).
* Verification results: [docs/VERIFICATION.md](docs/VERIFICATION.md).

## Architecture

```
app/ (Kotlin, Compose)                     engine/ (C++20, no Android deps)
 ├─ ui/ Home, Editor (preview, timeline,    ├─ document   immutable JSON doc, RFC 6902 patch undo/redo, gesture previews, branches
 │   inspector panels), Export, Caption     ├─ model      ~90 undoable operations (layers, keyframes, text, shapes, effects, masks,
 │   Studio, Script Studio, Library,        │             captions, time remap, precompose, capsules…)
 │   Extensions, Fonts, AI Models,          ├─ property   keyframes, easing (bezier, spring, elastic…), expressions (QuickJS)
 │   Settings, Developer Center,            ├─ renderer   CPU compositor: AA rasterizer, blend modes, masks, mattes, adjustment layers,
 │   Project Inspector, Performance         │             precomp cache, ~60 effects, transitions, software 3D (OBJ/GLB, PBR-ish,
 ├─ engine/ NativeBridge (JSON JNI),        │             shadows), particles, captions, motion blur
 │   EditorState                            ├─ audio      mixer, EQ, compressor, gate, pitch, buses, ducking, limiter, FFT analysis
 ├─ media/ MediaBridge (MediaCodec decode,  ├─ captions   whisper.cpp ASR, word timestamps, VAD, segmentation, SRT/VTT/ASS
 │   YUV→RGBA in native), Importer          ├─ storage    atomic saves, CRC32 journal, session lock → crash recovery, versions, migration
 ├─ playback/ Player (render thread,        ├─ package    zip + manifest + BLAKE2b, Ed25519 signatures, Argon2 + XChaCha20 encryption
 │   AudioTrack clock)                      ├─ scripting  sandboxed QuickJS with a permission-gated `mf` API
 └─ export/ ExportEngine (EGL → MediaCodec  ├─ tracking   point / two-point tracking, stabilization
     → MediaMuxer + validation), queue,     ├─ gif        GIF encoder
     foreground service                     └─ engine     facade + .mftest scenario runner
```

Every user action becomes an engine operation (`EditorState.apply` → `NativeBridge.call("apply")`). Each operation is exactly one undo step. Slider and drag gestures use `preview` and `commitPreview`, so dragging stays live and still produces a single undo entry.

## What is not implemented

This build does not include the following:

* **GPU renderer.** Rendering is a multithreaded CPU compositor. Preview resolution adapts to keep playback interactive. Final export always renders at full quality.
* **Native-code and WASM extensions.** Extensions can contain scripts, presets and composite effects. Packages that declare native code are rejected.
* **FBX import.** FBX is proprietary. Use GLB/glTF or OBJ.
* **Speaker diarization.** Captions have a speaker field you can edit, but speakers are not detected automatically.
* **Planar or mesh tracking.** Point tracking, two-point tracking (position, rotation and scale) and stabilization are implemented.
* **Bundled ASR model.** Only English tiny is bundled. Multilingual Whisper models can be imported from the AI Models screen; the app makes no network downloads.
