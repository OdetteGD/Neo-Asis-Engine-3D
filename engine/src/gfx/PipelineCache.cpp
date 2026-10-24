#include "neo/gfx/PipelineCache.h"
#include "neo/core/AssetSystem.h"
#include "neo/core/Log.h"
#include "neo/gfx/CommandList.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace neo {

namespace {
constexpr const char* kSpvExtension = ".spv";

// Graphics stages: vertex + fragment (+ optional geometry/mesh later).
VkPipelineShaderStageCreateInfo stage_info(VkShaderStageFlagBits stage, VkShaderModule mod) {
  VkPipelineShaderStageCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  si.stage = stage;
  si.module = mod;
  si.pName = "main";
  return si;
}
} // namespace

std::string resolve_shader_path(std::string_view relative) {
  std::string rel(relative);
  if (rel.size() < 5 || rel.substr(rel.size() - 4) != kSpvExtension)
    rel += kSpvExtension;

#if defined(NEO_SHADER_IN_ASSETS)
  // Android: shaders are packed under assets/shaders and read through AAsset.
  return std::string("shaders/") + rel;
#else
  std::string base = NEO_SHADER_DIR;
  if (base.empty())
    base = "shaders";
  return base + "/" + rel;
#endif
}

PipelineCache::PipelineCache(VulkanContext& ctx) : ctx_(ctx) {
  VkPipelineCacheCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  NEO_VK_CHECK(vkCreatePipelineCache(ctx_.device(), &ci, nullptr, &cache_));
}

PipelineCache::~PipelineCache() {
  release_all();
  for (VkPipelineLayout l : layouts_)
    vkDestroyPipelineLayout(ctx_.device(), l, nullptr);
  for (VkDescriptorSetLayout l : set_layouts_) {
    vkDestroyDescriptorSetLayout(ctx_.device(), l, nullptr);
  }
  if (cache_ != VK_NULL_HANDLE)
    vkDestroyPipelineCache(ctx_.device(), cache_, nullptr);
}

std::vector<u32> PipelineCache::read_spirv(const std::string& relative_path) {
  std::vector<u8> bytes = assets().read_binary(relative_path);
  if (bytes.empty()) {
    NEO_ERROR(kGfx, "Shader not found: {}", relative_path);
    return {};
  }
  if (bytes.size() % 4 != 0) {
    NEO_ERROR(kGfx, "Shader {} is not a valid SPIR-V blob ({} bytes)", relative_path, bytes.size());
    return {};
  }
  std::vector<u32> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());

  // SPIR-V magic (little endian) guard: catches accidentally loading GLSL.
  constexpr u32 kMagic = 0x07230203u;
  if (words.empty() || words[0] != kMagic) {
    NEO_ERROR(kGfx, "{} does not start with the SPIR-V magic number", relative_path);
    return {};
  }
  return words;
}

VkShaderModule PipelineCache::load(const std::string& relative_path) {
  if (auto it = modules_.find(relative_path); it != modules_.end())
    return it->second;

  const std::vector<u32> code = read_spirv(relative_path);
  if (code.empty())
    return VK_NULL_HANDLE;

  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = code.size() * sizeof(u32);
  ci.pCode = code.data();

  VkShaderModule mod = VK_NULL_HANDLE;
  const VkResult r = vkCreateShaderModule(ctx_.device(), &ci, nullptr, &mod);
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "vkCreateShaderModule failed for {}: {}", relative_path, vk_result_name(r));
    return VK_NULL_HANDLE;
  }
  ctx_.set_object_name(reinterpret_cast<u64>(mod), VK_OBJECT_TYPE_SHADER_MODULE,
                       relative_path.c_str());
  modules_.emplace(relative_path, mod);
  return mod;
}

void PipelineCache::release_all() {
  for (VkPipeline p : pipelines_)
    vkDestroyPipeline(ctx_.device(), p, nullptr);
  pipelines_.clear();
  for (auto& [path, mod] : modules_)
    vkDestroyShaderModule(ctx_.device(), mod, nullptr);
  modules_.clear();
}

