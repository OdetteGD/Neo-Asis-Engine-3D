#include "neo/render/Resources.h"

#include "neo/core/Log.h"
#include "neo/gfx/CommandList.h"
#include "neo/gfx/GpuAllocator.h"
#include "neo/gfx/VulkanContext.h"

#include <algorithm>
#include <cstring>

namespace neo {

namespace {

// 1x1 opaque white, used for every unbound texture slot.
constexpr u8 kWhitePixel[4] = {255, 255, 255, 255};

// One-shot upload helper: allocates a staging buffer, copies, submits, waits.
// Startup and load-time only — never call this per frame.
bool upload_buffer_blocking(VulkanContext& ctx, GpuAllocator& alloc, VkCommandPool pool,
                            VkQueue queue, VkBuffer dst, u64 dst_offset, const void* src,
                            u64 bytes) {
  if (bytes == 0)
    return true;

  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation alloc_h = VK_NULL_HANDLE;
  void* mapped = nullptr;

  BufferDesc bd{};
  bd.name = "staging";
  bd.size = bytes;
  bd.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bd.memory = MemoryClass::Upload;
  bd.persistently_mapped = true;
  if (!alloc.create_buffer(bd, staging, alloc_h, &mapped))
    return false;
  std::memcpy(mapped, src, bytes);

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(ctx.device(), &ai, &cmd) != VK_SUCCESS) {
    alloc.destroy_buffer(staging, alloc_h);
    return false;
  }

  CommandList cl(cmd);
  cl.begin();
  const VkBufferCopy region{dst_offset, 0, bytes};
  vkCmdCopyBuffer(cmd, staging, dst, 1, &region);
  cl.end();

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(ctx.device(), &fci, nullptr, &fence);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue, 1, &si, fence);
  vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);

  vkDestroyFence(ctx.device(), fence, nullptr);
  vkFreeCommandBuffers(ctx.device(), pool, 1, &cmd);
  alloc.destroy_buffer(staging, alloc_h);
  return true;
}

} // namespace

// --- Vertex ----------------------------------------------------------------
Vertex Vertex::from_pnuv(const vec3& p, const vec3& n, const vec2& t) {
  Vertex v{};
  v.position = p;
  // Octahedral encode — same function as oct_encode() in frame_data.glsl.
  const vec3 nn = glm::normalize(n);
  vec3 a = nn / (std::abs(nn.x) + std::abs(nn.y) + std::abs(nn.z));
  vec2 e(a.x, a.y);
  if (a.z < 0.0f) {
    e = (1.0f - glm::abs(vec2(a.y, a.x))) * glm::sign(vec2(a.x, a.y));
  }
  v.normal_oct = e * 0.5f + 0.5f;
  v.uv = t;
  return v;
}

void Vertex::write_to(float* dst) const {
  dst[0] = position.x;
  dst[1] = position.y;
  dst[2] = position.z;
  dst[3] = normal_oct.x;
  dst[4] = normal_oct.y;
  dst[5] = uv.x;
  dst[6] = uv.y;
  dst[7] = padding;
}

// --- Lifetime --------------------------------------------------------------
std::unique_ptr<ResourceRegistry> ResourceRegistry::create(VulkanContext& ctx) {
  auto r = std::unique_ptr<ResourceRegistry>(new ResourceRegistry());
  r->ctx_ = &ctx;
  r->alloc_ = &ctx.allocator();
  return r;
}

ResourceRegistry::~ResourceRegistry() {
  if (!ctx_)
    return;
  const VkDevice dev = ctx_->device();

  destroy_buffers();

  for (Texture& t : textures_) {
    if (t.sampler)
      vkDestroySampler(dev, t.sampler, nullptr);
    if (t.view)
      vkDestroyImageView(dev, t.view, nullptr);
    if (t.image)
      alloc_->destroy_image(t.image, t.alloc);
  }
  if (default_texture_.view)
    vkDestroyImageView(dev, default_texture_.view, nullptr);
  if (default_texture_.image) {
    alloc_->destroy_image(default_texture_.image, default_texture_.alloc);
  }
  if (default_sampler_)
    vkDestroySampler(dev, default_sampler_, nullptr);
  if (bindless_pool_)
    vkDestroyDescriptorPool(dev, bindless_pool_, nullptr);
  if (bindless_layout_)
    vkDestroyDescriptorSetLayout(dev, bindless_layout_, nullptr);
}

