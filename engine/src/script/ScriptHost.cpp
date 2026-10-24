#include "neo/script/ScriptHost.h"

#include "neo/core/Log.h"
#include "neo/render/Renderer.h"
#include "neo/scene/Scene.h"

#include <chrono>
#include <cstring>

#if defined(NEO_PLATFORM_ANDROID)
#  include <dlfcn.h>
#else
#  include <dlfcn.h>
#endif

namespace neo {

namespace {

// The world pointer the managed side sees is the Scene itself. Scripts never
// dereference it; every access goes through a host function, so the C# side
// cannot corrupt engine memory.
inline NeoWorld* to_world(Scene* s) {
  return reinterpret_cast<NeoWorld*>(s);
}
inline Scene* to_scene(NeoWorld* w) {
  return reinterpret_cast<Scene*>(w);
}

f64 g_start_time = 0.0;
f64 now_seconds() {
  using clock = std::chrono::steady_clock;
  const f64 t = std::chrono::duration<f64>(clock::now().time_since_epoch()).count();
  if (g_start_time == 0.0)
    g_start_time = t;
  return t - g_start_time;
}

ScriptHost* g_active_host = nullptr;
f32 g_frame_delta = 0.0f;
u64 g_frame_index = 0;

} // namespace

// ===========================================================================
//  Host API trampolines
//  These are plain C functions; they are assigned into NeoHostApi.
// ===========================================================================
namespace {

void host_log(i32 level, const char* message) {
  const char* text = message ? message : "(null)";
  switch (level) {
  case 0:
    NEO_DEBUG(kScript, "[cs] {}", text);
    break;
  case 1:
    NEO_DEBUG(kScript, "[cs] {}", text);
    break;
  case 3:
    NEO_WARN(kScript, "[cs] {}", text);
    break;
  case 4:
    NEO_ERROR(kScript, "[cs] {}", text);
    break;
  default:
    NEO_INFO(kScript, "[cs] {}", text);
    break;
  }
}

double host_time_seconds() {
  return now_seconds();
}
float host_delta_seconds() {
  return g_frame_delta;
}
u64 host_frame_index() {
  return g_frame_index;
}

u32 host_entity_is_alive(NeoWorld* w, u32 entity) {
  Scene* s = to_scene(w);
  if (!s)
    return 0;
  const EntityHandle h = EntityHandle::make(entity, 1);
  return s->entity_count() > entity ? (h.valid() ? 1u : 0u) : 0u;
}

u32 host_entity_find(NeoWorld* w, const char* name) {
  (void)w;
  (void)name;
  // Name lookup lands with the entity registry rework (M5). Returning 0 is a
  // defined "not found" and scripts already handle it.
  return 0;
}

void host_transform_get(NeoWorld* w, u32 entity, float* out7) {
  Scene* s = to_scene(w);
  if (!s || !out7)
    return;
  const Transform& t = s->transform(EntityHandle::make(entity, 1));
  out7[0] = t.position.x;
  out7[1] = t.position.y;
  out7[2] = t.position.z;
  out7[3] = t.rotation.x;
  out7[4] = t.rotation.y;
  out7[5] = t.rotation.z;
  out7[6] = t.rotation.w;
}

void host_transform_set(NeoWorld* w, u32 entity, const float* in7) {
  Scene* s = to_scene(w);
  if (!s || !in7)
    return;
  Transform& t = s->transform(EntityHandle::make(entity, 1));
  t.position = vec3(in7[0], in7[1], in7[2]);
  t.rotation = quat(in7[6], in7[3], in7[4], in7[5]);
}

void host_scale_get(NeoWorld* w, u32 entity, float* out3) {
  Scene* s = to_scene(w);
  if (!s || !out3)
    return;
  const vec3 sc = s->transform(EntityHandle::make(entity, 1)).scale;
  out3[0] = sc.x;
  out3[1] = sc.y;
  out3[2] = sc.z;
}

void host_scale_set(NeoWorld* w, u32 entity, const float* in3) {
  Scene* s = to_scene(w);
  if (!s || !in3)
    return;
  s->transform(EntityHandle::make(entity, 1)).scale = vec3(in3[0], in3[1], in3[2]);
}

u32 host_key_down(i32 key) {
  (void)key;
  // Input reaches scripts once the platform input queue is bridged (M5).
  return 0;
}

void host_pointer_position(float* out2) {
  if (!out2)
    return;
  out2[0] = 0.0f;
  out2[1] = 0.0f;
}

u32 host_pointer_down() {
  return 0;
}

void host_set_sun_elevation(NeoWorld* w, f32 degrees) {
  if (Scene* s = to_scene(w))
    s->sky().elevation_degrees = degrees;
}

void host_set_cloud_coverage(NeoWorld* w, f32 coverage) {
  if (Scene* s = to_scene(w))
    s->sky().cloud_coverage = clamp(coverage, 0.0f, 1.0f);
}

void host_request_ibl_rebake(NeoWorld* w) {
  (void)w;
}

} // namespace

NeoHostApi ScriptHost::make_api() {
  NeoHostApi api{};
  std::memset(&api, 0, sizeof(api));
  api.abi_version = NEO_SCRIPT_ABI_VERSION;
  api.log = &host_log;
  api.time_seconds = &host_time_seconds;
  api.delta_seconds = &host_delta_seconds;
  api.frame_index = &host_frame_index;
  api.entity_is_alive = &host_entity_is_alive;
  api.entity_find = &host_entity_find;
  api.transform_get = &host_transform_get;
  api.transform_set = &host_transform_set;
  api.scale_get = &host_scale_get;
  api.scale_set = &host_scale_set;
  api.key_down = &host_key_down;
  api.pointer_position = &host_pointer_position;
  api.pointer_down = &host_pointer_down;
  api.set_sun_elevation = &host_set_sun_elevation;
  api.set_cloud_coverage = &host_set_cloud_coverage;
  api.request_ibl_rebake = &host_request_ibl_rebake;
  return api;
}

ScriptHost* ScriptHost::from_world(NeoWorld* world) {
  (void)world;
  return g_active_host;
}

// ===========================================================================
//  Lifetime
// ===========================================================================
std::unique_ptr<ScriptHost> ScriptHost::create(const ScriptDesc& desc) {
  auto h = std::unique_ptr<ScriptHost>(new ScriptHost());
  h->desc_ = desc;
  h->api_ = make_api();
  return h;
}

ScriptHost::~ScriptHost() {
  if (library_) {
    if (shutdown_ && bound_scene_)
      shutdown_(to_world(bound_scene_));
    dlclose(library_);
    library_ = nullptr;
  }
  if (g_active_host == this)
    g_active_host = nullptr;
}

bool ScriptHost::resolve_symbols() {
  init_ = reinterpret_cast<NeoScriptInitFn>(dlsym(library_, "neo_script_init"));
  update_ = reinterpret_cast<NeoScriptUpdateFn>(dlsym(library_, "neo_script_update"));
  shutdown_ = reinterpret_cast<NeoScriptShutdownFn>(dlsym(library_, "neo_script_shutdown"));

  if (!init_) {
    last_error_ = "neo_script_init not found in " + desc_.library_path;
    NEO_ERROR(kScript, "{}", last_error_);
    return false;
  }
  if (!update_) {
    last_error_ = "neo_script_update not found in " + desc_.library_path;
    NEO_ERROR(kScript, "{}", last_error_);
    return false;
  }
  return true;
}

bool ScriptHost::load(Scene& scene) {
  if (status_ == ScriptStatus::Loaded)
    return true;

  const std::string path = desc_.library_path;
  library_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_) {
    last_error_ = dlerror() ? dlerror() : "dlopen failed";
    status_ = ScriptStatus::Failed;
    NEO_WARN(kScript, "Could not load '{}': {}", path, last_error_);
    return false;
  }

