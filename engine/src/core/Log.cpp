#include "neo/core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>

#if defined(NEO_PLATFORM_ANDROID)
#  include <android/log.h>
#endif

namespace neo::log {

LogCategory kCore{"core", LogLevel::Info};
LogCategory kGfx{"gfx", LogLevel::Info};
LogCategory kRender{"render", LogLevel::Info};
LogCategory kAsset{"asset", LogLevel::Info};
LogCategory kScript{"script", LogLevel::Info};
LogCategory kPlatform{"platform", LogLevel::Info};

namespace {

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};

#if !defined(NEO_PLATFORM_ANDROID)
constexpr const char* kAnsi[] = {"\x1b[90m", "\x1b[36m", "\x1b[32m",
                                 "\x1b[33m", "\x1b[31m", "\x1b[91m"};
#endif

std::mutex g_log_mutex;

#if defined(NEO_PLATFORM_ANDROID)
int to_logcat(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return ANDROID_LOG_VERBOSE;
  case LogLevel::Debug:
    return ANDROID_LOG_DEBUG;
  case LogLevel::Info:
    return ANDROID_LOG_INFO;
  case LogLevel::Warn:
    return ANDROID_LOG_WARN;
  case LogLevel::Error:
    return ANDROID_LOG_ERROR;
  case LogLevel::Fatal:
    return ANDROID_LOG_FATAL;
  default:
    return ANDROID_LOG_DEFAULT;
  }
}
#endif

} // namespace

void set_min_level(LogLevel level) noexcept {
  for (LogCategory* cat : {&kCore, &kGfx, &kRender, &kAsset, &kScript, &kPlatform}) {
    cat->min_level = level;
  }
}

void set_level(std::string_view category, LogLevel level) noexcept {
  for (LogCategory* cat : {&kCore, &kGfx, &kRender, &kAsset, &kScript, &kPlatform}) {
    if (category == cat->name)
      cat->min_level = level;
  }
}

void write(LogCategory& cat, LogLevel level, std::string_view message,
           const std::source_location& loc) noexcept {
  if (level < cat.min_level)
    return;

  const auto idx = static_cast<usize>(level);
  const char* lvl = idx < std::size(kLevelNames) ? kLevelNames[idx] : "?????";

  std::lock_guard lock(g_log_mutex);

#if defined(NEO_PLATFORM_ANDROID)
  // Logcat has its own framing; keep lines short and skip source locations
  // (they blow up the buffer on low-end devices).
  __android_log_print(to_logcat(level), "NeoAsis", "[%s] %.*s", cat.name,
                      static_cast<int>(message.size()), message.data());
#else
  std::FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
  const auto line = loc.line();
  std::fprintf(out, "%s[%s][%-6s]\x1b[0m %.*s  \x1b[90m(%s:%u)\x1b[0m\n", kAnsi[idx], cat.name, lvl,
               static_cast<int>(message.size()), message.data(), loc.file_name(),
               static_cast<unsigned>(line));
  std::fflush(out);
#endif
}

void abort_now() noexcept {
#if defined(NEO_DEBUG) && !defined(NEO_PLATFORM_ANDROID)
  std::fflush(stdout);
  std::fflush(stderr);
  std::abort();
#else
  std::fflush(stdout);
  std::fflush(stderr);
  std::abort();
#endif
}

} // namespace neo::log
