# ---------------------------------------------------------------------------
#  Android native build entry point.
#  Gradle invokes this with the NDK toolchain already configured.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.24)

project(neoasis_android LANGUAGES C CXX)

# The repository root holds the engine; this file lives in android/app/.
get_filename_component(NEO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${NEO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/neoasis")

# Shaders are compiled to SPIR-V by the engine target and copied into the APK
# assets so the runtime can load them through AAssetManager.
get_property(NEO_SPV_OUTPUTS GLOBAL PROPERTY NEO_SPV_OUTPUTS)
if(NEO_SPV_OUTPUTS)
  set(ASSET_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/shaders")
  add_custom_target(neo_android_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSET_SHADER_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/shaders" "${ASSET_SHADER_DIR}"
    COMMENT "Staging compiled shaders into APK assets")
  add_dependencies(neo_android_shaders neo_engine)
endif()
