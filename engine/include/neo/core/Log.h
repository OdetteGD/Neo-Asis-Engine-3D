// ===========================================================================
//  Logging — tiny, allocation-light, and Android-aware.
//  Level filtering + Logcat on Android + ANSI colours on desktop.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <format>
#include <source_location>
#include <string_view>

namespace neo {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

struct LogCategory {
  const char* name;
  LogLevel    min_level;
};

namespace log {
// Named categories used across the engine.
extern LogCategory kCore;
extern LogCategory kGfx;
extern LogCategory kRender;
extern LogCategory kAsset;
extern LogCategory kScript;
extern LogCategory kPlatform;

void set_min_level(LogLevel level) noexcept;
void set_level(std::string_view category, LogLevel level) noexcept;

void write(LogCategory& cat, LogLevel level,
           std::string_view message,
           const std::source_location& loc = std::source_location::current()) noexcept;
} // namespace log

template <typename... Args>
inline void log_message(LogCategory& cat, LogLevel level, std::format_string<Args...> fmt,
                        Args&&... args) {
  if (level >= cat.min_level) {
    log::write(cat, level, std::format(fmt, std::forward<Args>(args)...));
  }
}

} // namespace neo

// --- Convenience macros ----------------------------------------------------
#define NEO_TRACE(cat, ...) ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Trace, __VA_ARGS__)
#define NEO_DEBUG(cat, ...) ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Debug, __VA_ARGS__)
#define NEO_INFO(cat, ...)  ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Info, __VA_ARGS__)
#define NEO_WARN(cat, ...)  ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Warn, __VA_ARGS__)
#define NEO_ERROR(cat, ...) ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Error, __VA_ARGS__)

#define NEO_FATAL(cat, ...)                                                     \
  do {                                                                          \
    ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Fatal, __VA_ARGS__);    \
    ::neo::log::abort_now();                                                     \
  } while (0)

#define NEO_ASSERT(cond, cat, ...)                                              \
  do {                                                                          \
    if (!(cond)) [[unlikely]] {                                                 \
      ::neo::log_message(::neo::log::cat, ::neo::LogLevel::Error,                \
                         "assertion failed: " #cond " | " __VA_ARGS__);          \
      ::neo::log::abort_now();                                                   \
    }                                                                           \
  } while (0)

namespace neo::log {
[[noreturn]] void abort_now() noexcept;
}
