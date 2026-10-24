// ===========================================================================
//  Resources — mesh and material storage backed by GPU arenas.
//
//  Two grow-only buffers hold every mesh in the scene:
//
//    vertex arena : tightly packed Vertex records (32 B each)
//    index arena  : uint32 triangle indices
//
//  A mesh is just a (first_index, index_count, vertex_offset) triple into
//  those arenas, so drawing a scene is one pipeline bind plus N draw calls
//  with only the index buffer offset changing. This is what makes bindless
//  vertex fetching in gbuffer.vert possible: the shader reads
//  `vertices[gl_VertexIndex * 8 + n]` from a single SSBO.
//
//  Textures live in a bindless descriptor array. Index 0 is always a 1x1
//  white default, so `textureIndices.x == 0` is a safe "untextured" marker.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"
#include "neo/gfx/VulkanContext.h"

#include <vk_mem_alloc.h>

#include <memory>
#include <string>
#include <vector>

namespace neo {

class GpuAllocator;

inline constexpr u32 kMaxBindlessTextures = 64;
inline constexpr u32 kFloatsPerVertex = 8; // pos(3) + oct-normal(2) + uv(2) + pad(1)

// CPU-side vertex layout. Must match gbuffer.vert's stride of 8 floats.
struct Vertex {
  vec3 position{0.0f};
  vec2 normal_oct{0.5f, 0.5f}; // octahedral encoded, 0..1
  vec2 uv{0.0f};
  f32 padding = 0.0f;

  [[nodiscard]] static Vertex from_pnuv(const vec3& p, const vec3& n, const vec2& t);
  void write_to(float* dst) const;
};

struct Mesh {
  u32 first_index = 0;
  u32 index_count = 0;
  u32 vertex_offset = 0;
  u32 vertex_count = 0;
  Aabb bounds{};
  std::string name;

  [[nodiscard]] bool valid() const { return index_count > 0; }
};

struct Material {
  std::string name;
  vec4 base_color_factor{1.0f};
  f32 metallic = 0.0f;
  f32 roughness = 0.5f;
  f32 emissive_strength = 1.0f;
  f32 occlusion_strength = 1.0f;
  vec3 emissive_factor{0.0f};

  // Bindless slot indices; 0 means "unbound".
  u32 albedo_texture = 0;
  u32 normal_texture = 0;

  bool double_sided = false;
  bool alpha_test = false;
  f32 alpha_cutoff = 0.5f;
};

struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  u32 width = 0;
  u32 height = 0;
  u32 bindless_index = 0; // 0 = default white
};

// Raw texture data before it reaches the GPU.
struct TextureData {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> pixels; // RGBA8, tightly packed
  std::string name;
};

class ResourceRegistry {
public:
  static std::unique_ptr<ResourceRegistry> create(VulkanContext& ctx);
  ~ResourceRegistry();

  ResourceRegistry(const ResourceRegistry&) = delete;
  ResourceRegistry& operator=(const ResourceRegistry&) = delete;

  // Must be called once before any upload; builds the default white texture,
  // the bindless descriptor set and the initial (tiny) arenas.
  bool initialize(VkCommandPool upload_pool, VkQueue upload_queue);

  // --- Meshes ------------------------------------------------------------
  [[nodiscard]] MeshHandle add_mesh(std::vector<Vertex> vertices, std::vector<u32> indices,
                                    std::string name = {});
  [[nodiscard]] const Mesh* mesh(MeshHandle h) const;
  [[nodiscard]] u32 mesh_count() const { return static_cast<u32>(meshes_.size() - 1); }

  // --- Materials ---------------------------------------------------------
  [[nodiscard]] MaterialHandle add_material(const Material& material);
  [[nodiscard]] Material* material(MaterialHandle h);
  [[nodiscard]] const Material* material(MaterialHandle h) const;

  // --- Textures ----------------------------------------------------------
  [[nodiscard]] TextureHandle upload_texture(const TextureData& data, bool srgb = true);
  [[nodiscard]] u32 bindless_index(TextureHandle h) const;

  // --- GPU state ---------------------------------------------------------
  // Uploads any pending geometry. Call once per frame before recording draws.
  void flush_geometry(VkCommandBuffer cmd);
  [[nodiscard]] bool geometry_dirty() const { return geometry_dirty_; }

  [[nodiscard]] VkBuffer vertex_buffer() const { return vertex_buffer_; }
  [[nodiscard]] VkBuffer index_buffer() const { return index_buffer_; }
  [[nodiscard]] VkDescriptorSetLayout bindless_layout() const { return bindless_layout_; }
  [[nodiscard]] VkDescriptorSet bindless_set() const { return bindless_set_; }

  // --- Statistics --------------------------------------------------------
  [[nodiscard]] u64 geometry_bytes() const;
  [[nodiscard]] u32 texture_count() const { return static_cast<u32>(textures_.size()); }

  // --- Vertex/index counts for the renderer's draw loop ------------------
  [[nodiscard]] u32 total_vertices() const { return static_cast<u32>(cpu_vertices_.size()); }
  [[nodiscard]] u32 total_indices() const { return static_cast<u32>(cpu_indices_.size()); }

private:
  ResourceRegistry() = default;

  bool create_buffers(u64 vertex_bytes, u64 index_bytes);
  void destroy_buffers();
  void create_default_texture();
  bool build_bindless_set();

  VulkanContext* ctx_ = nullptr;
  GpuAllocator* alloc_ = nullptr;

  // CPU staging
  std::vector<Vertex> cpu_vertices_;
  std::vector<u32> cpu_indices_;
  u64 uploaded_vertex_bytes_ = 0;
  u64 uploaded_index_bytes_ = 0;
  bool geometry_dirty_ = false;

  // GPU arenas
  VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
  VmaAllocation vertex_alloc_ = VK_NULL_HANDLE;
  u64 vertex_capacity_ = 0;
  VkBuffer index_buffer_ = VK_NULL_HANDLE;
  VmaAllocation index_alloc_ = VK_NULL_HANDLE;
  u64 index_capacity_ = 0;

  // Bindless textures
  VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
  VkSampler default_sampler_ = VK_NULL_HANDLE;
  Texture default_texture_{};

  std::vector<Mesh> meshes_;
  std::vector<Material> materials_;
  std::vector<Texture> textures_;

  VkCommandPool upload_pool_ = VK_NULL_HANDLE;
  VkQueue upload_queue_ = VK_NULL_HANDLE;
};

} // namespace neo
