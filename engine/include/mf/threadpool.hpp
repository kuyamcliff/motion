// Minimal job system: parallel_for over integer ranges.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mf {

class ThreadPool {
   public:
    static ThreadPool& instance();
    explicit ThreadPool(int threads);
    ~ThreadPool();
    int size() const { return (int)workers_.size() + 1; }
    // Calls fn(begin, end) over chunks of [0, n). Blocks until done. Reentrant calls run serially.
    void parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk = 16);
    static void setThreadCount(int n);  // must be called before first use

   private:
    void workerLoop();
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, doneCv_;
    const std::function<void(int, int)>* job_ = nullptr;
    std::atomic<int> next_{0};
    int n_ = 0, chunk_ = 1;
    std::atomic<int> active_{0};
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::mutex callMutex_;
};

inline void parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk = 16) {
    ThreadPool::instance().parallelFor(n, fn, minChunk);
}

}  // namespace mf
