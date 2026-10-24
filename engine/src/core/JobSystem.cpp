#include "neo/core/JobSystem.h"
#include "neo/core/Log.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace neo {

namespace {
struct RangePayload {
  u32 begin, end, grain;
  JobFn fn;
  void* data;
};
} // namespace

struct JobSystem::Impl {
  struct Worker {
    std::deque<Job> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    std::atomic<bool> waking{false};
  };

  std::vector<std::unique_ptr<Worker>> workers;
  std::atomic<bool> quit{false};
  std::atomic<u32> busy{0};
  std::mutex idle_mutex;
  std::condition_variable idle_cv;

  void enqueue(u32 idx, Job job) {
    Worker& w = *workers[idx % workers.size()];
    {
      std::lock_guard lock(w.mutex);
      w.queue.push_back(job);
      w.waking.store(true, std::memory_order_release);
    }
    w.cv.notify_one();
  }

  bool try_steal(u32 self, Job& out) {
    // Rotate the victim order so a single hot queue is not always targeted.
    const u32 n = static_cast<u32>(workers.size());
    for (u32 i = 1; i < n; ++i) {
      const u32 victim = (self + i) % n;
      Worker& w = *workers[victim];
      std::lock_guard lock(w.mutex);
      if (!w.queue.empty()) {
        out = w.queue.front();
        w.queue.pop_front();
        return true;
      }
    }
    return false;
  }

  void worker_loop(u32 index) {
    while (!quit.load(std::memory_order_acquire)) {
      Job job{};
      bool got = false;
      {
        Worker& w = *workers[index];
        std::unique_lock lock(w.mutex);
        w.waking.store(false, std::memory_order_release);
        if (!w.queue.empty()) {
          job = w.queue.front();
          w.queue.pop_front();
          got = true;
        } else {
          w.cv.wait_for(lock, std::chrono::microseconds(250),
                        [&] { return !w.queue.empty() || quit.load(std::memory_order_relaxed); });
          if (!w.queue.empty()) {
            job = w.queue.front();
            w.queue.pop_front();
            got = true;
          }
        }
      }

      if (!got)
        got = try_steal(index, job);
      if (!got)
        continue;

      busy.fetch_add(1, std::memory_order_relaxed);
      if (job.fn)
        job.fn(job.data);
      if (busy.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        idle_cv.notify_all();
      }
    }
  }

  void start(u32 count) {
    workers.reserve(count);
    for (u32 i = 0; i < count; ++i) {
      auto w = std::make_unique<Worker>();
      Worker* raw = w.get();
      workers.push_back(std::move(w));
      raw->thread = std::thread([this, i] { worker_loop(i); });
    }
  }

  void stop() {
    quit.store(true, std::memory_order_release);
    for (auto& w : workers) {
      {
        std::lock_guard lock(w->mutex);
      }
      w->cv.notify_all();
    }
    for (auto& w : workers) {
      if (w->thread.joinable())
        w->thread.join();
    }
    workers.clear();
  }
};

JobSystem::JobSystem() : JobSystem(Config{}) {}

JobSystem::JobSystem(const Config& cfg) : impl_(std::make_unique<Impl>()) {
  u32 count = cfg.worker_count;
  if (count == 0) {
    const u32 hw = hardware_threads();
    count = hw > 2 ? hw - 1 : 1;
  }
  worker_count_ = std::clamp(count, 1u, 16u);
  impl_->start(worker_count_);
  NEO_INFO(kCore, "JobSystem: {} workers ({} hw threads)", worker_count_, hardware_threads());
}

JobSystem::~JobSystem() {
  impl_->stop();
}

void JobSystem::run(const Job& job) {
  // Dispatch round-robin across workers; cheap and good enough for
  // frame-scoped work (culling, animation, asset decode).
  static std::atomic<u32> rr{0};
  impl_->enqueue(rr.fetch_add(1, std::memory_order_relaxed), job);
}

void JobSystem::run_range(u32 count, u32 grain, JobFn fn, void* data, JobCounter* counter) {
  if (count == 0)
    return;
  grain = std::max(grain, 1u);
  const u32 chunks = (count + grain - 1) / grain;

  auto* payloads = new RangePayload[chunks];
  if (counter)
    counter->increment(static_cast<i32>(chunks));

  for (u32 c = 0; c < chunks; ++c) {
    payloads[c] = RangePayload{c * grain, std::min(count, (c + 1) * grain), grain, fn, data};
    run(Job{[](void* raw) {
              auto* p = static_cast<RangePayload*>(raw);
              p->fn(p->data);
            },
            &payloads[c], 0});
  }
  // NOTE: payload lifetime is owned by the caller through `counter`; for the
  // common "fire and wait_idle" path wait_idle() drains before we would free.
  if (counter) {
    // Attach a reaper so the array outlives the jobs.
    struct Reaper {
      RangePayload* p;
      JobCounter* c;
    };
    run(Job{[](void* raw) {
              auto* r = static_cast<Reaper*>(raw);
              r->c->wait();
              delete[] r->p;
              delete r;
            },
            new Reaper{payloads, counter}, 0});
  }
}

void JobSystem::wait_idle() {
  std::unique_lock lock(impl_->idle_mutex);
  impl_->idle_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
    if (impl_->busy.load(std::memory_order_acquire) != 0)
      return false;
    for (auto& w : impl_->workers) {
      std::lock_guard wl(w->mutex);
      if (!w->queue.empty())
        return false;
    }
    return true;
  });
}

u32 JobSystem::hardware_threads() {
  const u32 n = std::thread::hardware_concurrency();
  return n == 0 ? 2u : n;
}

JobSystem& jobs() {
  static JobSystem instance;
  return instance;
}

} // namespace neo
