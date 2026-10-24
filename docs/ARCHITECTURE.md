# Architecture

This document explains how the pieces of Neo-Asis fit together and, more
importantly, *why* each boundary is where it is.

## Layering

```
        ┌─────────────────────────────────────────────┐
        │  Gameplay / samples / editor (not yet)      │
        ├─────────────────────────────────────────────┤
        │  scene::Scene      entity + renderable + light storage
        ├─────────────────────────────────────────────┤
        │  render::Renderer  frame orchestration, passes, quality
        ├─────────────────────────────────────────────┤
        │  gfx::*            Vulkan: context, swapchain, allocator,
        │                    pipelines, command list
        ├─────────────────────────────────────────────┤
        │  core::*           log, jobs, assets, math
        ├─────────────────────────────────────────────┤
        │  platform::*       window, input, lifecycle
        └─────────────────────────────────────────────┘
```

Rules that keep this from turning into a ball of mud:

1. **`core` depends on nothing.** No Vulkan, no platform headers.
2. **`gfx` owns every Vulkan handle.** Nothing above it includes `<volk.h>`
   except through `gfx` headers.
3. **`render` never creates a `VkDevice`.** It receives a `VulkanContext`.
4. **`scene` never touches Vulkan.** The renderer reads the scene; the scene
   does not know the renderer exists. This is what makes headless testing and
   offline baking possible later.

## Frame model

Two frames in flight. This is deliberate: three frames adds latency that is
noticeable on a 60 Hz phone panel and costs extra GPU memory for the G-buffer
chain, while two is enough to hide CPU submission behind GPU work.

```
CPU:  cull ─► pack lights ─► record ─► submit ─────────────► present
GPU:                              └─► shadow ─► gbuffer ─► ...
```

`Renderer::begin_frame()` waits on the in-flight fence, acquires the swapchain
image, and resets the command pool. `render()` records every pass into one
primary command buffer. `end_frame()` presents and runs the dynamic-resolution
controller.

Passes are recorded in one buffer rather than one-per-pass because on mobile
every `vkQueueSubmit` has measurable overhead; a single submit per frame keeps
the driver's job small.

## The constant-buffer contract

`engine/include/neo/render/FrameData.h` and
`engine/shaders/common/frame_data.glsl` describe the same memory. This is the
one place in the codebase where a silent mismatch produces visually plausible
but wrong output, so it is guarded three ways:

- `static_assert` on alignment in C++
- an explicit field-order comment in both files
- a CI check (`lint.yml`) that greps the GLSL for sentinel field names

**If you change one side, change the other in the same commit.**

## Android lifecycle

This is where most mobile engines accumulate bugs. The state machine:

| Android event | Engine action |
|---|---|
| `APP_CMD_INIT_WINDOW` | Create `ANativeWindow` surface, start render thread |
| `APP_CMD_TERM_WINDOW` | Stop using the surface; destroy surface-sized targets |
| `APP_CMD_GAINED_FOCUS` | Resume rendering |
| `APP_CMD_LOST_FOCUS` | Pause rendering, **keep** `VkDevice` |
| `APP_CMD_LOW_MEMORY` | Force render-scale down, drop caches |
| `destroyRequested` | Join render thread, destroy everything in order |

The render loop lives on its own thread. `android_main` only pumps the looper.
The looper blocks (timeout `-1`) while the surface is gone, which is what stops
the classic "engine burns 15% battery in the background" bug.

## Quality tiers

`Renderer::detect_tier()` scores the device from
`maxComputeWorkGroupInvocations` (a decent proxy for shader core count) and API
level, then maps to a preset. Presets are conservative on purpose — it is much
easier to raise quality after profiling than to ship a preset that stutters on
a Mali-G57.

Dynamic resolution is a proportional controller with hysteresis:

```
if (gpu_ms > target * 1.15) scale -= 0.05
if (gpu_ms < 0.75 * target)  scale += 0.02
```

The asymmetric step sizes prevent oscillation. The controller also reacts to the
VMA memory-budget watermark before the OS killer does.

## Known limitations

- No mesh upload path yet; `gbuffer.vert` reads a vertex SSBO that the renderer
  has not started filling.
- Cluster assignment is specified but not implemented — the light loop is flat
  and bounded at 64.
- `Swapchain::rebuild()` is a stub; resizing goes through
  `Renderer::resize()` which recreates the swapchain from scratch. That is
  correct but does not preserve the old one for a seamless transition.
- Only `arm64-v8a` and `x86_64` are configured for Android.
