#include "neo/scene/Scene.h"
#include "neo/core/Log.h"

#include <algorithm>
#include <cmath>

namespace neo {

EntityHandle Scene::create_entity(std::string_view name) {
  // Reuse a free slot when one exists so handles stay dense.
  for (u32 i = 0; i < alive_.size(); ++i) {
    if (!alive_[i]) {
      alive_[i] = true;
      names_[i] = std::string(name);
      transforms_[i] = Transform{};
      world_matrices_[i] = mat4(1.0f);
      return EntityHandle::make(i, static_cast<u32>(i) + 2u);
    }
  }
  names_.emplace_back(name);
  transforms_.emplace_back();
  world_matrices_.emplace_back(1.0f);
  alive_.push_back(true);
  return EntityHandle::make(static_cast<u32>(names_.size() - 1),
                            static_cast<u32>(names_.size()) + 1);
}

void Scene::destroy_entity(EntityHandle e) {
  if (!e.valid() || e.index() >= alive_.size())
    return;
  alive_[e.index()] = false;
  renderable_ids_.erase(std::remove_if(renderable_ids_.begin(), renderable_ids_.end(),
                                       [&](EntityHandle h) { return h == e; }),
                        renderable_ids_.end());
}

EntityHandle Scene::add_renderable(const RenderableDesc& desc) {
  const EntityHandle e = create_entity("Renderable");
  renderables_.push_back(desc);
  renderable_ids_.push_back(e);
  // Renderable index is mirrored in the entity slot for O(1) lookup.
  return e;
}

RenderableDesc* Scene::renderable(EntityHandle e) {
  for (usize i = 0; i < renderable_ids_.size(); ++i) {
    if (renderable_ids_[i] == e)
      return &renderables_[i];
  }
  return nullptr;
}

EntityHandle Scene::add_light(const LightDesc& desc) {
  const EntityHandle e = create_entity("Light");
  lights_.push_back(desc);
  light_ids_.push_back(e);
  return e;
}

void Scene::set_light(EntityHandle e, const LightDesc& desc) {
  for (usize i = 0; i < light_ids_.size(); ++i) {
    if (light_ids_[i] == e) {
      lights_[i] = desc;
      return;
    }
  }
}

Transform& Scene::transform(EntityHandle e) {
  if (!e.valid() || e.index() >= transforms_.size()) {
    static Transform fallback{};
    return fallback;
  }
  return transforms_[e.index()];
}

const Transform& Scene::transform(EntityHandle e) const {
  if (!e.valid() || e.index() >= transforms_.size()) {
    static const Transform fallback{};
    return fallback;
  }
  return transforms_[e.index()];
}

const mat4& Scene::world_matrix(EntityHandle e) const {
  static const mat4 identity(1.0f);
  if (!e.valid() || e.index() >= world_matrices_.size())
    return identity;
  return world_matrices_[e.index()];
}

vec3 Scene::sun_direction() const {
  const f32 el = sky_.elevation_degrees * kDegToRad;
  const f32 az = sky_.azimuth_degrees * kDegToRad;
  // Azimuth 0 = +Z, increasing towards +X. Elevation above the horizon.
  return glm::normalize(
      vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)));
}

void Scene::cull(const Frustum& frustum, Visibility& out) const {
  out.renderables.clear();
  out.light_indices.clear();
  out.culled = 0;

  // Renderables are culled against the mesh bounding sphere. Mesh bounds live
  // in the mesh registry; for this milestone we use the transform scale as a
  // conservative proxy until Mesh uploads real bounds.
  for (usize i = 0; i < renderable_ids_.size(); ++i) {
    const RenderableDesc& r = renderables_[i];
    if (!r.visible)
      continue;
    const vec3 center = r.transform.position;
    const f32 radius = std::max({r.transform.scale.x, r.transform.scale.y, r.transform.scale.z});
    if (frustum.intersects_sphere(center, radius * 0.5f + 0.001f)) {
      out.renderables.push_back(renderable_ids_[i]);
    } else {
      ++out.culled;
    }
  }

  // Lights: point/spot culled by radius, directional always visible.
  for (usize i = 0; i < lights_.size(); ++i) {
    const LightDesc& l = lights_[i];
    if (l.type == LightType::Directional || frustum.intersects_sphere(l.position, l.range)) {
      out.light_indices.push_back(static_cast<u32>(i));
    }
  }
}

void Scene::pack_lights(std::vector<GpuLight>& out) const {
  out.clear();
  out.reserve(lights_.size());
  for (const LightDesc& l : lights_) {
    GpuLight g{};
    g.position_range = vec4(l.position, l.range);
    g.direction_angle =
        vec4(glm::normalize(l.direction), std::cos(l.outer_cone_degrees * kDegToRad));
    g.color_intensity = vec4(l.color, l.intensity);
    g.params = vec4(static_cast<f32>(l.type), l.casts_shadow ? 0.0f : -1.0f,
                    std::cos(l.inner_cone_degrees * kDegToRad), 0.0f);
    out.push_back(g);
  }
}

void Scene::clear() {
  names_.clear();
  transforms_.clear();
  world_matrices_.clear();
  alive_.clear();
  renderables_.clear();
  renderable_ids_.clear();
  lights_.clear();
  light_ids_.clear();
}

} // namespace neo
