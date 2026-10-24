// ===========================================================================
//  hello-world — desktop player used for engine development.
//
//  Deliberately tiny: create a window, a renderer, a scene with a sun and a
//  spinning set of cubes, then run. This is the harness every renderer change
//  is validated against.
// ===========================================================================
#include "neo/core/Log.h"
#include "neo/editor/EditorShell.h"
#include "neo/platform/Platform.h"
#include "neo/render/Camera.h"
#include "neo/render/GltfLoader.h"
#include "neo/render/Renderer.h"
#include "neo/render/Resources.h"
#include "neo/scene/Scene.h"
#include "neo/script/ScriptHost.h"

#include <cstdio>
#include <vector>

using namespace neo;

namespace {

struct Spinner {
  EntityHandle entity;
  f32 speed = 1.0f;
  vec3 axis{0.0f, 1.0f, 0.0f};
};

class FlyCamera {
public:
  void update(Camera& camera, const InputState& input, f32 dt) {
    // Hold right mouse to look, WASD to move, Shift to sprint.
    if (input.pointer_down) {
      yaw_ -= input.mouse_delta.x * 0.12f;
      pitch_ -= input.mouse_delta.y * 0.12f;
      pitch_ = clamp(pitch_, -89.0f, 89.0f);
    }

    quat rot = glm::angleAxis(yaw_ * kDegToRad, vec3(0, 1, 0)) *
               glm::angleAxis(pitch_ * kDegToRad, vec3(1, 0, 0));
    camera.set_rotation(rot);

    const f32 speed = (input.is_down(Key::Shift) ? 24.0f : 8.0f) * dt;
    vec3 move{0.0f};
    if (input.is_down(Key::W))
      move += camera.forward();
    if (input.is_down(Key::S))
      move -= camera.forward();
    if (input.is_down(Key::D))
      move += camera.right();
    if (input.is_down(Key::A))
      move -= camera.right();
    if (input.is_down(Key::E))
      move += vec3(0, 1, 0);
    if (input.is_down(Key::Q))
      move -= vec3(0, 1, 0);

    if (glm::length(move) > 0.0f) {
      camera.set_position(camera.position() + glm::normalize(move) * speed);
    }
  }

  void set_angles(f32 yaw, f32 pitch) {
    yaw_ = yaw;
    pitch_ = pitch;
  }

private:
  f32 yaw_ = 0.0f;   // degrees
  f32 pitch_ = 0.0f; // degrees
};

} // namespace

