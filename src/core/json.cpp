#include "core/json.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

#include <algorithm>

namespace rd {
namespace {

// Finds the outermost balanced {...} or [...] span, ignoring braces that sit
// inside string literals.
bool find_json_span(std::string_view s, size_t& begin, size_t& end)
{
    size_t start = std::string_view::npos;
    char   open = 0, close = 0;

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{' || s[i] == '[') {
            start = i;
            open  = s[i];
            close = (s[i] == '{') ? '}' : ']';
            break;
        }
    }
    if (start == std::string_view::npos) return false;

    int  depth      = 0;
    bool in_string  = false;
    bool escaped    = false;

    for (size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
            if (escaped)            escaped = false;
            else if (c == '\\')     escaped = true;
            else if (c == '"')      in_string = false;
            continue;
        }
        if (c == '"')            { in_string = true; continue; }
        if (c == open)           { ++depth; continue; }
        if (c == close) {
            if (--depth == 0) { begin = start; end = i + 1; return true; }
        }
    }
    return false;
}

} // namespace

Json json_parse_lenient(std::string_view text, std::string& error)
{
    error.clear();

    // 1. Straight parse.
    {
        Json j = Json::parse(text, nullptr, false, /*ignore_comments=*/true);
        if (!j.is_discarded()) return j;
    }

    // 2. Strip a markdown fence if there is one.
    std::string body(text);
    const size_t fence = body.find("```");
    if (fence != std::string::npos) {
        size_t start = body.find('\n', fence);
        if (start != std::string::npos) {
            ++start;
            const size_t close = body.find("```", start);
            if (close != std::string::npos) {
                Json j = Json::parse(body.substr(start, close - start), nullptr, false, true);
                if (!j.is_discarded()) return j;
            }
        }
    }

    // 3. Carve out the first balanced brace span.
    size_t b = 0, e = 0;
    if (find_json_span(body, b, e)) {
        Json j = Json::parse(body.substr(b, e - b), nullptr, false, true);
        if (!j.is_discarded()) return j;
    }

    error = "response did not contain parsable JSON";
    return Json();
}

std::string json_dump(const Json& j, int indent)
{
    try {
        return j.dump(indent, ' ', /*ensure_ascii=*/false,
                      Json::error_handler_t::replace);
    } catch (const std::exception& e) {
        RD_ERROR("json dump failed: %s", e.what());
        return "{}";
    }
}

bool json_load_file(const std::string& path, Json& out, std::string& error)
{
    std::string text;
    if (!paths::read_file(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    Json j = Json::parse(text, nullptr, false, true);
    if (j.is_discarded()) {
        error = "malformed JSON in " + path;
        return false;
    }
    out = std::move(j);
    return true;
}

bool json_save_file(const std::string& path, const Json& j, std::string& error)
{
    if (!paths::write_file(path, json_dump(j))) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace rd
