# Feature status inventory

Each feature is marked with the strongest evidence that currently exists for it:

| Status | Meaning |
|---|---|
| **WORKING_ANDROID** | An automated test on an Android device or emulator exercised it end to end and verified the result: engine state, a save/reopen round trip, or pixels in an export. |
| **WORKING_ENGINE_ONLY** | Verified by host engine tests (including combination and stress suites). The Android UI for it exists, but no device test drives it yet. |
| **PARTIAL** | Works, with a documented limitation. |
| **UI_ONLY** | A control exists, but the behavior behind it isn't verified, or is missing. |
| **BROKEN** | Known not to work. |
| **NOT_IMPLEMENTED** | Not built. |

## Summary

Device suite: 34 tests in 8 classes, all passing on the current code; the whole-suite run on the `uitest` build is in VERIFICATION.md. Host: 59 test groups, including 300 + 3,000 combinations and 402 effect × layer pairs.

Bugs found and fixed by this phase's tests:
1. Extension effects never appeared in the effect browser, and the registry was cached for the app's lifetime.
2. Capsule controls bound to effect parameters broke on insert, because effect IDs were re-generated.
3. Relink in Project Inspector always failed: the new file wasn't passed.
4. Expression errors leaked between projects (an engine-global map keyed by layer ID).
5. The extension Uninstall button was pushed off-screen.
6. The Developer Center Scenarios tab was clipped off-screen on phones.
7. (Earlier in this phase) a playback AudioTrack use-after-release crash, and UI-thread starvation during playback.

Test names refer to `app/src/androidTest/...` (device) and `engine/tests/...` (host). Latest results are in [VERIFICATION.md](VERIFICATION.md).

## Projects & persistence
| Feature | Status | Evidence |
|---|---|---|
| Create / open / close project | WORKING_ANDROID | UiE2ETest, ScreensE2ETest (every test) |
| Rename / duplicate / delete project | WORKING_ENGINE_ONLY | host storage tests; UI menu not driven by a device test |
| Save, autosave, reopen | WORKING_ANDROID | EngineE2ETest.editUndoRedoRenderAndPersist, ScreensE2ETest.saveAndReopen |
| Crash recovery from journal | WORKING_ANDROID | EngineE2ETest.crashRecoveryFromJournal |
| Saved versions / restore | WORKING_ANDROID (save) / WORKING_ENGINE_ONLY (restore) | ScreensE2ETest.projectInspectorAndPerformance |
| Project packages, encryption | WORKING_ANDROID | EngineE2ETest.packageRoundTrip |
| Package signing (Ed25519) | WORKING_ENGINE_ONLY | host packages tests |
| Templates (save / new from template) | WORKING_ENGINE_ONLY | — |
| Multiple compositions, switching | WORKING_ENGINE_ONLY | combo tests (precompose) |

## Editing core
| Feature | Status | Evidence |
|---|---|---|
| Undo / redo, one step per op | WORKING_ANDROID | UiE2ETest, EngineE2ETest; host combo suite checks undo-all/redo-all for every combination |
| History list / jump | WORKING_ENGINE_ONLY | host document tests |
| Command palette | WORKING_ANDROID | ScreensE2ETest.timePanelAndCommandPalette |
| Keyboard shortcuts | WORKING_ANDROID (Space) | UiE2ETest pauses with Space; other shortcuts not driven |
| Split / trim / move / ripple delete | WORKING_ANDROID | WorkflowRegressionTest (trim, split), ScreensE2ETest (split) |
| Duplicate / delete / reorder layers | WORKING_ENGINE_ONLY | combo tests |
| Precompose | WORKING_ANDROID | CombinationsDeviceTest (24/24 random combinations pass on the device) |
| Parenting | WORKING_ENGINE_ONLY | host render + combo tests |
| Markers | WORKING_ENGINE_ONLY | script API v2 test |

## Preview & playback
| Feature | Status | Evidence |
|---|---|---|
| Play / pause / frame step | WORKING_ANDROID | UiE2ETest |
| Audio-synced playback | PARTIAL | Runs on device; A/V sync not measured automatically |
| Adaptive preview resolution | PARTIAL | Implemented; performance not measured on real phones |
| Preview handles (move/scale/rotate), two-finger rotate | WORKING_ANDROID (move, scale) / UI_ONLY (rotate handle, two-finger rotate) | GestureE2ETest.preview_moveScaleMaskDrawAndPen: real touch drags, one undo step each |
| Mask draw, pen tool, freehand draw | WORKING_ANDROID | GestureE2ETest.preview_moveScaleMaskDrawAndPen (mask rectangle, freehand stroke → shape, three pen taps → shape) |
| Safe areas, grid, rulers, checkerboard | UI_ONLY | Overlay toggles; not pixel-tested |

