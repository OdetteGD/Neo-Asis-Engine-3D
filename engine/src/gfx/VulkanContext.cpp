#include "neo/gfx/VulkanContext.h"
#include "neo/gfx/GpuAllocator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>

namespace neo {

const char* vk_result_name(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_RESULT_UNRECOGNISED";
  }
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  const bool is_error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  const bool is_warn = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
  const bool is_perf = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0;

  if (is_error) {
    NEO_ERROR(kGfx, "[validation] {}", data->pMessage);
  } else if (is_warn || is_perf) {
    NEO_WARN(kGfx, "[validation{}] {}", is_perf ? "/perf" : "", data->pMessage);
  } else {
    NEO_DEBUG(kGfx, "[validation] {}", data->pMessage);
  }
  return VK_FALSE;
}

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
  return std::any_of(all.begin(), all.end(),
                     [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
    allocator_.reset();
    vkDestroyDevice(device_, nullptr);
  }
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
  }
  if (debug_messenger_ != VK_NULL_HANDLE) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance_, debug_messenger_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

std::unique_ptr<VulkanContext> VulkanContext::create(const DeviceDesc& desc) {
  auto ctx = std::unique_ptr<VulkanContext>(new VulkanContext());
  ctx->create_instance(desc);
  ctx->pick_physical_device(desc);
  ctx->create_logical_device(desc);

  GpuAllocatorDesc ad{};
  ad.instance = ctx->instance_;
  ad.physical_device = ctx->physical_device_;
  ad.device = ctx->device_;
  ad.api_version = VK_API_VERSION_1_2;
  ctx->allocator_ = GpuAllocator::create(ad);
  return ctx;
}

bool VulkanContext::validation_layer_available() const {
  u32 count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
    return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
  });
}

