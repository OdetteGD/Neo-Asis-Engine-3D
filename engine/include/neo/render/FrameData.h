// ===========================================================================
//  FrameData — the CPU-side mirror of every GPU constant buffer.
//
//  LAYOUT CONTRACT: these structs are byte-identical to the std430 blocks in
//  engine/shaders/common/frame_data.glsl. `static_assert`s below guard the
//  match. When you change one side, change the other in the same commit.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"

#include <array>

namespace neo {

inline constexpr u32 kMaxShadowCascades = 4;
inline constexpr u32 kMaxLights = 1024;
inline constexpr u32 kMaxBones = 256;

// --- Global per-frame constants (set 0, binding 0) -------------------------
struct FrameConstants {
  mat4 view = mat4(1.0f);
  mat4 proj = mat4(1.0f);
  mat4 view_proj = mat4(1.0f);
  mat4 inv_view_proj = mat4(1.0f);
  mat4 prev_view_proj = mat4(1.0f);

  vec4 camera_position = vec4(0.0f); // w = exposure
  vec4 camera_forward = vec4(0.0f, 0.0f, -1.0f, 0.0f);
  vec4 screen_size = vec4(0.0f); // xy = pixel size, zw = 1/size

  // x = time, y = delta, z = frame index, w = near plane
  vec4 timing = vec4(0.0f);

  std::array<mat4, kMaxShadowCascades> shadow_matrices{};
  vec4 shadow_params = vec4(0.0f); // x = cascade count, y = map size,
                                   // z = depth bias, w = normal bias

  // Sun direction (xyz, pointing *from* sun to scene), intensity in w.
  vec4 sun_direction = vec4(0.0f, 1.0f, 0.0f, 0.0f);
  vec4 sun_color = vec4(1.0f); // rgb = colour, w = angular radius (rad)

  // Atmosphere parameters (Preetham/Bruneton hybrid, physically scaled).
  vec4 atmosphere_rayleigh = vec4(5.8e-6f, 13.5e-6f, 33.1e-6f, 1.0f); // w = height falloff
  vec4 atmosphere_mie = vec4(21e-6f, 21e-6f, 21e-6f, 0.76f);          // w = anisotropy g
  vec4 atmosphere_scale = vec4(6360e3f, 6460e3f, 0.0f, 0.0f);         // sea level, top
  vec4 atmosphere_ozone = vec4(0.65e-6f, 1.881e-6f, 0.085e-6f, 1.0f);

  // Volumetric cloud layer.
  vec4 cloud_layer = vec4(1500.0f, 4200.0f, 0.0f, 0.0f); // x = base, y = top
  vec4 cloud_params = vec4(0.42f, 0.9f, 1.0f, 0.0f);     // coverage, density, steps, frame
  vec4 cloud_wind = vec4(0.02f, 0.005f, 0.0f, 0.0f);

  vec4 fog_params = vec4(0.00002f, 0.25f, 0.0f, 0.0f); // density, height falloff

  // x = render scale, y = exposure key, z = bloom intensity, w = quality tier
  vec4 post_params = vec4(1.0f, 0.18f, 0.06f, 1.0f);

  // Clustered lighting: x = near, y = far, z = grid X, w = grid Y
  vec4 cluster_params = vec4(0.1f, 1000.0f, 16.0f, 9.0f);
  // x = grid Z, y = tile width px, z = tile height px, w = lights per cluster
  vec4 cluster_dims = vec4(24.0f, 1.0f, 1.0f, 32.0f);
};

static_assert((sizeof(FrameConstants) % 16) == 0,
              "FrameConstants must stay 16-byte aligned for std430");

// --- Scene / draw constants (set 0, binding 1) -----------------------------
struct DrawConstants {
  mat4 model = mat4(1.0f);
  mat4 normal_matrix = mat4(1.0f); // inverse-transpose of model (mat4 for alignment)
  vec4 base_color_factor = vec4(1.0f);
  // x = metallic, y = roughness, z = emissive strength, w = occlusion strength
  vec4 material_params = vec4(0.0f, 0.5f, 1.0f, 1.0f);
  vec4 uv_scale_offset = vec4(1.0f, 1.0f, 0.0f, 0.0f);
  // Bindless indices: 0 = none.
  uvec4 texture_indices = uvec4(0u);
  vec4 flags = vec4(0.0f); // x = alpha mode, y = double sided, z = flags, w = object id
};

// --- Light (std430, 64 bytes) ---------------------------------------------
struct GpuLight {
  vec4 position_range = vec4(0.0f);  // xyz = world pos, w = radius
  vec4 direction_angle = vec4(0.0f); // xyz = dir, w = spot cos(angle)
  vec4 color_intensity = vec4(0.0f); // rgb = linear colour, w = intensity
  // x = type (0 dir, 1 point, 2 spot), y = shadow index (-1 none), z/w = spare
  vec4 params = vec4(0.0f);
};
static_assert(sizeof(GpuLight) == 64);

// --- Cluster grid ----------------------------------------------------------
struct ClusterConfig {
  uvec4 grid = uvec4(16u, 9u, 24u, 0u); // x, y, z slices
  vec4 params = vec4(0.0f);             // x = z near, y = z far, zw = 1/log ratio
};

// --- Quality tier detection ------------------------------------------------
enum class GpuTier : u8 { Low = 0, Mid = 1, High = 2, Ultra = 3 };

} // namespace neo
