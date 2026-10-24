// ===========================================================================
//  Swapchain — present chain with per-frame resource rotation.
//
//  Mobile specifics handled here:
//   * Surface pre-rotation: query currentTransform and build the projection
//     transform so we never pay for a rotated blit on portrait devices.
//   * Lazy recreation on VK_ERROR_OUT_OF_DATE_KHR / SUBOPTIMAL_KHR.
//   * Composite-alpha treated as opaque (required by many Android compositors).
//   * sRGB-encoded swapchain + linear internal HDR target, converted in the
//     final tonemap pass.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <volk.h>

#include <memory>
#include <vector>

namespace neo {

class VulkanContext;

struct SwapchainDesc {
  u32 width = 1280;
  u32 height = 720;
  bool vsync = true;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImageUsageFlags requested_extra = 0;
  u32 preferred_image_count = 3;  // triple buffering: the mobile sweet spot
};

class Swapchain {
public:
  static std::unique_ptr<Swapchain> create(VulkanContext& ctx, const SwapchainDesc& desc);
  ~Swapchain();

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // Returns true when the swapchain had to be rebuilt (caller re-created
  // size-dependent targets).
  bool acquire(VkSemaphore image_available, u32* out_image_index);

  // Returns true when out-of-date; caller should rebuild and skip presenting.
  bool present(VkSemaphore render_finished, u32 image_index);

  bool rebuild(u32 width, u32 height, bool vsync);

  [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
  [[nodiscard]] VkFormat format() const { return format_; }
  [[nodiscard]] VkColorSpaceKHR color_space() const { return color_space_; }
  [[nodiscard]] VkExtent2D extent() const { return extent_; }
  [[nodiscard]] u32 image_count() const { return static_cast<u32>(images_.size()); }
  [[nodiscard]] VkImage image(u32 i) const { return images_[i]; }
  [[nodiscard]] VkImageView view(u32 i) const { return views_[i]; }
  [[nodiscard]] VkSurfaceTransformFlagBitsKHR transform() const { return transform_; }
  [[nodiscard]] bool needs_rebuild() const { return needs_rebuild_; }
  [[nodiscard]] u32 min_image_count() const { return min_image_count_; }

  // Rotation-aware clip-space transform applied in the tonemap pass.
  [[nodiscard]] f32 rotation_matrix_element(u32 row, u32 col) const;

private:
  Swapchain() = default;
  bool create_internal(VulkanContext& ctx, const SwapchainDesc& desc);
  void destroy_views();

  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue present_queue_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;

  VkFormat format_ = VK_FORMAT_B8G8R8A8_SRGB;
  VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkExtent2D extent_{0, 0};
  VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  VkCompositeAlphaFlagBitsKHR composite_alpha_ = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

  u32 min_image_count_ = 3;
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  bool needs_rebuild_ = false;
};

} // namespace neo
