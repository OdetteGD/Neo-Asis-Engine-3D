// ===========================================================================
//  GltfLoader — glTF 2.0 / GLB ingestion into the ResourceRegistry.
//
//  Scope for this milestone: static meshes, PBR metallic-roughness materials,
//  embedded or external images, node hierarchy flattened into world transforms.
//  Skinning, morph targets and animations are parsed for later use but not yet
//  driven by the renderer (see docs/ROADMAP.md M2/M3).
//
//  GLB (single file) is the primary format; `.gltf` works when its buffers are
//  data-URI embedded, because the engine reads through AssetSystem rather than
//  raw filesystem paths.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"
#include "neo/render/Resources.h"

#include <string>
#include <string_view>
#include <vector>

namespace neo {

struct GltfNode {
  Transform transform;
  MeshHandle mesh;
  MaterialHandle material;
  std::string name;
};

struct GltfScene {
  std::vector<GltfNode> nodes;
  std::vector<MeshHandle> meshes;
  std::vector<MaterialHandle> materials;
  std::vector<TextureHandle> textures;
  Aabb bounds{};
  std::string source_path;
  bool ok = false;
};

class GltfLoader {
public:
  // Loads `virtual_path` through AssetSystem. Returns ok=false and logs on
  // failure; never throws.
  static GltfScene load(ResourceRegistry& registry, std::string_view virtual_path);
};

// Procedural primitives. These exist so the sample scene renders something
// real without shipping binary assets, and so unit tests have known geometry.
namespace primitives {

[[nodiscard]] MeshHandle cube(ResourceRegistry& registry, f32 size = 1.0f);
[[nodiscard]] MeshHandle sphere(ResourceRegistry& registry, f32 radius = 0.5f, u32 segments = 32,
                                u32 rings = 16);
[[nodiscard]] MeshHandle plane(ResourceRegistry& registry, f32 extent = 10.0f,
                               u32 subdivisions = 4);
[[nodiscard]] MeshHandle grid(ResourceRegistry& registry, f32 extent = 10.0f, u32 divisions = 10);

} // namespace primitives

} // namespace neo
