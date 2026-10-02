# GPU-first renderer: staged plan

## Where things stand

```
Kotlin UI → JNI (JSON) → C++ engine → CPU compositor (multithreaded) → Bitmap → Compose
MediaCodec decode → YUV→RGBA (native) → CPU frame cache                 Export: RGBA → EGL surface → MediaCodec encoder
```

The CPU compositor is correct and deterministic. It's covered by bit-exact host tests: every effect × layer kind, and thousands of feature combinations. That makes it the **reference** a GPU path must match. It is not the fast path a phone needs for 4K or for heavy effect stacks.

## Target

```
Kotlin UI → JNI → C++ engine → render graph → GLES 3 / Vulkan backend → AHardwareBuffer → SurfaceView
MediaCodec → Surface (OES texture, zero copy) ───────────────┘                     └→ encoder input Surface (zero copy)
```

## Stages

Each stage ships behind a setting (`Renderer: Auto / GPU / CPU`) and falls back to the CPU path per layer when a feature isn't supported yet.

1. **Zero-copy display and decode.**
   * Preview is drawn into a `SurfaceView` through EGL instead of `Bitmap → Compose Image`.
   * Video decodes to a `SurfaceTexture`; YUV→RGB happens in a fragment shader.
   * The export encoder renders straight into the encoder's input surface (it already uses EGL).
   * Gate: identical frames to CPU within ±2/255 on the golden set; no copies per frame.
2. **GPU compositor for the common path.**
   * Layer quads with full transforms (2D/2.5D), opacity and all 18 blend modes as shaders.
   * Masks as stencil or coverage textures; track mattes; precomps rendered to FBOs (the cache moves to textures).
   * The CPU path remains for anything not yet ported.
   * Gate: `mftests` golden images re-rendered through the GPU backend on the emulator (SwiftShader) and on real devices (Test Lab), with per-pixel tolerance.
3. **Effects as shaders.** Port in order of usage and cost: blur family (separable Gaussian, then mip-chain for large radii), color corrections (LUT texture), glow/bloom, distortion (displacement), glitch, keying. Each effect gets a CPU/GPU parity test.
4. **Text, shapes and particles.** Glyph atlas plus SDF for large sizes, tessellated or stencil-and-cover paths, instanced particles.
5. **3D.** Port the software rasterizer (meshes, PBR-ish shading, shadow maps) to GLES. Use Vulkan only where its gains justify the second backend.

## Rules that keep this safe

* The render graph stays backend-neutral. The engine decides *what* to draw; backends decide *how*.
* Every backend feature needs a parity test against the CPU reference before it's enabled by default.
* The export path may stay on the CPU (deterministic) until GPU parity is proven on the device matrix.
* Performance targets are measured on real low-, mid- and high-end phones (see the device matrix), never on the software emulator.
