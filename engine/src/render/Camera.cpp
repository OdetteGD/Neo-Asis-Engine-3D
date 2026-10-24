#include "neo/render/Camera.h"
#include "neo/core/Math.h"

namespace neo {

Frustum frustum_from_view_proj(const mat4& vp) {
  Frustum f{};
  const mat4 m = glm::transpose(vp);  // rows of vp = columns of m

  auto normalize_plane = [](const vec4& p) {
    const f32 len = glm::length(vec3(p));
    return len > 1e-8f ? p / len : p;
  };

  // Gribb-Hartmann extraction; clip depth is [0,1] so the near plane is row2
  // rather than row3 + row2.
  f.planes[0] = normalize_plane(m[3] + m[0]);  // left
  f.planes[1] = normalize_plane(m[3] - m[0]);  // right
  f.planes[2] = normalize_plane(m[3] + m[1]);  // bottom
  f.planes[3] = normalize_plane(m[3] - m[1]);  // top
  f.planes[4] = normalize_plane(m[2]);         // near  (z_clip >= 0)
  f.planes[5] = normalize_plane(m[3] - m[2]);  // far
  return f;
}

void Camera::set_perspective(f32 fov_degrees, f32 aspect, f32 near_plane, f32 far_plane) {
  desc_.projection = ProjectionMode::Perspective;
  desc_.vertical_fov_degrees = fov_degrees;
  desc_.aspect = aspect;
  desc_.near_plane = near_plane;
  desc_.far_plane = far_plane;
}

void Camera::set_aspect(f32 aspect) { desc_.aspect = aspect > 0.0f ? aspect : 1.0f; }

void Camera::look_at(const vec3& eye, const vec3& target, const vec3& up) {
  position_ = eye;
  const vec3 f = glm::normalize(target - eye);
  const vec3 r = glm::normalize(glm::cross(f, up));
  const vec3 u = glm::cross(r, f);
  // Build a quaternion from the orthonormal basis (-Z forward).
  const mat3 basis(r, u, -f);
  rotation_ = glm::quat_cast(basis);
}

mat4 Camera::view() const {
  return glm::inverse(glm::translate(mat4(1.0f), position_) * glm::mat4_cast(rotation_));
}

mat4 Camera::projection() const {
  if (desc_.projection == ProjectionMode::Orthographic) {
    const f32 h = desc_.far_plane * 0.5f;
    const f32 w = h * desc_.aspect;
    return orthographic_zo(-w, w, -h, h, desc_.near_plane, desc_.far_plane);
  }
  const f32 fov = desc_.vertical_fov_degrees * kDegToRad;
  if (desc_.infinite_far) return perspective_infinite(fov, desc_.aspect, desc_.near_plane);
  return perspective_zo(fov, desc_.aspect, desc_.near_plane, desc_.far_plane);
}

Frustum Camera::frustum() const { return frustum_from_view_proj(view_projection()); }

vec3 Camera::world_from_depth(const vec2& uv, f32 depth) const {
  const vec4 ndc{uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth, 1.0f};
  vec4 world = inverse_view_projection() * ndc;
  return vec3(world) / world.w;
}

} // namespace neo
