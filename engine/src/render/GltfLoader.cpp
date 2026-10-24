#include "neo/render/GltfLoader.h"

#include "neo/core/AssetSystem.h"
#include "neo/core/Log.h"
#include "neo/render/Resources.h"

#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace neo {

namespace {

// Decode an embedded image buffer to tightly packed RGBA8.
bool decode_image(const u8* bytes, usize size, TextureData& out, const std::string& name) {
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &channels, 4);
  if (!pixels) {
    NEO_WARN(kAsset, "Image decode failed for '{}': {}", name, stbi_failure_reason());
    return false;
  }
  out.width = static_cast<u32>(w);
  out.height = static_cast<u32>(h);
  out.pixels.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
  out.name = name;
  stbi_image_free(pixels);
  return true;
}

vec4 to_vec4(const cgltf_float* v, vec4 fallback) {
  if (!v)
    return fallback;
  return vec4(v[0], v[1], v[2], v[3]);
}

vec3 to_vec3(const cgltf_float* v, vec3 fallback) {
  if (!v)
    return fallback;
  return vec3(v[0], v[1], v[2]);
}

Material convert_material(const cgltf_material& src,
                          const std::unordered_map<const cgltf_texture*, u32>& texture_slots) {
  Material m{};
  m.name = src.name ? src.name : "material";

  const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
  m.base_color_factor = to_vec4(pbr.base_color_factor, vec4(1.0f));
  m.metallic = pbr.metallic_factor;
  m.roughness = pbr.roughness_factor;
  m.emissive_factor = to_vec3(src.emissive_factor, vec3(0.0f));

  if (src.has_emissive_strength)
    m.emissive_strength = src.emissive_strength.emissive_strength;

  auto slot = [&](const cgltf_texture_view& view) -> u32 {
    if (!view.texture)
      return 0;
    auto it = texture_slots.find(view.texture);
    return it != texture_slots.end() ? it->second : 0;
  };

  m.albedo_texture = slot(pbr.base_color_texture);
  m.normal_texture = slot(src.normal_texture);

  m.double_sided = src.double_sided;
  if (src.alpha_mode == cgltf_alpha_mode_mask) {
    m.alpha_test = true;
    m.alpha_cutoff = src.alpha_cutoff;
  }
  m.occlusion_strength = src.occlusion_texture.texture ? 1.0f : 1.0f;
  return m;
}

// Recursively flatten the node graph into world-space transforms.
void walk_nodes(const cgltf_node* node, const Transform& parent, GltfScene& out,
                const cgltf_data* data,
                const std::unordered_map<const cgltf_mesh*, MeshHandle>& mesh_map,
                const std::unordered_map<const cgltf_material*, MaterialHandle>& material_map) {
  Transform world{};
  if (node->has_matrix) {
    const mat4 m = glm::make_mat4(node->matrix);
    world.position = vec3(m[3]);
    world.rotation = glm::quat_cast(mat3(m));
    world.scale = vec3(glm::length(vec3(m[0])), glm::length(vec3(m[1])), glm::length(vec3(m[2])));
  } else {
    if (node->has_translation) {
      world.position = vec3(node->translation[0], node->translation[1], node->translation[2]);
    }
    if (node->has_rotation) {
      world.rotation =
          quat(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
    }
    if (node->has_scale)
      world.scale = vec3(node->scale[0], node->scale[1], node->scale[2]);
  }

  // Compose with the parent (parent * local).
  Transform composed{};
  composed.position = parent.position + parent.rotation * (parent.scale * world.position);
  composed.rotation = parent.rotation * world.rotation;
  composed.scale = parent.scale * world.scale;

  if (node->mesh) {
    auto it = mesh_map.find(node->mesh);
    if (it != mesh_map.end()) {
      GltfNode gn{};
      gn.transform = composed;
      gn.mesh = it->second;
      gn.name = node->name ? node->name : "node";

      if (node->mesh->primitives_count > 0) {
        const cgltf_material* mat = node->mesh->primitives[0].material;
        if (mat) {
          auto mit = material_map.find(mat);
          if (mit != material_map.end())
            gn.material = mit->second;
        }
      }
      out.nodes.push_back(std::move(gn));
    }
  }

  for (cgltf_size c = 0; c < node->children_count; ++c) {
    walk_nodes(node->children[c], composed, out, data, mesh_map, material_map);
  }
}

} // namespace

