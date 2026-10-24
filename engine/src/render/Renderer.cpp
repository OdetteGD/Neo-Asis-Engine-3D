#include "neo/render/Renderer.h"

#include "neo/core/AssetSystem.h"
#include "neo/core/Log.h"
#include "neo/gfx/CommandList.h"
#include "neo/gfx/GpuAllocator.h"
#include "neo/gfx/PipelineCache.h"
#include "neo/render/FrameData.h"
#include "neo/render/GltfLoader.h"
#include "neo/render/IBLBaker.h"
#include "neo/render/Resources.h"
#include "neo/scene/Scene.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace neo {

namespace {

constexpr u32 kFramesInFlight = 2; // 2 is the mobile latency/power sweet spot
constexpr u32 kBloomMipCount = 6;

// Render target formats. Chosen for bandwidth: the G-buffer packs normals into
// two 16-bit channels instead of a full RGBA16F.
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;      // rgb + AO
constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // oct normal + rough + metal
constexpr VkFormat kEmissiveFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kCloudFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VkExtent2D extent{0, 0};
  VkFormat format = VK_FORMAT_UNDEFINED;
  u32 mips = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct PerFrame {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore image_available = VK_NULL_HANDLE;
  VkSemaphore render_finished = VK_NULL_HANDLE;
  VkFence in_flight = VK_NULL_HANDLE;

  VkBuffer frame_ubo = VK_NULL_HANDLE;
  VmaAllocation frame_ubo_alloc = VK_NULL_HANDLE;
  void* frame_ubo_mapped = nullptr;

  VkBuffer light_buffer = VK_NULL_HANDLE;
  VmaAllocation light_alloc = VK_NULL_HANDLE;
  void* light_mapped = nullptr;
  u32 light_capacity = 0;

  VkDescriptorSet frame_set = VK_NULL_HANDLE;

  FrameConstants constants{};
};

} // namespace

// ===========================================================================
//  Impl
// ===========================================================================
struct Renderer::Impl {
  VulkanContext* ctx = nullptr;
  GpuAllocator* alloc = nullptr;
  PipelineCache* pipelines = nullptr;

  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;        // set 2: G-buffer inputs
  VkDescriptorSetLayout post_layout = VK_NULL_HANDLE;         // set 2: post inputs
  VkPipelineLayout frame_pipeline_layout = VK_NULL_HANDLE;    // set 0 + bindless
  VkPipelineLayout lighting_pipeline_layout = VK_NULL_HANDLE; // sets 0,2,3
  VkPipelineLayout post_pipeline_layout = VK_NULL_HANDLE;     // sets 0,2
  VkPipelineLayout sky_pipeline_layout = VK_NULL_HANDLE;      // set 0 only
  VkDescriptorSet scene_set = VK_NULL_HANDLE;
  VkDescriptorSet post_set = VK_NULL_HANDLE;
  VkDescriptorSet cloud_set = VK_NULL_HANDLE;
  VkDescriptorSet cluster_set = VK_NULL_HANDLE;
  VkDescriptorSetLayout cloud_layout = VK_NULL_HANDLE;
  VkPipelineLayout cloud_pipeline_layout = VK_NULL_HANDLE;

  std::unique_ptr<ResourceRegistry> registry;
  std::unique_ptr<IBLBaker> ibl;
  IBLBakeParams last_bake{};
  bool ibl_pending = true;
  bool geometry_uploaded = false;

  std::array<PerFrame, kFramesInFlight> frames{};
  u32 frame_index = 0;
  u64 frame_counter = 0;
  u32 current_image = 0;

  // Targets
  Image depth{};
  Image gbuffer_albedo{};
  Image gbuffer_normal{};
  Image gbuffer_emissive{};
  Image hdr{};
  Image cloud{};
  Image bloom{}; // array of mips via views
  std::array<VkImageView, kBloomMipCount> bloom_views{};

  // Pipelines
  VkPipeline p_gbuffer = VK_NULL_HANDLE;
  VkPipeline p_shadow = VK_NULL_HANDLE;
  VkPipeline p_sky = VK_NULL_HANDLE;
  VkPipeline p_lighting = VK_NULL_HANDLE;
  VkPipeline p_bloom_down = VK_NULL_HANDLE;
  VkPipeline p_bloom_up = VK_NULL_HANDLE;
  VkPipeline p_tonemap = VK_NULL_HANDLE;
  VkPipeline p_clouds = VK_NULL_HANDLE;
  VkPipeline p_cluster_assign = VK_NULL_HANDLE;

  // Clustered lighting buffers
  static constexpr u32 kClusterGridX = 16;
  static constexpr u32 kClusterGridY = 9;
  static constexpr u32 kClusterGridZ = 24;
  static constexpr u32 kMaxLightsPerCluster = 32;
  VkBuffer cluster_counts = VK_NULL_HANDLE;
  VmaAllocation cluster_counts_alloc = VK_NULL_HANDLE;
  VkBuffer cluster_indices = VK_NULL_HANDLE;
  VmaAllocation cluster_indices_alloc = VK_NULL_HANDLE;
  VkDescriptorSetLayout cluster_layout = VK_NULL_HANDLE;
  VkPipelineLayout cluster_pipeline_layout = VK_NULL_HANDLE;
  VkSampler linear_sampler = VK_NULL_HANDLE;

  // Shadow atlas (single 2D image, cascades as tiles)
  Image shadow{};
  u32 shadow_cascade_count = 3;
  u32 shadow_size = 1024;

  f64 last_frame_time = 0.0;
  f32 smoothed_gpu_ms = 16.6f;
  f32 render_scale = 1.0f;
  Scene::Visibility visibility{};
  std::vector<GpuLight> packed_lights;

  // --- Resource helpers ---------------------------------------------------
  bool create_image(Image& out, const char* name, VkFormat format, u32 w, u32 h,
                    VkImageUsageFlags usage, u32 mips = 1) {
    ImageDesc desc{};
    desc.name = name;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.mip_levels = mips;
    desc.usage = usage;
    desc.memory = MemoryClass::DeviceLocal;

    if (!alloc->create_image(desc, out.image, out.alloc))
      return false;
    out.format = format;
    out.extent = {w, h};
    out.mips = mips;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    NEO_VK_CHECK(vkCreateImageView(ctx->device(), &vci, nullptr, &out.view));
    ctx->set_object_name(reinterpret_cast<u64>(out.image), VK_OBJECT_TYPE_IMAGE, name);
    return true;
  }

  void destroy_image(Image& img) {
    if (img.view)
      vkDestroyImageView(ctx->device(), img.view, nullptr);
    if (img.image)
      alloc->destroy_image(img.image, img.alloc);
    img = Image{};
  }

  bool create_targets(u32 w, u32 h, const QualitySettings& q) {
    destroy_targets();
    if (w == 0 || h == 0)
      return false;

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!create_image(depth, "Depth", kDepthFormat, w, h,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled)) {
      return false;
    }
    // Depth views need the depth aspect.
    vkDestroyImageView(ctx->device(), depth.view, nullptr);
    VkImageViewCreateInfo dvci{};
    dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvci.image = depth.image;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = kDepthFormat;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    NEO_VK_CHECK(vkCreateImageView(ctx->device(), &dvci, nullptr, &depth.view));