## Timeline gestures
| Feature | Status | Evidence |
|---|---|---|
| Trim / move clips with snapping + haptics | WORKING_ANDROID (drag trim and move, one undo step each) / WORKING_ENGINE_ONLY (snapping) | GestureE2ETest.timeline_trimMoveTwoFingerCancelAndPinch (snapping off in the test) |
| Two-finger pan, pinch zoom on tracks | WORKING_ANDROID (pinch on the zoom strip) / PARTIAL (two-finger pan not asserted) | GestureE2ETest |
| Second finger cancels a drag (no commit) | WORKING_ANDROID | GestureE2ETest: clip unchanged, no undo step added |
| Edge auto-scroll while dragging | UI_ONLY | Added in this phase |
| Multi-select, moving several clips | UI_ONLY | Added in this phase (Multi-select chip) |
| Swipe actions | NOT_IMPLEMENTED | — |

## Layers, animation, effects
| Feature | Status | Evidence |
|---|---|---|
| Text layer, typography, shadow/box | WORKING_ANDROID | ScreensE2ETest.textPanel_presetShadowAndPersist |
| Text animation presets / custom animators | WORKING_ANDROID (presets) / WORKING_ENGINE_ONLY (custom) | ScreensE2ETest; script API v2 test |
| Shapes, merge ops, path operators | WORKING_ANDROID | ScreensE2ETest.shapePanel_itemsAndPathOperators |
| Keyframes, easing, bezier | WORKING_ANDROID | ScreensE2ETest.keyframesPanel_graphEditorEasingExpression, WorkflowRegressionTest |
| Graph editor (view, drag keyframes), bezier handles | WORKING_ANDROID | GestureE2ETest.graphEditor_dragKeyframeAndBezierHandles: key dragged 2 s → 3 s, custom bezier set, both persist |
| Expressions | WORKING_ANDROID | ScreensE2ETest (apply + persist), host expression tests |
| 67 effects | WORKING_ANDROID (browser, glow, invert) / WORKING_ENGINE_ONLY (all 67 × 6 layer kinds) | ScreensE2ETest, WorkflowRegressionTest (invert pixels), test_combo |
| Masks | WORKING_ANDROID | ScreensE2ETest, WorkflowRegressionTest (mask pixels in the export) |
| Track mattes, blend modes, adjustment layers | WORKING_ENGINE_ONLY | host render + combo tests |
| Transitions (13) | WORKING_ANDROID (wipe) / WORKING_ENGINE_ONLY (all) | ScreensE2ETest, host render tests |
| Behaviors (17), bake | WORKING_ANDROID (shake, bake) / WORKING_ENGINE_ONLY (all) | ScreensE2ETest |
| Motion blur | WORKING_ENGINE_ONLY | combo tests, CombinationsDeviceTest |
| Speed, reverse, freeze, time remap | WORKING_ENGINE_ONLY | combo tests |
| 3D layers, camera moves, lights | WORKING_ANDROID | ScreensE2ETest.addPanel_3dCameraLightParticles |
| 3D models (primitives) | WORKING_ANDROID | same |
| 3D models (OBJ/GLB import) | WORKING_ENGINE_ONLY | host 3D tests |
| Particles | WORKING_ANDROID | same |

## Audio
| Feature | Status | Evidence |
|---|---|---|
| Audio import & decode (MediaCodec) | WORKING_ANDROID | EngineE2ETest.whisperCaptions, WorkflowRegressionTest |
| Volume / pan / EQ / compressor / pitch / buses / limiter | WORKING_ENGINE_ONLY | host audio tests |
| Auto-ducking | WORKING_ENGINE_ONLY | host audio tests |

## Captions (offline Whisper)
| Feature | Status | Evidence |
|---|---|---|
| Whisper transcription on device | WORKING_ANDROID | EngineE2ETest, WorkflowRegressionTest |
| Caption styles / uppercase / boxed | WORKING_ANDROID | ScreensE2ETest.captionStudio_stylesReplaceEditExport |
| Find & replace | WORKING_ANDROID | same |
| Split / merge / edit timing | WORKING_ENGINE_ONLY | host caption tests |
| Edit by transcript | WORKING_ENGINE_ONLY | host caption tests |
| SRT/VTT/ASS export | WORKING_ANDROID (SRT) | EngineE2ETest, ScreensE2ETest |
| Speaker diarization | NOT_IMPLEMENTED | — |

