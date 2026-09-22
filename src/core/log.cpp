#include "core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace rd::log {
namespace {

constexpr size_t kRingCapacity = 8192;

struct State {
    std::mutex                          mutex;
    std::vector<Entry>                  ring;
    size_t                              head  = 0;   // next slot to write
    size_t                              size  = 0;   // valid entries
    uint64_t                            drops = 0;
    std::chrono::steady_clock::time_point start;
    std::unordered_map<std::thread::id, uint32_t> thread_ids;
    uint32_t                            next_thread_id = 0;
    std::atomic<uint64_t>               counts[static_cast<size_t>(Level::Count)];
    std::atomic<Level>                  min_level{Level::Debug};
    std::atomic<bool>                   echo_stderr{false};
    bool                                ready = false;
};

State& state()
{
    static State s;
    return s;
}

uint32_t thread_slot(State& s)
{
    const auto id = std::this_thread::get_id();
    auto it = s.thread_ids.find(id);
    if (it != s.thread_ids.end()) return it->second;
    const uint32_t slot = s.next_thread_id++;
    s.thread_ids.emplace(id, slot);
    return slot;
}

} // namespace

void init()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.ready) return;
    s.ring.resize(kRingCapacity);
    s.start = std::chrono::steady_clock::now();
    for (auto& c : s.counts) c.store(0, std::memory_order_relaxed);
    s.ready = true;
}

void shutdown()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.ready = false;
}

void set_min_level(Level level) { state().min_level.store(level, std::memory_order_relaxed); }
void set_echo_stderr(bool on)   { state().echo_stderr.store(on, std::memory_order_relaxed); }
Level min_level()               { return state().min_level.load(std::memory_order_relaxed); }

void write_v(Level level, const char* fmt, va_list args)
{
    State& s = state();
    if (level < s.min_level.load(std::memory_order_relaxed)) return;

    char    stack_buf[1024];
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);

    std::string text;
    if (needed < 0) {
        text = "<malformed log message>";
    } else if (static_cast<size_t>(needed) < sizeof(stack_buf)) {
        text.assign(stack_buf, static_cast<size_t>(needed));
    } else {
        text.resize(static_cast<size_t>(needed));
        std::vsnprintf(text.data(), text.size() + 1, fmt, args);
    }

    s.counts[static_cast<size_t>(level)].fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lock(s.mutex);
    if (!s.ready) {
        std::fprintf(level >= Level::Warn ? stderr : stdout, "%s\n", text.c_str());
        return;
    }

    const double t = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - s.start).count();

    Entry& slot = s.ring[s.head];
    slot.level  = level;
    slot.time_s = t;
    slot.thread = thread_slot(s);
    slot.text   = std::move(text);

    s.head = (s.head + 1) % kRingCapacity;
    if (s.size < kRingCapacity) ++s.size;
    else                        ++s.drops;

    // Errors always reach stderr so a crash still leaves a trace on the console.
    if (s.echo_stderr.load(std::memory_order_relaxed))
        std::fprintf(stderr, "%7.3f %-5s %s\n", t, level_name(level), slot.text.c_str());
    else if (level >= Level::Error)
        std::fprintf(stderr, "[error] %s\n", slot.text.c_str());
}

void write(Level level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write_v(level, fmt, args);
    va_end(args);
}

void snapshot(std::vector<Entry>& out)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    out.clear();
    out.reserve(s.size);
    const size_t first = (s.head + kRingCapacity - s.size) % kRingCapacity;
    for (size_t i = 0; i < s.size; ++i)
        out.push_back(s.ring[(first + i) % kRingCapacity]);
}

uint64_t dropped()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.drops;
}

void clear()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.head = 0;
    s.size = 0;
    s.drops = 0;
    for (auto& c : s.counts) c.store(0, std::memory_order_relaxed);
}

uint64_t count(Level level)
{
    return state().counts[static_cast<size_t>(level)].load(std::memory_order_relaxed);
}

const char* level_name(Level level)
{
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
    default:           return "?";
    }
}

} // namespace rd::log