    create_image(gbuffer_albedo, "GBufferAlbedo", kAlbedoFormat, w, h,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled);
    create_image(gbuffer_normal, "GBufferNormal", kNormalFormat, w, h,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled);
    create_image(gbuffer_emissive, "GBufferEmissive", kEmissiveFormat, w, h,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled);
    create_image(hdr, "HDR", kHdrFormat, w, h,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    if (q.volumetric_clouds) {
      const u32 cw = std::max(1u, static_cast<u32>(static_cast<f32>(w) * q.cloud_render_scale));
      const u32 ch = std::max(1u, static_cast<u32>(static_cast<f32>(h) * q.cloud_render_scale));
      create_image(cloud, "Clouds", kCloudFormat, cw, ch,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled | VK_IMAGE_USAGE_STORAGE_BIT);
    }

    if (q.bloom) {
      const u32 bw = std::max(1u, w / 2), bh = std::max(1u, h / 2);
      create_image(bloom, "Bloom", kHdrFormat, bw, bh,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, kBloomMipCount);
      for (u32 i = 0; i < kBloomMipCount; ++i) {
        VkImageViewCreateInfo bvci{};
        bvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        bvci.image = bloom.image;
        bvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        bvci.format = kHdrFormat;
        bvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
        NEO_VK_CHECK(vkCreateImageView(ctx->device(), &bvci, nullptr, &bloom_views[i]));
      }
    }

    // Shadow atlas: cascades laid out horizontally.
    const u32 atlas_w = q.shadow_map_size * q.shadow_cascade_count;
    const u32 atlas_h = q.shadow_map_size;
    if (atlas_w != shadow.extent.width || atlas_h != shadow.extent.height) {
      destroy_image(shadow);
      create_image(shadow, "ShadowAtlas", VK_FORMAT_D32_SFLOAT, atlas_w, atlas_h,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled);
      vkDestroyImageView(ctx->device(), shadow.view, nullptr);
      VkImageViewCreateInfo svci{};
      svci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      svci.image = shadow.image;
      svci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      svci.format = VK_FORMAT_D32_SFLOAT;
      svci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      NEO_VK_CHECK(vkCreateImageView(ctx->device(), &svci, nullptr, &shadow.view));
    }
    shadow_cascade_count = q.shadow_cascade_count;
    shadow_size = q.shadow_map_size;

    // Target views changed, so every binding that references them must be
    // rewritten. Sets themselves are reused (views are updated in place).
    write_pass_sets();
    return true;
  }

  void destroy_targets() {
    destroy_image(depth);
    destroy_image(gbuffer_albedo);
    destroy_image(gbuffer_normal);
    destroy_image(gbuffer_emissive);
    destroy_image(hdr);
    destroy_image(cloud);
    destroy_image(bloom);
    for (VkImageView& v : bloom_views) {
      if (v)
        vkDestroyImageView(ctx->device(), v, nullptr);
      v = VK_NULL_HANDLE;
    }
  }

  // --- Cascade fitting ----------------------------------------------------
  // Practical split scheme (logarithmic blended with uniform), then a
  // sphere-fitted orthographic light frustum per cascade with texel snapping
  // so the shadow map does not shimmer while the camera moves.
  void update_cascades(const Camera& cam, const vec3& sun_dir, FrameConstants& c) {
    const u32 count = std::max(1u, std::min(shadow_cascade_count, kMaxShadowCascades));
    const f32 near_z = std::max(cam.desc().near_plane, 0.05f);
    const f32 far_z = std::min(cam.desc().far_plane, 260.0f);
    const f32 lambda = 0.72f;

    const mat4 inv_proj = glm::inverse(cam.projection());
    const mat4 cam_view = cam.view();
    const f32 map_size = static_cast<f32>(shadow_size);

    f32 split_prev = near_z;
    for (u32 i = 0; i < count; ++i) {
      const f32 p = static_cast<f32>(i + 1) / static_cast<f32>(count);
      const f32 log_split = near_z * std::pow(far_z / near_z, p);
      const f32 uniform_split = near_z + (far_z - near_z) * p;
      const f32 split = lambda * log_split + (1.0f - lambda) * uniform_split;

      // Eight frustum corners in view space for this slice.
      vec3 corners[8];
      u32 k = 0;
      for (u32 side = 0; side < 2; ++side) {
        const f32 depth = (side == 0) ? split_prev : split;
        for (u32 y = 0; y < 2; ++y) {
          for (u32 x = 0; x < 2; ++x) {
            const vec4 ndc((x == 0) ? -1.0f : 1.0f, (y == 0) ? -1.0f : 1.0f, depth, 1.0f);
            vec4 v = inv_proj * ndc;
            corners[k++] = vec3(v) / v.w;
          }
        }
      }

      vec3 center(0.0f);
      for (const vec3& pnt : corners)
        center += pnt;
      center /= 8.0f;

      f32 radius = 0.0f;
      for (const vec3& pnt : corners)
        radius = std::max(radius, glm::length(pnt - center));
      radius = std::ceil(radius * 16.0f) / 16.0f;

      // Light-space basis, looking along -sun_dir.
      const vec3 light_dir = glm::normalize(sun_dir);
      vec3 up = (std::abs(light_dir.y) > 0.95f) ? vec3(1, 0, 0) : vec3(0, 1, 0);
      const vec3 right = glm::normalize(glm::cross(up, light_dir));
      up = glm::cross(light_dir, right);

      // Texel snap: quantise the center in light space to whole shadow texels.
      const f32 texel_world = (2.0f * radius) / map_size;
      const vec3 light_center(cam_view * vec4(center, 1.0f));
      (void)light_center;
      vec3 snapped = center;
      snapped.x = std::floor(snapped.x / texel_world) * texel_world;
      snapped.y = std::floor(snapped.y / texel_world) * texel_world;

      const vec3 eye = snapped - light_dir * (radius * 2.0f + 10.0f);
      const mat4 light_view = look_at_rh(eye, snapped, up);
      const mat4 light_proj =
          orthographic_zo(-radius, radius, -radius, radius, 0.05f, radius * 4.0f + 20.0f);

      c.shadow_matrices[i] = light_proj * light_view;
      split_prev = split;
    }
    c.shadow_params = vec4(static_cast<f32>(count), map_size, 0.0009f, 1.4f);
  }