bool ResourceRegistry::initialize(VkCommandPool upload_pool, VkQueue upload_queue) {
  upload_pool_ = upload_pool;
  upload_queue_ = upload_queue;

  // Handles are 1-based, so slot 0 is a null placeholder in every array.
  meshes_.emplace_back();
  materials_.emplace_back();
  textures_.emplace_back();

  if (!create_buffers(1 << 16, 1 << 16))
    return false;
  create_default_texture();
  return build_bindless_set();
}

bool ResourceRegistry::create_buffers(u64 vertex_bytes, u64 index_bytes) {
  destroy_buffers();

  BufferDesc vd{};
  vd.name = "VertexArena";
  vd.size = vertex_bytes;
  vd.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vd.memory = MemoryClass::DeviceLocal;
  if (!alloc_->create_buffer(vd, vertex_buffer_, vertex_alloc_))
    return false;
  vertex_capacity_ = vertex_bytes;

  BufferDesc id{};
  id.name = "IndexArena";
  id.size = index_bytes;
  id.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  id.memory = MemoryClass::DeviceLocal;
  if (!alloc_->create_buffer(id, index_buffer_, index_alloc_))
    return false;
  index_capacity_ = index_bytes;

  NEO_DEBUG(kRender, "Geometry arenas: {} KiB vertices, {} KiB indices", vertex_bytes >> 10,
            index_bytes >> 10);
  return true;
}

void ResourceRegistry::destroy_buffers() {
  if (vertex_buffer_) {
    alloc_->destroy_buffer(vertex_buffer_, vertex_alloc_);
    vertex_buffer_ = VK_NULL_HANDLE;
    vertex_alloc_ = VK_NULL_HANDLE;
    vertex_capacity_ = 0;
  }
  if (index_buffer_) {
    alloc_->destroy_buffer(index_buffer_, index_alloc_);
    index_buffer_ = VK_NULL_HANDLE;
    index_alloc_ = VK_NULL_HANDLE;
    index_capacity_ = 0;
  }
}

void ResourceRegistry::create_default_texture() {
  const VkDevice dev = ctx_->device();

  ImageDesc desc{};
  desc.name = "DefaultWhite";
  desc.format = VK_FORMAT_R8G8B8A8_SRGB;
  desc.width = 1;
  desc.height = 1;
  desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!alloc_->create_image(desc, default_texture_.image, default_texture_.alloc))
    return;

  // Upload the single white texel.
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation staging_alloc = VK_NULL_HANDLE;
  void* mapped = nullptr;
  BufferDesc bd{};
  bd.size = 4;
  bd.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bd.memory = MemoryClass::Upload;
  bd.persistently_mapped = true;
  if (!alloc_->create_buffer(bd, staging, staging_alloc, &mapped))
    return;
  std::memcpy(mapped, kWhitePixel, 4);

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = upload_pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &ai, &cmd);

  CommandList cl(cmd);
  cl.begin();
  cl.image_barrier(default_texture_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {1, 1, 1};
  vkCmdCopyBufferToImage(cmd, staging, default_texture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &region);

  cl.image_barrier(default_texture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  cl.end();

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(dev, &fci, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(upload_queue_, 1, &si, fence);
  vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(dev, fence, nullptr);
  vkFreeCommandBuffers(dev, upload_pool_, 1, &cmd);
  alloc_->destroy_buffer(staging, staging_alloc);

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = default_texture_.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_SRGB;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(dev, &vci, nullptr, &default_texture_.view);
  default_texture_.width = 1;
  default_texture_.height = 1;
  default_texture_.bindless_index = 0;

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.maxLod = VK_LOD_CLAMP_NONE;
  vkCreateSampler(dev, &sci, nullptr, &default_sampler_);
}

bool ResourceRegistry::build_bindless_set() {
  const VkDevice dev = ctx_->device();

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = kMaxBindlessTextures;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binding.pImmutableSamplers = nullptr;

  VkDescriptorBindingFlags flags =
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
  flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flags_ci.bindingCount = 1;
  flags_ci.pBindingFlags = &flags;

  VkDescriptorSetLayoutCreateInfo lci{};
  lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  lci.pNext = &flags_ci;
  lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  lci.bindingCount = 1;
  lci.pBindings = &binding;
  NEO_VK_CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &bindless_layout_));

  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures};
  VkDescriptorPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 1;
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &size;
  NEO_VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &bindless_pool_));

  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = bindless_pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &bindless_layout_;
  NEO_VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &bindless_set_));

  // Slot 0 is the default white texture for every unbound material.
  VkDescriptorImageInfo info{default_sampler_, default_texture_.view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = bindless_set_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  return true;
}

