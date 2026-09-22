#include "core/thread_pool.h"

#include "core/log.h"

#include <algorithm>

namespace rd {

ThreadPool::ThreadPool(unsigned worker_count)
{
    if (worker_count == 0) {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        worker_count = hw > 1 ? hw - 1 : 0;
    }
    workers_.reserve(worker_count);
    for (unsigned i = 0; i < worker_count; ++i)
        workers_.emplace_back([this, i] { worker_main(i + 1); });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

ThreadPool& ThreadPool::shared()
{
    static ThreadPool pool;
    return pool;
}

void ThreadPool::worker_main(unsigned /*lane*/)
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
    }
}

void ThreadPool::enqueue(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void ThreadPool::parallel_ranges(size_t count, size_t min_chunk,
                                 const std::function<void(size_t, size_t, unsigned)>& body)
{
    if (count == 0) return;
    min_chunk = std::max<size_t>(min_chunk, 1);

    const size_t lanes = std::max<size_t>(1, lane_count());
    // Slightly more chunks than lanes keeps things balanced when the cost per
    // element is uneven (it always is, for anything that touches a BVH).
    size_t chunk = (count + lanes * 4 - 1) / (lanes * 4);
    chunk = std::max(chunk, min_chunk);
    const size_t chunk_count = (count + chunk - 1) / chunk;

    if (chunk_count <= 1 || workers_.empty()) {
        body(0, count, 0);
        return;
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> remaining{chunk_count};
    std::mutex              done_mutex;
    std::condition_variable done_cv;

    auto run_lane = [&](unsigned lane) {
        for (;;) {
            const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= chunk_count) return;
            const size_t begin = idx * chunk;
            const size_t end   = std::min(count, begin + chunk);
            body(begin, end, lane);
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(done_mutex);
                done_cv.notify_one();
            }
        }
    };

    const size_t helpers = std::min<size_t>(workers_.size(), chunk_count - 1);
    for (size_t i = 0; i < helpers; ++i) {
        const unsigned lane = static_cast<unsigned>(i + 1);
        enqueue([&run_lane, lane] { run_lane(lane); });
    }

    run_lane(0);

    std::unique_lock lock(done_mutex);
    done_cv.wait(lock, [&] { return remaining.load(std::memory_order_acquire) == 0; });
}

void ThreadPool::parallel_for(size_t count, size_t min_chunk,
                              const std::function<void(size_t, unsigned)>& body)
{
    parallel_ranges(count, min_chunk, [&body](size_t b, size_t e, unsigned lane) {
        for (size_t i = b; i < e; ++i) body(i, lane);
    });
}

} // namespace rd
