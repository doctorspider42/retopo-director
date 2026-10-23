#pragma once

// Keeping an installed copy current.
//
// Every green push to main is a GitHub release tagged v<version>-build.<n>, and
// the build that produced it has <n> compiled in as RD_BUILD_NUMBER. Checking
// for an update is one GET of the latest release; a higher number means there
// is one. Installing is the release's Inno Setup installer run silently, which
// closes this copy, replaces it and starts it again.
//
// The network work runs on its own thread and the interface reads the state
// every frame, the same arrangement as the checkpoint download.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace rd::ui {

enum class UpdateState : int {
    Idle = 0,     // nothing asked yet
    Checking,
    UpToDate,
    Available,    // a newer build exists; nothing downloaded yet
    Downloading,
    Ready,        // the installer is on disk
    Failed,
};

class Updater {
public:
    ~Updater();

    // The build this executable is. 0 for a local build, which never offers
    // to replace itself: it would be replaced by something older than the code
    // sitting in the working tree.
    static int  current_build();
    static bool enabled() { return current_build() > 0; }
    // True when this copy was put here by the installer (it leaves its
    // uninstaller next to the executable). A copy unpacked from the zip gets
    // the release page instead of an installer that would put a second copy
    // somewhere else.
    static bool installed();

    // Both start work on the worker thread and return at once. `quiet` checks
    // stay out of the way: no toast when there is nothing new or the network
    // is down.
    void check(bool quiet);
    void download();
    // Starts the downloaded installer. On true the caller should close the
    // window promptly: the installer waits for this process to go.
    bool install(std::string* error);
    void join();

    UpdateState state() const { return state_.load(); }
    bool        quiet() const { return quiet_.load(); }
    float       fraction() const;     // download progress, 0..1

    int         latest_build() const;
    std::string latest_tag() const;
    std::string notes() const;
    std::string page_url() const;
    std::string error() const;

    // A state change the interface has not reported yet, returned once.
    bool        take_news(UpdateState* state);

private:
    void finish(UpdateState s);

    std::thread              worker_;
    std::atomic<UpdateState> state_{UpdateState::Idle};
    std::atomic<bool>        quiet_{true};
    std::atomic<bool>        cancel_{false};
    std::atomic<bool>        news_{false};
    std::atomic<uint64_t>    received_{0};
    std::atomic<uint64_t>    total_{0};

    mutable std::mutex       mutex_;
    int                      latest_build_ = 0;
    std::string              latest_tag_;
    std::string              notes_;
    std::string              page_url_;
    std::string              asset_url_;
    uint64_t                 asset_size_ = 0;
    std::filesystem::path    installer_;
    std::string              error_;
};

// Opens a web page in the default browser.
void open_url(const std::string& url);

} // namespace rd::ui
