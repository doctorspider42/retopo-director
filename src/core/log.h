#pragma once

// Thread safe logging with a bounded in-memory ring buffer.
// The UI console reads straight out of the ring; nothing is ever reallocated
// while the pipeline is hammering it from worker threads.

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rd::log {

enum class Level : uint8_t { Trace = 0, Debug, Info, Warn, Error, Count };

struct Entry {
    Level       level   = Level::Info;
    double      time_s  = 0.0;   // seconds since log::init()
    uint32_t    thread  = 0;     // small dense id, not the OS handle
    std::string text;
};

void init();
void shutdown();

void write(Level level, const char* fmt, ...);
void write_v(Level level, const char* fmt, va_list args);

// Snapshot of the ring, oldest first. Cheap enough to call once per UI frame.
void snapshot(std::vector<Entry>& out);

// Number of entries dropped because the ring wrapped around.
uint64_t dropped();
void     clear();

// Live counters per level, used for the status bar badges.
uint64_t count(Level level);

const char* level_name(Level level);

// Minimum level that gets recorded at all.
void  set_min_level(Level level);
Level min_level();

// Mirrors every recorded entry to stderr. Used by the headless runner, where
// there is no console panel to read the ring buffer.
void  set_echo_stderr(bool on);

} // namespace rd::log

#define RD_TRACE(...) ::rd::log::write(::rd::log::Level::Trace, __VA_ARGS__)
#define RD_DEBUG(...) ::rd::log::write(::rd::log::Level::Debug, __VA_ARGS__)
#define RD_INFO(...)  ::rd::log::write(::rd::log::Level::Info,  __VA_ARGS__)
#define RD_WARN(...)  ::rd::log::write(::rd::log::Level::Warn,  __VA_ARGS__)
#define RD_ERROR(...) ::rd::log::write(::rd::log::Level::Error, __VA_ARGS__)
