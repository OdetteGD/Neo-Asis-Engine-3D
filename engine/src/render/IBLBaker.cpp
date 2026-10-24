#include "neo/render/IBLBaker.h"

#include "neo/core/Log.h"
#include "neo/gfx/CommandList.h"
#include "neo/gfx/GpuAllocator.h"
#include "neo/gfx/PipelineCache.h"

#include <array>
#include <cmath>
#include <cstring>

namespace neo {

namespace {

constexpr VkFormat kEnvFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16_SFLOAT;

VkImageViewType cube_view_type() {
  return VK_IMAGE_VIEW_TYPE_CUBE;
}

} // namespace

std::unique_ptr<IBLBaker> IBLBaker::create(VulkanContext& ctx, PipelineCache& pipelines,
                                           const IBLDesc& desc) {
  auto b = std::unique_ptr<IBLBaker>(new IBLBaker());
  b->ctx_ = &ctx;
  b->alloc_ = &ctx.allocator();
  b->pipelines_ = &pipelines;
  b->desc_ = desc;
  return b;
}

IBLBaker::~IBLBaker() {
  if (!ctx_)
    return;
  const VkDevice dev = ctx_->device();
  destroy_textures();

  if (p_capture_)
    vkDestroyPipeline(dev, p_capture_, nullptr);
  if (p_irradiance_)
    vkDestroyPipeline(dev, p_irradiance_, nullptr);
  if (p_prefilter_)
    vkDestroyPipeline(dev, p_prefilter_, nullptr);
  if (p_brdf_)
    vkDestroyPipeline(dev, p_brdf_, nullptr);
  if (capture_pipeline_layout_)
    vkDestroyPipelineLayout(dev, capture_pipeline_layout_, nullptr);
  if (source_pipeline_layout_)
    vkDestroyPipelineLayout(dev, source_pipeline_layout_, nullptr);
  if (capture_layout_)
    vkDestroyDescriptorSetLayout(dev, capture_layout_, nullptr);
  if (source_layout_)
    vkDestroyDescriptorSetLayout(dev, source_layout_, nullptr);
  if (ibl_layout_)
    vkDestroyDescriptorSetLayout(dev, ibl_layout_, nullptr);
  if (pool_)
    vkDestroyDescriptorPool(dev, pool_, nullptr);
}

void IBLBaker::destroy_textures() {
  const VkDevice dev = ctx_->device();
  for (IBLTexture* t : {&environment_, &irradiance_, &prefiltered_, &brdf_lut_}) {
    if (t->sampler)
      vkDestroySampler(dev, t->sampler, nullptr);
    if (t->view)
      vkDestroyImageView(dev, t->view, nullptr);
    if (t->image)
      alloc_->destroy_image(t->image, t->alloc);
    *t = IBLTexture{};
  }
}

bool IBLBaker::initialize(VkDescriptorSetLayout frame_layout, VkCommandPool pool, VkQueue queue) {
  (void)frame_layout;
  pool_cmd_ = pool;
  queue_ = queue;
  if (!create_textures())
    return false;
  if (!create_pipelines())
    return false;
  return build_descriptor_set();
}

bool IBLBaker::create_textures() {
  const VkDevice dev = ctx_->device();

  auto make_cube = [&](IBLTexture& tex, const char* name, u32 size, u32 mips,
                       VkFormat format) -> bool {
    ImageDesc desc{};
    desc.name = name;
    desc.type = VK_IMAGE_TYPE_2D;
    desc.format = format;
    desc.width = size;
    desc.height = size;
    desc.depth = 1;
    desc.mip_levels = mips;
    desc.array_layers = 6;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    desc.memory = MemoryClass::DeviceLocal;
    if (!alloc_->create_image(desc, tex.image, tex.alloc))
      return false;
    tex.format = format;
    tex.size = size;
    tex.mips = mips;
    tex.layers = 6;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = tex.image;
    vci.viewType = cube_view_type();
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    NEO_VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &tex.view));

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = static_cast<f32>(mips);
    NEO_VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &tex.sampler));
    return true;
  };

  if (!make_cube(environment_, "IBL_Environment", desc_.environment_size, desc_.environment_mips,
                 kEnvFormat)) {
    return false;
  }
  if (!make_cube(irradiance_, "IBL_Irradiance", desc_.irradiance_size, 1, kEnvFormat)) {
    return false;
  }
  if (!make_cube(prefiltered_, "IBL_Prefiltered", desc_.prefilter_size, desc_.environment_mips,
                 kEnvFormat)) {
    return false;
  }

  // --- BRDF LUT (2D, not a cube) ------------------------------------------
  ImageDesc ld{};
  ld.name = "IBL_BRDFLUT";
  ld.format = kLutFormat;
  ld.width = desc_.brdf_lut_size;
  ld.height = desc_.brdf_lut_size;
  ld.mip_levels = 1;
  ld.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  if (!alloc_->create_image(ld, brdf_lut_.image, brdf_lut_.alloc))
    return false;
  brdf_lut_.format = kLutFormat;
  brdf_lut_.size = desc_.brdf_lut_size;
  brdf_lut_.mips = 1;
  brdf_lut_.layers = 1;

  VkImageViewCreateInfo lvci{};
  lvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  lvci.image = brdf_lut_.image;
  lvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  lvci.format = kLutFormat;
  lvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  NEO_VK_CHECK(vkCreateImageView(dev, &lvci, nullptr, &brdf_lut_.view));

  VkSamplerCreateInfo lsci{};
  lsci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  lsci.magFilter = VK_FILTER_LINEAR;
  lsci.minFilter = VK_FILTER_LINEAR;
  lsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  lsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  lsci.maxLod = 1.0f;
  NEO_VK_CHECK(vkCreateSampler(dev, &lsci, nullptr, &brdf_lut_.sampler));

  NEO_INFO(kRender, "IBL targets: env {}^2/{} mips, irradiance {}^2, prefilter {}^2, LUT {}^2",
           desc_.environment_size, desc_.environment_mips, desc_.irradiance_size,
           desc_.prefilter_size, desc_.brdf_lut_size);
  return true;
}