## Export
| Feature | Status | Evidence |
|---|---|---|
| MP4 via MediaCodec + validation | WORKING_ANDROID | EngineE2ETest, WorkflowRegressionTest (pixels, tracks, duration), ScreensE2ETest (queue UI) |
| GIF | WORKING_ANDROID | EngineE2ETest, ScreensE2ETest |
| PNG sequence / WAV / M4A / WebM | WORKING_ENGINE_ONLY (WAV, PNG) / PARTIAL (M4A, WebM depend on device encoders) | host scenario |
| Export queue, foreground service | WORKING_ANDROID | ScreensE2ETest.exportScreen_gifAndMp4ThroughQueue |

## Media management
| Feature | Status | Evidence |
|---|---|---|
| Import files (gallery/Files, multiple) | WORKING_ANDROID (import path) | WorkflowRegressionTest, ScreensE2ETest.mediaManager. The system picker itself isn't automated. |
| Imported video: MediaCodec decode, composite, re-export | WORKING_ANDROID | VideoLayerDeviceTest (decoder pixels, render pixels, exported pixels) |
| Import folder | UI_ONLY | Needs the system folder picker |
| Media manager: usage, unused, metadata | WORKING_ANDROID | ScreensE2ETest.mediaManager |
| Replace / relink media | WORKING_ANDROID | same (**bug fixed this phase:** Relink in Project Inspector always failed) |
| Collect media into project | WORKING_ANDROID | same |
| Remove unused media | WORKING_ANDROID | same |
| Clear media caches | UI_ONLY | — |
| Thumbnails | PARTIAL | Video/image thumbnails in the media manager |
| Proxies (create, use in preview, remove, preview toggle) | WORKING_ANDROID | VideoLayerDeviceTest.proxyUsedInPreviewOriginalInExport: proxy transcoded on device; preview decodes it; export and the "off" setting decode the original; a deleted proxy falls back silently; persists across reopen. Host: video_proxy_used_in_preview_never_in_export |
| Proxies | NOT_IMPLEMENTED | Needs a transcoder; the adaptive preview resolution covers some of this |

## Library, capsules, scripting, extensions
| Feature | Status | Evidence |
|---|---|---|
| Presets: save / favorite / duplicate / delete | WORKING_ANDROID | ScreensE2ETest.library_presetFavoriteDuplicateDelete |
| Capsules: create / insert | WORKING_ANDROID | CapsuleScriptDeviceTest |
| Capsule v2 typed controls (text, color, size, position, intensity, speed) | WORKING_ANDROID | CapsuleScriptDeviceTest (text/color/size/position/speed incl. speed-time equivalence); test_capsule (intensity). **Bug fixed:** effect-bound controls broke on insert |
| .mfcapsule with embedded fonts + media | WORKING_ANDROID | CapsuleScriptDeviceTest (export → delete → import → insert); test_capsule (media embedding) |
| Script Studio: run example, single undo | WORKING_ANDROID | ScreensE2ETest.scriptStudio_runExampleIsOneUndoStep |
| Script API v2 (100+ functions), UI panels | WORKING_ANDROID | CapsuleScriptDeviceTest.scriptApiV2…, test_script_api |
| Extensions: install / effect in browser / disable / uninstall | WORKING_ANDROID | ScreensE2ETest.extensions_installEnableUseUninstall (**bugs fixed:** extension effects never appeared; Uninstall button was off-screen) |

## App screens
| Feature | Status | Evidence |
|---|---|---|
| Fonts screen, favorites | WORKING_ANDROID | ScreensE2ETest.homeScreens |
| AI Models screen | WORKING_ANDROID | same |
| Settings | WORKING_ANDROID | same |
| Developer Center + scenario runner | WORKING_ANDROID | same (**bug fixed:** Scenarios tab was clipped off-screen on phones) |
| Project Inspector, Performance | WORKING_ANDROID | ScreensE2ETest.projectInspectorAndPerformance (**bug fixed:** expression errors from other projects leaked into diagnostics) |

## Architecture items not yet done
| Item | Status | Notes |
|---|---|---|
| GPU-first renderer (GLES/Vulkan compositor, hardware buffers) | NOT_IMPLEMENTED | Rendering is a multithreaded CPU compositor. Export already uses EGL surfaces and hardware encoders. |
| Real-phone device matrix | NOT_IMPLEMENTED here | Configured in CI (emulators with KVM plus an optional Firebase Test Lab job). See CLOUD_ANDROID_TESTING.md. |
| Edit performance on very large projects | PARTIAL | Each edit copies the document. 500 layers / 2000 keyframes took ~19 s to build op by op on the host. On the device (software emulator), adding 500 layers as one batch took 7.3 s, rendering 260 visible layers at 640×360 took 8.1 s, and saving took 2.2 s. |
