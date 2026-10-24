#include "neo/platform/Platform.h"
#include "neo/core/Log.h"
#include "neo/render/Camera.h"

#if !defined(NEO_PLATFORM_ANDROID)
#  define GLFW_INCLUDE_NONE
#  include <GLFW/glfw3.h>

#  if defined(__linux__)
#    define GLFW_EXPOSE_NATIVE_X11
#    include <GLFW/glfw3native.h>
#  endif
#endif

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>

namespace neo {

bool InputState::was_pressed(Key k) const {
  return std::find(pressed_this_frame.begin(), pressed_this_frame.end(), k) !=
         pressed_this_frame.end();
}

#if defined(NEO_PLATFORM_ANDROID)

// The Android implementation lives in engine/src/platform/android/AndroidMain.cpp
// and is created by the GameActivity glue. This translation unit only needs to
// satisfy the vtable for the base class on that platform.
std::unique_ptr<Platform> Platform::create(const WindowDesc&) {
  NEO_FATAL(kPlatform, "Platform::create must not be called on Android");
  return nullptr;
}

#else // ---------------------------------------------------------------------

namespace {

class DesktopPlatform final : public Platform {
public:
  explicit DesktopPlatform(const WindowDesc& desc) : desc_(desc) {}

  ~DesktopPlatform() override {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
  }

  bool init() {
    glfwSetErrorCallback([](int code, const char* msg) {
      NEO_ERROR(kPlatform, "GLFW error {}: {}", code, msg);
    });

    if (!glfwInit()) {
      NEO_ERROR(kPlatform, "glfwInit failed (no display?)");
      return false;
    }

    // No client API: Vulkan owns the surface.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc_.resizable ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(static_cast<int>(desc_.width), static_cast<int>(desc_.height),
                               desc_.title.data(), nullptr, nullptr);
    if (!window_) {
      NEO_ERROR(kPlatform, "glfwCreateWindow failed");
      return false;
    }

    glfwSetWindowUserPointer(window_, this);
    install_callbacks();

    int w = 0, h = 0;
    glfwGetWindowSize(window_, &w, &h);
    width_ = static_cast<u32>(w);
    height_ = static_cast<u32>(h);

    NEO_INFO(kPlatform, "Window created {}x{} (\"{}\")", width_, height_, desc_.title);
    return true;
  }

  // --- Platform -----------------------------------------------------------
  bool pump_events() override {
    input_.pressed_this_frame.clear();
    input_.released_this_frame.clear();
    input_.mouse_delta = vec2(0.0f);

    glfwPollEvents();

    if (glfwWindowShouldClose(window_)) return false;

    // Accumulate mouse delta from the cursor callback position history.
    if (has_cursor_) {
      const vec2 d = cursor_ - last_cursor_;
      if (glm::length(d) > 0.0f) {
        input_.mouse_delta = d;
        input_.mouse = cursor_;
      }
      last_cursor_ = cursor_;
      has_cursor_ = true;
    }

    return !quit_;
  }

  void present_frame() override {
    // Vsync-limit on desktop without burning CPU: GLFW's swap interval only
    // applies to GL, so we pace the loop ourselves when vsync is requested.
    if (desc_.vsync) {
      constexpr f64 kTarget = 1.0 / 60.0;
      f64 now = now_seconds();
      f64 elapsed = now - last_present_;
      if (elapsed < kTarget) {
        std::this_thread::sleep_for(std::chrono::duration<f64>(kTarget - elapsed));
      }
      last_present_ = now_seconds();
    }
  }

  f64 now_seconds() const override {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<f64>(clock::now().time_since_epoch()).count();
  }

  NativeWindow native_window() const override {
    NativeWindow nw{};
#if defined(__linux__)
    nw.connection = glfwGetX11Display();
    nw.window = reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(window_)));
    nw.display = glfwGetX11Display();
#endif
    return nw;
  }

  std::vector<const char*> required_surface_extensions() const override {
    u32 count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    return std::vector<const char*>(exts, exts + count);
  }

  u32 width() const override { return width_; }
  u32 height() const override { return height_; }
  bool vsync() const override { return desc_.vsync; }
  void set_vsync(bool enabled) override { desc_.vsync = enabled; }

  const InputState& input() const override { return input_; }
  void set_event_callback(EventCallback cb) override { callback_ = std::move(cb); }
  std::string_view asset_root() const override { return "assets"; }

private:
  void install_callbacks() {
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int width, int height) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      self->width_ = static_cast<u32>(width);
      self->height_ = static_cast<u32>(height);
      if (self->callback_) self->callback_(AppEvent::Resize, self->width_, self->height_);
    });

    glfwSetWindowCloseCallback(window_, [](GLFWwindow* w) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      self->quit_ = true;
      if (self->callback_) self->callback_(AppEvent::Quit, self->width_, self->height_);
    });

    glfwSetWindowFocusCallback(window_, [](GLFWwindow* w, int focused) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      if (self->callback_) {
        self->callback_(focused ? AppEvent::FocusGained : AppEvent::FocusLost, self->width_,
                        self->height_);
      }
    });

    glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      const auto k = static_cast<Key>(key);
      if (k >= Key::Count) return;
      if (action == GLFW_PRESS) {
        self->input_.down[static_cast<usize>(k)] = true;
        self->input_.pressed_this_frame.push_back(k);
      } else if (action == GLFW_RELEASE) {
        self->input_.down[static_cast<usize>(k)] = false;
        self->input_.released_this_frame.push_back(k);
      }
    });

    glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      self->cursor_ = vec2(static_cast<f32>(x), static_cast<f32>(y));
      self->has_cursor_ = true;
      if (self->input_.pointer_down) {
        self->input_.mouse = self->cursor_;
      }
    });

    glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      if (button == GLFW_MOUSE_BUTTON_LEFT) {
        self->input_.pointer_down = (action == GLFW_PRESS);
      }
    });

    glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double y) {
      auto* self = static_cast<DesktopPlatform*>(glfwGetWindowUserPointer(w));
      self->input_.scroll += static_cast<f32>(y);
    });
  }

  WindowDesc desc_;
  GLFWwindow* window_ = nullptr;
  InputState input_{};
  EventCallback callback_;
  u32 width_ = 0;
  u32 height_ = 0;
  bool quit_ = false;

  vec2 cursor_{0.0f};
  vec2 last_cursor_{0.0f};
  bool has_cursor_ = false;
  f64 last_present_ = 0.0;
};

} // namespace

std::unique_ptr<Platform> Platform::create(const WindowDesc& desc) {
  auto p = std::make_unique<DesktopPlatform>(desc);
  if (!p->init()) return nullptr;
  return p;
}

#endif

} // namespace neo