bool IBLBaker::create_pipelines() {
  // set 0: frame constants (sun + atmosphere). set 1: the bake target/source.
  capture_layout_ = pipelines_->create_set_layout({
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  });
  capture_pipeline_layout_ =
      pipelines_->create_layout({capture_layout_}, 64, VK_SHADER_STAGE_COMPUTE_BIT);

  source_layout_ = pipelines_->create_set_layout({
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  });
  source_pipeline_layout_ =
      pipelines_->create_layout({source_layout_}, 64, VK_SHADER_STAGE_COMPUTE_BIT);

  // Lighting-side layout: irradiance, prefiltered, BRDF LUT (set 2).
  ibl_layout_ = pipelines_->create_set_layout({
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  });

  VkShaderModule cap = pipelines_->load("ibl/sky_capture.comp");
  VkShaderModule irr = pipelines_->load("ibl/irradiance.comp");
  VkShaderModule pre = pipelines_->load("ibl/prefilter.comp");
  VkShaderModule lut = pipelines_->load("ibl/brdf_lut.comp");
  if (!cap || !irr || !pre || !lut) {
    NEO_ERROR(kRender, "IBL shaders missing — run the shader build target");
    return false;
  }

  p_capture_ =
      pipelines_->create(ComputePipelineDesc{"ibl_capture", cap, capture_pipeline_layout_});
  p_irradiance_ =
      pipelines_->create(ComputePipelineDesc{"ibl_irradiance", irr, source_pipeline_layout_});
  p_prefilter_ =
      pipelines_->create(ComputePipelineDesc{"ibl_prefilter", pre, source_pipeline_layout_});
  p_brdf_ = pipelines_->create(ComputePipelineDesc{"ibl_brdf", lut, source_pipeline_layout_});

  pipelines_built_ = p_capture_ && p_irradiance_ && p_prefilter_ && p_brdf_;
  return pipelines_built_;
}