void VulkanContext::create_instance(const DeviceDesc& desc) {
  NEO_VK_CHECK(volkInitialize());

  const u32 loader_version = volkGetInstanceVersion();
  NEO_INFO(kGfx, "Vulkan loader {}.{}.{}", VK_API_VERSION_MAJOR(loader_version),
           VK_API_VERSION_MINOR(loader_version), VK_API_VERSION_PATCH(loader_version));

  validation_enabled_ = desc.enable_validation && validation_layer_available();
  if (desc.enable_validation && !validation_enabled_) {
    NEO_WARN(kGfx, "Validation requested but VK_LAYER_KHRONOS_validation is unavailable");
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = desc.app_name.data();
  app.applicationVersion = desc.app_version;
  app.pEngineName = "Neo-Asis Engine 3D";
  app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  // 1.1 is the floor on Android; 1.2 unlocks better descriptor and shader
  // features that the modern path uses when present.
  app.apiVersion = VK_API_VERSION_1_2;

  // Universe of instance extensions we want.
  std::vector<const char*> extensions{
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
  };
#if defined(NEO_PLATFORM_ANDROID)
  extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
  extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(__linux__)
  extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
  for (const char* e : desc.extra_instance_extensions) extensions.push_back(e);

  u32 ext_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> available(ext_count);
  vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, available.data());

  std::vector<const char*> enabled;
  enabled.reserve(extensions.size() + 1);
  std::unordered_set<std::string> missing;
  for (const char* e : extensions) {
    if (has_extension(available, e)) {
      enabled.push_back(e);
    } else {
      missing.insert(e);
    }
  }

  // Debug utils only when we actually have validation to feed it.
  const bool want_debug_utils = validation_enabled_;
  if (want_debug_utils && has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
  for (const auto& m : missing) {
    NEO_WARN(kGfx, "Instance extension unavailable: {}", m);
  }

  std::vector<const char*> layers;
  if (validation_enabled_) layers.push_back("VK_LAYER_KHRONOS_validation");

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = static_cast<u32>(enabled.size());
  ci.ppEnabledExtensionNames = enabled.data();
  ci.enabledLayerCount = static_cast<u32>(layers.size());
  ci.ppEnabledLayerNames = layers.data();

  const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
  if (r != VK_SUCCESS) {
    NEO_FATAL(kGfx, "vkCreateInstance failed: {}. Is a Vulkan driver installed?", vk_result_name(r));
  }
  volkLoadInstance(instance_);

  if (want_debug_utils) {
    VkDebugUtilsMessengerCreateInfoEXT dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = debug_callback;
    auto create_dbg = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (create_dbg) create_dbg(instance_, &dci, nullptr, &debug_messenger_);
  }
}

void VulkanContext::pick_physical_device(const DeviceDesc& desc) {
  u32 count = 0;
  NEO_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
  if (count == 0) NEO_FATAL(kGfx, "No Vulkan-capable physical device found");

  std::vector<VkPhysicalDevice> devices(count);
  NEO_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

  i32 best_score = -1;
  VkPhysicalDevice best = VK_NULL_HANDLE;

  for (VkPhysicalDevice dev : devices) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(dev, &p);

    // Android: API level matters more than raw "discrete" heuristics.
    i16 score = 0;
    switch (p.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = desc.prefer_discrete_gpu ? 1000 : 400; break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 700; break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 300; break;
      default: score = 100; break;
    }
    if (p.apiVersion < VK_API_VERSION_1_1) score -= 500;
    if (p.limits.maxComputeWorkGroupInvocations < 128) score -= 200;
    // Prefer tile-friendly hardware with a decent memory budget.
    if (p.limits.maxStorageBufferRange >= (256u << 20)) score += 50;

    NEO_INFO(kGfx, "  [{}] {} (API {}.{}.{}, type {}, score {})", dev == best ? "*" : " ",
             p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion), static_cast<int>(p.deviceType), score);

    if (score > best_score) {
      best_score = score;
      best = dev;
    }
  }

  physical_device_ = best;
  vkGetPhysicalDeviceProperties(physical_device_, &props_);
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props_);

  // Queue families: index the graphics-capable ones, remember a compute-only
  // family separately (async compute) when the device has one spare.
  u32 families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &families, nullptr);
  std::vector<VkQueueFamilyProperties> fprops(families);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &families, fprops.data());

  graphics_family_ = ~0u;
  compute_family_ = ~0u;
  for (u32 i = 0; i < families; ++i) {
    const VkQueueFlags f = fprops[i].queueFlags;
    if ((f & VK_QUEUE_GRAPHICS_BIT) && graphics_family_ == ~0u) graphics_family_ = i;
    if ((f & VK_QUEUE_COMPUTE_BIT) && graphics_family_ != i && compute_family_ == ~0u) {
      compute_family_ = i;
    }
  }
  if (graphics_family_ == ~0u) NEO_FATAL(kGfx, "Device has no graphics queue family");
  if (compute_family_ == ~0u) compute_family_ = graphics_family_;

  NEO_INFO(kGfx, "Selected: {} — graphics queue {}, compute queue {}", props_.deviceName,
           graphics_family_, compute_family_);
}

void VulkanContext::query_features() {
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

  VkPhysicalDeviceVulkan11Features f11{};
  f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

  f2.pNext = &f11;
  f11.pNext = &f12;
  f12.pNext = &f13;

  vkGetPhysicalDeviceFeatures2(physical_device_, &f2);

  features_.dynamic_rendering = f13.dynamicRendering == VK_TRUE;
  features_.synchronization2 = f13.synchronization2 == VK_TRUE;
  features_.timeline_semaphore = f12.timelineSemaphore == VK_TRUE;
  features_.descriptor_indexing = f12.descriptorIndexing == VK_TRUE;
  features_.buffer_device_address = f12.bufferDeviceAddress == VK_TRUE;
  features_.sampler_filter_minmax = f12.samplerFilterMinmax == VK_TRUE;
  features_.max_anisotropy_supported = f2.features.samplerAnisotropy == VK_TRUE;
  features_.max_push_constants = props_.limits.maxPushConstantsSize;

  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  VkPhysicalDeviceVulkan11Properties pr11{};
  pr11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
  p2.pNext = &pr11;
  vkGetPhysicalDeviceProperties2(physical_device_, &p2);
  features_.min_uniform_buffer_alignment = pr11.minUniformBufferOffsetAlignment;
  features_.min_storage_buffer_alignment = pr11.minStorageBufferOffsetAlignment;

  // Texture formats that matter for mobile memory bandwidth.
  auto supports = [&](VkFormat fmt) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, fmt, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  };
  features_.astc_ldr = supports(VK_FORMAT_ASTC_4x4_UNORM_BLOCK) &&
                       supports(VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
  features_.etc2 = supports(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) &&
                   supports(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK);

  NEO_INFO(kGfx, "Features: dynamicRendering={} sync2={} timeline={} bindless={} astc={} etc2={}",
           features_.dynamic_rendering, features_.synchronization2, features_.timeline_semaphore,
           features_.descriptor_indexing, features_.astc_ldr, features_.etc2);
}

