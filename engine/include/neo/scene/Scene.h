// ===========================================================================
//  Scene — flat ECS-lite storage: transforms, meshes, materials, lights.
//
//  Deliberately not a full archetype ECS. A mobile renderer wants contiguous
//  SoA arrays it can memcpy into instance buffers; that is what this gives us.
//  Gameplay systems live above this in C++ or (optionally) C# scripting.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"
#include "neo/render/FrameData.h"

#include <string>
#include <string_view>
#include <vector>

namespace neo {

enum class LightType : u8 { Directional = 0, Point = 1, Spot = 2 };

struct LightDesc {
  LightType type = LightType::Point;
  vec3 position{0.0f};
  vec3 direction{0.0f, -1.0f, 0.0f};
  vec3 color{1.0f};
  f32 intensity = 1.0f;
  f32 range = 10.0f;
  f32 inner_cone_degrees = 25.0f;
  f32 outer_cone_degrees = 40.0f;
  bool casts_shadow = false;
};

struct RenderableDesc {
  MeshHandle mesh;
  MaterialHandle material;
  Transform transform;
  bool casts_shadow = true;
  bool receives_shadow = true;
  bool visible = true;
  u32 layer = 0;
};

// The sun/atmosphere description is what drives the sky, IBL and CSM together,
// so they can never disagree.
struct SkyDesc {
  f32 elevation_degrees = 18.0f;
  f32 azimuth_degrees = 210.0f;
  f32 sun_intensity = 110000.0f;  // lux-ish; tonemapping handles the range
  vec3 sun_color = vec3(1.0f, 0.96f, 0.9f);
  f32 angular_radius_degrees = 0.27f;
  f32 turbidity = 2.5f;
  bool atmospheric_scattering = true;
  bool volumetric_clouds = true;
  f32 cloud_coverage = 0.42f;
  f32 cloud_density = 0.9f;
  vec2 wind_direction{0.02f, 0.005f};
};

class Scene {
public:
  [[nodiscard]] EntityHandle create_entity(std::string_view name = "Entity");
  void destroy_entity(EntityHandle e);

  // --- Renderables -------------------------------------------------------
  EntityHandle add_renderable(const RenderableDesc& desc);
  [[nodiscard]] RenderableDesc* renderable(EntityHandle e);
  [[nodiscard]] const std::vector<EntityHandle>& renderables() const { return renderable_ids_; }

  // --- Lights ------------------------------------------------------------
  EntityHandle add_light(const LightDesc& desc);
  void set_light(EntityHandle e, const LightDesc& desc);
  [[nodiscard]] const std::vector<LightDesc>& lights() const { return lights_; }

  // --- Environment -------------------------------------------------------
  SkyDesc& sky() { return sky_; }
  [[nodiscard]] const SkyDesc& sky() const { return sky_; }

  // Direction from the scene towards the sun, derived from elevation/azimuth.
  [[nodiscard]] vec3 sun_direction() const;

  // --- Accessors used by the renderer ------------------------------------
  [[nodiscard]] Transform& transform(EntityHandle e);
  [[nodiscard]] const Transform& transform(EntityHandle e) const;

  void clear();
  [[nodiscard]] u32 entity_count() const { return static_cast<u32>(names_.size()); }

  // Called by the renderer once per frame after culling.
  struct Visibility {
    std::vector<EntityHandle> renderables;
    std::vector<u32> light_indices;
    u32 culled = 0;
  };
  void cull(const Frustum& frustum, Visibility& out) const;

  // Pack lights into the GPU layout the renderer expects.
  void pack_lights(std::vector<GpuLight>& out) const;

  [[nodiscard]] const mat4& world_matrix(EntityHandle e) const;

private:
  std::vector<std::string> names_;
  std::vector<Transform> transforms_;
  std::vector<mat4> world_matrices_;
  std::vector<bool> alive_;
  std::vector<RenderableDesc> renderables_;
  std::vector<EntityHandle> renderable_ids_;
  std::vector<LightDesc> lights_;
  std::vector<EntityHandle> light_ids_;
  SkyDesc sky_{};
};

} // namespace neo
