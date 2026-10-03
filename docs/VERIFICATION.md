# Verification

All results below come from the build environment: a Linux container, and an Android 11 x86_64 emulator running in **software emulation** (no KVM). The raw logs are in [`test-logs/`](test-logs). Per-feature status is in [FEATURE_STATUS.md](FEATURE_STATUS.md).

## Host (engine)

| Suite | Result |
|---|---|
| `mftests`: 59 test groups (model, undo, keyframes, expressions, rendering, effects, text, 3D, audio, storage/recovery, packages, scripting, capsules, tracking, scenarios, regressions) | **59/59 pass** |
| Pair combinations: every pair of 24 feature builders | **300/300 pass** |
| Random combinations: 3–8 features each, with determinism, round-trip, undo-all/redo-all and validation checks | **400/400** by default; **3,000/3,000** in the long run (37,820 ops applied, 38 cleanly rejected) |
| Every effect × 6 layer kinds, with keyframed parameters | **402/402 pass** |
| Stress: 500 layers / 2000 keyframes, 4K with 5 effects, 8-deep nesting + particles + 20 models + 2000 captions, 1 h timeline | pass (timings in `host-mftests.txt`) |
| Whisper on `jfk.wav` | WER 0.000 |
| Scenarios `basic_edit`, `whisper_captions` | pass |

## Android device suites

30 tests in 7 classes, run by `scripts/android/run-tests.sh` (one instrumentation per class) on the non-debuggable `uitest` build. The final full run (`test-logs/android-full-suite-uitest.txt`) passed 29/30 tests: every class except `UiE2ETest`, whose pause step relied on an injected Space key that didn't arrive. The test now taps Pause through UiAutomator and retries the Space key, and passes (`test-logs/android-UiE2ETest-rerun.txt`).

| Class | Result on the current code | Covers |
|---|---|---|
| `EngineE2ETest` (7) | **7/7** | edit/undo/render/persist, crash recovery, Whisper captions on device, MP4 + GIF export validation, script single-undo + permission denial, encrypted package round trip |
| `UiE2ETest` (1) | **1/1** | new project → add text → undo/redo buttons → frame step → play → tap Pause → Space play/pause → save → reopen |
| `WorkflowRegressionTest` (1) | **1/1** | import video → trim → split → invert FX → mask → animated title → audio → Whisper captions → MP4 export → reopen → pixel and track inspection of the export |
| `CombinationsDeviceTest` (2) | **2/2** | 24 random feature combinations, each with save/reopen and render comparison; 500-layer stress test |
| `VideoLayerDeviceTest` (3) | **3/3** | MediaCodec decode → composite → re-export pixels; 38-step decoder seek stress; proxies (transcode on device, preview uses the proxy, export and the "off" setting use the original, deleted proxy falls back, persists) |
| `GestureE2ETest` (3) | **3/3** | real touch input: timeline trim, move, two-finger cancel, pinch zoom; preview move and scale handles, mask rectangle, freehand draw, pen tool; graph-editor keyframe drag and bezier handles (persisted) |
| `CapsuleScriptDeviceTest` (2) | **2/2** | Capsule v2 controls, plus the `.mfcapsule` export → delete → import → insert round trip; Script API v2 across every area as one undo step |
| `ScreensE2ETest` (15) | **15/15 as one class run** on the `uitest` build. On the *debug* build, an emulator input-dispatch ANR in the first test aborted whole-class runs (see below). | every screen and inspector panel, verified after reopening |
| `FailureArtifacts` (rule) | — | screenshot + UI dump on every failure |

## Bugs the device and combination suites found (all fixed)

1. **Video showed green frames after the decoder started.** The decoder flushed right after `start()`, which discards the H.264 SPS/PPS. Found by pixel inspection of the workflow export; ffmpeg confirmed the source file was clean.
2. **A relinked video could keep showing the old file.** The decoder pool and the native frame cache were keyed by asset ID only.
3. **Extension effects never appeared in the effect browser,** and the registry was cached for the app's lifetime.
4. **Capsule controls bound to effect parameters broke on insert,** because effect IDs were re-generated.
5. **Relink in Project Inspector always failed:** the new file wasn't passed to the engine.
6. **Expression errors leaked between projects:** an engine-global map keyed by layer ID.
7. **The extension Uninstall button was pushed off-screen** by the Enabled switch row.
8. **The Developer Center Scenarios tab was clipped off-screen** on phone widths.
9. **Playback crashed on stop:** AudioTrack was used after release.
10. **The UI thread starved during playback:** a spinning frame loop, the render thread at high priority, and the inspector re-evaluating every frame.
11. **The first launch copied the 31 MB speech model on the main thread** (ANR risk on low-end phones).

Found by the gesture tests (`GestureE2ETest`), which drive real touch input:

12. **On phones the timeline showed no clips.** The "pinch here to zoom" hint wrapped one letter per line, which grew the zoom strip until the layer rows had zero height, so nothing could be seen or touched. The hint is now single-line, and the compact timeline has a 140 dp minimum.
13. **Trims and moves stalled after the first frame of a drag.** The clip drag handler was keyed on the document revision and scroll position, so the first live-preview update or edge auto-scroll restarted it mid-gesture.
14. **Pinch-to-zoom on the zoom strip only worked over the hint text.** The handler ignored touches that started on a chip, and chips fill the strip on a phone.
15. **Pen tool points were lost when tapping quickly:** the second of two quick taps counted as a double tap (view reset).
16. **The editor top bar was unreadable on phones.** The title had no explicit color (dark on dark), and the timecode wrapped in a column about 8 dp wide. On phones, the command palette and composition settings now live in the More menu.
17. **(Found while fixing 16.)** The phone-only menu items were dropped, because `forEach` bound only to the second list.

## Test-infrastructure findings (emulator, not app bugs)

These are documented in [CLOUD_ANDROID_TESTING.md](CLOUD_ANDROID_TESTING.md):

* **Android 11's "Lost network stack" crash loop** of `system_server`: rate-limited through DeviceConfig.
* **"System UI isn't responding" dialogs** covering the app: `hide_error_dialogs`.
* **Debuggable apps can't be AOT-compiled beyond `quicken`.** Compose then runs through the JIT, which under software emulation stalls the UI thread into input-dispatch ANRs. Device tests therefore target the **`uitest`** build type (debug-signed, not debuggable, AOT `speed-profile`; a full `speed` compile fails in dex2oat on the large material-icons dex).
* **Builds while tests run can kill `system_server`.** The runner waits for a stable `system_server` PID.
* **The Wi-Fi stack crashed `system_server`** (`WifiHandlerThread: Could not fetch IpMemoryStore`) when the network stack was slow. `start-emulator.sh` turns off Wi-Fi and mobile data, which the tests don't need.
* **Failure screenshots showed the launcher:** JUnit runs `@After` (which closed the activity) before a rule's `failed()`. UI tests now close the activity from the `FailureArtifacts` rule, after the capture.

## Not verified here

* Real phones, GPU drivers, vendor codecs, and performance and thermals. The emulator is software-only. A device matrix (KVM emulators for API 24/30/33/34, phone/tablet, portrait/landscape) and an optional Firebase Test Lab job on physical devices are configured in `.github/workflows/device-matrix.yml` but haven't been run from here.
* Gesture-level UI interactions: drag-to-trim, pinch, two-finger cancel, multi-select drags. The engine operations behind them are verified; the gestures themselves aren't automated yet.