int main(int argc, char** argv) {
  log::set_min_level(LogLevel::Info);

  WindowDesc wd{};
  wd.title = "Neo-Asis Engine 3D — Hello World";
  wd.width = 1600;
  wd.height = 900;
  wd.vsync = true;

  auto platform = Platform::create(wd);
  if (!platform) {
    NEO_ERROR(kPlatform, "Could not create a window. Exiting.");
    return 1;
  }

  const NativeWindow native = platform->native_window();

  RendererDesc rd{};
  rd.width = platform->width();
  rd.height = platform->height();
  rd.vsync = platform->vsync();
  rd.app_name = "Neo-Asis Hello World";
#if defined(NEO_DEBUG)
  rd.enable_validation = true;
#endif
  rd.native_window = native.window;
  rd.native_display = native.display;

  auto renderer = Renderer::create(rd);
  if (!renderer) {
    NEO_ERROR(kRender, "Renderer creation failed");
    return 1;
  }

  platform->set_event_callback([&](AppEvent event, u32 w, u32 h) {
    if (event == AppEvent::Resize)
      renderer->resize(w, h);
  });

  // --- Scene ---------------------------------------------------------------
  Scene scene;
  scene.sky().elevation_degrees = 14.0f;
  scene.sky().azimuth_degrees = 205.0f;
  scene.sky().turbidity = 2.8f;
  scene.sky().cloud_coverage = 0.45f;

  LightDesc sun{};
  sun.type = LightType::Directional;
  sun.direction = -scene.sun_direction();
  sun.color = vec3(1.0f, 0.96f, 0.9f);
  sun.intensity = 5.0f;
  sun.casts_shadow = true;
  scene.add_light(sun);

  LightDesc fill{};
  fill.type = LightType::Point;
  fill.position = vec3(2.0f, 2.5f, 1.0f);
  fill.color = vec3(0.3f, 0.6f, 1.0f);
  fill.intensity = 12.0f;
  fill.range = 12.0f;
  scene.add_light(fill);

  std::vector<Spinner> spinners;
  for (int i = 0; i < 9; ++i) {
    RenderableDesc r{};
    const f32 angle = static_cast<f32>(i) * kTau / 9.0f;
    r.transform.position =
        vec3(std::cos(angle) * 3.0f, 0.5f + 0.3f * (i % 3), std::sin(angle) * 3.0f);
    r.transform.scale = vec3(0.6f);
    const EntityHandle e = scene.add_renderable(r);
    spinners.push_back(Spinner{e, 0.4f + 0.1f * static_cast<f32>(i), vec3(0, 1, 0)});
  }

  Camera camera;
  camera.set_perspective(55.0f,
                         static_cast<f32>(platform->width()) / static_cast<f32>(platform->height()),
                         0.1f, 2000.0f);
  camera.set_position(vec3(0.0f, 3.5f, 9.0f));
  camera.look_at(vec3(0.0f, 3.5f, 9.0f), vec3(0.0f, 1.0f, 0.0f));

  FlyCamera fly;
  fly.set_angles(0.0f, -12.0f);

  // --- Managed gameplay scripts (optional) --------------------------------
  // The assembly is loaded at runtime; if it is absent the engine runs exactly
  // as before. Build it with ./scripting/build-scripts.sh desktop.
  std::unique_ptr<ScriptHost> scripts;
  {
    ScriptDesc sd{};
    sd.library_path = "libneoasis_scripts.so";
    sd.hot_reload = true;
    scripts = ScriptHost::create(sd);
    if (!scripts->load(scene)) {
      NEO_INFO(kScript, "No managed scripts loaded (engine-only mode)");
    }
  }

  // --- Editor shell (M5 foundation) ---------------------------------------
  EditorShell editor;
  editor.attach(&scene);
  editor.play(); // start in play mode so the sample behaves as before

  // --- Loop ----------------------------------------------------------------
  f64 last = platform->now_seconds();
  u32 frames = 0;
  f64 fps_accum = 0.0;

  while (platform->pump_events()) {
    const f64 now = platform->now_seconds();
    const f32 dt = static_cast<f32>(std::min(now - last, 0.1));
    last = now;

    fly.update(camera, platform->input(), dt);

    // Spin the props so the frame counter is visibly doing something.
    for (Spinner& s : spinners) {
      Transform& t = scene.transform(s.entity);
      t.rotation = glm::angleAxis(dt * s.speed, s.axis) * t.rotation;
    }

    if (!renderer->begin_frame())
      continue;
    renderer->render(scene, camera, dt);
    renderer->end_frame();
    platform->present_frame();

    // Per-second stats line, plus a quality-tier report on the first frame.
    fps_accum += dt;
    ++frames;
    if (fps_accum >= 1.0) {
      const FrameStats& st = renderer->stats();
      std::printf("fps %5.1f | draws %4u | visible %5u | lights %3u | res %.2f | gpu %.1f MiB\n",
                  static_cast<f64>(frames) / fps_accum, st.draw_calls, st.visible_instances,
                  st.visible_lights, static_cast<f64>(st.resolution_scale),
                  static_cast<f64>(st.gpu_memory_bytes) / (1024.0 * 1024.0));
      frames = 0;
      fps_accum = 0.0;
    }
  }

  if (scripts)
    scripts->unload(scene);
  renderer.reset();
  return 0;
}
