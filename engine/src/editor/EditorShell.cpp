#include "neo/editor/EditorShell.h"

#include "neo/core/Log.h"
#include "neo/render/Renderer.h"

#include <fmt/format.h>

#include <algorithm>
#include <sstream>

namespace neo {

namespace {
constexpr usize kMaxConsoleLines = 200;
}

void EditorShell::print(std::string line) {
  NEO_INFO(kEditor, "{}", line);
  console_.push_back(std::move(line));
  if (console_.size() > kMaxConsoleLines) {
    console_.erase(console_.begin(), console_.begin() + (console_.size() - kMaxConsoleLines));
  }
}

void EditorShell::capture_snapshot() {
  if (!scene_)
    return;
  snapshot_.transforms.clear();
  snapshot_.lights.clear();

  // entity_count() is 1-based (slot 0 is the null handle).
  for (u32 i = 1; i < scene_->entity_count(); ++i) {
    snapshot_.transforms.push_back(scene_->transform(EntityHandle::make(i, 1)));
  }
  snapshot_.lights = scene_->lights();
  snapshot_.sky = scene_->sky();
  snapshot_valid_ = true;
}

void EditorShell::restore_snapshot() {
  if (!scene_ || !snapshot_valid_)
    return;

  const usize count = std::min<usize>(snapshot_.transforms.size(), scene_->entity_count());
  for (usize i = 1; i <= count && i <= snapshot_.transforms.size(); ++i) {
    if (i - 1 < snapshot_.transforms.size()) {
      scene_->transform(EntityHandle::make(static_cast<u32>(i), 1)) = snapshot_.transforms[i - 1];
    }
  }

  // Lights and sky are copied wholesale.
  const std::vector<LightDesc>& lights = snapshot_.lights;
  for (usize i = 0; i < lights.size() && i < scene_->lights().size(); ++i) {
    scene_->set_light(EntityHandle::make(static_cast<u32>(i + 1), 1), lights[i]);
  }
  scene_->sky() = snapshot_.sky;

  print(fmt::format("editor: scene restored from snapshot ({} transforms, {} lights)",
                    snapshot_.transforms.size(), snapshot_.lights.size()));
}

void EditorShell::play() {
  // Capture before the first simulated frame so stop() can rewind exactly.
  if (!snapshot_valid_)
    capture_snapshot();
  state_ = PlayState::Playing;
  play_elapsed_ = 0.0;
  play_frames_ = 0;
  print("editor: PLAY");
}

void EditorShell::pause() {
  if (state_ == PlayState::Playing) {
    state_ = PlayState::Paused;
    print(fmt::format("editor: PAUSED at t={:.2f}s", play_elapsed_));
  }
}

void EditorShell::stop() {
  const bool was_live = (state_ != PlayState::Stopped);
  state_ = PlayState::Stopped;
  step_requested_ = false;
  // Rewind: play mode must never mutate the authored scene permanently.
  if (was_live && snapshot_valid_)
    restore_snapshot();
  print("editor: STOPPED");
}

void EditorShell::step() {
  state_ = PlayState::Paused;
  step_requested_ = true;
  print("editor: STEP");
}

void EditorShell::toggle_pause() {
  if (state_ == PlayState::Playing) {
    pause();
  } else if (state_ == PlayState::Paused || state_ == PlayState::Stopped) {
    play();
  }
}

bool EditorShell::tick(f32 delta) {
  switch (state_) {
  case PlayState::Playing:
    play_elapsed_ += delta;
    ++play_frames_;
    return true;

  case PlayState::StepOnce:
  case PlayState::Paused:
    if (step_requested_) {
      step_requested_ = false;
      play_elapsed_ += delta;
      ++play_frames_;
      return true; // exactly one frame of simulation
    }
    return false;

  case PlayState::Stopped:
  default:
    return false;
  }
}

void EditorShell::select(EntityHandle e) {
  selection_.entity = e;
  selection_.valid = e.valid();
  if (selection_.valid)
    print(fmt::format("editor: selected entity {}", e.index()));
}

std::vector<std::string> EditorShell::hierarchy_lines() const {
  std::vector<std::string> lines;
  if (!scene_)
    return lines;

  lines.push_back(fmt::format("Scene ({} entities)", scene_->entity_count()));
  for (u32 i = 1; i < scene_->entity_count(); ++i) {
    const EntityHandle h = EntityHandle::make(i, 1);
    const Transform& t = scene_->transform(h);
    const RenderableDesc* r = const_cast<Scene*>(scene_)->renderable(h);
    lines.push_back(
        fmt::format("  [{}] {}  pos=({:.2f}, {:.2f}, {:.2f}){}", i, r ? "renderable" : "entity",
                    t.position.x, t.position.y, t.position.z,
                    (selection_.valid && selection_.entity.index() == i) ? "   <- selected" : ""));
  }
  return lines;
}

std::vector<std::pair<std::string, std::string>> EditorShell::inspector_rows() const {
  std::vector<std::pair<std::string, std::string>> rows;
  if (!scene_ || !selection_.valid) {
    rows.emplace_back("selection", "(none)");
    return rows;
  }

  const Transform& t = scene_->transform(selection_.entity);
  rows.emplace_back("entity", fmt::format("{}", selection_.entity.index()));
  rows.emplace_back(
      "position", fmt::format("{:.3f}, {:.3f}, {:.3f}", t.position.x, t.position.y, t.position.z));
  rows.emplace_back("rotation", fmt::format("{:.3f}, {:.3f}, {:.3f}, {:.3f}", t.rotation.x,
                                            t.rotation.y, t.rotation.z, t.rotation.w));
  rows.emplace_back("scale",
                    fmt::format("{:.3f}, {:.3f}, {:.3f}", t.scale.x, t.scale.y, t.scale.z));

  if (const RenderableDesc* r = const_cast<Scene*>(scene_)->renderable(selection_.entity)) {
    rows.emplace_back("mesh", r->mesh.valid() ? fmt::format("{}", r->mesh.index()) : "none");
    rows.emplace_back("material",
                      r->material.valid() ? fmt::format("{}", r->material.index()) : "none");
    rows.emplace_back("casts_shadow", r->casts_shadow ? "yes" : "no");
  }

  const SkyDesc& sky = scene_->sky();
  rows.emplace_back("sun elevation", fmt::format("{:.1f} deg", sky.elevation_degrees));
  rows.emplace_back("cloud coverage", fmt::format("{:.2f}", sky.cloud_coverage));
  return rows;
}

std::string EditorShell::execute(std::string_view command, Renderer* renderer) {
  std::istringstream in{std::string(command)};
  std::string verb;
  in >> verb;

  if (verb.empty())
    return "";

  if (verb == "play") {
    play();
    return "playing";
  }
  if (verb == "pause") {
    pause();
    return "paused";
  }
  if (verb == "stop") {
    stop();
    return "stopped";
  }
  if (verb == "step") {
    step();
    return "stepped one frame";
  }
  if (verb == "list") {
    for (const std::string& line : hierarchy_lines())
      print(line);
    return "dumped hierarchy to the console";
  }
  if (verb == "inspect") {
    for (auto& [k, v] : inspector_rows())
      print(fmt::format("  {} = {}", k, v));
    return "dumped inspector";
  }
  if (verb == "select") {
    u32 index = 0;
    if (!(in >> index))
      return "usage: select <entity-index>";
    if (!scene_ || index >= scene_->entity_count())
      return "no such entity";
    select(EntityHandle::make(index, 1));
    return fmt::format("selected {}", index);
  }
  if (verb == "stats") {
    if (!renderer)
      return "no renderer";
    const FrameStats& s = renderer->stats();
    return fmt::format("draws={} visible={} lights={} res={:.2f} gpu={:.1f}MiB", s.draw_calls,
                       s.visible_instances, s.visible_lights, static_cast<f64>(s.resolution_scale),
                       static_cast<f64>(s.gpu_memory_bytes) / (1024.0 * 1024.0));
  }
  if (verb == "help") {
    return "play | pause | stop | step | list | inspect | select <i> | stats";
  }
  return fmt::format("unknown command '{}' (try help)", verb);
}

} // namespace neo
