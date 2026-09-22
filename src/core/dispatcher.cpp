#include "core/dispatcher.h"

namespace rd {

bool MainThreadDispatcher::run_and_wait(std::function<void()> fn)
{
    std::condition_variable cv;
    bool                    done = false;

    {
        std::lock_guard lock(mutex_);
        if (stopped_) return false;
        tasks_.push(Task{std::move(fn), &done, &cv});
    }

    std::unique_lock lock(mutex_);
    cv.wait(lock, [&] { return done || stopped_; });
    return done;
}

void MainThreadDispatcher::post(std::function<void()> fn)
{
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    tasks_.push(Task{std::move(fn), nullptr, nullptr});
}

size_t MainThreadDispatcher::drain()
{
    size_t ran = 0;
    for (;;) {
        Task task;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        if (task.fn) task.fn();
        ++ran;

        if (task.done) {
            std::lock_guard lock(mutex_);
            *task.done = true;
            if (task.cv) task.cv->notify_all();
        }
    }
    return ran;
}

void MainThreadDispatcher::shutdown()
{
    std::queue<Task> pending;
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        pending.swap(tasks_);
    }
    // Release anyone still blocked; their work simply never ran.
    while (!pending.empty()) {
        Task& task = pending.front();
        if (task.cv) task.cv->notify_all();
        pending.pop();
    }
}

bool MainThreadDispatcher::stopped() const
{
    std::lock_guard lock(mutex_);
    return stopped_;
}

} // namespace rd
