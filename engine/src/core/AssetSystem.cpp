#include "neo/core/AssetSystem.h"
#include "neo/core/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#if defined(NEO_PLATFORM_ANDROID)
#  include <android/asset_manager.h>
#  include <android/asset_manager_jni.h>
#endif

namespace neo {

AssetSystem& AssetSystem::instance() {
  static AssetSystem s;
  return s;
}

AssetType AssetSystem::type_from_extension(std::string_view ext) {
  if (ext == ".spv" || ext == ".glsl") return AssetType::Shader;
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".ktx2" ||
      ext == ".astc" || ext == ".basis") {
    return AssetType::Texture;
  }
  if (ext == ".gltf" || ext == ".glb" || ext == ".mesh") return AssetType::Mesh;
  if (ext == ".nmat") return AssetType::Material;
  if (ext == ".nscene") return AssetType::Scene;
  if (ext == ".wav" || ext == ".ogg") return AssetType::Audio;
  if (ext == ".cs" || ext == ".wasm") return AssetType::Script;
  return AssetType::Unknown;
}

void AssetSystem::set_desktop_root(std::filesystem::path root) {
  std::lock_guard lock(mutex_);
  root_ = std::move(root);
  cache_root_ = root_ / "cache";
  std::error_code ec;
  std::filesystem::create_directories(cache_root_, ec);
  NEO_INFO(kAsset, "Asset root: {}", root_.string());
}

#if defined(NEO_PLATFORM_ANDROID)
void AssetSystem::set_android_assets(void* asset_manager, std::string_view external_root) {
  std::lock_guard lock(mutex_);
  asset_manager_ = asset_manager;
  root_ = std::filesystem::path(std::string(external_root));
  cache_root_ = root_ / "cache";
  std::error_code ec;
  std::filesystem::create_directories(cache_root_, ec);
  NEO_INFO(kAsset, "Android assets mounted; writable root: {}", cache_root_.string());
}
#endif

std::vector<u8> AssetSystem::read_binary(std::string_view virtual_path) const {
  std::lock_guard lock(mutex_);
  const std::string path(virtual_path);

#if defined(NEO_PLATFORM_ANDROID)
  if (asset_manager_) {
    AAsset* a = AAssetManager_open(static_cast<AAssetManager*>(asset_manager_), path.c_str(),
                                   AASSET_MODE_BUFFER);
    if (a) {
      const off64_t len = AAsset_getLength64(a);
      std::vector<u8> out(static_cast<usize>(len));
      const int read = AAsset_read(a, out.data(), static_cast<size_t>(len));
      AAsset_close(a);
      if (read == static_cast<int>(len)) return out;
      NEO_ERROR(kAsset, "Short read on asset {}", path);
      return {};
    }
    // Fall through: cooked content may live in the writable root.
  }
#endif

  const std::filesystem::path full = root_ / path;
  std::ifstream in(full, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const auto size = static_cast<usize>(in.tellg());
  in.seekg(0);
  std::vector<u8> out(size);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
  return in ? out : std::vector<u8>{};
}

std::string AssetSystem::read_text(std::string_view virtual_path) const {
  const std::vector<u8> bytes = read_binary(virtual_path);
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool AssetSystem::exists(std::string_view virtual_path) const {
  return !read_binary(virtual_path).empty();
}

bool AssetSystem::write_binary(std::string_view path, const std::vector<u8>& data) const {
  std::lock_guard lock(mutex_);
  const std::filesystem::path full = cache_root_ / std::string(path);
  std::error_code ec;
  std::filesystem::create_directories(full.parent_path(), ec);
  std::ofstream out(full, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return out.good();
}

bool AssetSystem::write_text(std::string_view path, std::string_view text) const {
  const std::vector<u8> bytes(text.begin(), text.end());
  return write_binary(path, bytes);
}

std::vector<AssetMetadata> AssetSystem::list(std::string_view virtual_dir) const {
  std::lock_guard lock(mutex_);
  std::vector<AssetMetadata> out;
  const std::filesystem::path dir = root_ / std::string(virtual_dir);

  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return out;

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    AssetMetadata m{};
    m.virtual_path = (std::filesystem::path(std::string(virtual_dir)) /
                      entry.path().filename()).generic_string();
    m.type = type_from_extension(entry.path().extension().string());
    m.size_bytes = entry.file_size(ec);
    out.push_back(std::move(m));
  }
  std::sort(out.begin(), out.end(), [](const AssetMetadata& a, const AssetMetadata& b) {
    return a.virtual_path < b.virtual_path;
  });
  return out;
}

} // namespace neo
