#pragma once

// One place that pulls in nlohmann/json, plus forgiving accessors. Everything
// the LLM hands back is treated as hostile: wrong types, missing keys and
// out-of-range numbers must never take the process down.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace rd {

using Json = nlohmann::json;

template <typename T>
T json_get(const Json& j, std::string_view key, T fallback)
{
    const auto it = j.find(std::string(key));
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

inline const Json& json_object_or_empty(const Json& j, std::string_view key)
{
    static const Json empty = Json::object();
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_object()) return empty;
    return *it;
}

inline const Json& json_array_or_empty(const Json& j, std::string_view key)
{
    static const Json empty = Json::array();
    const auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_array()) return empty;
    return *it;
}

// Parses text that may be wrapped in prose or a markdown fence, which is what
// chat models produce no matter how loudly the prompt asks for bare JSON.
// Returns a null Json on failure and fills `error`.
Json json_parse_lenient(std::string_view text, std::string& error);

// Pretty print with a stable key order (nlohmann keeps objects sorted), so the
// knob panel diffs cleanly between iterations.
std::string json_dump(const Json& j, int indent = 2);

bool json_load_file(const std::string& path, Json& out, std::string& error);
bool json_save_file(const std::string& path, const Json& j, std::string& error);

} // namespace rd