void VulkanContext::create_logical_device(const DeviceDesc& desc) {
  query_features();

  std::vector<const char*> device_exts{
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
  };
  // Optional but valuable on mobile GPUs.
  std::vector<const char*> optional{
      VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
      VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
  };
  for (const char* e : desc.extra_device_extensions) device_exts.push_back(e);

  u32 n = 0;
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> avail(n);
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &n, avail.data());
  for (const char* e : optional) {
    if (has_extension(avail, e)) device_exts.push_back(e);
  }

  // --- Queue creation -----------------------------------------------------
  std::vector<u32> families{graphics_family_};
  if (compute_family_ != graphics_family_) families.push_back(compute_family_);
  if (present_family_ != ~0u && std::find(families.begin(), families.end(), present_family_) ==
                                      families.end()) {
    families.push_back(present_family_);
  }

  u32 total_queues = 0;
  {
    u32 families_available = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &families_available, nullptr);
    std::vector<VkQueueFamilyProperties> fp(families_available);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &families_available, fp.data());
    for (u32 f : families) total_queues += std::max(1u, fp[f].queueCount > 2 ? 2u : 1u);
  }

  const f32 priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> qcis;
  qcis.reserve(families.size());
  for (u32 f : families) {
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = f;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    qcis.push_back(qci);
  }
  (void)total_queues;

  // --- Feature chain ------------------------------------------------------
  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = features_.dynamic_rendering ? VK_TRUE : VK_FALSE;
  f13.synchronization2 = features_.synchronization2 ? VK_TRUE : VK_FALSE;

  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.pNext = &f13;
  f12.timelineSemaphore = features_.timeline_semaphore ? VK_TRUE : VK_FALSE;
  f12.bufferDeviceAddress = features_.buffer_device_address ? VK_TRUE : VK_FALSE;
  f12.samplerFilterMinmax = features_.sampler_filter_minmax ? VK_TRUE : VK_FALSE;
  if (features_.descriptor_indexing) {
    // Bindless: sampled textures in one big update-after-bind array.
    f12.descriptorIndexing = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
  }

  VkPhysicalDeviceVulkan11Features f11{};
  f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  f11.pNext = &f12;
  f11.multiview = VK_FALSE;
  f11.shaderDrawParameters = VK_TRUE;

  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &f11;
  f2.features.samplerAnisotropy = features_.max_anisotropy_supported ? VK_TRUE : VK_FALSE;
  f2.features.fillModeNonSolid = VK_FALSE;
  f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
  f2.features.fragmentStoresAndAtomics = VK_TRUE;
  f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

  // --- Choose API version -------------------------------------------------
  const u32 api = VK_API_VERSION_MAJOR(props_.apiVersion) > 1 ||
                          VK_API_VERSION_MINOR(props_.apiVersion) >= 3
                      ? VK_API_VERSION_1_3
                      : VK_API_VERSION_1_2;

  VkDeviceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  ci.pNext = &f2;
  ci.queueCreateInfoCount = static_cast<u32>(qcis.size());
  ci.pQueueCreateInfos = qcis.data();
  ci.enabledExtensionCount = static_cast<u32>(device_exts.size());
  ci.ppEnabledExtensionNames = device_exts.data();
  // Vulkan 1.1+ deprecates device layers entirely.
  ci.enabledLayerCount = 0;
  ci.pEnabledFeatures = nullptr;

  const VkResult r = vkCreateDevice(physical_device_, &ci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    NEO_FATAL(kGfx, "vkCreateDevice failed: {} (api {}.{})", vk_result_name(r),
              VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
  }
  volkLoadDevice(device_);

  vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
  vkGetDeviceQueue(device_, compute_family_, 0, &compute_queue_);
  if (present_family_ == ~0u) present_family_ = graphics_family_;
  vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);

  NEO_INFO(kGfx, "Logical device ready. Push constants: {} bytes. UB alignment: {}",
           features_.max_push_constants, features_.min_uniform_buffer_alignment);
}

