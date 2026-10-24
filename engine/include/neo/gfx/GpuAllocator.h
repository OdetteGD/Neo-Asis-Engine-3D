// ===========================================================================
//  GpuAllocator — VMA wrapper tuned for mobile.
//
//  Notes on the defaults:
//   * Sub-allocation block sizes are small (64 MB) because Android devices
//     frequently sit near their memory budget and a monolithic pool starves
//     the OS.
//   * Images get dedicated allocations above 8 MB; buffers above 32 MB.
//   * A device-local + host-visible staging ring is exposed for uploads so the
//     renderer can stream textures without per-upload allocations.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <memory>
#include <string_view>

namespace neo {

struct GpuAllocatorDesc {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  u32 api_version = VK_API_VERSION_1_2;
  u64 preferred_block_size = 64ull << 20;
};

// Memory classes the renderer asks for, mapped onto VMA usage.
enum class MemoryClass : u8 {
  DeviceLocal,       // GPU-only, fastest (textures, vertex data, render targets)
  DeviceLocalHostVisible,  // Resizable BAR / UMA (mobile) upload targets
  Upload,            // Host-visible staging, mapped persistently
  Readback,
};

struct BufferDesc {
  std::string_view name;
  u64 size = 0;
  VkBufferUsageFlags usage = 0;
  MemoryClass memory = MemoryClass::DeviceLocal;
  bool persistently_mapped = false;
};

struct ImageDesc {
  std::string_view name;
  VkImageType type = VK_IMAGE_TYPE_2D;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  u32 width = 1, height = 1, depth = 1;
  u32 mip_levels = 1, array_layers = 1;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
  VkImageUsageFlags usage = 0;
  MemoryClass memory = MemoryClass::DeviceLocal;
};

class GpuAllocator {
public:
  static std::unique_ptr<GpuAllocator> create(const GpuAllocatorDesc& desc);
  ~GpuAllocator();

  GpuAllocator(const GpuAllocator&) = delete;
  GpuAllocator& operator=(const GpuAllocator&) = delete;

  // --- Raw allocation ----------------------------------------------------
  [[nodiscard]] bool create_buffer(const BufferDesc& desc, VkBuffer& out_buffer,
                                   VmaAllocation& out_alloc, void** out_mapped = nullptr);
  void destroy_buffer(VkBuffer buffer, VmaAllocation alloc);

  [[nodiscard]] bool create_image(const ImageDesc& desc, VkImage& out_image,
                                  VmaAllocation& out_alloc);
  void destroy_image(VkImage image, VmaAllocation alloc);

  // --- Stats -------------------------------------------------------------
  [[nodiscard]] u64 allocated_bytes() const;
  [[nodiscard]] u64 peak_bytes() const { return peak_bytes_; }
  [[nodiscard]] f32 budget_fraction_used() const;  // 0..1, uses VK_EXT_memory_budget

  void log_stats(std::string_view label) const;

  [[nodiscard]] VmaAllocator raw() const { return allocator_; }

private:
  GpuAllocator() = default;
  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  u64 peak_bytes_ = 0;
};

} // namespace neo
