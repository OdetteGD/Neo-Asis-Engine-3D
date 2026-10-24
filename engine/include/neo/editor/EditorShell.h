// ===========================================================================
//  EditorShell — M5 editor foundation.
//
//  This is the editor's *state machine*, not its window: selection, play mode
//  (play / pause / step / stop), hierarchy and inspector queries, and a command
//  console. It is deliberately independent of any UI so the same logic drives
//  the eventual in-engine panel, a headless CI run, or a remote control channel.
//
//  Rendering the panel needs a font atlas + immediate-mode UI pass, which is the
//  next milestone. Until then every action is also logged, so the shell is
//  usable from the console today.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"
#include "neo/scene/Scene.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neo {

enum class PlayState : u8 {
  Stopped = 0, // editing: scripts do not tick
  Playing,     // scripts tick, scene is live
  Paused,      // frozen for inspection
  StepOnce,    // advances exactly one frame
};

struct EditorSelection {
  EntityHandle entity;
  bool valid = false;
};

class EditorShell {
public:
  EditorShell() = default;

  void attach(Scene* scene) { scene_ = scene; }

  // --- Frame --------------------------------------------------------------
  // Returns true when gameplay scripts should tick this frame.
  bool tick(f32 delta);

  // --- Play mode ----------------------------------------------------------
  void play();
  void pause();
  void stop();
  void step();
  void toggle_pause();
  [[nodiscard]] PlayState play_state() const { return state_; }
  [[nodiscard]] bool is_playing() const { return state_ == PlayState::Playing; }
  [[nodiscard]] f64 elapsed_play_seconds() const { return play_elapsed_; }
  [[nodiscard]] u64 frames_since_play() const { return play_frames_; }

  // --- Selection / inspection --------------------------------------------
  void select(EntityHandle e);
  [[nodiscard]] EditorSelection selection() const { return selection_; }

  // Human-readable hierarchy: one line per entity, indented by depth.
  [[nodiscard]] std::vector<std::string> hierarchy_lines() const;

  // Key/value properties of the selected entity, for the inspector panel.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> inspector_rows() const;

  // --- Command console ----------------------------------------------------
  // Minimal command set: play, pause, stop, step, select <index>, list,
  // stats. Unknown commands return an error string.
  [[nodiscard]] std::string execute(std::string_view command, class Renderer* renderer);

  // --- Console history ----------------------------------------------------
  [[nodiscard]] const std::vector<std::string>& console() const { return console_; }
  void clear_console() { console_.clear(); }

  [[nodiscard]] bool visible() const { return visible_; }
  void set_visible(bool v) { visible_ = v; }
  void toggle_visible() { visible_ = !visible_; }

  // Snapshot / restore: entering play mode captures the authored scene, and
  // stopping restores it exactly. This is what makes play-in-editor safe — no
  // simulation edit can leak back into the saved scene.
  [[nodiscard]] bool has_snapshot() const { return snapshot_valid_; }
  void capture_snapshot();
  void restore_snapshot();

private:
  void print(std::string line);

  Scene* scene_ = nullptr;
  PlayState state_ = PlayState::Stopped;
  EditorSelection selection_{};
  bool step_requested_ = false;
  f64 play_elapsed_ = 0.0;
  u64 play_frames_ = 0;
  bool visible_ = true;
  std::vector<std::string> console_;

  // Scene snapshot taken on play() and restored on stop().
  struct Snapshot {
    std::vector<Transform> transforms;
    std::vector<LightDesc> lights;
    SkyDesc sky{};
  };
  Snapshot snapshot_{};
  bool snapshot_valid_ = false;
};

} // namespace neo
