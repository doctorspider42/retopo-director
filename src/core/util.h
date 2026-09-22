#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rd {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
std::string              format(const char* fmt, ...);
std::string              to_lower(std::string_view s);
std::string              trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep, bool keep_empty = false);
std::string              join(const std::vector<std::string>& parts, std::string_view sep);
bool                     starts_with(std::string_view s, std::string_view prefix);
bool                     ends_with(std::string_view s, std::string_view suffix);
bool                     iequals(std::string_view a, std::string_view b);
std::string              replace_all(std::string s, std::string_view from, std::string_view to);

// Turns an arbitrary label into something safe for a filename / identifier.
std::string slugify(std::string_view s);

// Base64 for inlining images into API payloads.
std::string base64_encode(const void* data, size_t size);

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
class Stopwatch {
public:
    Stopwatch() { reset(); }
    void   reset()  { start_ = std::chrono::steady_clock::now(); }
    double seconds() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    double millis() const { return seconds() * 1000.0; }

private:
    std::chrono::steady_clock::time_point start_;
};

// Logs "<label>: 12.3 ms" when it goes out of scope.
class ScopedTimer {
public:
    explicit ScopedTimer(std::string label);
    ~ScopedTimer();

private:
    std::string label_;
    Stopwatch   watch_;
};

#define RD_TIMED(label) ::rd::ScopedTimer rd_timer_##__LINE__(label)

// Compact duration string: "820 us", "12.4 ms", "3.15 s".
std::string format_duration(double seconds);

} // namespace rd
