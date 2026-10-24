// ===========================================================================
//  Platform abstraction — window, input, native handles, Android lifecycle.
//
//  Desktop  : SDL3 (dev target, fast iteration)
//  Android  : GameActivity + ANativeWindow, with the full lifecycle mapped
//             onto engine events so GPU resources survive backgrounding.
// ===========================================================================
#pragma once

#include "neo/core/Math.h"
#include "neo/core/Types.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace neo {

struct WindowDesc {
  std::string_view title = "Neo-Asis Engine";
  u32 width = 1280;
  u32 height = 720;
  bool resizable = true;
  bool fullscreen = false;
  bool vsync = true;
  u32 target_fps = 0;  // 0 = uncapped
};

// Native surface handles, opaque to the engine core.
struct NativeWindow {
  void* display = nullptr;  // ANativeWindow* / SDL_Window*
  void* connection = nullptr;
  void* window = nullptr;   // X11 Window / Wayland surface (desktop)
};

enum class Key : u16 {
  Unknown = 0,
  A = 'A', B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  Num0 = '0', Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
  Escape = 256, Space, Enter, Tab, Backspace, Left, Right, Up, Down,
  Shift, Ctrl, Alt, Count
};

struct InputState {
  std::vector<Key> pressed_this_frame;
  std::vector<Key> released_this_frame;
  bool down[static_cast<usize>(Key::Count)]{};
  vec2 mouse{0.0f};
  vec2 mouse_delta{0.0f};
  f32 scroll = 0.0f;
  bool pointer_down = false;

  [[nodiscard]] bool is_down(Key k) const { return down[static_cast<usize>(k)]; }
  [[nodiscard]] bool was_pressed(Key k) const;
};

enum class AppEvent : u8 {
  Resize,
  FocusGained,
  FocusLost,
  AppPaused,     // Android onPause / desktop minimise
  AppResumed,    // Android onResume
  LowMemory,     // Android onLowMemory / onTrimMemory(RUNNING_CRITICAL)
  Quit,
};

using EventCallback = std::function<void(AppEvent, u32 width, u32 height)>;

class Platform {
public:
  virtual ~Platform() = default;

  [[nodiscard]] static std::unique_ptr<Platform> create(const WindowDesc& desc);

  // --- Frame loop --------------------------------------------------------
  virtual bool pump_events() = 0;       // false => quit requested
  virtual void present_frame() = 0;
  [[nodiscard]] virtual f64 now_seconds() const = 0;

  // --- Surface -----------------------------------------------------------
  [[nodiscard]] virtual NativeWindow native_window() const = 0;
  [[nodiscard]] virtual std::vector<const char*> required_surface_extensions() const = 0;
  [[nodiscard]] virtual u32 width() const = 0;
  [[nodiscard]] virtual u32 height() const = 0;
  [[nodiscard]] virtual bool vsync() const = 0;
  virtual void set_vsync(bool enabled) = 0;

  // --- Input -------------------------------------------------------------
  [[nodiscard]] virtual const InputState& input() const = 0;
  virtual void set_event_callback(EventCallback cb) = 0;

  // --- Paths (APK assets on Android, ./assets on desktop) ----------------
  [[nodiscard]] virtual std::string_view asset_root() const = 0;
};

} // namespace neo
