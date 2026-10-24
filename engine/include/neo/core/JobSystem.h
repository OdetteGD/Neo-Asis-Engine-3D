// ===========================================================================
//  Job system — work-stealing task scheduler sized for mobile big.LITTLE.
//
//  Design notes:
//   * One worker thread per physical core, minus the render thread.
//   * Per-worker Chase-Lev deques; idle workers steal from the back.
//   * A Job is a plain function pointer + capture-free data blob (32 B), so
//     scheduling never allocates. Larger payloads go through a handle.
//   * JobCounter gives fork/join and "wait for a group" semantics.
// ===========================================================================
#pragma once

#include "neo/core/Types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace neo {

using JobFn = void (*)(void* data);

struct Job {
  JobFn fn = nullptr;
  void* data = nullptr;
  u64   payload = 0;   // optional inline scalar argument
};

class JobCounter {
public:
  explicit JobCounter(i32 initial = 0) : count_(initial) {}

  void increment(i32 n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }
  void decrement(i32 n = 1) {
    if (count_.fetch_sub(n, std::memory_order_acq_rel) == n) {
      // Last one flips the latch.
      count_.notify_all();
    }
  }
  void wait() const {
    i32 v = count_.load(std::memory_order_acquire);
    while (v > 0) {
      count_.wait(v, std::memory_order_acquire);
      v = count_.load(std::memory_order_acquire);
    }
  }
  [[nodiscard]] i32 value() const { return count_.load(std::memory_order_relaxed); }

private:
  mutable std::atomic<i32> count_;
};

class JobSystem {
public:
  struct Config {
    u32 worker_count = 0;          // 0 => auto-detect (hw - 1), clamped [1, 8]
    u32 deque_capacity = 4096;     // per worker
  };

  JobSystem();
  explicit JobSystem(const Config& cfg);
  ~JobSystem();

  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;

  // --- Run --------------------------------------------------------------
  void run(const Job& job);
  void run(JobFn fn, void* data) { run(Job{fn, data, 0}); }
  void run_range(u32 count, u32 grain, JobFn fn, void* data, JobCounter* counter = nullptr);

  // --- Wait -------------------------------------------------------------
  void wait_idle();

  // --- Introspection ----------------------------------------------------
  [[nodiscard]] u32 worker_count() const { return worker_count_; }
  [[nodiscard]] static u32 hardware_threads();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  u32 worker_count_ = 0;
};

// Global engine-wide scheduler (created lazily on first use).
JobSystem& jobs();

} // namespace neo
