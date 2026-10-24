// ===========================================================================
//  VulkanContext — instance, physical/logical device, queues, VMA allocator.
//
//  Hard requirements the renderer relies on:
//    * Vulkan 1.1+ (1.3 preferred) with dynamicRendering + synchronization2
//      when available (Android API 33+ typically exposes these).
//    * A single graphics+compute+present queue family when possible; otherwise
//      a graphics family plus a dedicated present family (mobile often splits).
//    * Descriptor indexing for bindless textures, if the driver supports it.
//    * On mobile: required device extensions are the swapchain + a small set
//      of optional ones that unlock better bandwidth (ASTC is core, so we
//      gate on format *properties* rather than extensions).
// ===========================================================================
#pragma once

#include "neo/core/Log.h"
#include "neo/core/Math.h"
#include "neo/core/Types.h"

#include <volk.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neo {

struct DeviceFeatures {
  bool dynamic_rendering = false;
  bool synchronization2 = false;
  bool timeline_semaphore = false;
  bool descriptor_indexing = false;
  bool buffer_device_address = false;
  bool sampler_filter_minmax = false;
  bool max_anisotropy_supported = false;
  f32 max_sampler_anisotropy = 1.0f;
  bool astc_ldr = false;
  bool etc2 = false;
  u32 max_push_constants = 128;
  u64 min_uniform_buffer_alignment = 64;
  u64 min_storage_buffer_alignment = 16;
};

struct DeviceDesc {
  std::string_view app_name = "Neo-Asis";
  u32 app_version = 0;
  bool enable_validation = false;
  bool prefer_discrete_gpu = true;
  std::vector<const char*> extra_instance_extensions;
  std::vector<const char*> extra_device_extensions;
};

class GpuAllocator;

class VulkanContext {
public:
  static std::unique_ptr<VulkanContext> create(const DeviceDesc& desc);
  ~VulkanContext();

  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  // --- Core objects (engine-internal users only) -------------------------
  [[nodiscard]] VkInstance instance() const { return instance_; }
  [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
  [[nodiscard]] VkDevice device() const { return device_; }
  [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
  [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
  [[nodiscard]] VkQueue compute_queue() const { return compute_queue_; }
  [[nodiscard]] VkQueue present_queue() const { return present_queue_; }
  [[nodiscard]] u32 graphics_family() const { return graphics_family_; }
  [[nodiscard]] u32 compute_family() const { return compute_family_; }
  [[nodiscard]] u32 present_family() const { return present_family_; }
  [[nodiscard]] GpuAllocator& allocator() const { return *allocator_; }

  [[nodiscard]] const DeviceFeatures& features() const { return features_; }
  [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return props_; }
  [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memory_properties() const {
    return mem_props_;
  }
  [[nodiscard]] std::string_view device_name() const { return props_.deviceName; }

  // --- Surface -----------------------------------------------------------
  void create_surface(void* native_display, void* native_window);
  [[nodiscard]] bool surface_supported_by_queue_family(u32 family) const;

  // --- Format queries ----------------------------------------------------
  // Picks the best swapchain format preferring low-bandwidth mobile formats
  // (RGB10A2, then BGRA8 sRGB) and ensuring the renderer's HDR pipeline can
  // present correctly.
  [[nodiscard]] VkSurfaceFormatKHR
  choose_surface_format(const std::vector<VkSurfaceFormatKHR>& available) const;

  [[nodiscard]] std::optional<u32> find_memory_type(u32 type_bits,
                                                    VkMemoryPropertyFlags flags) const;

  // --- Synchronisation ---------------------------------------------------
  void wait_idle();

  // --- Debug -------------------------------------------------------------
  void set_object_name(u64 handle, VkObjectType type, const char* name) const;
  void begin_debug_label(VkCommandBuffer cmd, const char* name, vec4 color) const;
  void end_debug_label(VkCommandBuffer) const;

private:
  VulkanContext();
  void create_instance(const DeviceDesc& desc);
  void pick_physical_device(const DeviceDesc& desc);
  void create_logical_device(const DeviceDesc& desc);
  void query_features();
  bool validation_layer_available() const;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;

  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue compute_queue_ = VK_NULL_HANDLE;
  VkQueue present_queue_ = VK_NULL_HANDLE;
  u32 graphics_family_ = 0;
  u32 compute_family_ = 0;
  u32 present_family_ = 0;

  VkPhysicalDeviceProperties props_{};
  VkPhysicalDeviceMemoryProperties mem_props_{};
  DeviceFeatures features_{};
  std::unique_ptr<GpuAllocator> allocator_;
  bool validation_enabled_ = false;
};

} // namespace neo

#define NEO_VK_CHECK(expr)                                                                         \
  do {                                                                                             \
    const VkResult neo_vk_result_ = (expr);                                                        \
    if (neo_vk_result_ != VK_SUCCESS) [[unlikely]] {                                               \
      NEO_FATAL(kGfx, "Vulkan call failed: {} -> {} ({}:{})", #expr,                               \
                ::neo::vk_result_name(neo_vk_result_), __FILE__, __LINE__);                        \
    }                                                                                              \
  } while (0)

namespace neo {
[[nodiscard]] const char* vk_result_name(VkResult r);
}
