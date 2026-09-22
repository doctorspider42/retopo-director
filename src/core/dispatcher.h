#pragma once

// Lets a worker thread borrow the main thread.
//
// The GL context lives on the thread that created the window, so every render
// the pipeline needs has to be executed there. The worker posts a closure and
// blocks; the main loop drains the queue once per frame.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace rd {

class MainThreadDispatcher {
public:
    // Called from a worker. Blocks until the main loop has run `fn`.
    // Returns false if the dispatcher was shut down before it could run.
    bool run_and_wait(std::function<void()> fn);

    // Fire and forget.
    void post(std::function<void()> fn);

    // Called from the main thread once per frame. Returns how many ran.
    size_t drain();

    // Wakes every waiter and refuses new work. Call before joining workers.
    void shutdown();
    bool stopped() const;

private:
    struct Task {
        std::function<void()> fn;
        bool*                 done = nullptr;
        std::condition_variable* cv = nullptr;
    };

    mutable std::mutex      mutex_;
    std::queue<Task>        tasks_;
    std::condition_variable idle_cv_;
    bool                    stopped_ = false;
};

} // namespace rd
