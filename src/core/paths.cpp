#include "core/paths.h"

#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(RD_PLATFORM_WINDOWS)
#  include <windows.h>
#endif

namespace rd::paths {
namespace {

struct State {
    fs::path exe;
    fs::path project;
    bool     initialised = false;
};

State& state()
{
    static State s;
    return s;
}

fs::path detect_exe_dir(const char* argv0)
{
#if defined(RD_PLATFORM_WINDOWS)
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return fs::path(buf).parent_path();
#endif
    std::error_code ec;
    if (argv0 && *argv0) {
        const fs::path p = fs::absolute(fs::path(argv0), ec);
        if (!ec) return p.parent_path();
    }
    return fs::current_path(ec);
}

} // namespace

void init(const char* argv0)
{
    State& s = state();
    if (s.initialised) return;
    s.exe         = detect_exe_dir(argv0);
    s.project     = s.exe / "work";
    s.initialised = true;
    ensure_dir(s.project);
    RD_INFO("exe dir     : %s", s.exe.string().c_str());
    RD_INFO("project dir : %s", s.project.string().c_str());
}

const fs::path& exe_dir()     { return state().exe; }
const fs::path& project_dir() { return state().project; }

void set_project_dir(const fs::path& p)
{
    State& s = state();
    std::error_code ec;
    s.project = fs::absolute(p, ec);
    if (ec) s.project = p;
    ensure_dir(s.project);
    RD_INFO("project dir : %s", s.project.string().c_str());
}

fs::path profiles_dir() { return exe_dir() / "profiles"; }

fs::path config_dir()
{
#if defined(RD_PLATFORM_WINDOWS)
    if (const char* appdata = std::getenv("APPDATA"))
        return fs::path(appdata) / "RetopoDirector";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        return fs::path(xdg) / "retopo-director";
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".config" / "retopo-director";
#endif
    return exe_dir() / "config";
}

fs::path config_file() { return config_dir() / "settings.json"; }

fs::path renders_dir()  { return project_dir() / "renders"; }
fs::path bake_dir()     { return project_dir() / "bake"; }
fs::path export_dir()   { return project_dir() / "export"; }
fs::path reports_dir()  { return project_dir() / "reports"; }

fs::path iteration_dir(int iteration)
{
    char name[32];
    std::snprintf(name, sizeof(name), "iter_%02d", iteration);
    return project_dir() / "iterations" / name;
}

bool ensure_dir(const fs::path& p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) return fs::is_directory(p, ec);
    fs::create_directories(p, ec);
    if (ec) {
        RD_ERROR("cannot create directory %s: %s", p.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool read_file(const fs::path& p, std::string& out)
{
    std::vector<uint8_t> bytes;
    if (!read_file(p, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool read_file(const fs::path& p, std::vector<uint8_t>& out)
{
    std::FILE* f = nullptr;
#if defined(RD_PLATFORM_WINDOWS)
    f = _wfopen(p.wstring().c_str(), L"rb");
#else
    f = std::fopen(p.string().c_str(), "rb");
#endif
    if (!f) {
        RD_WARN("cannot open %s for reading", p.string().c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) { std::fclose(f); return false; }

    out.resize(static_cast<size_t>(size));
    const size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    if (got != out.size()) {
        RD_WARN("short read on %s (%zu of %zu bytes)", p.string().c_str(), got, out.size());
        out.resize(got);
    }
    return true;
}

bool write_file(const fs::path& p, const std::string& data)
{
    return write_file(p, data.data(), data.size());
}

bool write_file(const fs::path& p, const void* data, size_t size)
{
    ensure_dir(p.parent_path());
    std::FILE* f = nullptr;
#if defined(RD_PLATFORM_WINDOWS)
    f = _wfopen(p.wstring().c_str(), L"wb");
#else
    f = std::fopen(p.string().c_str(), "wb");
#endif
    if (!f) {
        RD_ERROR("cannot open %s for writing", p.string().c_str());
        return false;
    }
    const size_t put = size > 0 ? std::fwrite(data, 1, size, f) : 0;
    std::fclose(f);
    if (put != size) {
        RD_ERROR("short write on %s", p.string().c_str());
        return false;
    }
    return true;
}

std::string extension_of(const fs::path& p)
{
    std::string e = p.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string format_bytes(uint64_t bytes)
{
    const char*  units[] = {"B", "KB", "MB", "GB", "TB"};
    double       v       = static_cast<double>(bytes);
    int          unit    = 0;
    while (v >= 1024.0 && unit < 4) { v /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", v, units[unit]);
    return buf;
}

} // namespace rd::paths
