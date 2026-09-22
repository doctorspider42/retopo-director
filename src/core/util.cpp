#include "core/util.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>

namespace rd {

std::string format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char    stack_buf[512];
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);

    std::string out;
    if (needed < 0) {
        out = "<format error>";
    } else if (static_cast<size_t>(needed) < sizeof(stack_buf)) {
        out.assign(stack_buf, static_cast<size_t>(needed));
    } else {
        out.resize(static_cast<size_t>(needed));
        std::vsnprintf(out.data(), out.size() + 1, fmt, args);
    }
    va_end(args);
    return out;
}

std::string to_lower(std::string_view s)
{
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> split(std::string_view s, char sep, bool keep_empty)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            if (i > start || keep_empty) out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string slugify(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool last_dash = false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            last_dash = false;
        } else if (!last_dash && !out.empty()) {
            out.push_back('_');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("unnamed") : out;
}

std::string base64_encode(const void* data, size_t size)
{
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string    out;
    out.reserve(((size + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out.push_back(table[(v >> 18) & 0x3f]);
        out.push_back(table[(v >> 12) & 0x3f]);
        out.push_back(table[(v >> 6) & 0x3f]);
        out.push_back(table[v & 0x3f]);
    }
    if (i < size) {
        uint32_t v = uint32_t(p[i]) << 16;
        const bool has_second = (i + 1 < size);
        if (has_second) v |= uint32_t(p[i + 1]) << 8;
        out.push_back(table[(v >> 18) & 0x3f]);
        out.push_back(table[(v >> 12) & 0x3f]);
        out.push_back(has_second ? table[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

ScopedTimer::ScopedTimer(std::string label) : label_(std::move(label)) {}

ScopedTimer::~ScopedTimer()
{
    RD_DEBUG("%s: %s", label_.c_str(), format_duration(watch_.seconds()).c_str());
}

std::string format_duration(double seconds)
{
    if (seconds < 1e-3) return format("%.0f us", seconds * 1e6);
    if (seconds < 1.0)  return format("%.1f ms", seconds * 1e3);
    if (seconds < 60.0) return format("%.2f s", seconds);
    const int mins = static_cast<int>(seconds / 60.0);
    return format("%d m %.0f s", mins, seconds - mins * 60.0);
}

} // namespace rd