VkDescriptorSetLayout
PipelineCache::create_set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                                 VkDescriptorSetLayoutCreateFlags flags) {
  std::vector<VkDescriptorBindingFlags> binding_flags(bindings.size(),
                                                      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
  flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;

  VkDescriptorSetLayoutCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  ci.bindingCount = static_cast<u32>(bindings.size());
  ci.pBindings = bindings.data();
  ci.flags = flags;

  if (flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) {
    flags_ci.bindingCount = static_cast<u32>(binding_flags.size());
    flags_ci.pBindingFlags = binding_flags.data();
    ci.pNext = &flags_ci;
    ci.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  }

  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  NEO_VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device(), &ci, nullptr, &layout));
  set_layouts_.push_back(layout);
  return layout;
}

VkPipelineLayout PipelineCache::create_layout(std::vector<VkDescriptorSetLayout> sets,
                                              u32 push_constant_size, VkShaderStageFlags stages) {
  VkPushConstantRange range{};
  range.stageFlags = stages;
  range.offset = 0;
  range.size = push_constant_size;

  VkPipelineLayoutCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  ci.setLayoutCount = static_cast<u32>(sets.size());
  ci.pSetLayouts = sets.data();
  if (push_constant_size > 0) {
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &range;
  }

  VkPipelineLayout layout = VK_NULL_HANDLE;
  NEO_VK_CHECK(vkCreatePipelineLayout(ctx_.device(), &ci, nullptr, &layout));
  layouts_.push_back(layout);
  return layout;
}

VkPipeline PipelineCache::create(const GraphicsPipelineDesc& desc) {
  if (desc.vert == VK_NULL_HANDLE || desc.frag == VK_NULL_HANDLE) {
    NEO_ERROR(kGfx, "Pipeline '{}': missing shader module", desc.name);
    return VK_NULL_HANDLE;
  }

  const VkPipelineShaderStageCreateInfo stages[] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, desc.vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, desc.frag),
  };

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vi.vertexBindingDescriptionCount = static_cast<u32>(desc.vertex_bindings.size());
  vi.pVertexBindingDescriptions = desc.vertex_bindings.data();
  vi.vertexAttributeDescriptionCount = static_cast<u32>(desc.vertex_attributes.size());
  vi.pVertexAttributeDescriptions = desc.vertex_attributes.data();

  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = desc.topology;
  ia.primitiveRestartEnable = VK_FALSE;

  // Dynamic: viewport and scissor are set per pass, so resizes are free.
  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.depthClampEnable = VK_FALSE;
  rs.rasterizerDiscardEnable = VK_FALSE;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = desc.cull;
  rs.frontFace = desc.front_face;
  rs.depthBiasEnable = VK_FALSE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = desc.samples;
  ms.sampleShadingEnable = VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  ds.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = desc.depth_compare;
  ds.depthBoundsTestEnable = VK_FALSE;
  ds.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend_attachment.blendEnable = desc.blend ? VK_TRUE : VK_FALSE;
  blend_attachment.srcColorBlendFactor = desc.src_color;
  blend_attachment.dstColorBlendFactor = desc.dst_color;
  blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
  blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = desc.color_formats.empty() ? 0u : 1u;
  cb.pAttachments = desc.color_formats.empty() ? nullptr : &blend_attachment;
  // Multiple MRTs share the attachment state; extend when adding MRT.
  if (desc.color_formats.size() > 1) {
    static thread_local std::vector<VkPipelineColorBlendAttachmentState> multi;
    multi.assign(desc.color_formats.size(), blend_attachment);
    cb.attachmentCount = static_cast<u32>(multi.size());
    cb.pAttachments = multi.data();
  }

  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamic;

  // Dynamic rendering: the pipeline carries its own attachment formats.
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = static_cast<u32>(desc.color_formats.size());
  rendering.pColorAttachmentFormats = desc.color_formats.data();
  rendering.depthAttachmentFormat = desc.depth_format;

  VkGraphicsPipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  ci.pNext = &rendering;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dyn;
  ci.layout = desc.layout;
  ci.renderPass = VK_NULL_HANDLE;
  ci.subpass = 0;
  ci.basePipelineHandle = VK_NULL_HANDLE;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult r = vkCreateGraphicsPipelines(ctx_.device(), cache_, 1, &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "Pipeline '{}' creation failed: {}", desc.name, vk_result_name(r));
    return VK_NULL_HANDLE;
  }
  ctx_.set_object_name(reinterpret_cast<u64>(pipeline), VK_OBJECT_TYPE_PIPELINE,
                       std::string(desc.name).c_str());
  pipelines_.push_back(pipeline);
  NEO_DEBUG(kGfx, "Pipeline '{}' created", desc.name);
  return pipeline;
}

