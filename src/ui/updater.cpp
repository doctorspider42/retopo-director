#include "ui/updater.h"

#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "llm/http.h"

#include <algorithm>
#include <cstdlib>

#if defined(RD_PLATFORM_WINDOWS)
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef RD_BUILD_NUMBER
#define RD_BUILD_NUMBER 0
#endif
#ifndef RD_UPDATE_REPO
#define RD_UPDATE_REPO "doctorspider42/retopo-director"
#endif

namespace rd::ui {
namespace {

namespace fs = std::filesystem;

// "v0.1.0-build.41" -> 41; anything else -> 0, which is never newer.
int build_from_tag(const std::string& tag)
{
    const size_t at = tag.rfind("-build.");
    if (at == std::string::npos) return 0;
    return std::max(0, std::atoi(tag.c_str() + at + 7));
}

#if defined(RD_PLATFORM_WINDOWS)
std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
#endif

} // namespace

int Updater::current_build() { return RD_BUILD_NUMBER; }

bool Updater::installed()
{
    std::error_code ec;
    return fs::exists(paths::exe_dir() / "unins000.exe", ec);
}

Updater::~Updater() { join(); }

void Updater::join()
{
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
}

void Updater::finish(UpdateState s)
{
    state_.store(s);
    news_.store(true);
}

bool Updater::take_news(UpdateState* state)
{
    if (!news_.exchange(false)) return false;
    if (state) *state = state_.load();
    return true;
}

float Updater::fraction() const
{
    const uint64_t total = total_.load();
    if (!total) return 0.0f;
    return std::clamp(float(double(received_.load()) / double(total)), 0.0f, 1.0f);
}

int Updater::latest_build() const
{
    std::lock_guard lock(mutex_);
    return latest_build_;
}

std::string Updater::latest_tag() const
{
    std::lock_guard lock(mutex_);
    return latest_tag_;
}

std::string Updater::notes() const
{
    std::lock_guard lock(mutex_);
    return notes_;
}

std::string Updater::page_url() const
{
    std::lock_guard lock(mutex_);
    return page_url_;
}

std::string Updater::error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

// ---------------------------------------------------------------------------
void Updater::check(bool quiet)
{
    const UpdateState now = state_.load();
    if (now == UpdateState::Checking || now == UpdateState::Downloading) return;
    if (worker_.joinable()) worker_.join();

    quiet_.store(quiet);
    state_.store(UpdateState::Checking);
    worker_ = std::thread([this] {
        HttpRequest req;
        req.url    = "https://api.github.com/repos/" RD_UPDATE_REPO "/releases/latest";
        req.method = "GET";
        req.timeout_seconds = 20;
        req.cancel = &cancel_;
        // GitHub refuses API calls without a User-Agent.
        req.headers = {{"User-Agent", "ps2-retopo-director"},
                       {"Accept", "application/vnd.github+json"}};
        const HttpResponse res = http_request(req);

        std::string error;
        Json        release;
        if (!res.ok) {
            error = res.error.empty() ? format("http %d", res.status) : res.error;
        } else {
            release = json_parse_lenient(res.body, error);
            if (release.is_null() && error.empty()) error = "unreadable reply";
        }
        if (!error.empty()) {
            RD_WARN("update check failed: %s", error.c_str());
            std::lock_guard lock(mutex_);
            error_ = error;
            finish(UpdateState::Failed);
            return;
        }

        const std::string tag   = json_get<std::string>(release, "tag_name", "");
        const int         build = build_from_tag(tag);
        std::string asset_url;
        uint64_t    asset_size = 0;
        for (const Json& a : json_array_or_empty(release, "assets")) {
            const std::string name = json_get<std::string>(a, "name", "");
            if (name.size() > 10 && name.compare(name.size() - 10, 10, "-setup.exe") == 0) {
                asset_url  = json_get<std::string>(a, "browser_download_url", "");
                asset_size = json_get<uint64_t>(a, "size", 0);
            }
        }

        {
            std::lock_guard lock(mutex_);
            latest_build_ = build;
            latest_tag_   = tag;
            notes_        = json_get<std::string>(release, "body", "");
            page_url_     = json_get<std::string>(release, "html_url", "");
            asset_url_    = asset_url;
            asset_size_   = asset_size;
            error_.clear();
        }
        RD_INFO("update check: this is build %d, the latest release is %s",
                current_build(), tag.empty() ? "unknown" : tag.c_str());
        finish(build > current_build() ? UpdateState::Available : UpdateState::UpToDate);
    });
}

void Updater::download()
{
    if (state_.load() != UpdateState::Available) return;
    if (worker_.joinable()) worker_.join();

    std::string url;
    int         build = 0;
    {
        std::lock_guard lock(mutex_);
        url   = asset_url_;
        build = latest_build_;
        total_.store(asset_size_);
    }
    if (url.empty()) {
        std::lock_guard lock(mutex_);
        error_ = "the release has no installer";
        finish(UpdateState::Failed);
        return;
    }

    received_.store(0);
    state_.store(UpdateState::Downloading);
    worker_ = std::thread([this, url, build] {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "ps2-retopo-director";
        paths::ensure_dir(dir);
        const fs::path dest = dir / format("PS2-Retopo-Director-build.%d-setup.exe", build);

        HttpDownloadRequest req;
        req.url         = url;
        req.destination = dest.string();
        req.cancel      = &cancel_;
        req.timeout_seconds = 600;
        req.progress = [this](uint64_t received, uint64_t total) {
            received_.store(received);
            if (total) total_.store(total);
        };
        const HttpDownloadResult res = http_download(req);

        std::lock_guard lock(mutex_);
        if (!res.ok) {
            error_ = res.error.empty() ? "download failed" : res.error;
            RD_WARN("update download failed: %s", error_.c_str());
            finish(UpdateState::Failed);
            return;
        }
        installer_ = dest;
        RD_INFO("update downloaded: %s", dest.string().c_str());
        finish(UpdateState::Ready);
    });
}

bool Updater::install(std::string* error)
{
    fs::path installer;
    {
        std::lock_guard lock(mutex_);
        installer = installer_;
    }
    if (state_.load() != UpdateState::Ready || installer.empty()) {
        if (error) *error = "nothing downloaded yet";
        return false;
    }
#if defined(RD_PLATFORM_WINDOWS)
    // Silent, because the person already said yes in the window. /RELAUNCH is
    // the script's own: it starts the new copy once the files are in place.
    const std::wstring args = L"/SILENT /SP- /NOCANCEL /CLOSEAPPLICATIONS /RELAUNCH=1";
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", installer.wstring().c_str(),
                                      args.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        if (error) *error = format("could not start the installer (%d)",
                                   int(reinterpret_cast<INT_PTR>(h)));
        return false;
    }
    RD_INFO("installer started: %s", installer.string().c_str());
    return true;
#else
    if (error) *error = "installing updates is only supported on Windows";
    return false;
#endif
}

void open_url(const std::string& url)
{
    if (url.empty()) return;
#if defined(RD_PLATFORM_WINDOWS)
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(RD_PLATFORM_MACOS)
    if (url.find('\'') != std::string::npos) return;
    const std::string cmd = "open '" + url + "'";
    std::system(cmd.c_str());
#else
    // The address goes through a shell; one with a quote in it is not opened.
    if (url.find('\'') != std::string::npos) return;
    const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

} // namespace rd::ui
