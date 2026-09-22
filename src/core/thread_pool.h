#pragma once

// A persistent worker pool plus a parallel_for built on top of it.
// Every heavy loop in the pipeline (BVH build, AO bake, curvature, silhouette
// metrics) goes through here, so the thread count is tuned in exactly one place.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace rd {

class ThreadPool {
public:
    // worker_count == 0 means hardware_concurrency() - 1 (the caller joins in).
    explicit ThreadPool(unsigned worker_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned worker_count() const { return static_cast<unsigned>(workers_.size()); }

    // Total lanes available including the calling thread.
    unsigned lane_count() const { return worker_count() + 1; }

    void enqueue(std::function<void()> job);

    // Blocking data parallel loop. The calling thread participates, so this is
    // safe to call from the pipeline thread without deadlocking.
    // body(begin, end, lane) is invoked once per chunk.
    void parallel_ranges(size_t count, size_t min_chunk,
                         const std::function<void(size_t, size_t, unsigned)>& body);

    // Per element convenience wrapper.
    void parallel_for(size_t count, size_t min_chunk,
                      const std::function<void(size_t, unsigned)>& body);

    static ThreadPool& shared();

private:
    void worker_main(unsigned lane);

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    bool                              stopping_ = false;
};

} // namespace rd
