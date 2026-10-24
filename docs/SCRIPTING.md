# C# scripting

Write gameplay in C#. It compiles ahead of time into a native shared library
that the engine loads with `dlopen` — no Mono, no JIT, no reflection at startup.

## Why NativeAOT instead of Mono

| | Mono / Xamarin | NativeAOT (what we use) |
|---|---|---|
| Startup | ~120 ms runtime init | single-digit ms, just `dlopen` |
| APK size | +9 MB | +2 MB |
| Reflection | available | trimmed away |
| AOT on device | partial | fully compiled |
| Hot reload | yes | desktop only |

The trade is that reflection-based discovery is gone, so scripts register
themselves explicitly in `GameScripts.cs`. That costs you four lines and buys a
library that starts faster than the Vulkan device creation it runs alongside.

## Quick start

```bash
# Build the engine first (it generates the shaders and the player)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Then the managed assembly
./scripting/build-scripts.sh desktop     # linux-x64
./scripting/build-scripts.sh android     # linux-bionic-arm64

./build/bin/neo_player
```

`libneoasis_scripts.so` is loaded from the working directory on desktop, and
from the APK's native library directory on Android.

## Writing a script

Edit `scripting/NeoAsis.Scripting/GameScripts.cs`:

```csharp
public static class GameScripts
{
    public static void Register()
    {
        ScriptRuntime.Add(new SpinScript { Entity = 2, Speed = 1.5f });
        ScriptRuntime.Add(new DayCycleSystem());
    }
}

public sealed class SpinScript : ScriptBehaviour
{
    public float Speed { get; init; } = 1f;

    public override void OnCreate() => Log($"spinning entity {Entity}");

    public override void OnUpdate(float dt) => RotateY(Speed * dt);
}
```

Rebuild and, on desktop, watch it reload live without restarting the engine.

## Lifecycle

```
neo_script_init(world, host)      <- assembly load; register everything here
    GameScripts.Register()
    every script's OnCreate()
neo_script_update(world, dt)      <- once per frame
    OnUpdate(dt) for every enabled script
    OnLateUpdate(dt)
    WorldSystem.OnUpdate(dt)
neo_script_shutdown(world)        <- reverse order OnDestroy()
```

`OnLateUpdate` runs after `OnUpdate`, so a camera that follows an entity can
read the final position of the frame without an ordering problem.

## The host API

Everything a script can do goes through `Engine` (a thin wrapper over the
function table in `ScriptApi.h`):

| Category | API |
|---|---|
| Logging | `Engine.Log(level, msg)`, `Engine.Print(msg)` |
| Time | `Engine.Time`, `Engine.DeltaTime`, `Engine.Frame` |
| Entities | `Engine.IsAlive`, `Engine.Find` |
| Transform | `Engine.GetPose`, `SetPose`, `RotateY` |
| Scale | `Engine.GetScale`, `SetScale` |
| Scene | `Engine.SetSunElevation`, `SetCloudCoverage` |

Scripts never receive a pointer they can dereference. `NeoWorld` is an opaque
handle; every access is a checked host call, so a bug in gameplay code cannot
corrupt engine memory — the worst case is an exception the engine logs.

## Adding your own API

Three places, in this order:

1. `engine/include/neo/script/ScriptApi.h` — add the function pointer to
   `NeoHostApi`. **Append only**; never reorder. Bump
   `NEO_SCRIPT_ABI_VERSION`.
2. `engine/src/script/ScriptHost.cpp` — implement the trampoline in the
   anonymous namespace and assign it in `make_api()`.
3. `scripting/NeoAsis.Scripting/Engine.cs` — mirror the field in
   `NeoHostApi` (same position!) and add a friendly wrapper.

If you skip step 3's position, calls after that field silently receive the
wrong pointer. The ABI-version check in `neo_script_init` catches a full
mismatch but not a reorder, which is why the rule is absolute.

## Performance notes

- One managed call per frame per script. At 200 scripts that is ~0.2 ms of
  interop on a mid-range phone; the tick loop itself is the cost, not the
  boundary.
- `Engine.Log` encodes UTF-8 into a stack buffer for messages under 512 bytes —
  no allocation. Logging in a hot loop is still slow because of the formatting,
  so gate per-frame logs behind a flag.
- Entity lookups are O(1) by index. `Engine.Find(name)` is not implemented yet
  and returns 0; use ids until the entity registry lands (M5).

## Android specifics

The managed assembly is packaged as a second native library, so Android's
loader resolves it from the same directory as `libneoasis.so`:

```
lib/arm64-v8a/libneoasis.so
lib/arm64-v8a/libneoasis_scripts.so
```

Gradle picks the second one up automatically if you build it into
`android/app/src/main/jniLibs/<abi>/`. `scripting/build-scripts.sh android`
prints the expected output path; copy it there (or point the script at the
Gradle jniLibs directory) before `assembleRelease`.

Hot reload is disabled on device: the APK is read-only, and re-`dlopen`ing a
library that the linker already mapped is not reliable across Android versions.
