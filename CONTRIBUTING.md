# Contributing to Neo-Asis Engine 3D

Thanks for wanting to help. This project has opinions, and they are written
down so you do not have to guess.

## Before you start

- **Open an issue first for anything larger than ~200 lines.** Several parts of
  the renderer are still being designed (see `docs/ROADMAP.md`), and a large PR
  that conflicts with an in-flight decision wastes your time.
- **One concern per PR.** "Fix shadow bias and add a mesh loader" is two PRs.
- Check that your change builds in CI. If you cannot run Android locally, make
  sure at least the `Desktop Linux` and `Lint` jobs pass — they run on every PR.

## Code style

C++20. Formatting is enforced by `.clang-format` (LLVM base, 2-space indent,
100 columns):

```bash
clang-format -i $(find engine samples -name '*.cpp' -o -name '*.h')
```

Beyond formatting:

- **`neo::` namespace everywhere.** No `using namespace` in headers.
- **`auto` only when the type is obvious** from the right-hand side.
- **Prefer `[[nodiscard]]`** on anything whose return value matters.
- **No raw `new`/`delete`** outside of the few places that own Vulkan handles
  through a `std::unique_ptr` factory.
- **Comments explain *why*, not *what*.** The code already says what it does.
  A comment that says "loop over lights" is noise; one that says "bounded to 64
  so the shader unrolls on mobile" is valuable.

## GLSL conventions

- `#version 450` with `GL_GOOGLE_include_directive` for shared files.
- Shared code lives in `engine/shaders/common/` and is included, never copied.
- Any change to `common/frame_data.glsl` **requires** the matching change in
  `engine/include/neo/render/FrameData.h` in the same commit. CI checks for
  drift, but only for sentinel field names — it cannot catch a reorder.

## Commit messages

```
<subsystem>: short imperative summary

Why the change was needed, and anything a reviewer would otherwise ask.
```

Subsystems: `gfx`, `render`, `scene`, `core`, `android`, `ci`, `docs`.

Examples:

```
render: clamp roughness to normal variance for specular AA

Specular highlights shimmered on high-frequency normal maps at 1080p.
Clamping roughness to the pixel-footprint normal variance removes it for
~0.1 ms/frame. Matches the approach the IBL spec describes.
```

## Testing

There is no test suite yet (M1 adds screenshot comparison). Until then, your
verification bar is:

1. The sample scene renders correctly on your machine.
2. `Desktop Linux` CI passes.
3. If you touched a shader, `shaders` CI passes — and you actually looked at the
   result, not just the exit code.

## Reporting rendering bugs

A useful bug report includes:

- Device and GPU (`adb shell dumpsys SurfaceFlinger | grep GLES`, or the
  engine's startup log line with the device name)
- The quality preset in use
- A screenshot
- The exact log lines from `NeoAsis`

"You renderer is broken" cannot be acted on. "Clouds have hard banding on
Mali-G57 at Medium" can.

## License

By contributing you agree your work is licensed under the MIT license in
`LICENSE`.
