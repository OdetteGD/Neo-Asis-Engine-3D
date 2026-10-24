# Building for Android

Everything you need to get an APK out of this repository, and what to check when
it does not work.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| JDK | 17 | Gradle 8.9 requires 17+ |
| Android SDK | API 35 platform, build-tools 34 | `sdkmanager` installs these |
| Android NDK | 26.3.11579264 | Pinned in `android/app/build.gradle.kts` |
| CMake | 3.24+ (3.22.1 from SDK also works) | Ships with the NDK bundle |
| glslang | any recent | Compiles GLSL to SPIR-V at build time |

```bash
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/26.3.11579264
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH
```

## Build

```bash
cd android
gradle :app:assembleDebug
```

Output: `android/app/build/outputs/apk/debug/app-debug.apk`

Install and watch the log:

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -s NeoAsis:V AndroidRuntime:E
```

The engine logs under the `NeoAsis` tag. On a crash, `AndroidRuntime` carries
the tombstone — check there first before assuming the renderer is at fault.

## Release build

```bash
gradle :app:assembleRelease
```

Unsigned by default. To sign, add a keystore and configure
`signingConfigs` in `android/app/build.gradle.kts`. Never commit a keystore;
`.gitignore` blocks `*.jks` and `*.keystore` for that reason.

## How the native build is wired

```
Gradle (app/build.gradle.kts)
   └─ externalNativeBuild.cmake → android/app/CMakeLists.txt
        └─ add_subdirectory(<repo root>)
             ├─ FetchContent: Vulkan-Headers, volk, VMA, glm, cgltf
             ├─ neo_compile_shaders() → <build>/shaders/**/*.spv
             └─ cmake/Android.cmake → neo_player_android (libneoasis.so)
```

Gradle advertises `prefab = true`, which is what makes
`find_package(game-activity)` resolve against the Jetpack Games SDK.

The `neo_android_shaders` target copies compiled SPIR-V into
`android/app/src/main/assets/shaders/` so the runtime loads them through
`AAssetManager` (see `AssetSystem::read_binary`).

## Troubleshooting

**`find_package(game-activity)` fails**
Prefab is not enabled, or the Games SDK dependency is missing. Check that
`android/app/build.gradle.kts` has both `buildFeatures { prefab = true }` and
the `androidx.games:games-activity` dependency.

**Shaders not found at runtime**
The `.spv` files are not in the APK assets. Verify with:

```bash
unzip -l app-debug.apk | grep 'assets/shaders'
```

If the listing is empty, `glslangValidator` was not on `PATH` during configure,
so `neo_compile_shaders()` bailed with a warning. Install `glslang-tools` and
rebuild.

**`VK_ERROR_FEATURE_NOT_PRESENT` at device creation**
The device does not expose a feature the engine requests. `VulkanContext`
already gates dynamic rendering, synchronization2 and descriptor indexing on
`vkGetPhysicalDeviceFeatures2`, so a failure here usually means a very old
driver reporting 1.1 while claiming a 1.2 API version.

**Black screen but no crash**
Almost always lifecycle ordering in `AndroidMain.cpp`: the renderer was created
before `APP_CMD_INIT_WINDOW`, or the surface was destroyed without
`on_surface_lost()`. Check `adb logcat -s NeoAsis:V` for the "Window ready"
line ordering.

**Emulator**
`x86_64` is in the ABI list, but the emulator's Vulkan driver is a software
implementation (SwiftShader) on many images. Expect it to be slow and to lack
some features. Test on real hardware for anything performance-related.

## CI

`.github/workflows/android.yml` builds both variants on every push and uploads
the APKs. Tagging a `v*` commit publishes them to a GitHub Release. The
`shaders` job runs first and fails fast on a GLSL error, which is what breaks
most often.
