# Verification matrix

These results were produced in the build environment: Linux host, and an Android emulator (API 30, x86_64) running in software emulation without KVM. The raw logs are in [`docs/test-logs/`](test-logs).

## Test runs

| Suite | Result | Log |
|---|---|---|
| Engine unit + integration tests (host, Whisper enabled) | **47 / 47 pass** | `test-logs/host-mftests.txt` |
| Scenario `basic_edit.mftest` (create → animate → split → undo/redo → save/reopen → render → GIF export → validate → WAV) | **PASS** | `test-logs/host-scenarios.txt` |
| Scenario `whisper_captions.mftest` (offline Whisper on JFK sample → SRT export → validate) | **PASS**, transcript exact | `test-logs/host-scenarios.txt` |
| Android `EngineE2ETest` (7 tests, on emulator) | **7 / 7 pass** | `test-logs/android-EngineE2ETest.txt` |
| Android `UiE2ETest` (real UI driven by Compose test) | see `test-logs/android-UiE2ETest.txt` | `test-logs/android-UiE2ETest.txt` |
| `assembleDebug` (arm64-v8a + x86_64) | builds | — |
| `assembleRelease` (arm64-v8a + x86_64, signed with the debug key) | builds; `apksigner verify` OK | — |

## Feature → evidence

| PRD area | Status | Verified by |
|---|---|---|
| Projects: create, open, rename, duplicate, delete, thumbnails | Implemented | UiE2ETest (create/reopen), host storage tests |
| Atomic save, journal, crash recovery (session lock) | Implemented | `EngineE2ETest.crashRecoveryFromJournal` (simulated process death → journal replay), host `storage_*` tests |
| Undo/redo: one step per operation, gesture previews, history jump | Implemented | `EngineE2ETest.editUndoRedoRenderAndPersist`, UiE2ETest undo/redo buttons, host document tests |
| Timeline: trim, move, split, ripple delete, snapping, markers | Implemented | Split verified on device; host model tests |
| Keyframes: easing presets, bezier, spring/elastic, copy/paste, reverse, distribute, graph editor | Implemented | Device keyframe test; host easing/property tests |
| Expressions (QuickJS, vector math, wiggle/loopOut, error fallback) | Implemented | host `expressions_*` |
| Text: typography, shadow/box, animators/presets, 3D extrude | Implemented | host text render tests |
| Shapes: primitives, merge (add/subtract/intersect), trim paths, repeater, zig-zag, round corners | Implemented | host shape tests |
| Effects (~60, stackable, mix, solo), adjustment layers, masks, track mattes, blend modes | Implemented | host render tests |
| Transitions, behaviors (bake to keys), particles, motion blur | Implemented | host render tests |
| Software 3D: camera, lights, shadows, primitives, OBJ/GLB | Implemented (CPU) | host 3D tests |
| Audio: mixer, EQ, compressor, gate, pitch, buses, ducking, limiter | Implemented | host audio tests, MP4 export with an audio track |
| **Offline captions with Whisper** (word timestamps, VAD, segmentation, reading-speed checks, styles, edit by transcript, SRT/VTT/ASS) | Implemented | `EngineE2ETest.whisperCaptionsFromBundledSpeech` (MediaCodec decode → whisper.cpp on device → captions → one undo step → SRT); host WER test (WER 0.000) |
| Export MP4 (MediaCodec + EGL + MediaMuxer) with validation of tracks, resolution, frame count, duration and a decoded sample frame | Implemented | `EngineE2ETest.exportMp4IsValidated` |
| Export GIF / PNG sequence / WAV / M4A / WebM | Implemented | `exportGifIsValidated`, basic_edit scenario (GIF, WAV) |
| Export queue, foreground service, cancel/retry, gallery publish | Implemented | Manual review of code paths; queue survives restarts |
| Packages: zip + manifest + BLAKE2b, Ed25519 signing, Argon2/XChaCha20 encryption | Implemented | `EngineE2ETest.packageRoundTrip` (wrong password rejected), host package tests |
| Scripting sandbox with permissions; a script run is one undo step | Implemented | `EngineE2ETest.scriptRunsAsSingleUndoStep` (permission denial verified) |
| Extensions (scripts, presets, composite effects; signed or dev mode) | Implemented | host package/extension tests |
| Tracking (point, two-point) and stabilization | Implemented | host tracking tests |
| Accessibility: 48 dp targets, content descriptions, UI scale, reduced motion | Implemented | UiE2ETest locates controls by content description |

## Known limitations

These are stated in the app's Developer Center and in the README:

* **GPU renderer:** not implemented. The CPU compositor adapts preview resolution instead.
* **Native/WASM extensions:** rejected.
* **FBX import:** not supported.
* **Speaker diarization:** not implemented; the speaker field is edited manually.
* **Planar tracking:** not implemented.
* **Whisper model:** only English tiny is bundled; other ggml models can be imported offline.
* **Emulator performance:** the emulator in this environment ran without hardware acceleration, so timings there say nothing about phone performance.