VkPipeline PipelineCache::create(const ComputePipelineDesc& desc) {
  if (desc.comp == VK_NULL_HANDLE) {
    NEO_ERROR(kGfx, "Compute pipeline '{}': missing shader module", desc.name);
    return VK_NULL_HANDLE;
  }

  VkComputePipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage = stage_info(VK_SHADER_STAGE_COMPUTE_BIT, desc.comp);
  ci.layout = desc.layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult r = vkCreateComputePipelines(ctx_.device(), cache_, 1, &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "Compute pipeline '{}' creation failed: {}", desc.name, vk_result_name(r));
    return VK_NULL_HANDLE;
  }
  ctx_.set_object_name(reinterpret_cast<u64>(pipeline), VK_OBJECT_TYPE_PIPELINE,
                       std::string(desc.name).c_str());
  pipelines_.push_back(pipeline);
  return pipeline;
}

void PipelineCache::release(VkPipeline pipeline) {
  if (pipeline == VK_NULL_HANDLE)
    return;
  vkDestroyPipeline(ctx_.device(), pipeline, nullptr);
  pipelines_.erase(std::remove(pipelines_.begin(), pipelines_.end(), pipeline), pipelines_.end());
}

void PipelineCache::release(VkPipelineLayout layout) {
  if (layout == VK_NULL_HANDLE)
    return;
  vkDestroyPipelineLayout(ctx_.device(), layout, nullptr);
  layouts_.erase(std::remove(layouts_.begin(), layouts_.end(), layout), layouts_.end());
}

void PipelineCache::release(VkDescriptorSetLayout layout) {
  if (layout == VK_NULL_HANDLE)
    return;
  vkDestroyDescriptorSetLayout(ctx_.device(), layout, nullptr);
  set_layouts_.erase(std::remove(set_layouts_.begin(), set_layouts_.end(), layout),
                     set_layouts_.end());
}

void PipelineCache::load_disk_cache(std::string_view path) {
  const std::vector<u8> bytes = assets().read_binary(std::string(path));
  if (bytes.empty()) {
    NEO_INFO(kGfx, "No pipeline cache blob at {} (first run)", path);
    return;
  }
  VkPipelineCacheCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  ci.initialDataSize = bytes.size();
  ci.pInitialData = bytes.data();

  VkPipelineCache loaded = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(ctx_.device(), &ci, nullptr, &loaded) == VK_SUCCESS) {
    vkDestroyPipelineCache(ctx_.device(), cache_, nullptr);
    cache_ = loaded;
    NEO_INFO(kGfx, "Loaded pipeline cache: {:.1f} KiB", static_cast<f64>(bytes.size()) / 1024.0);
  }
}

void PipelineCache::save_disk_cache(std::string_view path) const {
  usize size = 0;
  if (vkGetPipelineCacheData(ctx_.device(), cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
    return;
  }
  std::vector<u8> data(size);
  if (vkGetPipelineCacheData(ctx_.device(), cache_, &size, data.data()) != VK_SUCCESS)
    return;
  if (assets().write_binary(std::string(path), data)) {
    NEO_INFO(kGfx, "Saved pipeline cache: {:.1f} KiB", static_cast<f64>(size) / 1024.0);
  }
}

} // namespace neo
