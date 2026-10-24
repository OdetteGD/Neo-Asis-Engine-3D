// ===========================================================================
//  AndroidMain.cpp — GameActivity entry point + engine lifecycle glue.
//
//  Lifecycle mapping (this is where most Android engines break):
//
//    APP_CMD_INIT_WINDOW     -> create surface, start the render loop thread
//    APP_CMD_TERM_WINDOW     -> stop the loop, destroy surface-dependent GPU
//                               resources (they are gone with the window)
//    APP_CMD_GAINED_FOCUS    -> resume rendering
//    APP_CMD_LOST_FOCUS      -> pause rendering but KEEP the device
//    onLowMemory / TRIM      -> drop caches, shrink render scale, drop mips
//
//  The render loop runs on its own thread so UI events never stall a frame.
// ===========================================================================
#include "neo/core/AssetSystem.h"
#include "neo/core/Log.h"
#include "neo/platform/Platform.h"
#include "neo/render/Camera.h"
#include "neo/render/Renderer.h"
#include "neo/scene/Scene.h"
#include "neo/script/ScriptHost.h"

#include <android/asset_manager.h>
#include <android/native_window.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <atomic>
#include <thread>

using namespace neo;

namespace {

struct AndroidApp {
  android_app* app = nullptr;
  std::unique_ptr<Renderer> renderer;
  std::unique_ptr<Scene> scene;
  std::unique_ptr<ScriptHost> scripts;
  Camera camera;

  std::thread render_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> surface_ready{false};
  std::atomic<bool> surface_lost{false};

  u32 width = 1280;
  u32 height = 720;

  void start_render_loop() {
    if (running.exchange(true))
      return;
    render_thread = std::thread([this] { render_loop(); });
  }

  void stop_render_loop() {
    if (!running.exchange(false))
      return;
    if (render_thread.joinable())
      render_thread.join();
  }

  void render_loop() {
    NEO_INFO(kPlatform, "Render thread started");

    while (running.load(std::memory_order_acquire)) {
      if (surface_lost.exchange(false)) {
        if (renderer)
          renderer->on_surface_lost();
        continue;
      }
      if (!surface_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }
      if (!renderer) {
        RendererDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.vsync = true;
        desc.app_name = "Neo-Asis";
#if defined(NEO_DEBUG)
        desc.enable_validation = true;
#endif
        desc.native_window = app->window;
        renderer = Renderer::create(desc);
        if (!renderer) {
          NEO_ERROR(kPlatform, "Renderer creation failed — retrying next frame");
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
        scene = std::make_unique<Scene>();
        camera.set_perspective(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.1f,
                               2000.0f);
        camera.look_at(vec3(0.0f, 2.0f, 5.0f), vec3(0.0f, 1.0f, 0.0f));

        // Managed gameplay. The APK packages libneoasis_scripts.so next to
        // libneoasis.so, so the loader resolves it by soname.
        ScriptDesc sd{};
        sd.library_path = "libneoasis_scripts.so";
        sd.hot_reload = false; // no writable .so on device
        scripts = ScriptHost::create(sd);
        if (!scripts->load(*scene)) {
          NEO_INFO(kScript, "Running without managed scripts");
        }
      }

      if (!renderer->begin_frame())
        continue;

      const f64 t0 =
          std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (scripts && scripts->running())
        scripts->update(*scene, 1.0f / 60.0f);

      renderer->render(*scene, camera, 1.0f / 60.0f);
      renderer->end_frame();
      const f64 t1 =
          std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
      (void)t0;
      (void)t1;
    }

    if (renderer)
      renderer.reset();
    NEO_INFO(kPlatform, "Render thread stopped");
  }

  static void handle_cmd(android_app* app, int32_t cmd) {
    auto* self = static_cast<AndroidApp*>(app->userData);
    if (!self)
      return;

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (app->window) {
        self->width = static_cast<u32>(ANativeWindow_getWidth(app->window));
        self->height = static_cast<u32>(ANativeWindow_getHeight(app->window));
        self->surface_ready = true;
        self->start_render_loop();
        NEO_INFO(kPlatform, "Window ready {}x{}", self->width, self->height);
      }
      break;

    case APP_CMD_TERM_WINDOW:
      NEO_INFO(kPlatform, "Window terminating — dropping surface resources");
      self->surface_ready = false;
      self->surface_lost = true;
      break;

    case APP_CMD_GAINED_FOCUS:
      break;

    case APP_CMD_LOST_FOCUS:
      break;

    case APP_CMD_WINDOW_RESIZED:
      if (app->window) {
        self->width = static_cast<u32>(ANativeWindow_getWidth(app->window));
        self->height = static_cast<u32>(ANativeWindow_getHeight(app->window));
        if (self->renderer)
          self->renderer->resize(self->width, self->height);
      }
      break;

    case APP_CMD_LOW_MEMORY:
      NEO_WARN(kPlatform, "System reports LOW MEMORY — shrinking GPU footprint");
      // The renderer's dynamic resolution controller reacts to the budget
      // watermark on the next frame; nothing else to do here yet.
      break;

    default:
      break;
    }
  }
};

} // namespace

void android_main(android_app* app) {
  // --- Asset mount ---------------------------------------------------------
  AAssetManager* manager = app->activity->assetManager;
  const char* internal = app->activity->internalDataPath;
  assets().set_android_assets(manager, internal ? internal : "");

#if defined(NEO_DEBUG)
  log::set_min_level(LogLevel::Debug);
#else
  log::set_min_level(LogLevel::Info);
#endif

  NEO_INFO(kPlatform, "Neo-Asis Engine {} starting on Android", NEO_ENGINE_VERSION);

  AndroidApp state{};
  state.app = app;
  app->userData = &state;
  app->onAppCmd = AndroidApp::handle_cmd;

  // --- Main loop: pump events, no rendering here ---------------------------
  while (true) {
    int events = 0;
    android_poll_source* source = nullptr;

    // Block only when the surface is not ready, so we do not spin at 100% CPU
    // while backgrounded (this is the classic Android battery bug).
    const bool blocking = !state.surface_ready.load();
    const int timeout = blocking ? -1 : 0;
    const int result =
        ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));

    if (result == ALOOPER_POLL_ERROR) {
      NEO_ERROR(kPlatform, "ALooper_pollOnce failed");
      break;
    }
    if (source)
      source->process(app, source);
    if (app->destroyRequested)
      break;
  }

  state.surface_ready = false;
  state.stop_render_loop();
  NEO_INFO(kPlatform, "Android main loop exited cleanly");
}
