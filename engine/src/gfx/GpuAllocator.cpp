#include "neo/gfx/GpuAllocator.h"
#include "neo/core/Log.h"
#include "neo/gfx/VulkanContext.h"

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace neo {

std::unique_ptr<GpuAllocator> GpuAllocator::create(const GpuAllocatorDesc& desc) {
  auto a = std::unique_ptr<GpuAllocator>(new GpuAllocator());
  a->device_ = desc.device;

  VmaVulkanFunctions funcs{};
  funcs.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
  funcs.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo ci{};
  ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  ci.physicalDevice = desc.physical_device;
  ci.device = desc.device;
  ci.instance = desc.instance;
  ci.vulkanApiVersion = desc.api_version;
  ci.pVulkanFunctions = &funcs;
  ci.preferredLargeHeapBlockSize = desc.preferred_block_size;

  const VkResult r = vmaCreateAllocator(&ci, &a->allocator_);
  if (r != VK_SUCCESS) {
    // Retry without buffer device address (some older mobile drivers reject it).
    NEO_WARN(kGfx, "VMA init with BDA failed ({}), retrying without", vk_result_name(r));
    ci.flags = 0;
    NEO_VK_CHECK(vmaCreateAllocator(&ci, &a->allocator_));
  }
  NEO_INFO(kGfx, "VMA allocator ready (block {} MiB)", desc.preferred_block_size >> 20);
  return a;
}

GpuAllocator::~GpuAllocator() {
  if (allocator_ != VK_NULL_HANDLE) {
    log_stats("shutdown");
    vmaDestroyAllocator(allocator_);
  }
}

bool GpuAllocator::create_buffer(const BufferDesc& desc, VkBuffer& out_buffer,
                                 VmaAllocation& out_alloc, void** out_mapped) {
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = desc.size;
  bci.usage = desc.usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  switch (desc.memory) {
    case MemoryClass::DeviceLocal:
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      break;
    case MemoryClass::DeviceLocalHostVisible:
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryClass::Upload:
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryClass::Readback:
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
  }

  VmaAllocationInfo info{};
  const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &out_buffer, &out_alloc, &info);
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "Buffer allocation failed: {} ({} bytes)", vk_result_name(r), desc.size);
    return false;
  }
  if (out_mapped) {
    *out_mapped = desc.persistently_mapped ? info.pMappedData : nullptr;
  }
  peak_bytes_ = std::max(peak_bytes_, allocated_bytes());
  return true;
}

void GpuAllocator::destroy_buffer(VkBuffer buffer, VmaAllocation alloc) {
  if (buffer != VK_NULL_HANDLE && alloc != VK_NULL_HANDLE) {
    vmaDestroyBuffer(allocator_, buffer, alloc);
  }
}

bool GpuAllocator::create_image(const ImageDesc& desc, VkImage& out_image,
                                VmaAllocation& out_alloc) {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = desc.type;
  ici.format = desc.format;
  ici.extent = {desc.width, desc.height, desc.depth};
  ici.mipLevels = desc.mip_levels;
  ici.arrayLayers = desc.array_layers;
  ici.samples = desc.samples;
  ici.tiling = desc.tiling;
  ici.usage = desc.usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (desc.type == VK_IMAGE_TYPE_CUBE ||
      (desc.array_layers == 6 && desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
    ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }

  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (desc.memory == MemoryClass::DeviceLocalHostVisible) {
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  }

  const VkResult r = vmaCreateImage(allocator_, &ici, &aci, &out_image, &out_alloc, nullptr);
  if (r != VK_SUCCESS) {
    NEO_ERROR(kGfx, "Image allocation failed: {} ({}x{} {})", vk_result_name(r), desc.width,
              desc.height, static_cast<int>(desc.format));
    return false;
  }
  peak_bytes_ = std::max(peak_bytes_, allocated_bytes());
  return true;
}

void GpuAllocator::destroy_image(VkImage image, VmaAllocation alloc) {
  if (image != VK_NULL_HANDLE && alloc != VK_NULL_HANDLE) {
    vmaDestroyImage(allocator_, image, alloc);
  }
}

u64 GpuAllocator::allocated_bytes() const {
  VmaTotalStatistics stats{};
  vmaCalculateStatistics(allocator_, &stats);
  return stats.total.statistics.allocationBytes;
}

f32 GpuAllocator::budget_fraction_used() const {
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(allocator_, budgets);
  const VkPhysicalDeviceMemoryProperties* mp = nullptr;
  vmaGetMemoryProperties(allocator_, &mp);
  if (!mp) return 0.0f;

  f64 used = 0.0, total = 0.0;
  for (u32 i = 0; i < mp->memoryHeapCount; ++i) {
    if ((mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
    used += static_cast<f64>(budgets[i].usage);
    total += static_cast<f64>(budgets[i].budget);
  }
  return total > 0.0 ? static_cast<f32>(used / total) : 0.0f;
}

void GpuAllocator::log_stats(std::string_view label) const {
  VmaTotalStatistics stats{};
  vmaCalculateStatistics(allocator_, &stats);
  NEO_INFO(kGfx,
           "GPU memory [{}]: {:.1f} MiB in {} allocations ({} blocks), budget used {:.0f}%",
           label, static_cast<f64>(stats.total.statistics.allocationBytes) / (1024.0 * 1024.0),
           stats.total.statistics.allocationCount, stats.total.statistics.blockCount,
           static_cast<f64>(budget_fraction_used()) * 100.0);
}

} // namespace neo