bool IBLBaker::build_descriptor_set() {
  const VkDevice dev = ctx_->device();

  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
  };
  VkDescriptorPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.maxSets = 8;
  pci.poolSizeCount = 2;
  pci.pPoolSizes = sizes;
  NEO_VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &pool_));

  auto alloc_set = [&](VkDescriptorSetLayout layout, VkDescriptorSet& out) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    NEO_VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &out));
  };

  alloc_set(capture_layout_, capture_set_);
  alloc_set(source_layout_, irradiance_set_);
  alloc_set(source_layout_, prefilter_set_);
  alloc_set(source_layout_, lut_set_);
  alloc_set(ibl_layout_, ibl_set_);

  auto write_storage = [&](VkDescriptorSet set, u32 binding, VkImageView view) {
    VkDescriptorImageInfo info{};
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  };

  auto write_sampled = [&](VkDescriptorSet set, u32 binding, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  };

  write_storage(capture_set_, 0, environment_.view);
  write_sampled(irradiance_set_, 0, environment_.view, environment_.sampler);
  write_storage(irradiance_set_, 1, irradiance_.view);
  write_sampled(prefilter_set_, 0, environment_.view, environment_.sampler);
  write_storage(prefilter_set_, 1, prefiltered_.view);
  write_storage(lut_set_, 1, brdf_lut_.view);

  write_sampled(ibl_set_, 0, irradiance_.view, irradiance_.sampler);
  write_sampled(ibl_set_, 1, prefiltered_.view, prefiltered_.sampler);
  write_sampled(ibl_set_, 2, brdf_lut_.view, brdf_lut_.sampler);
  return true;
}

bool IBLBaker::run_capture(VkCommandBuffer cmd, VkDescriptorSet frame_set,
                           const IBLBakeParams& params) {
  CommandList cl(cmd);
  cl.bind_compute(p_capture_);
  cl.bind_compute_descriptor_sets(capture_pipeline_layout_, 0, 1, &frame_set);
  cl.bind_compute_descriptor_sets(capture_pipeline_layout_, 1, 1, &capture_set_);
  (void)params;

  u32 size = desc_.environment_size;
  for (u32 mip = 0; mip < desc_.environment_mips; ++mip) {
    struct {
      vec4 params;
    } pc{vec4(static_cast<f32>(size), static_cast<f32>(mip), 32.0f, 0.0f)};
    cl.push_constants(capture_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &pc);

    cl.image_barrier(environment_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1);

    const u32 groups = (size + 7) / 8;
    cl.dispatch(groups, groups, 6);

    cl.memory_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    size = std::max(1u, size / 2);
  }
  return true;
}

