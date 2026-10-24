// ===========================================================================
//  ScriptHost — loads and drives a managed gameplay assembly.
//
//  The C# project is compiled ahead of time with NativeAOT into a shared
//  library (libneoasis_scripts.so). The engine dlopen()s it and resolves three
//  exports. There is no Mono, no JIT and no reflection at runtime on device,
//  which is what makes it viable on Android: startup is a plain dlopen.
//
//  Desktop builds additionally support hot reload: watch the .so mtime, unload
//  and reload when it changes.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"
#include "neo/script/ScriptApi.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace neo {

class Scene;

enum class ScriptStatus : u8 { NotLoaded = 0, Loaded, Failed, AbiMismatch };

struct ScriptDesc {
  std::string library_path; // e.g. "libneoasis_scripts.so"
  u32 abi_version = NEO_SCRIPT_ABI_VERSION;
  bool hot_reload = false; // desktop only
  f32 hot_reload_interval = 0.5f;
};

class ScriptHost {
public:
  static std::unique_ptr<ScriptHost> create(const ScriptDesc& desc);
  ~ScriptHost();

  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  // Loads the assembly and calls neo_script_init. Safe to call repeatedly:
  // a failed load retries on the next attempt().
  bool load(Scene& scene);
  void unload(Scene& scene);

  // Called once per frame. `delta` is wall-clock seconds.
  void update(Scene& scene, f32 delta);

  // Polls the library mtime; reloads when it changes (hot reload builds).
  void poll_hot_reload(Scene& scene, f32 delta);

  [[nodiscard]] ScriptStatus status() const { return status_; }
  [[nodiscard]] bool running() const { return status_ == ScriptStatus::Loaded; }
  [[nodiscard]] std::string_view last_error() const { return last_error_; }
  [[nodiscard]] u32 script_count() const { return script_count_; }
  [[nodiscard]] f64 total_update_ms() const { return total_update_ms_; }

private:
  ScriptHost() = default;

  bool resolve_symbols();
  void build_host_api();
  void log_managed(i32 level, const char* message);

  // Static trampolines the host API points at. They recover the ScriptHost
  // instance from the world pointer, so the ABI needs no user-data slot.
  static NeoHostApi make_api();
  static ScriptHost* from_world(NeoWorld* world);

  ScriptDesc desc_{};
  ScriptStatus status_ = ScriptStatus::NotLoaded;
  std::string last_error_;

  void* library_ = nullptr;
  NeoScriptInitFn init_ = nullptr;
  NeoScriptUpdateFn update_ = nullptr;
  NeoScriptShutdownFn shutdown_ = nullptr;

  NeoHostApi api_{};
  Scene* bound_scene_ = nullptr;
  u32 script_count_ = 0;
  f64 total_update_ms_ = 0.0;
  f32 reload_timer_ = 0.0f;
  std::filesystem::file_time_type library_mtime_{};
};

} // namespace neo
