// ===========================================================================
//  AssetSystem — virtual file layer.
//
//  Desktop : plain filesystem rooted at the executable's asset directory.
//  Android : APK assets through AAssetManager, plus an external writable root
//            (filesDir) for caches, screenshots and cooked content.
//
//  Read paths are hot (shader load, texture stream), so reads avoid copies
//  where the platform allows it.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neo {

enum class AssetType : u8 { Unknown = 0, Shader, Texture, Mesh, Material, Scene, Audio, Script };

struct AssetMetadata {
  std::string virtual_path;
  AssetType type = AssetType::Unknown;
  u64 size_bytes = 0;
};

class AssetSystem {
public:
  static AssetSystem& instance();

  // --- Platform hooks ----------------------------------------------------
#if defined(NEO_PLATFORM_ANDROID)
  void set_android_assets(void* asset_manager, std::string_view external_root);
#endif
  void set_desktop_root(std::filesystem::path root);

  // --- Reading -----------------------------------------------------------
  [[nodiscard]] std::vector<u8> read_binary(std::string_view virtual_path) const;
  [[nodiscard]] std::string read_text(std::string_view virtual_path) const;
  [[nodiscard]] bool exists(std::string_view virtual_path) const;

  // --- Writing (cache / user content; APK assets are read-only) ----------
  bool write_binary(std::string_view path, const std::vector<u8>& data) const;
  bool write_text(std::string_view path, std::string_view text) const;

  // --- Discovery ---------------------------------------------------------
  [[nodiscard]] std::vector<AssetMetadata> list(std::string_view virtual_dir) const;

  // --- Caches ------------------------------------------------------------
  [[nodiscard]] std::filesystem::path cache_root() const { return cache_root_; }

  [[nodiscard]] static AssetType type_from_extension(std::string_view ext);

private:
  AssetSystem() = default;

  std::filesystem::path root_;
  std::filesystem::path cache_root_;
  mutable std::mutex mutex_;

#if defined(NEO_PLATFORM_ANDROID)
  void* asset_manager_ = nullptr;
#endif
};

inline AssetSystem& assets() { return AssetSystem::instance(); }

} // namespace neo