void VulkanContext::create_surface(void* native_display, void* native_window) {
#if defined(NEO_PLATFORM_ANDROID)
  VkAndroidSurfaceCreateInfoKHR si{};
  si.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  si.pNext = nullptr;
  si.flags = 0;
  si.window = reinterpret_cast<ANativeWindow*>(native_window);
  NEO_VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &si, nullptr, &surface_));
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
  VkWaylandSurfaceCreateInfoKHR si{};
  si.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  si.display = reinterpret_cast<wl_display*>(native_display);
  si.surface = reinterpret_cast<wl_surface*>(native_window);
  NEO_VK_CHECK(vkCreateWaylandSurfaceKHR(instance_, &si, nullptr, &surface_));
#elif defined(__linux__)
  VkXlibSurfaceCreateInfoKHR si{};
  si.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
  si.dpy = reinterpret_cast<Display*>(native_display);
  si.window = static_cast<Window>(reinterpret_cast<uintptr_t>(native_window));
  NEO_VK_CHECK(vkCreateXlibSurfaceKHR(instance_, &si, nullptr, &surface_));
#else
  (void)native_display;
  (void)native_window;
  NEO_FATAL(kGfx, "Surface creation is not implemented for this platform");
#endif

  // Resolve the present family now that a surface exists.
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, graphics_family_, surface_, &supported);
  present_family_ = supported ? graphics_family_ : ~0u;
  if (present_family_ == ~0u) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fp(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, fp.data());
    for (u32 i = 0; i < count; ++i) {
      vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &supported);
      if (supported && (fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        present_family_ = i;
        break;
      }
    }
  }
  if (present_family_ == ~0u) NEO_FATAL(kGfx, "No queue family can present to this surface");

  NEO_INFO(kGfx, "Surface created; present queue family {}", present_family_);
}

bool VulkanContext::surface_supported_by_queue_family(u32 family) const {
  if (surface_ == VK_NULL_HANDLE) return false;
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, family, surface_, &supported);
  return supported == VK_TRUE;
}

VkSurfaceFormatKHR VulkanContext::choose_surface_format(
    const std::vector<VkSurfaceFormatKHR>& available) const {
  // Preference order is bandwidth-driven for mobile:
  //   RGB10A2 (no shader-side dither, 4 bytes) > BGRA8 SRGB > first reported.
  const VkFormat preferred[] = {
      VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_SRGB,
  };
  for (VkFormat want : preferred) {
    for (const auto& f : available) {
      if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    }
  }
  if (available.size() == 1 && available[0].format == VK_FORMAT_UNDEFINED) {
    VkSurfaceFormatKHR f{};
    f.format = VK_FORMAT_B8G8R8A8_SRGB;
    f.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    return f;
  }
  return available.empty() ? VkSurfaceFormatKHR{} : available[0];
}

std::optional<u32> VulkanContext::find_memory_type(u32 type_bits,
                                                   VkMemoryPropertyFlags flags) const {
  for (u32 i = 0; i < mem_props_.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (mem_props_.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return std::nullopt;
}

void VulkanContext::wait_idle() { vkDeviceWaitIdle(device_); }

void VulkanContext::set_object_name(u64 handle, VkObjectType type, const char* name) const {
  if (!validation_enabled_) return;
  VkDebugUtilsObjectNameInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  info.objectType = type;
  info.objectHandle = handle;
  info.pObjectName = name;
  auto fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
      vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
  if (fn) fn(device_, &info);
}

void VulkanContext::begin_debug_label(VkCommandBuffer cmd, const char* name, vec4 color) const {
  if (!validation_enabled_) return;
  VkDebugUtilsLabelEXT label{};
  label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
  label.pLabelName = name;
  label.color[0] = color.r;
  label.color[1] = color.g;
  label.color[2] = color.b;
  label.color[3] = color.a;
  auto fn = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
      vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
  if (fn) fn(cmd, &label);
}

void VulkanContext::end_debug_label(VkCommandBuffer cmd) const {
  if (!validation_enabled_) return;
  auto fn = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
      vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
  if (fn) fn(cmd);
}

} // namespace neo
