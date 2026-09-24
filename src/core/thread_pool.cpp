#include "core/thread_pool.h"

#include "core/log.h"

#include <algorithm>
#include <exception>
#include <memory>

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

        // Nothing in the engine path is supposed to throw, but an exception
        // reaching the top of a worker is std::terminate: the process dies with
        // no log line, no error in the interface and nothing to go on. Whatever
        // the job did wrong, the pool survives to report it.
        try {
            job();
        } catch (const std::exception& e) {
            RD_ERROR("worker job threw: %s", e.what());
        } catch (...) {
            RD_ERROR("worker job threw an unknown exception");
        }
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

    // The bookkeeping lives on the heap, shared with every helper, and nothing
    // a helper touches after the last chunk is claimed lives on this frame.
    //
    // It has to be that way round. A lane that finishes the final chunk releases
    // the caller, and the caller returns the moment it wakes; a helper still
    // going round the loop would then be reading a dead stack frame, and taking
    // a mutex that no longer exists. That is exactly what it looked like: a
    // segfault reading the loop bounds on one run, and "system_error: Invalid
    // argument" out of a destroyed mutex on the next, on a mesh whose only
    // distinction was producing enough small batches to lose the race.
    //
    // The ordering argument for `body` is what lets it stay a plain reference:
    // a lane only calls it while holding a chunk, and while a chunk is held
    // `remaining` is non zero, so the caller is still in the wait below. Past
    // that point a straggling lane reads the shared state and nothing else.
    struct LaneState {
        std::atomic<size_t>     next{0};
        std::atomic<size_t>     remaining;
        size_t                  count = 0, chunk = 0, chunk_count = 0;
        std::mutex              mutex;
        std::condition_variable cv;
    };
    const auto state = std::make_shared<LaneState>();
    state->remaining.store(chunk_count, std::memory_order_relaxed);
    state->count       = count;
    state->chunk       = chunk;
    state->chunk_count = chunk_count;

    auto run_lane = [state, &body](unsigned lane) {
        for (;;) {
            const size_t idx = state->next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= state->chunk_count) return;
            const size_t begin = idx * state->chunk;
            const size_t end   = std::min(state->count, begin + state->chunk);
            // The chunk is counted off even if body throws. worker_main catches
            // and logs whatever escapes a helper, and a chunk that never
            // reported back would leave the caller waiting forever - a hang
            // with no log line is worse than the crash the catch is there for.
            struct Done {
                LaneState& s;
                ~Done()
                {
                    // Signalled while holding the lock, so a waiter that evaluated
                    // the predicate just before the decrement cannot miss the wakeup.
                    if (s.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard lock(s.mutex);
                        s.cv.notify_one();
                    }
                }
            } done{*state};
            body(begin, end, lane);
        }
    };

    const size_t helpers = std::min<size_t>(workers_.size(), chunk_count - 1);
    if (helpers == 0) {
        run_lane(0);
        return;
    }

    for (size_t i = 0; i < helpers; ++i) {
        const unsigned lane = static_cast<unsigned>(i + 1);
        enqueue([run_lane, lane] { run_lane(lane); });
    }

    // If the caller's own lane throws, the helpers may still be inside `body`,
    // which lives on the caller's side; wait them out before letting it unwind.
    std::exception_ptr failure;
    try {
        run_lane(0);
    } catch (...) {
        failure = std::current_exception();
    }

    std::unique_lock lock(state->mutex);
    state->cv.wait(lock, [&] { return state->remaining.load(std::memory_order_acquire) == 0; });
    if (failure) std::rethrow_exception(failure);
}

void ThreadPool::parallel_for(size_t count, size_t min_chunk,
                              const std::function<void(size_t, unsigned)>& body)
{
    parallel_ranges(count, min_chunk, [&body](size_t b, size_t e, unsigned lane) {
        for (size_t i = b; i < e; ++i) body(i, lane);
    });
}

} // namespace rd
