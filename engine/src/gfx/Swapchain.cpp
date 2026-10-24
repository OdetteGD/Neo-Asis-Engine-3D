#include "neo/gfx/Swapchain.h"
#include "neo/core/Log.h"
#include "neo/gfx/VulkanContext.h"

#include <algorithm>

namespace neo {

std::unique_ptr<Swapchain> Swapchain::create(VulkanContext& ctx, const SwapchainDesc& desc) {
  auto sc = std::unique_ptr<Swapchain>(new Swapchain());
  if (!sc->create_internal(ctx, desc)) return nullptr;
  return sc;
}

Swapchain::~Swapchain() {
  destroy_views();
  if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
}

void Swapchain::destroy_views() {
  for (VkImageView v : views_) {
    if (v != VK_NULL_HANDLE) vkDestroyImageView(device_, v, nullptr);
  }
  views_.clear();
  images_.clear();
}

bool Swapchain::create_internal(VulkanContext& ctx, const SwapchainDesc& desc) {
  device_ = ctx.device();
  present_queue_ = ctx.present_queue();
  physical_device_ = ctx.physical_device();
  surface_ = ctx.surface();

  VkSurfaceCapabilitiesKHR caps{};
  NEO_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps));

  u32 format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

  u32 mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, modes.data());

  const VkSurfaceFormatKHR chosen = ctx.choose_surface_format(formats);
  format_ = chosen.format;
  color_space_ = chosen.colorSpace;

  // Present mode: MAILBOX when asked for uncapped, else FIFO (always available
  // and the lowest-power option, which matters on mobile).
  present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  if (!desc.vsync) {
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
      present_mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (std::find(modes.begin(), modes.end(),
                         VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) {
      present_mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  extent_ = caps.currentExtent;
  if (extent_.width == UINT32_MAX || caps.currentExtent.width == 0xFFFFFFFFu) {
    extent_.width = std::clamp(desc.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent_.height = std::clamp(desc.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  transform_ = caps.currentTransform;
  // Prefer pre-rotation so the compositor does no extra work.
  if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
    transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  }

  composite_alpha_ = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
    // Some Android compositors only accept INHERIT.
    composite_alpha_ = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }

  u32 desired = std::max(desc.preferred_image_count, caps.minImageCount);
  if (caps.maxImageCount > 0) desired = std::min(desired, caps.maxImageCount);
  min_image_count_ = desired;

  VkImageUsageFlags usage = desc.usage;
  if ((caps.supportedUsageFlags & usage) != usage) {
    NEO_WARN(kGfx, "Requested swapchain usage unsupported; falling back to color attachment only");
    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  usage |= desc.requested_extra & caps.supportedUsageFlags;

  VkSwapchainCreateInfoKHR ci{};
  ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  ci.surface = surface_;
  ci.minImageCount = desired;
  ci.imageFormat = format_;
  ci.imageColorSpace = color_space_;
  ci.imageExtent = extent_;
  ci.imageArrayLayers = 1;
  ci.imageUsage = usage;

  const u32 families[] = {ctx.graphics_family(), ctx.present_family()};
  if (families[0] != families[1]) {
    ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = 2;
    ci.pQueueFamilyIndices = families;
  } else {
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  ci.preTransform = transform_;
  ci.compositeAlpha = composite_alpha_;
  ci.presentMode = present_mode_;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = swapchain_;

  NEO_VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
  if (ci.oldSwapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, ci.oldSwapchain, nullptr);
  }

  u32 count = 0;
  NEO_VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr));
  images_.resize(count);
  NEO_VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()));

  views_.resize(count);
  for (u32 i = 0; i < count; ++i) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = images_[i];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format_;
    vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NEO_VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &views_[i]));
  }

  needs_rebuild_ = false;
  NEO_INFO(kGfx, "Swapchain {}x{} x{} [{}] presentMode={} transform=0x{:x}", extent_.width,
           extent_.height, count, "mobile", static_cast<int>(present_mode_),
           static_cast<u32>(transform_));
  return true;
}

bool Swapchain::rebuild(u32 width, u32 height, bool vsync) {
  SwapchainDesc desc{};
  desc.width = width;
  desc.height = height;
  desc.vsync = vsync;
  desc.preferred_image_count = min_image_count_;

  destroy_views();
  vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;

  // Rebuild needs a context; the renderer owns one, so this thin overload is
  // only used through Renderer::resize which re-runs create_internal().
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
  if (extent_.width != 0 && extent_.height != 0 && width == 0) {
    desc.width = extent_.width;
    desc.height = extent_.height;
  }
  NEO_WARN(kGfx, "Swapchain rebuild requested ({}x{}) — recreate via Renderer", desc.width,
           desc.height);
  needs_rebuild_ = true;
  return false;
}

bool Swapchain::acquire(VkSemaphore image_available, u32* out_image_index) {
  const VkResult r =
      vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available, VK_NULL_HANDLE,
                            out_image_index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    needs_rebuild_ = true;
    return false;
  }
  if (r == VK_SUBOPTIMAL_KHR) {
    needs_rebuild_ = true;  // still presentable this frame
  } else if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "vkAcquireNextImageKHR failed: {}", vk_result_name(r));
    return false;
  }
  return true;
}

bool Swapchain::present(VkSemaphore render_finished, u32 image_index) {
  const VkPresentInfoKHR pi{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext = nullptr,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished,
      .swapchainCount = 1,
      .pSwapchains = &swapchain_,
      .pImageIndices = &image_index,
      .pResults = nullptr,
  };
  const VkResult r = vkQueuePresentKHR(present_queue_, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    needs_rebuild_ = true;
    return false;
  }
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "vkQueuePresentKHR failed: {}", vk_result_name(r));
    return false;
  }
  return true;
}

f32 Swapchain::rotation_matrix_element(u32 row, u32 col) const {
  // Row-major 2x2 clip-space rotation for the pre-rotation transform.
  switch (transform_) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  // { 0, -1 }, { 1, 0 }
      if (row == 0 && col == 0) return 0.0f;
      if (row == 0 && col == 1) return 1.0f;
      if (row == 1 && col == 0) return -1.0f;
      return 0.0f;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
      return (row == col) ? -1.0f : 0.0f;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:  // { 0, 1 }, { -1, 0 }
      if (row == 0 && col == 0) return 0.0f;
      if (row == 0 && col == 1) return -1.0f;
      if (row == 1 && col == 0) return 1.0f;
      return 0.0f;
    default:
      return (row == col) ? 1.0f : 0.0f;
  }
}

} // namespace neo