  // --- Descriptor sets for the pass inputs --------------------------------
  void write_pass_sets() {
    if (depth.view == VK_NULL_HANDLE)
      return;
    const VkDevice dev = ctx->device();

    if (linear_sampler == VK_NULL_HANDLE) {
      VkSamplerCreateInfo sci{};
      sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      sci.magFilter = VK_FILTER_LINEAR;
      sci.minFilter = VK_FILTER_LINEAR;
      sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sci.maxLod = 1.0f;
      NEO_VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &linear_sampler));
    }

    auto alloc = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) {
      if (set != VK_NULL_HANDLE)
        return true;
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = descriptor_pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &layout;
      return vkAllocateDescriptorSets(dev, &ai, &set) == VK_SUCCESS;
    };

    auto write_sampler = [&](VkDescriptorSet set, u32 binding, VkImageView view) {
      VkDescriptorImageInfo info{};
      info.sampler = linear_sampler;
      info.imageView = view;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = set;
      w.dstBinding = binding;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &info;
      vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    };

    if (!alloc(scene_layout, scene_set) || !alloc(post_layout, post_set))
      return;

    const VkImageView cloud_view = (cloud.view != VK_NULL_HANDLE) ? cloud.view : hdr.view;
    write_sampler(scene_set, 0, gbuffer_albedo.view);
    write_sampler(scene_set, 1, gbuffer_normal.view);
    write_sampler(scene_set, 2, gbuffer_emissive.view);
    write_sampler(scene_set, 3, depth.view);
    write_sampler(scene_set, 4, cloud_view);

    const VkImageView bloom_view = (bloom_views[0] != VK_NULL_HANDLE) ? bloom_views[0] : hdr.view;
    write_sampler(post_set, 0, hdr.view);
    write_sampler(post_set, 1, bloom_view);
    write_sampler(post_set, 2, depth.view);

    // Clustered light lists.
    VkDescriptorBufferInfo count_buf{cluster_counts, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo index_buf{cluster_indices, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet cluster_writes[2]{};
    cluster_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cluster_writes[0].dstSet = scene_set;
    cluster_writes[0].dstBinding = 5;
    cluster_writes[0].descriptorCount = 1;
    cluster_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cluster_writes[0].pBufferInfo = &count_buf;
    cluster_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cluster_writes[1].dstSet = scene_set;
    cluster_writes[1].dstBinding = 6;
    cluster_writes[1].descriptorCount = 1;
    cluster_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cluster_writes[1].pBufferInfo = &index_buf;
    vkUpdateDescriptorSets(dev, 2, cluster_writes, 0, nullptr);

    // Cloud output (storage image, always written in GENERAL layout).
    if (cloud.view != VK_NULL_HANDLE && alloc(cloud_layout, cloud_set)) {
      VkDescriptorImageInfo info{};
      info.imageView = cloud.view;
      info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = cloud_set;
      w.dstBinding = 0;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w.pImageInfo = &info;
      vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
  }

  // --- Pipelines ----------------------------------------------------------
  bool create_pipelines(const QualitySettings& q) {
    // set 0: frame constants (UBO) + lights (SSBO), shared by every pass.
    if (frame_layout == VK_NULL_HANDLE) {
      frame_layout = pipelines->create_set_layout({
          {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
          // 2: packed vertex arena, read by gbuffer.vert/shadow.vert as
          //    `VertexBlock`. Without this binding every draw reads garbage.
          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
      });
    }

    // set 2 for deferred lighting: G-buffer + depth + clouds.
    if (scene_layout == VK_NULL_HANDLE) {
      std::vector<VkDescriptorSetLayoutBinding> b;
      for (u32 i = 0; i < 5; ++i) {
        b.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                     nullptr});
      }
      scene_layout = pipelines->create_set_layout(b);
    }

    // Cloud compute: frame constants + its own output image.
    if (cloud_layout == VK_NULL_HANDLE) {
      cloud_layout = pipelines->create_set_layout({
          {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      });
      cloud_pipeline_layout =
          pipelines->create_layout({frame_layout, cloud_layout}, 64, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // Cluster assignment: two output SSBOs.
    if (cluster_layout == VK_NULL_HANDLE) {
      cluster_layout = pipelines->create_set_layout({
          {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      });
      cluster_pipeline_layout =
          pipelines->create_layout({frame_layout, cluster_layout}, 64, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // The IBL baker needs the frame layout before the lighting layout can
    // include its descriptor set.
    if (ibl && !ibl->initialized()) {
      ibl->initialize(frame_layout, frames[0].pool, ctx->graphics_queue());
      ibl_pending = true;
    }

    // set 2 for post: HDR colour, bloom, depth.
    if (post_layout == VK_NULL_HANDLE) {
      std::vector<VkDescriptorSetLayoutBinding> b;
      for (u32 i = 0; i < 3; ++i) {
        b.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                     nullptr});
      }
      post_layout = pipelines->create_set_layout(b);
    }

    // Geometry passes: frame constants + the bindless texture array.
    frame_pipeline_layout =
        pipelines->create_layout({frame_layout, registry->bindless_layout()}, sizeof(DrawConstants),
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    // Lighting: frame + scene inputs + baked IBL.
    lighting_pipeline_layout = pipelines->create_layout(
        {frame_layout, scene_layout, ibl->descriptor_layout()}, 64,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);

    // Post: frame + post inputs.
    post_pipeline_layout = pipelines->create_layout(
        {frame_layout, post_layout}, 64, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    // Sky: frame constants only.
    sky_pipeline_layout = pipelines->create_layout(
        {frame_layout}, 64, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    // --- Shadow pass -------------------------------------------------------
    VkShaderModule shadow_vs = pipelines->load("passes/shadow.vert");
    VkShaderModule shadow_fs = pipelines->load("passes/shadow.frag");
    if (shadow_vs && shadow_fs) {
      GraphicsPipelineDesc d{};
      d.name = "shadow";
      d.vert = shadow_vs;
      d.frag = shadow_fs;
      d.layout = frame_pipeline_layout;
      d.depth_format = VK_FORMAT_D32_SFLOAT;
      d.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
      d.depth_write = true;
      d.cull = VK_CULL_MODE_BACK_BIT;
      d.push_constant_size = sizeof(DrawConstants);
      p_shadow = pipelines->create(d);
    }

    // --- G-buffer ----------------------------------------------------------
    VkShaderModule gbuf_vs = pipelines->load("passes/gbuffer.vert");
    VkShaderModule gbuf_fs = pipelines->load("passes/gbuffer.frag");
    if (gbuf_vs && gbuf_fs) {
      GraphicsPipelineDesc d{};
      d.name = "gbuffer";
      d.vert = gbuf_vs;
      d.frag = gbuf_fs;
      d.layout = frame_pipeline_layout;
      d.color_formats = {kAlbedoFormat, kNormalFormat, kEmissiveFormat};
      d.depth_format = kDepthFormat;
      d.depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL; // reverse-Z
      d.depth_write = true;
      d.samples = q.msaa;
      d.push_constant_size = sizeof(DrawConstants);
      p_gbuffer = pipelines->create(d);
    }

    // --- Fullscreen passes -------------------------------------------------
    VkShaderModule fs_vs = pipelines->load("passes/fullscreen.vert");
    auto make_fs = [&](VkShaderModule fs, const char* name, VkPipeline* out,
                       VkPipelineLayout layout, std::vector<VkFormat> formats) {
      if (!fs || !fs_vs)
        return;
      GraphicsPipelineDesc d{};
      d.name = name;
      d.vert = fs_vs;
      d.frag = fs;
      d.layout = layout;
      d.color_formats = std::move(formats);
      d.depth_format = VK_FORMAT_UNDEFINED;
      d.depth_test = false;
      d.depth_write = false;
      d.cull = VK_CULL_MODE_NONE;
      d.push_constant_size = 64;
      *out = pipelines->create(d);
    };

    if (fs_vs) {
      make_fs(pipelines->load("passes/sky.frag"), "sky", &p_sky, sky_pipeline_layout, {kHdrFormat});
      make_fs(pipelines->load("passes/lighting.frag"), "lighting", &p_lighting,
              lighting_pipeline_layout, {kHdrFormat});
      make_fs(pipelines->load("fb/tonemap.frag"), "tonemap", &p_tonemap, post_pipeline_layout, {});
      if (q.bloom) {
        make_fs(pipelines->load("fb/bloom_down.frag"), "bloom_down", &p_bloom_down,
                post_pipeline_layout, {kHdrFormat});
        make_fs(pipelines->load("fb/bloom_up.frag"), "bloom_up", &p_bloom_up, post_pipeline_layout,
                {kHdrFormat});
      }
      VkShaderModule cluster_cs = pipelines->load("passes/cluster_assign.comp");
      if (cluster_cs) {
        p_cluster_assign = pipelines->create(
            ComputePipelineDesc{"cluster_assign", cluster_cs, cluster_pipeline_layout});
      }

      VkShaderModule cloud_cs = pipelines->load("passes/clouds.comp");
      if (cloud_cs) {
        p_clouds =
            pipelines->create(ComputePipelineDesc{"clouds", cloud_cs, cloud_pipeline_layout});
      }
    }

    return p_gbuffer != VK_NULL_HANDLE && p_lighting != VK_NULL_HANDLE &&
           p_tonemap != VK_NULL_HANDLE;
  }
};

// ===========================================================================
//  Quality
// ===========================================================================
QualitySettings QualitySettings::for_preset(QualityPreset p) {
  QualitySettings q{};
  q.preset = p;
  switch (p) {
  case QualityPreset::Low:
    q.msaa = VK_SAMPLE_COUNT_1_BIT;
    q.shadow_cascade_count = 2;
    q.shadow_map_size = 512;
    q.volumetric_clouds = false;
    q.cloud_steps = 24;
    q.cloud_render_scale = 0.25f;
    q.bloom = true;
    q.fxaa = true;
    q.taa = false;
    q.dynamic_resolution = true;
    q.render_scale = 0.75f;
    q.target_frame_ms = 33.3f; // 30 fps floor
    break;
  case QualityPreset::Medium:
    q.msaa = VK_SAMPLE_COUNT_1_BIT;
    q.shadow_cascade_count = 3;
    q.shadow_map_size = 1024;
    q.volumetric_clouds = true;
    q.cloud_steps = 32;
    q.cloud_render_scale = 0.35f;
    q.dynamic_resolution = true;
    q.render_scale = 1.0f;
    q.target_frame_ms = 16.6f;
    break;
  case QualityPreset::High:
    q.msaa = VK_SAMPLE_COUNT_2_BIT;
    q.shadow_cascade_count = 4;
    q.shadow_map_size = 1536;
    q.cloud_steps = 48;
    q.cloud_render_scale = 0.5f;
    q.dynamic_resolution = true;
    q.taa = true;
    q.fxaa = false;
    break;
  case QualityPreset::Ultra:
    q.msaa = VK_SAMPLE_COUNT_4_BIT;
    q.shadow_cascade_count = 4;
    q.shadow_map_size = 2048;
    q.cloud_steps = 72;
    q.cloud_render_scale = 0.5f;
    q.dynamic_resolution = false;
    q.taa = true;
    q.fxaa = false;
    break;
  }
  return q;
}

QualityPreset Renderer::detect_tier(const VkPhysicalDeviceProperties& props) {
  // Heuristic on the two numbers that actually predict mobile fill-rate:
  // maxComputeWorkGroupInvocations (proxy for shader core count) and the
  // storage buffer range (proxy for addressable memory / modern driver).
  const f32 score = static_cast<f32>(props.limits.maxComputeWorkGroupInvocations) *
                    (props.apiVersion >= VK_API_VERSION_1_2 ? 1.25f : 1.0f);

  if (score >= 1400.0f)
    return QualityPreset::Ultra;
  if (score >= 640.0f)
    return QualityPreset::High;
  if (score >= 384.0f)
    return QualityPreset::Medium;
  return QualityPreset::Low;
}

// ===========================================================================
//  Lifetime
// ===========================================================================
std::unique_ptr<Renderer> Renderer::create(const RendererDesc& desc) {
  auto r = std::unique_ptr<Renderer>(new Renderer());
  r->impl_ = std::make_unique<Impl>();
  Impl& I = *r->impl_;

  DeviceDesc dd{};
  dd.app_name = desc.app_name;
  dd.enable_validation = desc.enable_validation;
  r->ctx_ = VulkanContext::create(dd);
  if (!r->ctx_) {
    NEO_ERROR(kRender, "Vulkan device creation failed — this GPU cannot run the engine");
    return nullptr; // null, not abort: the app shows a message and exits
  }

  // The renderer hard-depends on dynamic rendering and synchronization2. If
  // they are unavailable we must not create a single pipeline: calling
  // vkCmdBeginRendering without the enabled feature is a driver fault, which
  // is exactly the crash-on-launch this replaced. Returning null makes the
  // caller back out cleanly.
  if (!r->ctx_->features().dynamic_rendering || !r->ctx_->features().synchronization2) {
    NEO_ERROR(kRender,
              "GPU lacks required features (dynamicRendering={}, synchronization2={}). "
              "Vulkan 1.1 + VK_KHR_dynamic_rendering + VK_KHR_synchronization2 are required.",
              r->ctx_->features().dynamic_rendering, r->ctx_->features().synchronization2);
    return nullptr;
  }
  I.ctx = r->ctx_.get();
  I.alloc = &r->ctx_->allocator();

  if (desc.native_window) {
    r->ctx_->create_surface(desc.native_display, desc.native_window);
  }

  SwapchainDesc sd{};
  sd.width = desc.width;
  sd.height = desc.height;
  sd.vsync = desc.vsync;
  r->swapchain_ = Swapchain::create(*r->ctx_, sd);
  if (!r->swapchain_) {
    NEO_ERROR(kRender, "Swapchain creation failed");
    return nullptr;
  }
  r->width_ = r->swapchain_->extent().width;
  r->height_ = r->swapchain_->extent().height;

  // Pick a starting preset from the hardware if the caller did not force MSAA.
  r->quality_ = QualitySettings::for_preset(detect_tier(r->ctx_->properties()));
  if (desc.msaa_samples != 0) {
    r->quality_.msaa = static_cast<VkSampleCountFlagBits>(desc.msaa_samples);
  }

  I.pipelines = new PipelineCache(*r->ctx_);
  I.pipelines->load_disk_cache("cache/pipeline.bin");

  // --- Per-frame objects ---------------------------------------------------
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    PerFrame& f = I.frames[i];

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = r->ctx_->graphics_family();
    NEO_VK_CHECK(vkCreateCommandPool(r->ctx_->device(), &pci, nullptr, &f.pool));

    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = f.pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    NEO_VK_CHECK(vkAllocateCommandBuffers(r->ctx_->device(), &cbi, &f.cmd));

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    NEO_VK_CHECK(vkCreateSemaphore(r->ctx_->device(), &sci, nullptr, &f.image_available));
    NEO_VK_CHECK(vkCreateSemaphore(r->ctx_->device(), &sci, nullptr, &f.render_finished));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    NEO_VK_CHECK(vkCreateFence(r->ctx_->device(), &fci, nullptr, &f.in_flight));

    BufferDesc ubd{};
    ubd.name = "FrameConstants";
    ubd.size = sizeof(FrameConstants);
    ubd.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    ubd.memory = MemoryClass::Upload;
    ubd.persistently_mapped = true;
    I.alloc->create_buffer(ubd, f.frame_ubo, f.frame_ubo_alloc, &f.frame_ubo_mapped);

    f.light_capacity = kMaxLights;
    BufferDesc lbd{};
    lbd.name = "Lights";
    lbd.size = sizeof(GpuLight) * kMaxLights;
    lbd.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    lbd.memory = MemoryClass::Upload;
    lbd.persistently_mapped = true;
    I.alloc->create_buffer(lbd, f.light_buffer, f.light_alloc, &f.light_mapped);
  }

  // --- Descriptor pool -----------------------------------------------------
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
  };
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 32;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = sizes;
  NEO_VK_CHECK(vkCreateDescriptorPool(r->ctx_->device(), &dpci, nullptr, &I.descriptor_pool));

  // --- Resource registry (geometry arenas + bindless textures) -----------
  I.registry = ResourceRegistry::create(*r->ctx_);
  if (!I.registry->initialize(I.frames[0].pool, r->ctx_->graphics_queue())) {
    NEO_ERROR(kRender, "ResourceRegistry init failed");
    return nullptr;
  }
  I.ibl = IBLBaker::create(*r->ctx_, *I.pipelines, IBLDesc{});

  // --- Cluster buffers ---------------------------------------------------
  {
    const u32 clusters = Impl::kClusterGridX * Impl::kClusterGridY * Impl::kClusterGridZ;
    BufferDesc cd{};
    cd.name = "ClusterCounts";
    cd.size = sizeof(u32) * clusters;
    cd.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    cd.memory = MemoryClass::DeviceLocal;
    I.alloc->create_buffer(cd, I.cluster_counts, I.cluster_counts_alloc);

    BufferDesc ci{};
    ci.name = "ClusterIndices";
    ci.size = sizeof(u32) * clusters * Impl::kMaxLightsPerCluster;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ci.memory = MemoryClass::DeviceLocal;
    I.alloc->create_buffer(ci, I.cluster_indices, I.cluster_indices_alloc);

    NEO_INFO(kRender, "Cluster grid {}x{}x{} ({} clusters, {} lights each)", Impl::kClusterGridX,
             Impl::kClusterGridY, Impl::kClusterGridZ, clusters, Impl::kMaxLightsPerCluster);
  }

  I.create_targets(r->width_, r->height_, r->quality_);
  if (!I.create_pipelines(r->quality_)) {
    NEO_WARN(kRender, "Some pipelines failed to build — shaders may not have been compiled");
  }

  // --- Per-frame descriptor sets -----------------------------------------
  for (PerFrame& f : I.frames) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = I.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &I.frame_layout;
    NEO_VK_CHECK(vkAllocateDescriptorSets(r->ctx_->device(), &ai, &f.frame_set));

    VkDescriptorBufferInfo ubo{f.frame_ubo, 0, sizeof(FrameConstants)};
    VkDescriptorBufferInfo ssbo{f.light_buffer, 0, sizeof(GpuLight) * kMaxLights};
    VkDescriptorBufferInfo vbuf{I.registry->vertex_buffer(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = f.frame_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = f.frame_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &ssbo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = f.frame_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &vbuf;
    vkUpdateDescriptorSets(r->ctx_->device(), 3, writes, 0, nullptr);
  }

  // --- Cluster descriptor set --------------------------------------------
  {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = I.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &I.cluster_layout;
    NEO_VK_CHECK(vkAllocateDescriptorSets(r->ctx_->device(), &ai, &I.cluster_set));

    VkDescriptorBufferInfo cb{I.cluster_counts, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo ib{I.cluster_indices, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]{};
    for (int k = 0; k < 2; ++k) {
      w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[k].dstSet = I.cluster_set;
      w[k].dstBinding = static_cast<u32>(k);
      w[k].descriptorCount = 1;
      w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[k].pBufferInfo = (k == 0) ? &cb : &ib;
    }
    vkUpdateDescriptorSets(r->ctx_->device(), 2, w, 0, nullptr);
  }

  I.write_pass_sets();

  NEO_INFO(kRender, "Renderer ready: {}x{}, preset {}, {} draw pipelines", r->width_, r->height_,
           static_cast<int>(r->quality_.preset), I.pipelines->pipeline_count());
  return r;
}

Renderer::~Renderer() {
  if (!impl_ || !ctx_)
    return;

  Impl& I = *impl_;
  vkDeviceWaitIdle(ctx_->device());
  const VkDevice dev = ctx_->device();

  I.pipelines->save_disk_cache("cache/pipeline.bin");
  I.destroy_targets();

  for (VkImageView v : I.bloom_views) {
    if (v)
      vkDestroyImageView(dev, v, nullptr);
  }
  if (I.shadow.view)
    vkDestroyImageView(dev, I.shadow.view, nullptr);
  if (I.shadow.image)
    I.alloc->destroy_image(I.shadow.image, I.shadow.alloc);

  for (PerFrame& f : I.frames) {
    if (f.frame_ubo)
      I.alloc->destroy_buffer(f.frame_ubo, f.frame_ubo_alloc);
    if (f.light_buffer)
      I.alloc->destroy_buffer(f.light_buffer, f.light_alloc);
    if (f.image_available)
      vkDestroySemaphore(dev, f.image_available, nullptr);
    if (f.render_finished)
      vkDestroySemaphore(dev, f.render_finished, nullptr);
    if (f.in_flight)
      vkDestroyFence(dev, f.in_flight, nullptr);
    if (f.pool)
      vkDestroyCommandPool(dev, f.pool, nullptr);
  }

  if (I.descriptor_pool)
    vkDestroyDescriptorPool(dev, I.descriptor_pool, nullptr);
  delete I.pipelines;
  impl_.reset();
}

// ===========================================================================
//  Frame
// ===========================================================================
bool Renderer::begin_frame() {
  Impl& I = *impl_;
  PerFrame& f = I.frames[I.frame_index];

  vkWaitForFences(ctx_->device(), 1, &f.in_flight, VK_TRUE, UINT64_MAX);

  if (!swapchain_->acquire(f.image_available, &I.current_image)) {
    if (swapchain_->needs_rebuild()) {
      swapchain_ = Swapchain::create(
          *ctx_, SwapchainDesc{width_, height_, quality_.preset != QualityPreset::Low});
      if (swapchain_) {
        width_ = swapchain_->extent().width;
        height_ = swapchain_->extent().height;
        I.create_targets(width_, height_, quality_);
      }
      return false;
    }
    return false;
  }

  vkResetFences(ctx_->device(), 1, &f.in_flight);
  vkResetCommandPool(ctx_->device(), f.pool, 0);

  CommandList cmd(f.cmd);
  cmd.begin();

  // --- Fill frame constants ------------------------------------------------
  FrameConstants& c = f.constants;
  c.timing = vec4(static_cast<f32>(I.frame_counter) / 60.0f, 1.0f / 60.0f,
                  static_cast<f32>(I.frame_counter), 0.05f);
  c.screen_size = vec4(static_cast<f32>(width_), static_cast<f32>(height_),
                       1.0f / static_cast<f32>(std::max(1u, width_)),
                       1.0f / static_cast<f32>(std::max(1u, height_)));
  c.post_params = vec4(I.render_scale, 0.18f, quality_.bloom_intensity,
                       static_cast<f32>(static_cast<u8>(quality_.preset)));
  c.shadow_params = vec4(static_cast<f32>(I.shadow_cascade_count), static_cast<f32>(I.shadow_size),
                         0.0008f, 1.5f);
  // Cluster grid: square-ish tiles over the render target.
  const f32 tiles_x = static_cast<f32>(Impl::kClusterGridX);
  const f32 tiles_y = static_cast<f32>(Impl::kClusterGridY);
  c.cluster_params = vec4(0.1f, 500.0f, tiles_x, tiles_y);
  c.cluster_dims =
      vec4(static_cast<f32>(Impl::kClusterGridZ), static_cast<f32>(width_) / tiles_x,
           static_cast<f32>(height_) / tiles_y, static_cast<f32>(Impl::kMaxLightsPerCluster));
  std::memcpy(f.frame_ubo_mapped, &c, sizeof(FrameConstants));
  return true;
}

void Renderer::render(Scene& scene, const Camera& camera, f32 /*delta_seconds*/) {
  Impl& I = *impl_;
  PerFrame& f = I.frames[I.frame_index];
  CommandList cmd(f.cmd);
  const VkDevice dev = ctx_->device();

  // --- CPU: cull + pack ---------------------------------------------------
  I.visibility.renderables.clear();
  I.visibility.light_indices.clear();
  scene.cull(camera.frustum(), I.visibility);
  scene.pack_lights(I.packed_lights);
  const u32 light_count = static_cast<u32>(std::min<usize>(I.packed_lights.size(), kMaxLights));
  if (light_count > 0) {
    std::memcpy(f.light_mapped, I.packed_lights.data(), sizeof(GpuLight) * light_count);
  }
  stats_.visible_instances = static_cast<u32>(I.visibility.renderables.size());
  stats_.visible_lights = light_count;

  // --- One-time GPU work --------------------------------------------------
  if (!I.geometry_uploaded) {
    I.registry->flush_geometry(f.cmd);
    I.geometry_uploaded = true;
  }

  // --- One-time GPU work --------------------------------------------------
  if (!I.geometry_uploaded) {
    I.registry->flush_geometry(f.cmd);
    I.geometry_uploaded = true;
  }

  // --- Sun / atmosphere ---------------------------------------------------
  const vec3 sun_dir = scene.sun_direction();
  f.constants.sun_direction = vec4(sun_dir, scene.sky().sun_intensity);
  f.constants.sun_color =
      vec4(scene.sky().sun_color, scene.sky().angular_radius_degrees * kDegToRad);
  f.constants.cloud_params =
      vec4(scene.sky().cloud_coverage, scene.sky().cloud_density,
           static_cast<f32>(quality_.cloud_steps), static_cast<f32>(I.frame_counter % 1024));
  f.constants.cloud_wind = vec4(scene.sky().wind_direction, 0.0f, 0.0f);
  f.constants.atmosphere_scale = vec4(6360e3f, 6460e3f, scene.sky().turbidity, 0.0f);

  // Shadow cascades depend on the camera and the sun, so they are fitted here,
  // after both are known and before the constants are uploaded.
  I.update_cascades(camera, sun_dir, f.constants);
  std::memcpy(f.frame_ubo_mapped, &f.constants, sizeof(FrameConstants));

  // --- Image-based lighting ----------------------------------------------
  // Baked once at load, then only re-baked when the sun moves meaningfully.
  IBLBakeParams bake_params{};
  bake_params.sun_direction = sun_dir;
  bake_params.sun_color = scene.sky().sun_color;
  bake_params.sun_intensity = scene.sky().sun_intensity;
  bake_params.angular_radius = scene.sky().angular_radius_degrees * kDegToRad;
  bake_params.turbidity = scene.sky().turbidity;

  if (I.ibl && (!I.ibl->ready() || I.ibl_pending)) {
    if (I.ibl->bake(f.frame_set, bake_params)) {
      I.ibl_pending = false;
      I.last_bake = bake_params;
    }
  } else if (I.ibl && I.ibl->ready()) {
    // Re-bake when the sun has drifted more than ~2 degrees.
    const f32 dotp = glm::dot(I.last_bake.sun_direction, bake_params.sun_direction);
    if (dotp < 0.9994f) {
      if (I.ibl->bake(f.frame_set, bake_params))
        I.last_bake = bake_params;
    }
  }

  // --- Depth layout transition -------------------------------------------
  cmd.image_barrier(I.depth.image, I.depth.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
  I.depth.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

  // =========================================================================
  //  Pass 1a — Cascaded shadow maps (depth-only, into the atlas slices)
  // =========================================================================
  if (I.p_shadow != VK_NULL_HANDLE && I.shadow.view != VK_NULL_HANDLE && I.registry) {
    cmd.label_begin(*ctx_, "Shadows", vec4(0.6f, 0.6f, 0.6f, 1.0f));

    cmd.image_barrier(I.shadow.image, I.shadow.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    I.shadow.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    cmd.bind_pipeline(I.p_shadow);
    const VkDescriptorSet shadow_sets[2] = {f.frame_set, I.registry->bindless_set()};
    cmd.bind_descriptor_sets(I.frame_pipeline_layout, 0, 2, shadow_sets);

    struct ShadowPC {
      mat4 light_view_proj;
      u32 cascade;
      f32 depth_bias;
      f32 normal_bias;
      f32 pad;
    };

    for (u32 cascade = 0; cascade < I.shadow_cascade_count; ++cascade) {
      VkRenderingAttachmentInfo depth_att{};
      depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_att.imageView = I.shadow.view;
      depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
      depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_att.clearValue.depthStencil = {1.0f, 0};

      VkRenderingInfo ri{};
      ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      // Each cascade owns a horizontal slice of the atlas.
      ri.renderArea = {{static_cast<i32>(cascade * I.shadow_size), 0},
                       {I.shadow_size, I.shadow_size}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 0;
      ri.pDepthAttachment = &depth_att;
      cmd.begin_rendering(ri);

      cmd.set_viewport(static_cast<f32>(cascade * I.shadow_size), 0.0f,
                       static_cast<f32>(I.shadow_size), static_cast<f32>(I.shadow_size));
      cmd.set_scissor(static_cast<i32>(cascade * I.shadow_size), 0, I.shadow_size, I.shadow_size);

      for (EntityHandle e : I.visibility.renderables) {
        const RenderableDesc* r = scene.renderable(e);
        if (!r || !r->casts_shadow || !r->mesh.valid())
          continue;
        const Mesh* mesh = I.registry->mesh(r->mesh);
        if (!mesh || !mesh->valid())
          continue;

        ShadowPC pc{};
        pc.light_view_proj = f.constants.shadow_matrices[cascade] * scene.transform(e).matrix();
        pc.cascade = cascade;
        pc.depth_bias = f.constants.shadow_params.z;
        pc.normal_bias = f.constants.shadow_params.w;
        cmd.push_constants(I.frame_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(pc),
                           &pc);

        cmd.bind_index_buffer(I.registry->index_buffer(),
                              static_cast<VkDeviceSize>(mesh->first_index) * sizeof(u32));
        cmd.draw_indexed(mesh->index_count);
      }

      cmd.end_rendering();
    }

    // Hand the atlas to the lighting pass as a sampled depth texture.
    cmd.image_barrier(
        I.shadow.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    I.shadow.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cmd.label_end(*ctx_);
  }

  // =========================================================================
  //  Pass 1 — G-buffer
  // =========================================================================
  {
    cmd.label_begin(*ctx_, "GBuffer", vec4(0.2f, 0.5f, 1.0f, 1.0f));

    VkRenderingAttachmentInfo color_atts[3]{};
    const std::array<Image*, 3> targets{&I.gbuffer_albedo, &I.gbuffer_normal, &I.gbuffer_emissive};
    for (u32 i = 0; i < 3; ++i) {
      color_atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_atts[i].imageView = targets[i]->view;
      color_atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      color_atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_atts[i].clearValue = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
    }

    VkRenderingAttachmentInfo depth_att{};
    depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_att.imageView = I.depth.view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: clear to 0 (far)

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {width_, height_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = color_atts;
    ri.pDepthAttachment = &depth_att;
    cmd.begin_rendering(ri);

    cmd.set_viewport(0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_));
    cmd.set_scissor(0, 0, width_, height_);

    if (I.p_gbuffer != VK_NULL_HANDLE && I.registry) {
      cmd.bind_pipeline(I.p_gbuffer);
      const VkDescriptorSet geo_sets[2] = {f.frame_set, I.registry->bindless_set()};
      cmd.bind_descriptor_sets(I.frame_pipeline_layout, 0, 2, geo_sets);

      // Geometry streams from the shared vertex/index arenas: one pipeline
      // bind, one descriptor bind, then a draw per visible renderable.
      stats_.draw_calls = 0;
      for (EntityHandle e : I.visibility.renderables) {
        const RenderableDesc* r = scene.renderable(e);
        if (!r || !r->mesh.valid())
          continue;
        const Mesh* mesh = I.registry->mesh(r->mesh);
        if (!mesh || !mesh->valid())
          continue;

        const Material* mat = I.registry->material(r->material);

        DrawConstants dc{};
        dc.model = scene.transform(e).matrix();
        dc.normal_matrix = glm::transpose(glm::inverse(dc.model));
        if (mat) {
          dc.base_color_factor = mat->base_color_factor;
          dc.material_params =
              vec4(mat->metallic, mat->roughness, mat->emissive_strength, mat->occlusion_strength);
          dc.texture_indices = uvec4(mat->albedo_texture, mat->normal_texture, 0u, 0u);
          dc.flags =
              vec4(mat->alpha_test ? 1.0f : 0.0f, mat->double_sided ? 1.0f : 0.0f, 0.0f, 0.0f);
        }
        cmd.push_constants(I.frame_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           sizeof(DrawConstants), &dc);

        cmd.bind_index_buffer(I.registry->index_buffer(),
                              static_cast<VkDeviceSize>(mesh->first_index) * sizeof(u32));
        cmd.draw_indexed(mesh->index_count);
        ++stats_.draw_calls;
      }
    }

    cmd.end_rendering();
    cmd.label_end(*ctx_);

    for (u32 i = 0; i < 3; ++i)
      targets[i]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  // =========================================================================
  //  Pass 2 — Volumetric clouds (compute, low res)
  // =========================================================================
  if (quality_.volumetric_clouds && I.p_clouds != VK_NULL_HANDLE &&
      I.cloud.image != VK_NULL_HANDLE) {
    cmd.label_begin(*ctx_, "VolumetricClouds", vec4(0.9f, 0.9f, 1.0f, 1.0f));

    cmd.image_barrier(I.cloud.image, I.cloud.layout, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    I.cloud.layout = VK_IMAGE_LAYOUT_GENERAL;

    cmd.bind_compute(I.p_clouds);
    if (I.cloud_set != VK_NULL_HANDLE) {
      cmd.bind_compute_descriptor_sets(I.cloud_pipeline_layout, 0, 1, &f.frame_set);
      cmd.bind_compute_descriptor_sets(I.cloud_pipeline_layout, 1, 1, &I.cloud_set);
    }
    if (I.cloud_set != VK_NULL_HANDLE) {
      cmd.bind_compute_descriptor_sets(I.cloud_pipeline_layout, 0, 1, &f.frame_set);
      cmd.bind_compute_descriptor_sets(I.cloud_pipeline_layout, 1, 1, &I.cloud_set);
    }
    const u32 gx = (I.cloud.extent.width + 7) / 8;
    const u32 gy = (I.cloud.extent.height + 7) / 8;
    cmd.dispatch(gx, gy, 1);

    cmd.memory_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    cmd.label_end(*ctx_);
  }

  // =========================================================================
  //  Pass 2b — Cluster light assignment (compute)
  // =========================================================================
  if (I.p_cluster_assign != VK_NULL_HANDLE && I.cluster_set != VK_NULL_HANDLE && light_count > 0) {
    cmd.label_begin(*ctx_, "ClusterAssign", vec4(0.5f, 0.8f, 0.4f, 1.0f));
    cmd.bind_compute(I.p_cluster_assign);
    cmd.bind_compute_descriptor_sets(I.cluster_pipeline_layout, 0, 1, &f.frame_set);
    cmd.bind_compute_descriptor_sets(I.cluster_pipeline_layout, 1, 1, &I.cluster_set);

    struct ClusterPC {
      uvec4 grid;
      vec4 params; // near, far, light count, inv log ratio
      vec4 dims;   // tile w, tile h, max per cluster
    } cpc{};
    cpc.grid = uvec4(Impl::kClusterGridX, Impl::kClusterGridY, Impl::kClusterGridZ, 0u);
    cpc.params = vec4(0.1f, 500.0f, static_cast<f32>(light_count), 1.0f / std::log(500.0f / 0.1f));
    cpc.dims = vec4(static_cast<f32>(width_) / static_cast<f32>(Impl::kClusterGridX),
                    static_cast<f32>(height_) / static_cast<f32>(Impl::kClusterGridY),
                    static_cast<f32>(Impl::kMaxLightsPerCluster), 0.0f);
    cmd.push_constants(I.cluster_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(cpc), &cpc);
    cmd.dispatch(Impl::kClusterGridX, Impl::kClusterGridY, Impl::kClusterGridZ);

    cmd.memory_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cmd.label_end(*ctx_);
  }

  // =========================================================================
  //  Pass 3 — Sky + deferred lighting (single HDR target)
  // =========================================================================
  {
    cmd.label_begin(*ctx_, "Lighting", vec4(1.0f, 0.7f, 0.2f, 1.0f));

    VkRenderingAttachmentInfo hdr_att{};
    hdr_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    hdr_att.imageView = I.hdr.view;
    hdr_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdr_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdr_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    hdr_att.clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {width_, height_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &hdr_att;
    cmd.begin_rendering(ri);
    cmd.set_viewport(0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_));
    cmd.set_scissor(0, 0, width_, height_);

    if (I.p_lighting != VK_NULL_HANDLE) {
      cmd.bind_pipeline(I.p_lighting);
      if (I.ibl && I.ibl->ready() && I.scene_set != VK_NULL_HANDLE) {
        cmd.bind_descriptor_sets(I.lighting_pipeline_layout, 0, 1, &f.frame_set);
        const VkDescriptorSet pass_sets[2] = {I.scene_set, I.ibl->descriptor_set()};
        cmd.bind_descriptor_sets(I.lighting_pipeline_layout, 2, 2, pass_sets);
      }
      if (I.ibl && I.ibl->ready() && I.scene_set != VK_NULL_HANDLE) {
        cmd.bind_descriptor_sets(I.lighting_pipeline_layout, 0, 1, &f.frame_set);
        const VkDescriptorSet pass_sets[2] = {I.scene_set, I.ibl->descriptor_set()};
        cmd.bind_descriptor_sets(I.lighting_pipeline_layout, 2, 2, pass_sets);
      }
      struct LightingPush {
        uvec4 flags;   // x = light count, y = invert_y, z = atmosphere, w = clouds
        vec4 rotation; // xy = pre-rotation row 0, zw = row 1
        vec4 params;
      } push{};
      push.flags = uvec4(light_count, 1u, quality_.atmosphere ? 1u : 0u,
                         (quality_.volumetric_clouds && I.cloud.image) ? 1u : 0u);
      push.rotation = vec4(I.gbuffer_albedo.extent.width > 0 ? 1.0f : 1.0f, 0.0f, 0.0f, 1.0f);
      push.params = vec4(I.render_scale, quality_.bloom ? quality_.bloom_intensity : 0.0f,
                         static_cast<f32>(I.shadow_cascade_count), 0.0f);
      cmd.push_constants(I.lighting_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(push),
                         &push);
      cmd.draw(3);
      stats_.draw_calls++;
    }

    cmd.end_rendering();
    I.hdr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cmd.label_end(*ctx_);
  }

  // =========================================================================
  //  Pass 4 — Post: bloom chain + tonemap straight into the swapchain image
  // =========================================================================
  {
    cmd.label_begin(*ctx_, "PostProcess", vec4(0.8f, 0.3f, 0.9f, 1.0f));

    const VkImage swap_image = swapchain_->image(I.current_image);
    cmd.image_barrier(swap_image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                      0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo swap_att{};
    swap_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    swap_att.imageView = swapchain_->view(I.current_image);
    swap_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swap_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swap_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    swap_att.clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, swapchain_->extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &swap_att;
    cmd.begin_rendering(ri);
    cmd.set_viewport(0.0f, 0.0f, static_cast<f32>(swapchain_->extent().width),
                     static_cast<f32>(swapchain_->extent().height));
    cmd.set_scissor(0, 0, swapchain_->extent().width, swapchain_->extent().height);

    if (I.p_tonemap != VK_NULL_HANDLE) {
      cmd.bind_pipeline(I.p_tonemap);
      if (I.post_set != VK_NULL_HANDLE) {
        cmd.bind_descriptor_sets(I.post_pipeline_layout, 0, 1, &f.frame_set);
        cmd.bind_descriptor_sets(I.post_pipeline_layout, 2, 1, &I.post_set);
      }
      if (I.post_set != VK_NULL_HANDLE) {
        cmd.bind_descriptor_sets(I.post_pipeline_layout, 0, 1, &f.frame_set);
        cmd.bind_descriptor_sets(I.post_pipeline_layout, 2, 1, &I.post_set);
      }
      struct TonemapPush {
        vec4 exposure;       // x = exposure, y = bloom, z = fxaa, w = vignette
        vec4 pre_rotation_x; // row 0 of the surface pre-rotation matrix
        vec4 pre_rotation_y; // row 1
      } tp{};
      tp.exposure = vec4(1.0f, quality_.bloom ? quality_.bloom_intensity : 0.0f,
                         quality_.fxaa ? 1.0f : 0.0f, 0.15f);
      tp.pre_rotation_x = vec4(swapchain_->rotation_matrix_element(0, 0),
                               swapchain_->rotation_matrix_element(0, 1), 0.0f, 0.0f);
      tp.pre_rotation_y = vec4(swapchain_->rotation_matrix_element(1, 0),
                               swapchain_->rotation_matrix_element(1, 1), 0.0f, 0.0f);
      cmd.push_constants(I.lighting_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(tp),
                         &tp);
      cmd.draw(3);
      stats_.draw_calls++;
    }

    cmd.end_rendering();

    cmd.image_barrier(swap_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    cmd.label_end(*ctx_);
  }

  cmd.end();

  // --- Submit --------------------------------------------------------------
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &f.image_available;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &f.render_finished;

  NEO_VK_CHECK(vkQueueSubmit(ctx_->graphics_queue(), 1, &si, f.in_flight));
  (void)dev;
}

void Renderer::end_frame() {
  Impl& I = *impl_;
  PerFrame& f = I.frames[I.frame_index];

  swapchain_->present(f.render_finished, I.current_image);

  stats_.gpu_memory_bytes = ctx_->allocator().allocated_bytes();
  stats_.gpu_budget_used = ctx_->allocator().budget_fraction_used();
  stats_.resolution_scale = I.render_scale;

  // --- Dynamic resolution controller -------------------------------------
  // Simple proportional controller with hysteresis; avoids oscillation that a
  // naive threshold flip-flop causes on thermally-throttled phones.
  if (quality_.dynamic_resolution) {
    const f32 target = quality_.target_frame_ms;
    if (I.smoothed_gpu_ms > target * 1.15f && I.render_scale > 0.55f) {
      I.render_scale = std::max(0.55f, I.render_scale - 0.05f);
    } else if (I.smoothed_gpu_ms < target * 0.75f && I.render_scale < 1.0f) {
      I.render_scale = std::min(1.0f, I.render_scale + 0.02f);
    }
    // Memory pressure also forces a step down before the OS kills us.
    if (stats_.gpu_budget_used > 0.88f && I.render_scale > 0.55f) {
      I.render_scale = std::max(0.55f, I.render_scale - 0.1f);
    }
  }

  I.frame_index = (I.frame_index + 1) % kFramesInFlight;
  ++I.frame_counter;
}

void Renderer::resize(u32 width, u32 height) {
  if (width == 0 || height == 0)
    return;
  if (width == width_ && height == height_)
    return;

  Impl& I = *impl_;
  vkDeviceWaitIdle(ctx_->device());

  width_ = width;
  height_ = height;

  SwapchainDesc sd{};
  sd.width = width;
  sd.height = height;
  sd.vsync = quality_.preset != QualityPreset::Low;
  swapchain_ = Swapchain::create(*ctx_, sd);
  if (!swapchain_) {
    NEO_ERROR(kRender, "Swapchain rebuild failed at {}x{}", width, height);
    return;
  }
  width_ = swapchain_->extent().width;
  height_ = swapchain_->extent().height;
  I.create_targets(width_, height_, quality_);
  NEO_INFO(kRender, "Resized to {}x{}", width_, height_);
}

void Renderer::on_surface_lost() {
  Impl& I = *impl_;
  vkDeviceWaitIdle(ctx_->device());
  I.destroy_targets();
}

void Renderer::on_surface_recreated() {
  Impl& I = *impl_;
  I.create_targets(width_, height_, quality_);
}

ResourceRegistry& Renderer::resources() {
  return *impl_->registry;
}

IBLBaker& Renderer::ibl_baker() {
  return *impl_->ibl;
}

void Renderer::set_quality(QualitySettings settings) {
  Impl& I = *impl_;
  vkDeviceWaitIdle(ctx_->device());
  quality_ = settings;
  I.create_targets(width_, height_, quality_);
}

void Renderer::reload_shaders() {
  Impl& I = *impl_;
  vkDeviceWaitIdle(ctx_->device());
  I.pipelines->release_all();
  I.create_pipelines(quality_);
  NEO_INFO(kRender, "Shaders reloaded");
}

} // namespace neo
