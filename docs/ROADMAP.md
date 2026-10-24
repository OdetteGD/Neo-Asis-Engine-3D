# Roadmap

Milestones are ordered by dependency, not by excitement. Each one is defined by
what becomes *possible* when it lands.

## M0 — Foundation (current)

Renderer infrastructure and the Android build.

- [x] Vulkan context, device selection, swapchain, allocator
- [x] Deferred G-buffer + PBR lighting passes
- [x] Atmosphere and volumetric cloud shaders
- [x] Android build, lifecycle, APK CI
- [x] Desktop player with a fly camera
- [ ] Publish the first tagged release

## M1 — Geometry actually renders

The renderer is structurally complete but has no mesh upload path.

- [ ] Mesh registry: vertex/index SSBO allocation, `cgltf` import
- [ ] Material registry: bindless texture array, sampler defaults
- [ ] `gbuffer.vert` wired to real per-mesh vertex ranges
- [ ] Frustum culling against real mesh bounds
- [ ] glTF sample scene (Sponza or a lighter equivalent) in CI as a smoke test
- [ ] Screenshot comparison in CI to catch visual regressions

## M2 — Lighting and shadows that hold up

- [ ] Clustered light assignment compute pass (grid already specified in `FrameData.h`)
- [ ] Shadow cascade fitting with texel snapping
- [ ] IBL baker running at load: equirect → cube → irradiance → prefilter chain
- [ ] Point/spot shadow atlas
- [ ] TAA with disocclusion rejection
- [ ] GPU profiler timestamps per pass, surfaced in the FPS overlay

## M3 — Content pipeline

- [ ] KTX2 / Basis texture import, ASTC transcode at cook time
- [ ] `.nmat` material authoring format
- [ ] Scene serialization (`.nscene`)
- [ ] Hot reload on desktop (shaders, materials, meshes)
- [ ] Asset cooker CLI: source tree → cooked APK assets

## M4 — Android performance hardening

- [ ] Frame-time histogram on real devices (Mali-G57, Adreno 644, Mali-G78)
- [ ] Thermal-aware quality stepping (drop tier, not just resolution)
- [ ] Predicated rendering / occlusion culling
- [ ] ASTC for all G-buffer and HDR targets where supported
- [ ] Subpass-style tile-local optimisations where the driver allows
- [ ] Battery drain benchmark (30 min idle scene at 30 fps)

## M5 — Editor and scripting

The repository description commits to a .NET 10 scripting layer for Android.

- [ ] Editor UI (immediate-mode, drawn by the engine itself)
- [ ] Scene hierarchy, inspector, asset browser
- [ ] C# scripting host: NativeAOT for Android, hot reload on desktop
- [ ] Managed ↔ native ABI (`scripting/` scaffolding exists)
- [ ] Play-in-editor
- [ ] Build pipeline that produces an APK from the editor on-device

## M6 — Polish

- [ ] Vulkan validation clean across the full sample scene
- [ ] CI matrix on macOS/Windows desktop builds
- [ ] Documentation pass over every public header
- [ ] Example projects: architectural walkthrough, stylised environment

## Non-goals

Being explicit about these keeps scope sane:

- **No forward renderer.** One good deferred path beats two mediocre ones.
- **No ray tracing.** Vulkan ray tracing is not available on mobile hardware.
- **No custom shading language.** GLSL compiles to SPIR-V; a proprietary SLOC
  would be a maintenance tax with no mobile benefit.
- **No web export.** The renderer's assumptions (tile-based GPUs, swapchain
  transforms, memory budgets) are mobile-native, not WebGPU-shaped.

## How to contribute to the roadmap

Pick an unchecked item in the earliest milestone that is not blocked. Open an
issue describing what you intend to do before writing a large patch — several
M2 items depend on decisions in M1 that are still being settled.
