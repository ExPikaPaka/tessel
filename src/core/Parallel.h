// Tiny row based parallel_for. Pixel work in Tessel is embarrassingly
// parallel over scanlines, so a fixed thread fan out is all that is needed.
#pragma once

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

namespace tessel {

inline unsigned hardwareThreads()
{
    static const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

// Calls f(begin, end) over disjoint sub ranges of [begin, end).
template <class F>
void parallelFor(int begin, int end, F&& f, int minRowsPerThread = 24)
{
    const int total = end - begin;
    if (total <= 0)
        return;
    const int wanted = std::max(1, total / std::max(1, minRowsPerThread));
    const int threads = std::min<int>(wanted, static_cast<int>(hardwareThreads()));
    if (threads <= 1) {
        f(begin, end);
        return;
    }
    const int chunk = (total + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads) - 1);
    for (int i = 1; i < threads; ++i) {
        const int lo = begin + i * chunk;
        const int hi = std::min(end, lo + chunk);
        if (lo >= hi)
            break;
        pool.emplace_back([&f, lo, hi] { f(lo, hi); });
    }
    f(begin, std::min(end, begin + chunk));
    for (auto& t : pool)
        t.join();
}

} // namespace tessel
