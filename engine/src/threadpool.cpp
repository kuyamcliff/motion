#include "mf/threadpool.hpp"

#include <algorithm>

namespace mf {

static int g_requestedThreads = 0;

ThreadPool& ThreadPool::instance() {
    static ThreadPool pool(g_requestedThreads > 0 ? g_requestedThreads
                                                  : std::max(1, std::min(8, (int)std::thread::hardware_concurrency())));
    return pool;
}

void ThreadPool::setThreadCount(int n) { g_requestedThreads = n; }

ThreadPool::ThreadPool(int threads) {
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::workerLoop() {
    uint64_t seen = 0;
    for (;;) {
        const std::function<void(int, int)>* job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || (job_ && generation_ != seen); });
            if (stop_) return;
            seen = generation_;
            job = job_;
            active_++;
        }
        for (;;) {
            int b = next_.fetch_add(chunk_);
            if (b >= n_) break;
            (*job)(b, std::min(n_, b + chunk_));
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (--active_ == 0) doneCv_.notify_all();
        }
    }
}

void ThreadPool::parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk) {
    if (n <= 0) return;
    std::unique_lock<std::mutex> callLock(callMutex_, std::try_to_lock);
    if (!callLock.owns_lock() || workers_.empty() || n <= minChunk) {
        fn(0, n);  // nested or tiny: run inline
        return;
    }
    int threads = size();
    int chunk = std::max(minChunk / 4 + 1, n / (threads * 4) + 1);
    {
        std::lock_guard<std::mutex> lk(m_);
        job_ = &fn;
        n_ = n;
        chunk_ = chunk;
        next_ = 0;
        generation_++;
    }
    cv_.notify_all();
    for (;;) {
        int b = next_.fetch_add(chunk);
        if (b >= n) break;
        fn(b, std::min(n, b + chunk));
    }
    std::unique_lock<std::mutex> lk(m_);
    doneCv_.wait(lk, [&] { return active_ == 0; });
    job_ = nullptr;
}

}  // namespace mf