GltfScene GltfLoader::load(ResourceRegistry& registry, std::string_view virtual_path) {
  GltfScene scene{};
  scene.source_path = std::string(virtual_path);

  std::vector<u8> bytes = assets().read_binary(virtual_path);
  if (bytes.empty()) {
    NEO_ERROR(kAsset, "glTF not found or empty: {}", virtual_path);
    return scene;
  }

  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
    NEO_ERROR(kAsset, "cgltf_parse failed for {}", virtual_path);
    return scene;
  }

  if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success) {
    NEO_ERROR(kAsset,
              "cgltf_load_buffers failed for {} (external .bin buffers are not "
              "supported; export as .glb)",
              virtual_path);
    cgltf_free(data);
    return scene;
  }

  // --- Images -> bindless textures ----------------------------------------
  std::unordered_map<const cgltf_texture*, u32> texture_slots;
  std::vector<TextureHandle> image_handles(data->images_count, TextureHandle{});

  for (cgltf_size i = 0; i < data->images_count; ++i) {
    const cgltf_image& img = data->images[i];
    TextureData td{};
    td.name = img.name ? img.name : "image";

    if (img.buffer_view) {
      const cgltf_buffer_view& bv = *img.buffer_view;
      if (decode_image(static_cast<const u8*>(bv.buffer->data) + bv.offset, bv.size, td, td.name)) {
        image_handles[i] = registry.upload_texture(td, true);
      }
    }
  }

  // Map each cgltf_texture to the bindless slot of its decoded image.
  for (cgltf_size t = 0; t < data->textures_count; ++t) {
    const cgltf_texture& tex = data->textures[t];
    if (!tex.image)
      continue;
    const usize img = static_cast<usize>(tex.image - data->images);
    if (img >= image_handles.size())
      continue;
    const u32 slot = registry.bindless_index(image_handles[img]);
    texture_slots[&tex] = slot;
    scene.textures.push_back(image_handles[img]);
  }

  // --- Materials ----------------------------------------------------------
  std::unordered_map<const cgltf_material*, MaterialHandle> material_map;
  for (cgltf_size i = 0; i < data->materials_count; ++i) {
    Material m = convert_material(data->materials[i], texture_slots);
    const MaterialHandle mh = registry.add_material(m);
    material_map[&data->materials[i]] = mh;
    scene.materials.push_back(mh);
  }

  // --- Meshes -------------------------------------------------------------
  std::unordered_map<const cgltf_mesh*, MeshHandle> mesh_map;
  for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
    const cgltf_mesh& gm = data->meshes[mi];
    if (gm.primitives_count == 0)
      continue;

    // Merge every primitive into one mesh; per-primitive materials are lost,
    // which is acceptable until the submesh split lands (M3).
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    for (cgltf_size pi = 0; pi < gm.primitives_count; ++pi) {
      const cgltf_primitive& prim = gm.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles)
        continue;

      const cgltf_accessor* pos_acc = nullptr;
      const cgltf_accessor* nrm_acc = nullptr;
      const cgltf_accessor* uv_acc = nullptr;
      for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& attr = prim.attributes[a];
        if (attr.type == cgltf_attribute_type_position)
          pos_acc = attr.data;
        if (attr.type == cgltf_attribute_type_normal)
          nrm_acc = attr.data;
        if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
          uv_acc = attr.data;
      }
      if (!pos_acc)
        continue;

      const u32 base_vertex = static_cast<u32>(vertices.size());
      const cgltf_size count = pos_acc->count;
      for (cgltf_size v = 0; v < count; ++v) {
        f32 p[3] = {0, 0, 0};
        cgltf_accessor_read_float(pos_acc, v, p, 3);

        f32 n[3] = {0, 1, 0};
        if (nrm_acc)
          cgltf_accessor_read_float(nrm_acc, v, n, 3);

        f32 uv[2] = {0, 0};
        if (uv_acc)
          cgltf_accessor_read_float(uv_acc, v, uv, 2);

        vertices.push_back(
            Vertex::from_pnuv(vec3(p[0], p[1], p[2]), vec3(n[0], n[1], n[2]), vec2(uv[0], uv[1])));
      }

      if (prim.indices) {
        const cgltf_size icount = prim.indices->count;
        for (cgltf_size k = 0; k < icount; ++k) {
          indices.push_back(base_vertex +
                            static_cast<u32>(cgltf_accessor_read_index(prim.indices, k)));
        }
      } else {
        for (cgltf_size k = 0; k + 2 < count; k += 3) {
          indices.push_back(base_vertex + static_cast<u32>(k));
          indices.push_back(base_vertex + static_cast<u32>(k + 1));
          indices.push_back(base_vertex + static_cast<u32>(k + 2));
        }
      }
    }

    if (vertices.empty() || indices.empty())
      continue;
    const MeshHandle mh =
        registry.add_mesh(std::move(vertices), std::move(indices), gm.name ? gm.name : "mesh");
    mesh_map[&gm] = mh;
    scene.meshes.push_back(mh);
  }

  // --- Nodes --------------------------------------------------------------
  const cgltf_scene* gs =
      data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
  if (gs) {
    for (cgltf_size i = 0; i < gs->nodes_count; ++i) {
      walk_nodes(gs->nodes[i], Transform{}, scene, data, mesh_map, material_map);
    }
  }

  // --- Bounds -------------------------------------------------------------
  for (const GltfNode& n : scene.nodes) {
    const Mesh* m = registry.mesh(n.mesh);
    if (!m)
      continue;
    const mat4 xf = n.transform.matrix();
    // Transform the 8 corners of the mesh AABB.
    for (int c = 0; c < 8; ++c) {
      const vec3 corner((c & 1) ? m->bounds.max.x : m->bounds.min.x,
                        (c & 2) ? m->bounds.max.y : m->bounds.min.y,
                        (c & 4) ? m->bounds.max.z : m->bounds.min.z);
      scene.bounds.expand(vec3(xf * vec4(corner, 1.0f)));
    }
  }

  scene.ok = !scene.nodes.empty();
  const u32 tris = [&] {
    u32 total = 0;
    for (MeshHandle h : scene.meshes) {
      if (const Mesh* m = registry.mesh(h))
        total += m->index_count / 3;
    }
    return total;
  }();

  NEO_INFO(kAsset, "Loaded {}: {} meshes, {} materials, {} textures, {} nodes, {} triangles",
           virtual_path, scene.meshes.size(), scene.materials.size(), scene.textures.size(),
           scene.nodes.size(), tris);

  cgltf_free(data);
  return scene;
}