bool IBLBaker::run_irradiance(VkCommandBuffer cmd) {
  CommandList cl(cmd);
  cl.bind_compute(p_irradiance_);
  cl.bind_compute_descriptor_sets(source_pipeline_layout_, 1, 1, &irradiance_set_);

  struct {
    vec4 params;
  } pc{vec4(static_cast<f32>(desc_.irradiance_size), static_cast<f32>(desc_.environment_size),
            0.05f, 0.0f)};
  cl.push_constants(source_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &pc);

  cl.image_barrier(irradiance_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  const u32 groups = (desc_.irradiance_size + 7) / 8;
  cl.dispatch(groups, groups, 6);
  return true;
}

bool IBLBaker::run_prefilter(VkCommandBuffer cmd) {
  CommandList cl(cmd);
  cl.bind_compute(p_prefilter_);
  cl.bind_compute_descriptor_sets(source_pipeline_layout_, 1, 1, &prefilter_set_);

  u32 size = desc_.prefilter_size;
  const u32 mips = desc_.environment_mips;
  for (u32 mip = 0; mip < mips; ++mip) {
    const f32 roughness = static_cast<f32>(mip) / static_cast<f32>(mips - 1);
    struct {
      vec4 params;
      vec4 extra;
    } pc{vec4(static_cast<f32>(size), roughness, static_cast<f32>(desc_.prefilter_samples),
              static_cast<f32>(mip)),
         vec4(static_cast<f32>(desc_.environment_size), 0.0f, 0.0f, 0.0f)};
    cl.push_constants(source_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &pc);

    cl.image_barrier(prefiltered_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mip, 1);

    const u32 groups = (size + 7) / 8;
    cl.dispatch(groups, groups, 6);
    size = std::max(1u, size / 2);
  }
  return true;
}

bool IBLBaker::run_brdf_lut(VkCommandBuffer cmd) {
  CommandList cl(cmd);
  cl.bind_compute(p_brdf_);
  cl.bind_compute_descriptor_sets(source_pipeline_layout_, 1, 1, &lut_set_);

  struct {
    vec4 params;
  } pc{vec4(static_cast<f32>(desc_.brdf_lut_size), 0.0f, 0.0f, 0.0f)};
  cl.push_constants(source_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), &pc);

  cl.image_barrier(brdf_lut_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  const u32 groups = (desc_.brdf_lut_size + 7) / 8;
  cl.dispatch(groups, groups, 1);
  return true;
}

bool IBLBaker::bake(VkDescriptorSet frame_set, const IBLBakeParams& params) {
  if (!pipelines_built_ || pool_cmd_ == VK_NULL_HANDLE || frame_set == VK_NULL_HANDLE) {
    return false;
  }

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool_cmd_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  NEO_VK_CHECK(vkAllocateCommandBuffers(ctx_->device(), &ai, &cmd));

  CommandList cl(cmd);
  cl.label_begin(*ctx_, "IBL Bake", vec4(0.4f, 0.9f, 0.6f, 1.0f));
  cl.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

  run_capture(cmd, frame_set, params);
  run_irradiance(cmd);
  run_prefilter(cmd);
  run_brdf_lut(cmd);

  // Hand everything to the fragment stage as sampled cubemaps.
  for (IBLTexture* t : {&environment_, &irradiance_, &prefiltered_}) {
    cl.image_barrier(t->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS);
  }
  cl.image_barrier(brdf_lut_.image, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

  cl.end();

  // Bakes happen at load, so a blocking submit is correct and keeps the API
  // simple. A frame-time-aware async bake is an M4 item.
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(ctx_->device(), &fci, nullptr, &fence);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(ctx_->graphics_queue(), 1, &si, fence);
  const VkResult r = vkWaitForFences(ctx_->device(), 1, &fence, VK_TRUE, 5'000'000'000ull);
  vkDestroyFence(ctx_->device(), fence, nullptr);
  vkFreeCommandBuffers(ctx_->device(), pool_cmd_, 1, &cmd);

  cl.label_end(*ctx_);

  if (r != VK_SUCCESS) {
    NEO_ERROR(kRender, "IBL bake timed out or failed: {}", vk_result_name(r));
    return false;
  }

  last_params_ = params;
  ready_ = true;
  NEO_INFO(kRender, "IBL baked (sun elevation driven), env {}^2", desc_.environment_size);
  return true;
}

u64 IBLBaker::vram_bytes() const {
  u64 total = 0;
  for (const IBLTexture* t : {&environment_, &irradiance_, &prefiltered_, &brdf_lut_}) {
    const u64 bpp = (t->format == kLutFormat) ? 4 : 8;
    u64 px = static_cast<u64>(t->size) * t->size * t->layers;
    // Mipmapped cubes are ~4/3 of the base level.
    if (t->mips > 1)
      px = px * 4 / 3;
    total += px * bpp;
  }
  return total;
}

} // namespace neo
