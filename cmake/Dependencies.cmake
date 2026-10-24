# ---------------------------------------------------------------------------
#  Pinned third-party dependencies
#  Everything is fetched at configure time so the repo stays small.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# --- Vulkan headers (no loader; we use volk) -------------------------------
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.3.290
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(vulkan_headers)
# Vulkan-Headers already exports Vulkan::Headers; only create it when a distro
# package or an older tag did not.
if(NOT TARGET Vulkan::Headers)
  add_library(Vulkan::Headers INTERFACE IMPORTED)
  target_include_directories(Vulkan::Headers INTERFACE ${vulkan_headers_SOURCE_DIR}/include)
endif()

# --- volk: meta-loader for Vulkan ------------------------------------------
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG        vulkan-sdk-1.3.290.0
  GIT_SHALLOW    TRUE)
set(VOLK_STATIC_DEFINE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)
# volk includes <vulkan/vulkan.h> but does not pull in the headers target.
if(TARGET volk AND TARGET Vulkan::Headers)
  target_link_libraries(volk PUBLIC Vulkan::Headers)
endif()

# --- VulkanMemoryAllocator -------------------------------------------------
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.1.0
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(vma)
if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
  add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
  target_include_directories(GPUOpen::VulkanMemoryAllocator INTERFACE
    ${vma_SOURCE_DIR}/include)
endif()

# --- glm -------------------------------------------------------------------
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glm)

# --- cgltf: glTF 2.0 / GLB ingestion ---------------------------------------
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG        v1.14
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(cgltf)
if(NOT TARGET cgltf::cgltf)
  add_library(cgltf::cgltf INTERFACE IMPORTED)
  target_include_directories(cgltf::cgltf INTERFACE ${cgltf_SOURCE_DIR})
endif()

# --- fmt: std::format is unavailable on GCC < 13 (the CI image ships 11) ---
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.0.2
  GIT_SHALLOW    TRUE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)

# --- stb_image: PNG/JPG decode for glTF textures ---------------------------
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)

# --- GLFW: desktop windowing + native handles (desktop only) ---------------
if(NOT ANDROID)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
endif()
