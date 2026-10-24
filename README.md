# Neo-Asis Engine 3D

**A mobile-first 3D game engine with a modern Vulkan renderer.**
C++20 core · advanced deferred HDR pipeline · physically based sky, clouds and
lighting · Android APK build out of the box.

[![Android APK](https://github.com/OdetteGD/Neo-Asis-Engine-3D/actions/workflows/android.yml/badge.svg)](https://github.com/OdetteGD/Neo-Asis-Engine-3D/actions/workflows/android.yml)
[![Desktop Linux](https://github.com/OdetteGD/Neo-Asis-Engine-3D/actions/workflows/desktop.yml/badge.svg)](https://github.com/OdetteGD/Neo-Asis-Engine-3D/actions/workflows/desktop.yml)

---

## What this is

Neo-Asis targets the graphics quality you expect from a desktop engine, inside
the power budget of a phone. That tension drives every design decision in the
renderer:

| Concern | Desktop habit | What Neo-Asis does on mobile |
|---|---|---|
| Bandwidth | RGBA16F everywhere | Packed G-buffer: 8-bit albedo, 16-bit oct normals, 11/10-bit emissive |
| Lighting | Hundreds of lights | Clustered forward+ with a per-cluster light list |
| Shadows | 4× 4K cascades | 2–4 cascades in a single tiled atlas, tier-dependent size |
| Sky | Full LUT chain every frame | Analytic single-scattering + surface pre-rotation aware |
| Clouds | Full-res raymarch | 0.25–0.5× res compute raymarch + temporal jitter |
| Frames | Triple-buffered, always 16 ms | Proportional dynamic resolution targeting a frame-time budget |
| MSAA | 4× | Off by default; FXAA or TAA instead |

## Feature set

**Rendering**
- Vulkan 1.1–1.3, `VK_EXT_dynamic_rendering` (no render-pass objects), `synchronization2`, timeline semaphores
- Deferred shading with a bandwidth-optimised G-buffer, reverse-Z depth
- Cook-Torrance PBR: GGX distribution, Smith height-correlated visibility, Schlick Fresnel, multiple-scattering energy compensation
- Image-based lighting: irradiance + prefiltered specular + split-sum BRDF (baked LUT, with an analytic fit on the cheap path)
- Physically based atmosphere: Rayleigh + Mie + ozone single scattering, analytic sun disk with limb darkening, aerial perspective
- Volumetric clouds: Perlin–Worley shape noise, Worley detail, 6-tap sun light march, powder term, jittered raymarch, temporal reprojection
- Cascaded shadow maps with slope-scaled bias and normal offset
- HDR post chain: dual-filter bloom, ACES tonemap, FXAA, ordered dither, vignette
- Bindless texture arrays and `bufferDeviceAddress` when the driver exposes them
- Surface pre-rotation (portrait/landscape without a rotated blit)

**Engine**
- Work-stealing job system sized for big.LITTLE
- Virtual asset layer: APK assets on Android, filesystem on desktop
- Pipeline cache persisted to disk (saves 100–400 ms of driver IR compile per launch)
- VMA-backed allocator with a mobile-tuned block size and a budget watermark
- Full Android lifecycle handling: surface loss, focus, low-memory trimming
- Quality presets (Low/Medium/High/Ultra) auto-selected from a GPU tier heuristic

## Quick start

### Desktop (Linux)

```bash
sudo apt install build-essential cmake ninja-build libvulkan-dev \
     vulkan-tools libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
     libxi-dev glslang-tools

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/neo_player
```

Hold **right mouse** to look, **WASD** to move, **Q/E** for down/up, **Shift** to
sprint. The console prints fps, draw calls, visible instances and GPU memory
once per second.

### Android

```bash
# Requires Android SDK + NDK 26.3.11579264, JDK 17
cd android
gradle :app:assembleDebug          # or :app:assembleRelease
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or download the APK from the **Actions** tab — every push to `main` produces
one. Tagging a commit `v0.1.0` publishes APKs to a GitHub Release automatically.

## Repository layout

```
engine/
  include/neo/          Public engine headers (core, gfx, render, scene, platform)
  src/                  Implementation
  shaders/
    common/             Shared GLSL: frame constants, PBR, atmosphere
    passes/             G-buffer, shadow, sky, lighting, volumetric clouds
    fb/                 Bloom downsample/upsample, tonemap
    ibl/                BRDF LUT integration
android/                Gradle project + GameActivity shell
samples/hello-world/    Desktop player
cmake/                  Dependency fetch, shader compilation, Android glue
docs/                   Architecture and rendering deep dives
```

## Render pipeline

```
ShadowCascades ──► GBuffer ──► VolumetricClouds (compute)
                       │                 │
                       ▼                 ▼
                  SkyAtmosphere ──► DeferredLighting (PBR + IBL + clouds)
                                         │
                                         ▼
                              Bloom ──► Tonemap ──► Pre-rotation ──► Present
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/RENDERING.md](docs/RENDERING.md) for the details, including the GPU
constant-buffer layout contract.

## Requirements

| Platform | Minimum |
|---|---|
| Android | API 26, Vulkan 1.1 driver, arm64-v8a |
| Desktop | Linux with a Vulkan 1.1 driver, X11 or Wayland |
| Toolchain | CMake 3.24+, C++20 compiler, glslang, JDK 17 + NDK 26 for Android |

## Scripting

Gameplay is written in C# and compiled ahead of time with NativeAOT into a
shared library the engine `dlopen`s:

```bash
./scripting/build-scripts.sh desktop    # or: android
```

```csharp
public sealed class SpinScript : ScriptBehaviour
{
    public float Speed { get; init; } = 1f;
    public override void OnUpdate(float dt) => RotateY(Speed * dt);
}
```

No Mono, no JIT, ~2 MB, single-digit-millisecond startup. See
[docs/SCRIPTING.md](docs/SCRIPTING.md).

## Engine status

Honest state of the tree — an actively built project, not a finished product:

- ✅ Vulkan device/swapchain/allocator/pipeline infrastructure
- ✅ Deferred G-buffer, PBR lighting, CSM plumbing, sky + clouds shaders
- ✅ Mesh and material registries: glTF/GLB import, bindless textures, real
      indexed draws from shared vertex/index arenas
- ✅ IBL baker: sky capture → irradiance → GGX prefilter → BRDF LUT, re-baked
      when the sun drifts
- ✅ Android build, lifecycle, and APK CI
- ✅ C# scripting: versioned host ABI, NativeAOT assembly, desktop hot reload
- 🚧 Clustered light assignment (the shader loop is still a flat bounded list;
      the cluster grid is specified in `FrameData.h` but not populated)
- 🚧 Editor UI and play-in-editor
- 📋 See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan

## License

MIT — see [LICENSE](LICENSE).