  if (!resolve_symbols()) {
    dlclose(library_);
    library_ = nullptr;
    status_ = ScriptStatus::Failed;
    return false;
  }

  g_active_host = this;
  bound_scene_ = &scene;

  const i32 result = init_(to_world(&scene), &api_);
  if (result != 0) {
    last_error_ = "neo_script_init returned " + std::to_string(result) +
                  (result == 1 ? " (ABI version mismatch)" : "");
    status_ = result == 1 ? ScriptStatus::AbiMismatch : ScriptStatus::Failed;
    NEO_ERROR(kScript, "{}", last_error_);
    dlclose(library_);
    library_ = nullptr;
    g_active_host = nullptr;
    return false;
  }

  status_ = ScriptStatus::Loaded;
  script_count_ = 1;

  std::error_code ec;
  library_mtime_ = std::filesystem::last_write_time(path, ec);

  NEO_INFO(kScript, "Script assembly loaded: {} (ABI {})", path, api_.abi_version);
  return true;
}

void ScriptHost::unload(Scene& scene) {
  if (!library_) {
    status_ = ScriptStatus::NotLoaded;
    return;
  }
  if (shutdown_)
    shutdown_(to_world(&scene));
  dlclose(library_);
  library_ = nullptr;
  init_ = nullptr;
  update_ = nullptr;
  shutdown_ = nullptr;
  if (g_active_host == this)
    g_active_host = nullptr;
  bound_scene_ = nullptr;
  status_ = ScriptStatus::NotLoaded;
  NEO_INFO(kScript, "Script assembly unloaded");
}

void ScriptHost::update(Scene& scene, f32 delta) {
  if (status_ != ScriptStatus::Loaded || !update_)
    return;

  const auto t0 = std::chrono::steady_clock::now();
  g_frame_delta = delta;
  ++g_frame_index;

  update_(to_world(&scene), delta);

  const auto t1 = std::chrono::steady_clock::now();
  total_update_ms_ = std::chrono::duration<f64, std::milli>(t1 - t0).count() + total_update_ms_;
}

void ScriptHost::poll_hot_reload(Scene& scene, f32 delta) {
  if (!desc_.hot_reload || !library_)
    return;
  reload_timer_ += delta;
  if (reload_timer_ < desc_.hot_reload_interval)
    return;
  reload_timer_ = 0.0f;

  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(desc_.library_path, ec);
  if (ec || mtime == library_mtime_)
    return;

  NEO_INFO(kScript, "Script assembly changed on disk — hot reloading");
  library_mtime_ = mtime;
  unload(scene);
  load(scene);
}

} // namespace neo
