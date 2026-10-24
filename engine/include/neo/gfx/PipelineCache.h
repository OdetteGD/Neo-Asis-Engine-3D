// ===========================================================================
//  PipelineCache — graphics/compute pipeline construction + shader loading.
//
//  All pipelines use VK_EXT_dynamic_rendering (no VkRenderPass / VkFramebuffer)
//  and dynamic viewport + scissor, so a resize never rebuilds a pipeline.
//  Vertex input is mostly empty: geometry streams through storage buffers, so
//  the builder only enables attributes for legacy glTF paths.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"
#include "neo/gfx/VulkanContext.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neo {

struct ShaderModule {
  VkShaderModule module = VK_NULL_HANDLE;
  std::string path;

  [[nodiscard]] bool valid() const { return module != VK_NULL_HANDLE; }
};

struct GraphicsPipelineDesc {
  std::string_view name;
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;

  std::vector<VkFormat> color_formats;   // empty => depth-only
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reverse-Z

  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  bool depth_write = true;
  bool depth_test = true;
  bool blend = false;
  VkBlendFactor src_color = VK_BLEND_FACTOR_SRC_ALPHA;
  VkBlendFactor dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  u32 push_constant_size = 0;
  VkShaderStageFlags push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  std::vector<VkVertexInputBindingDescription> vertex_bindings;
  std::vector<VkVertexInputAttributeDescription> vertex_attributes;
};

struct ComputePipelineDesc {
  std::string_view name;
  VkShaderModule comp = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

class PipelineCache {
public:
  explicit PipelineCache(VulkanContext& ctx);
  ~PipelineCache();

  // --- Shader modules (SPIR-V loaded from disk / APK assets) -------------
  [[nodiscard]] VkShaderModule load(const std::string& relative_path);
  void release_all();

  [[nodiscard]] VkPipeline create(const GraphicsPipelineDesc& desc);
  [[nodiscard]] VkPipeline create(const ComputePipelineDesc& desc);

  // --- Layout helpers ----------------------------------------------------
  [[nodiscard]] VkDescriptorSetLayout create_set_layout(
      const std::vector<VkDescriptorSetLayoutBinding>& bindings,
      VkDescriptorSetLayoutCreateFlags flags = 0);
  [[nodiscard]] VkPipelineLayout create_layout(std::vector<VkDescriptorSetLayout> sets,
                                               u32 push_constant_size,
                                               VkShaderStageFlags stages);

  void release(VkPipeline pipeline);
  void release(VkPipelineLayout layout);
  void release(VkDescriptorSetLayout layout);

  // Cold-start cache on disk avoids recompiling driver IR on every launch,
  // which is worth 100-400 ms on mobile.
  void load_disk_cache(std::string_view path);
  void save_disk_cache(std::string_view path) const;

  [[nodiscard]] VkPipelineCache handle() const { return cache_; }
  [[nodiscard]] usize pipeline_count() const { return pipelines_.size(); }

private:
  [[nodiscard]] std::vector<u32> read_spirv(const std::string& relative_path);

  VulkanContext& ctx_;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::unordered_map<std::string, VkShaderModule> modules_;
  std::vector<VkPipeline> pipelines_;
  std::vector<VkPipelineLayout> layouts_;
  std::vector<VkDescriptorSetLayout> set_layouts_;
};

// Resolve "<NEO_SHADER_DIR>/<relative>.spv" for both desktop and Android APK
// asset layouts. Returns an empty string when the file cannot be located.
[[nodiscard]] std::string resolve_shader_path(std::string_view relative);

} // namespace neo
