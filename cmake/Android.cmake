# ---------------------------------------------------------------------------
#  Android native build (GameActivity + Vulkan)
#  Consumed by android/app/CMakeLists.txt through add_subdirectory().
# ---------------------------------------------------------------------------
if(NOT NEO_PLATFORM_ANDROID)
  return()
endif()

message(STATUS "Neo-Asis: configuring for Android (ABI=${ANDROID_ABI}, API=${ANDROID_PLATFORM})")

# Vulkan on Android is loaded at runtime through libvulkan.so.
target_link_libraries(neo_engine PUBLIC vulkan android log)

# GameActivity from the Jetpack Games SDK (a prefab package pulled by Gradle).
find_package(game-activity REQUIRED CONFIG)
target_link_libraries(neo_engine PUBLIC game-activity::game-activity)
target_compile_definitions(neo_engine PUBLIC NEO_USE_GAME_ACTIVITY=1)

# Shaders live in the APK assets: the Gradle task copies
# ${CMAKE_BINARY_DIR}/shaders into app/src/main/assets/shaders before packaging.
get_property(SPV_OUTPUTS GLOBAL PROPERTY NEO_SPV_OUTPUTS)
if(SPV_OUTPUTS)
  target_compile_definitions(neo_engine PUBLIC NEO_SHADER_IN_ASSETS=1)
endif()

# Performance: NEON + full fp math for the math-kernel path.
target_compile_options(neo_engine PRIVATE -ffast-math -fno-finite-math-only)

# Shared library entry point for the Android player.
add_library(neo_player_android SHARED
  engine/src/platform/android/AndroidMain.cpp)
target_link_libraries(neo_player_android PRIVATE neo_engine)
set_target_properties(neo_player_android PROPERTIES
  OUTPUT_NAME "neoasis")  # -> libneoasis.so ; Android only loads lib*.so
