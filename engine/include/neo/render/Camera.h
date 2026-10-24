// ===========================================================================
//  Camera — view / projection state with mobile-tuned depth handling.
//
//  A single Camera instance is shared by the renderer and gameplay code. The
//  renderer only reads it; gameplay owns mutation.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"

namespace neo {

enum class ProjectionMode : u8 { Perspective = 0, Orthographic };

struct CameraDesc {
  ProjectionMode projection = ProjectionMode::Perspective;
  f32 vertical_fov_degrees = 60.0f;
  f32 near_plane = 0.1f;
  f32 far_plane = 1000.0f;
  f32 aspect = 16.0f / 9.0f;
  bool infinite_far = false;
};

class Camera {
public:
  Camera() = default;
  explicit Camera(const CameraDesc& desc) : desc_(desc) {}

  void set_perspective(f32 fov_degrees, f32 aspect, f32 near_plane, f32 far_plane);
  void set_aspect(f32 aspect);
  void set_infinite_far(bool v) { desc_.infinite_far = v; }

  // --- Transform ---------------------------------------------------------
  void set_position(const vec3& p) { position_ = p; }
  void set_rotation(const quat& q) { rotation_ = q; }
  void look_at(const vec3& eye, const vec3& target, const vec3& up = vec3(0, 1, 0));

  [[nodiscard]] const vec3& position() const { return position_; }
  [[nodiscard]] const quat& rotation() const { return rotation_; }
  [[nodiscard]] vec3 forward() const { return rotation_ * vec3(0, 0, -1); }
  [[nodiscard]] vec3 right() const { return rotation_ * vec3(1, 0, 0); }
  [[nodiscard]] vec3 up() const { return rotation_ * vec3(0, 1, 0); }

  // --- Matrices ----------------------------------------------------------
  [[nodiscard]] mat4 view() const;
  [[nodiscard]] mat4 projection() const;
  [[nodiscard]] mat4 view_projection() const { return projection() * view(); }
  [[nodiscard]] mat4 inverse_view_projection() const { return glm::inverse(view_projection()); }
  [[nodiscard]] Frustum frustum() const;

  [[nodiscard]] const CameraDesc& desc() const { return desc_; }
  [[nodiscard]] f32 aspect() const { return desc_.aspect; }

  // Reconstruct the world-space position of a pixel given a depth sample.
  // Exact for the [0,1] depth range the engine uses.
  [[nodiscard]] vec3 world_from_depth(const vec2& uv, f32 depth) const;

private:
  CameraDesc desc_{};
  vec3 position_{0.0f};
  quat rotation_{1.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace neo
