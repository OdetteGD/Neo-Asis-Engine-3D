// ===========================================================================
//  Math — thin wrapper over glm with engine conventions baked in.
//
//  Conventions (documented once, enforced everywhere):
//   * Right-handed coordinate system.
//   * +Y up, +X right, -Z forward (camera looks down its local -Z).
//   * Clip space depth is [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE).
//   * Matrices are column-major, matching GLSL/Vulkan defaults.
//   * "World" units are metres. Light intensity is in lux-ish units.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/transform.hpp>

#include <cmath>

namespace neo {

using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using ivec2 = glm::ivec2;
using ivec3 = glm::ivec3;
using uvec4 = glm::uvec4;
using mat3 = glm::mat3;
using mat4 = glm::mat4;
using quat = glm::quat;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTau = 2.0f * kPi;
constexpr f32 kDegToRad = kPi / 180.0f;
constexpr f32 kRadToDeg = 180.0f / kPi;
constexpr f32 kEpsilon = 1e-6f;

// --- Transform -------------------------------------------------------------
struct Transform {
  vec3 position{0.0f};
  quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  vec3 scale{1.0f};

  [[nodiscard]] mat4 matrix() const {
    return glm::translate(mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(mat4(1.0f), scale);
  }
  [[nodiscard]] mat4 inverse_matrix() const {
    return glm::inverse(matrix());
  }
  [[nodiscard]] vec3 forward() const { return rotation * vec3(0.0f, 0.0f, -1.0f); }
  [[nodiscard]] vec3 right() const { return rotation * vec3(1.0f, 0.0f, 0.0f); }
  [[nodiscard]] vec3 up() const { return rotation * vec3(0.0f, 1.0f, 0.0f); }
};

// --- AABB ------------------------------------------------------------------
struct Aabb {
  vec3 min{std::numeric_limits<f32>::max()};
  vec3 max{std::numeric_limits<f32>::lowest()};

  void expand(const vec3& p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
  }
  [[nodiscard]] vec3 center() const { return (min + max) * 0.5f; }
  [[nodiscard]] vec3 extent() const { return (max - min) * 0.5f; }
  [[nodiscard]] f32 radius() const { return glm::length(extent()); }
  [[nodiscard]] bool valid() const { return min.x <= max.x; }
};

// --- Colour ----------------------------------------------------------------
// Engine colour is linear sRGB in float. Authored hex literals are converted.
[[nodiscard]] inline vec3 srgb_to_linear(const vec3& c) {
  return glm::mix(c / 12.92f, glm::pow((c + 0.055f) / 1.055f, vec3(2.4f)),
                  glm::step(vec3(0.04045f), c));
}
[[nodiscard]] inline vec3 linear_to_srgb(const vec3& c) {
  return glm::mix(c * 12.92f, 1.055f * glm::pow(c, vec3(1.0f / 2.4f)) - 0.055f,
                  glm::step(vec3(0.0031308f), c));
}
[[nodiscard]] inline vec3 hex_color(u32 rgb, f32 intensity = 1.0f) {
  const vec3 srgb{static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f,
                  static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<f32>(rgb & 0xFF) / 255.0f};
  return srgb_to_linear(srgb) * intensity;
}

// --- Projection helpers (depth range [0,1], reverse-Z friendly) ------------
[[nodiscard]] inline mat4 perspective_zo(f32 fov_y_radians, f32 aspect, f32 near_plane,
                                         f32 far_plane) {
  // GLM_FORCE_DEPTH_ZERO_TO_ONE is set project-wide; this is explicit anyway.
  return glm::perspective(fov_y_radians, aspect, near_plane, far_plane);
}

// Infinite far plane: improves depth precision on mobile tiles enormously.
[[nodiscard]] inline mat4 perspective_infinite(f32 fov_y_radians, f32 aspect, f32 near_plane) {
  const f32 f = 1.0f / std::tan(fov_y_radians * 0.5f);
  mat4 m(0.0f);
  m[0][0] = f / aspect;
  m[1][1] = f;
  m[2][3] = -1.0f;
  m[3][2] = near_plane;
  return m;
}

[[nodiscard]] inline mat4 orthographic_zo(f32 left, f32 right, f32 bottom, f32 top,
                                          f32 near_plane, f32 far_plane) {
  return glm::orthoZO(left, right, bottom, top, near_plane, far_plane);
}

[[nodiscard]] inline mat4 look_at_rh(const vec3& eye, const vec3& target, const vec3& up) {
  return glm::lookAtRH(eye, target, up);
}

// --- Culling ---------------------------------------------------------------
struct Frustum {
  vec4 planes[6];  // xyz = normal, w = distance; positive side is inside

  [[nodiscard]] bool intersects_sphere(const vec3& center, f32 radius) const {
    for (const vec4& p : planes) {
      if (glm::dot(vec3(p), center) + p.w < -radius) return false;
    }
    return true;
  }
};

[[nodiscard]] Frustum frustum_from_view_proj(const mat4& vp);

} // namespace neo
