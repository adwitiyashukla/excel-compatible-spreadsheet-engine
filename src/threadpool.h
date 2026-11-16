#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sheet {

class ThreadPool {
 public:
  explicit ThreadPool(int threads) {
    int extra = std::max(0, threads - 1);
    for (int i = 0; i < extra; ++i) workers_.emplace_back([this] { Loop(); });
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  int size() const { return static_cast<int>(workers_.size()) + 1; }

  void ParallelFor(size_t n, size_t grain, const std::function<void(size_t)>& fn) {
    if (n == 0) return;
    if (workers_.empty() || n <= grain) {
      for (size_t i = 0; i < n; ++i) fn(i);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      job_ = &fn;
      total_ = n;
      grain_ = std::max<size_t>(1, grain);
      next_.store(0, std::memory_order_relaxed);
      busy_ = static_cast<int>(workers_.size());
      ++generation_;
    }
    wake_.notify_all();
    Work();
    std::unique_lock<std::mutex> lock(mu_);
    done_.wait(lock, [this] { return busy_ == 0; });
    job_ = nullptr;
  }

 private:
  void Work() {
    const std::function<void(size_t)>& job = *job_;
    size_t total = total_;
    size_t grain = grain_;
    while (true) {
      size_t start = next_.fetch_add(grain, std::memory_order_relaxed);
      if (start >= total) break;
      size_t end = std::min(total, start + grain);
      for (size_t i = start; i < end; ++i) job(i);
    }
  }

  void Loop() {
    uint64_t seen = 0;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mu_);
        wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
      }
      Work();
      {
        std::lock_guard<std::mutex> lock(mu_);
        --busy_;
      }
      done_.notify_one();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable wake_;
  std::condition_variable done_;
  const std::function<void(size_t)>* job_ = nullptr;
  alignas(64) std::atomic<size_t> next_{0};
  alignas(64) size_t total_ = 0;
  size_t grain_ = 1;
  int busy_ = 0;
  uint64_t generation_ = 0;
  bool stop_ = false;
};

}
