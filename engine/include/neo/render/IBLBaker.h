// ===========================================================================
//  IBLBaker — image-based lighting from the procedural sky.
//
//  Three-stage bake, run once at startup (and again when the sun moves far
//  enough to matter, or the artist asks):
//
//    1. SkyCapture   evaluate the atmosphere for every direction of a 6-face
//                    cubemap  ->  mipmapped environment cube (256^2, 6 mips)
//    2. Irradiance   Lambertian cosine convolution -> 32^2 cube (diffuse IBL)
//    3. Prefilter    GGX importance-sampled convolution per mip -> 128^2, 6 mips
//                    (specular IBL, one mip per roughness level)
//
//  A 256x256 RGBA16F BRDF LUT (split-sum) is baked separately and once.
//
//  Total cost on a mid-range phone: ~35 ms, one time. Re-baking every frame
//  would cost that again, which is why it does not happen.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"
#include "neo/gfx/VulkanContext.h"

#include <vk_mem_alloc.h>

#include <memory>
#include <string>

namespace neo {

class GpuAllocator;
class PipelineCache;
class ResourceRegistry;

struct IBLDesc {
  u32 environment_size = 256;
  u32 environment_mips = 6;
  u32 irradiance_size = 32;
  u32 prefilter_size = 128;
  u32 brdf_lut_size = 256;
  u32 prefilter_samples = 512;
};

// The sun/atmosphere state the bake depends on. A change in any of these
// invalidates the environment; the renderer compares it against the last bake.
struct IBLBakeParams {
  vec3 sun_direction{0.0f, 1.0f, 0.0f};
  vec3 sun_color{1.0f};
  f32 sun_intensity = 110000.0f;
  f32 angular_radius = 0.0047f;
  f32 turbidity = 2.5f;

  [[nodiscard]] bool operator==(const IBLBakeParams& o) const {
    return glm::length2(sun_direction - o.sun_direction) < 1e-6f &&
           glm::length2(sun_color - o.sun_color) < 1e-6f &&
           std::abs(sun_intensity - o.sun_intensity) < 1.0f &&
           std::abs(turbidity - o.turbidity) < 1e-3f;
  }
};

struct IBLTexture {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
  u32 size = 0;
  u32 mips = 1;
  u32 layers = 6;
};

class IBLBaker {
public:
  static std::unique_ptr<IBLBaker> create(VulkanContext& ctx, PipelineCache& pipelines,
                                          const IBLDesc& desc);
  ~IBLBaker();

  IBLBaker(const IBLBaker&) = delete;
  IBLBaker& operator=(const IBLBaker&) = delete;

  // Builds the shared pipeline layouts and pipelines. Requires the frame
  // descriptor set layout (the bake passes read sun constants from it).
  bool initialize(VkDescriptorSetLayout frame_layout, VkCommandPool pool, VkQueue queue);

  // True once pipelines and textures exist. Until then descriptor_layout() is
  // null, so the renderer must not build dependent pipeline layouts.
  [[nodiscard]] bool initialized() const { return pipelines_built_ && pool_ != VK_NULL_HANDLE; }

  // Runs every bake stage and blocks until the GPU finishes. Allocates its own
  // command buffer (a bake must never interleave with frame recording).
  // `frame_set` must hold the sun and atmosphere constants for this bake.
  bool bake(VkDescriptorSet frame_set, const IBLBakeParams& params);

  [[nodiscard]] const IBLTexture& environment() const { return environment_; }
  [[nodiscard]] const IBLTexture& irradiance() const { return irradiance_; }
  [[nodiscard]] const IBLTexture& prefiltered() const { return prefiltered_; }
  [[nodiscard]] const IBLTexture& brdf_lut() const { return brdf_lut_; }

  [[nodiscard]] bool ready() const { return ready_; }
  [[nodiscard]] const IBLBakeParams& last_params() const { return last_params_; }
  [[nodiscard]] u64 vram_bytes() const;

  // Descriptor set holding irradiance (0), prefiltered (1) and BRDF LUT (2),
  // which is exactly what lighting.frag expects at set 2.
  [[nodiscard]] VkDescriptorSet descriptor_set() const { return ibl_set_; }
  [[nodiscard]] VkDescriptorSetLayout descriptor_layout() const { return ibl_layout_; }

private:
  IBLBaker() = default;

  bool create_textures();
  bool create_pipelines();
  void destroy_textures();

  bool run_capture(VkCommandBuffer cmd, VkDescriptorSet frame_set, const IBLBakeParams& params);
  bool run_irradiance(VkCommandBuffer cmd);
  bool run_prefilter(VkCommandBuffer cmd);
  bool run_brdf_lut(VkCommandBuffer cmd);
  bool build_descriptor_set();

  VulkanContext* ctx_ = nullptr;
  GpuAllocator* alloc_ = nullptr;
  PipelineCache* pipelines_ = nullptr;
  IBLDesc desc_{};

  IBLTexture environment_{};
  IBLTexture irradiance_{};
  IBLTexture prefiltered_{};
  IBLTexture brdf_lut_{};

  VkDescriptorSetLayout capture_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout source_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout capture_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout source_pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet capture_set_ = VK_NULL_HANDLE;
  VkDescriptorSet irradiance_set_ = VK_NULL_HANDLE;
  VkDescriptorSet prefilter_set_ = VK_NULL_HANDLE;
  VkDescriptorSet lut_set_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;

  VkPipeline p_capture_ = VK_NULL_HANDLE;
  VkPipeline p_irradiance_ = VK_NULL_HANDLE;
  VkPipeline p_prefilter_ = VK_NULL_HANDLE;
  VkPipeline p_brdf_ = VK_NULL_HANDLE;

  VkCommandPool pool_cmd_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ibl_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet ibl_set_ = VK_NULL_HANDLE;

  IBLBakeParams last_params_{};
  bool ready_ = false;
  bool pipelines_built_ = false;
};

} // namespace neo
