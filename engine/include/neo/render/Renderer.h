// ===========================================================================
//  Renderer — frame orchestration for the Neo-Asis deferred HDR pipeline.
//
//  Pass graph (all passes may be individually disabled by quality preset):
//
//   1. ShadowCascades   CSM, 3 or 4 cascades, 16-bit VSM/PCF depth
//   2. GBuffer          Albedo+AO, Normal+Roughness, Emissive+Metallic, Depth
//   3. DepthPrepass     optional separate depth-only (mobile bandwidth saver)
//   4. SkyAtmosphere    Bruneton-style LUTs: transmittance -> multiscatter
//   5. VolumetricClouds 3D noise raymarch at 1/2 or 1/4 res, temporally
//                       reprojected with neighbourhood clamping
//   6. IBL              runtime bake or cached: irradiance SH + prefiltered
//                       specular chain + BRDF LUT (image-to-lighting)
//   7. DeferredLighting Clustered forward+, PBR (GGX + Smith), IBL, shadows,
//                       atmospheric aerial perspective, cloud composite
//   8. PostProcess      Bloom (dual-filter), auto exposure, ACES tonemap,
//                       mobile-friendly FXAA + optional TAA
//   9. UI/Present       sRGB encode, pre-rotation transform
//
//  Frame constants are pushed via a single UBO at a per-frame offset, shared
//  by every pass, so the CPU cost per frame is one small memcpy.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"
#include "neo/gfx/Swapchain.h"
#include "neo/gfx/VulkanContext.h"
#include "neo/render/Camera.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace neo {

struct RendererDesc {
  u32 width = 1280;
  u32 height = 720;
  bool vsync = true;
  bool enable_validation = false;
  std::string_view app_name = "Neo-Asis";
  // 0 = auto-detect from the GPU tier heuristic in Renderer::detect_tier().
  u32 msaa_samples = 0;
  void* native_window = nullptr;
  void* native_display = nullptr;
};

// Quality presets map onto the three real mobile tiers (see docs/RENDERING.md).
enum class QualityPreset : u8 { Low = 0, Medium, High, Ultra };

struct QualitySettings {
  QualityPreset preset = QualityPreset::Medium;

  // --- Geometry / shading ------------------------------------------------
  VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_1_BIT;
  u32 shadow_cascade_count = 3;
  u32 shadow_map_size = 1024;
  bool depth_prepass = true;
  bool clustered_lighting = true;

  // --- Atmosphere --------------------------------------------------------
  bool atmosphere = true;
  u32 atmosphere_lut_scale = 1;      // divisor for the LUT resolution
  bool volumetric_clouds = true;
  f32 cloud_render_scale = 0.5f;     // 0.5 => half-res raymarch
  u32 cloud_steps = 48;
  i32 cloud_noise_rebind_frames = 3;

  // --- Post --------------------------------------------------------------
  bool bloom = true;
  f32 bloom_intensity = 0.06f;
  bool taa = false;
  bool fxaa = true;

  // --- Dynamic resolution ------------------------------------------------
  f32 render_scale = 1.0f;           // 0.5 .. 1.0
  bool dynamic_resolution = true;
  f32 target_frame_ms = 16.6f;

  [[nodiscard]] static QualitySettings for_preset(QualityPreset p);
};

struct FrameStats {
  f32  gpu_frame_ms = 0.0f;
  f32  cpu_frame_ms = 0.0f;
  f32  fps = 0.0f;
  u32  draw_calls = 0;
  u32  visible_instances = 0;
  u32  visible_lights = 0;
  f32  resolution_scale = 1.0f;
  u64  gpu_memory_bytes = 0;
  f32  gpu_budget_used = 0.0f;
};

class Scene;

class Renderer {
public:
  static std::unique_ptr<Renderer> create(const RendererDesc& desc);
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // --- Frame -------------------------------------------------------------
  // Returns false when the swapchain was recreated and the frame was skipped.
  bool begin_frame();
  void render(Scene& scene, const Camera& camera, f32 delta_seconds);
  void end_frame();

  // --- Surface changes ---------------------------------------------------
  void resize(u32 width, u32 height);
  void on_surface_lost();     // Android: surface destroyed, drop size-dependent targets
  void on_surface_recreated();// Android: rebuild size-dependent targets

  // --- Settings ----------------------------------------------------------
  void set_quality(QualitySettings settings);
  [[nodiscard]] const QualitySettings& quality() const { return quality_; }
  void reload_shaders();      // hot reload for development on desktop

  // --- Introspection -----------------------------------------------------
  [[nodiscard]] const FrameStats& stats() const { return stats_; }
  [[nodiscard]] VulkanContext& context() { return *ctx_; }
  [[nodiscard]] Swapchain& swapchain() { return *swapchain_; }
  [[nodiscard]] u32 width() const { return width_; }
  [[nodiscard]] u32 height() const { return height_; }

  [[nodiscard]] static QualityPreset detect_tier(const VkPhysicalDeviceProperties& props);

private:
  Renderer() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::unique_ptr<VulkanContext> ctx_;
  std::unique_ptr<Swapchain> swapchain_;
  QualitySettings quality_{};
  FrameStats stats_{};
  u32 width_ = 0;
  u32 height_ = 0;
};

} // namespace neo