// --- Meshes ----------------------------------------------------------------
MeshHandle ResourceRegistry::add_mesh(std::vector<Vertex> vertices, std::vector<u32> indices,
                                      std::string name) {
  if (vertices.empty() || indices.empty()) {
    NEO_WARN(kRender, "add_mesh('{}') rejected: empty geometry", name);
    return {};
  }

  Mesh m{};
  m.first_index = static_cast<u32>(cpu_indices_.size());
  m.index_count = static_cast<u32>(indices.size());
  m.vertex_offset = static_cast<u32>(cpu_vertices_.size());
  m.vertex_count = static_cast<u32>(vertices.size());
  m.name = std::move(name);

  Aabb bounds{};
  for (const Vertex& v : vertices)
    bounds.expand(v.position);
  m.bounds = bounds;

  // Indices are rebased into the global vertex arena.
  cpu_vertices_.insert(cpu_vertices_.end(), vertices.begin(), vertices.end());
  cpu_indices_.reserve(cpu_indices_.size() + indices.size());
  for (u32 idx : indices)
    cpu_indices_.push_back(idx + m.vertex_offset);

  geometry_dirty_ = true;
  meshes_.push_back(std::move(m));
  return MeshHandle::make(static_cast<u32>(meshes_.size() - 1), 1);
}

const Mesh* ResourceRegistry::mesh(MeshHandle h) const {
  if (!h.valid() || h.index() >= meshes_.size())
    return nullptr;
  return &meshes_[h.index()];
}

// --- Materials -------------------------------------------------------------
MaterialHandle ResourceRegistry::add_material(const Material& material) {
  materials_.push_back(material);
  return MaterialHandle::make(static_cast<u32>(materials_.size() - 1), 1);
}

Material* ResourceRegistry::material(MaterialHandle h) {
  if (!h.valid() || h.index() >= materials_.size())
    return nullptr;
  return &materials_[h.index()];
}

const Material* ResourceRegistry::material(MaterialHandle h) const {
  if (!h.valid() || h.index() >= materials_.size())
    return nullptr;
  return &materials_[h.index()];
}