// ===========================================================================
//  Procedural primitives
// ===========================================================================
namespace primitives {

MeshHandle cube(ResourceRegistry& registry, f32 size) {
  const f32 h = size * 0.5f;
  std::vector<Vertex> v;
  std::vector<u32> idx;
  v.reserve(24);

  // Six faces, four vertices each, so normals stay flat-shaded and UVs are
  // independent per face.
  const vec3 face_normals[6] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
  for (int f = 0; f < 6; ++f) {
    const vec3 n = face_normals[f];
    const vec3 up = (std::abs(n.y) > 0.9f) ? vec3(0, 0, 1) : vec3(0, 1, 0);
    const vec3 right = glm::normalize(glm::cross(up, n));
    const vec3 u = glm::normalize(glm::cross(n, right));
    const vec3 center = n * h;
    const vec3 corners[4] = {center - right * h - u * h, center + right * h - u * h,
                             center + right * h + u * h, center - right * h + u * h};
    const vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int c = 0; c < 4; ++c)
      v.push_back(Vertex::from_pnuv(corners[c], n, uvs[c]));
    const u32 base = static_cast<u32>((f * 4));
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  return registry.add_mesh(std::move(v), std::move(idx), "cube");
}

MeshHandle sphere(ResourceRegistry& registry, f32 radius, u32 segments, u32 rings) {
  std::vector<Vertex> v;
  std::vector<u32> idx;
  for (u32 y = 0; y <= rings; ++y) {
    const f32 phi = kPi * static_cast<f32>(y) / static_cast<f32>(rings);
    for (u32 x = 0; x <= segments; ++x) {
      const f32 theta = kTau * static_cast<f32>(x) / static_cast<f32>(segments);
      const vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
      const vec2 uv(static_cast<f32>(x) / static_cast<f32>(segments),
                    static_cast<f32>(y) / static_cast<f32>(rings));
      v.push_back(Vertex::from_pnuv(n * radius, n, uv));
    }
  }
  for (u32 y = 0; y < rings; ++y) {
    for (u32 x = 0; x < segments; ++x) {
      const u32 a = y * (segments + 1) + x;
      const u32 b = a + segments + 1;
      idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  return registry.add_mesh(std::move(v), std::move(idx), "sphere");
}

MeshHandle plane(ResourceRegistry& registry, f32 extent, u32 subdivisions) {
  std::vector<Vertex> v;
  std::vector<u32> idx;
  subdivisions = std::max(1u, subdivisions);
  const f32 step = (extent * 2.0f) / static_cast<f32>(subdivisions);
  for (u32 z = 0; z <= subdivisions; ++z) {
    for (u32 x = 0; x <= subdivisions; ++x) {
      const f32 px = static_cast<f32>(x) * step - extent;
      const f32 pz = static_cast<f32>(z) * step - extent;
      const vec2 uv(static_cast<f32>(x) / static_cast<f32>(subdivisions),
                    static_cast<f32>(z) / static_cast<f32>(subdivisions));
      v.push_back(Vertex::from_pnuv(vec3(px, 0.0f, pz), vec3(0, 1, 0), uv));
    }
  }
  for (u32 z = 0; z < subdivisions; ++z) {
    for (u32 x = 0; x < subdivisions; ++x) {
      const u32 a = z * (subdivisions + 1) + x;
      const u32 b = a + subdivisions + 1;
      idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  return registry.add_mesh(std::move(v), std::move(idx), "plane");
}

MeshHandle grid(ResourceRegistry& registry, f32 extent, u32 divisions) {
  return plane(registry, extent, divisions);
}

} // namespace primitives

} // namespace neo
