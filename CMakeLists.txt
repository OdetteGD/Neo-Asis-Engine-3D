# ============================================================================
#  Neo-Asis Engine 3D — top level build
#  Targets: NeoAsis::Engine (static/shared), neo_player (desktop), neo_android
# ============================================================================
cmake_minimum_required(VERSION 3.24)

project(NeoAsisEngine
        VERSION 0.1.0
        DESCRIPTION "Mobile-first 3D game engine with an advanced Vulkan renderer"
        LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NEO_ENABLE_VALIDATION   "Enable Vulkan validation layers in debug" ON)
option(NEO_BUILD_PLAYER        "Build the desktop player" ON)
option(NEO_BUILD_SHADERS       "Compile GLSL to SPIR-V at build time" ON)
option(NEO_ENABLE_SCRIPTING    "Build the C# (.NET 10) scripting bridge" OFF)
option(NEO_STRICT_WARNINGS     "Treat warnings as errors" OFF)

if(ANDROID)
  set(NEO_PLATFORM_ANDROID ON)
  set(NEO_BUILD_PLAYER OFF)
else()
  set(NEO_PLATFORM_ANDROID OFF)
endif()

# ----------------------------------------------------------------------------
# Third-party dependencies (pinned, fetched at configure time)
# ----------------------------------------------------------------------------
include(Dependencies)

# ----------------------------------------------------------------------------
# Engine
# ----------------------------------------------------------------------------
set(NEO_ENGINE_SOURCES
  engine/src/core/Log.cpp
  engine/src/core/JobSystem.cpp
  engine/src/core/AssetSystem.cpp
  engine/src/platform/Platform.cpp
  engine/src/gfx/VulkanContext.cpp
  engine/src/gfx/Swapchain.cpp
  engine/src/gfx/GpuAllocator.cpp
  engine/src/gfx/PipelineCache.cpp
  engine/src/render/Renderer.cpp
  engine/src/render/Camera.cpp
  engine/src/scene/Scene.cpp)

add_library(neo_engine STATIC ${NEO_ENGINE_SOURCES})

add_library(NeoAsis::Engine ALIAS neo_engine)

target_include_directories(neo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/include>
    $<INSTALL_INTERFACE:include>)

target_compile_definitions(neo_engine
  PUBLIC
    NEO_ENGINE_VERSION="${PROJECT_VERSION}"
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    VK_NO_PROTOTYPES
  PRIVATE
    $<$<CONFIG:Debug>:NEO_DEBUG=1>
    $<$<BOOL:${NEO_ENABLE_VALIDATION}>:NEO_VALIDATION=1>)

if(NEO_PLATFORM_ANDROID)
  target_compile_definitions(neo_engine PUBLIC NEO_PLATFORM_ANDROID=1 VK_USE_PLATFORM_ANDROID_KHR)
else()
  target_compile_definitions(neo_engine PUBLIC NEO_PLATFORM_DESKTOP=1)
endif()

target_link_libraries(neo_engine
  PUBLIC
    volk::volk
    glm::glm
  PRIVATE
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    cgltf::cgltf
    ${CMAKE_DL_LIBS})

# Desktop windowing: the platform layer needs a window + native handle.
if(NOT NEO_PLATFORM_ANDROID)
  target_link_libraries(neo_engine PUBLIC glfw)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(neo_engine PRIVATE pthread)
  if(NOT NEO_PLATFORM_ANDROID)
    target_link_libraries(neo_engine PUBLIC ${CMAKE_DL_LIBS})
    find_package(Threads REQUIRED)
    target_link_libraries(neo_engine PUBLIC Threads::Threads)
  endif()
endif()

if(NEO_STRICT_WARNINGS)
  if(MSVC)
    target_compile_options(neo_engine PRIVATE /W4 /WX)
  else()
    target_compile_options(neo_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

# ----------------------------------------------------------------------------
# Shaders
# ----------------------------------------------------------------------------
if(NEO_BUILD_SHADERS)
  include(CompileShaders)
  neo_compile_shaders(neo_engine
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
endif()

# ----------------------------------------------------------------------------
# Platform entry points
# ----------------------------------------------------------------------------
if(NEO_PLATFORM_ANDROID)
  include(Android)
elseif(NEO_BUILD_PLAYER)
  add_subdirectory(samples/hello-world)
endif()

if(NEO_ENABLE_SCRIPTING)
  add_subdirectory(scripting)
endif()

# ----------------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS neo_engine EXPORT NeoAsisEngineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY engine/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders/" DESTINATION ${CMAKE_INSTALL_DATADIR}/neoasis/shaders)