// --- Textures --------------------------------------------------------------
TextureHandle ResourceRegistry::upload_texture(const TextureData& data, bool srgb) {
  if (data.width == 0 || data.height == 0 || data.pixels.empty())
    return {};
  if (textures_.size() >= kMaxBindlessTextures) {
    NEO_WARN(kRender, "Bindless texture array full ({} slots); '{}' dropped", kMaxBindlessTextures,
             data.name);
    return {};
  }

  const VkDevice dev = ctx_->device();
  const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

  Texture tex{};
  ImageDesc desc{};
  desc.name = data.name;
  desc.format = format;
  desc.width = data.width;
  desc.height = data.height;
  desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!alloc_->create_image(desc, tex.image, tex.alloc))
    return {};
  tex.width = data.width;
  tex.height = data.height;

  // Stage + copy.
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation staging_alloc = VK_NULL_HANDLE;
  void* mapped = nullptr;
  BufferDesc bd{};
  bd.size = data.pixels.size();
  bd.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bd.memory = MemoryClass::Upload;
  bd.persistently_mapped = true;
  if (!alloc_->create_buffer(bd, staging, staging_alloc, &mapped))
    return {};
  std::memcpy(mapped, data.pixels.data(), data.pixels.size());

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = upload_pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &ai, &cmd);

  CommandList cl(cmd);
  cl.begin();
  cl.image_barrier(tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {data.width, data.height, 1};
  vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  cl.image_barrier(tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  cl.end();

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(dev, &fci, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(upload_queue_, 1, &si, fence);
  vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(dev, fence, nullptr);
  vkFreeCommandBuffers(dev, upload_pool_, 1, &cmd);
  alloc_->destroy_buffer(staging, staging_alloc);

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = tex.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(dev, &vci, nullptr, &tex.view);

  VkSamplerCreateInfo tsci{};
  tsci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  tsci.magFilter = VK_FILTER_LINEAR;
  tsci.minFilter = VK_FILTER_LINEAR;
  tsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  tsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  tsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  tsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  tsci.maxLod = VK_LOD_CLAMP_NONE;
  vkCreateSampler(dev, &tsci, nullptr, &tex.sampler);

  tex.bindless_index = static_cast<u32>(textures_.size());
  textures_.push_back(tex);

  // Bindless writes are update-after-bind, so this is safe at any time.
  VkDescriptorImageInfo info{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = bindless_set_;
  write.dstBinding = 0;
  write.dstArrayElement = tex.bindless_index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

  return TextureHandle::make(tex.bindless_index, 1);
}

u32 ResourceRegistry::bindless_index(TextureHandle h) const {
  if (!h.valid() || h.index() >= textures_.size())
    return 0;
  return textures_[h.index()].bindless_index;
}

// --- GPU upload ------------------------------------------------------------
void ResourceRegistry::flush_geometry(VkCommandBuffer cmd) {
  if (!geometry_dirty_ || cmd == VK_NULL_HANDLE)
    return;

  const u64 vbytes = cpu_vertices_.size() * sizeof(Vertex);
  const u64 ibytes = cpu_indices_.size() * sizeof(u32);

  // Grow on demand (2x headroom) and re-upload everything. Geometry is
  // uploaded once at load time, so a full re-upload is cheaper than tracking
  // per-mesh dirty ranges.
  if (vbytes > vertex_capacity_ || ibytes > index_capacity_) {
    const u64 new_v = std::max<u64>(vertex_capacity_ * 2, vbytes);
    const u64 new_i = std::max<u64>(index_capacity_ * 2, ibytes);
    NEO_INFO(kRender, "Growing geometry arenas to {} KiB / {} KiB", new_v >> 10, new_i >> 10);
    if (!create_buffers(new_v, new_i)) {
      NEO_ERROR(kRender, "Geometry arena growth failed");
      return;
    }
    uploaded_vertex_bytes_ = 0;
    uploaded_index_bytes_ = 0;
  }

  if (vbytes > uploaded_vertex_bytes_) {
    // Pack the CPU vertices into the 8-float stride the shader expects.
    std::vector<float> packed(cpu_vertices_.size() * kFloatsPerVertex);
    for (usize i = 0; i < cpu_vertices_.size(); ++i) {
      cpu_vertices_[i].write_to(&packed[i * kFloatsPerVertex]);
    }
    // A direct device-local copy needs a staging buffer; do it synchronously
    // here because this runs at load, not mid-frame.
    upload_buffer_blocking(*ctx_, *alloc_, upload_pool_, upload_queue_, vertex_buffer_, 0,
                           packed.data(), packed.size() * sizeof(float));
    uploaded_vertex_bytes_ = vbytes;
  }

  if (ibytes > uploaded_index_bytes_) {
    upload_buffer_blocking(*ctx_, *alloc_, upload_pool_, upload_queue_, index_buffer_, 0,
                           cpu_indices_.data(), ibytes);
    uploaded_index_bytes_ = ibytes;
  }

  // Make the copies visible to the vertex/index fetch stages.
  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask =
      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);

  geometry_dirty_ = false;
  NEO_INFO(kRender, "Geometry uploaded: {} vertices, {} indices", cpu_vertices_.size(),
           cpu_indices_.size());
}

u64 ResourceRegistry::geometry_bytes() const {
  return cpu_vertices_.size() * sizeof(Vertex) + cpu_indices_.size() * sizeof(u32);
}

} // namespace neo
