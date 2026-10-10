// Row based parallel_for. Pixel work in Tessel is embarrassingly parallel over
// scanlines, so the work is split into one chunk per worker.
//
// The workers are started once and reused. Creating a fresh std::thread for
// every call cost about half a millisecond each time, which is more than some
// of the operations themselves: a lookup table pass over a 800 by 600 image is
// under a millisecond of actual work. That overhead landed on every effect
// preview, every composite and every brush dab.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tessel {

inline unsigned hardwareThreads()
{
    static const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

namespace detail {

// A fixed pool that runs one batch of chunks at a time. Nested use is not
// supported and never happens: the callers are leaf level pixel loops.
class ThreadPool {
public:
    static ThreadPool& instance()
    {
        static ThreadPool pool;
        return pool;
    }

    unsigned size() const { return workerCount_; }

    // Runs job(i) for i in [0, count) across the pool, returning once all of
    // them have finished. The calling thread takes a share of the work rather
    // than idling.
    void run(int count, const std::function<void(int)>& job)
    {
        if (count <= 0)
            return;
        if (count == 1 || workerCount_ == 0) {
            job(0);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            next_.store(0, std::memory_order_relaxed);
            remaining_.store(count, std::memory_order_relaxed);
            total_ = count;
            ++generation_;
        }
        wake_.notify_all();

        // Help out, then wait for whatever the workers have not finished.
        drain();
        std::unique_lock<std::mutex> lock(doneMutex_);
        done_.wait(lock, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
        std::lock_guard<std::mutex> lock2(mutex_);
        job_ = nullptr;
    }

private:
    ThreadPool()
    {
        // One worker fewer than the hardware allows, because the calling
        // thread joins in.
        workerCount_ = std::max(0u, hardwareThreads() - 1);
        workers_.reserve(workerCount_);
        for (unsigned i = 0; i < workerCount_; ++i)
            workers_.emplace_back([this] { worker(); });
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
    }

    void drain()
    {
        const std::function<void(int)>* job = job_;
        if (!job)
            return;
        while (true) {
            const int index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= total_)
                break;
            (*job)(index);
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(doneMutex_);
                done_.notify_all();
            }
        }
    }

    void worker()
    {
        unsigned seen = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this, &seen] { return generation_ != seen || stopping_; });
            if (stopping_)
                return;
            seen = generation_;
            lock.unlock();
            drain();
        }
    }

    std::vector<std::thread> workers_;
    unsigned workerCount_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    unsigned generation_ = 0;
    bool stopping_ = false;

    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0};
    std::atomic<int> remaining_{0};
    int total_ = 0;

    std::mutex doneMutex_;
    std::condition_variable done_;
};

} // namespace detail

// Calls f(begin, end) over disjoint sub ranges of [begin, end).
template <class F>
void parallelFor(int begin, int end, F&& f, int minRowsPerThread = 24)
{
    const int total = end - begin;
    if (total <= 0)
        return;
    const int wanted = std::max(1, total / std::max(1, minRowsPerThread));
    const int chunks = std::min<int>(wanted, int(hardwareThreads()));
    if (chunks <= 1) {
        f(begin, end);
        return;
    }
    const int chunk = (total + chunks - 1) / chunks;
    const std::function<void(int)> job = [&](int index) {
        const int lo = begin + index * chunk;
        const int hi = std::min(end, lo + chunk);
        if (lo < hi)
            f(lo, hi);
    };
    detail::ThreadPool::instance().run(chunks, job);
}

} // namespace tessel
