#include "llm/process.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(RD_PLATFORM_WINDOWS)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace rd {
namespace fs = std::filesystem;

std::string quote_argument(const std::string& arg)
{
#if defined(RD_PLATFORM_WINDOWS)
    // CommandLineToArgvW rules: backslashes only escape a following quote.
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;

    std::string out = "\"";
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
#else
    if (!arg.empty() && arg.find_first_of(" \t\n\"'\\$`*?[]{}();&|<>#~") == std::string::npos)
        return arg;
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

std::string format_command(const std::string& exe, const std::vector<std::string>& args)
{
    std::string out = quote_argument(exe);
    for (const std::string& a : args) {
        out.push_back(' ');
        out += quote_argument(a);
    }
    return out;
}

std::string which(const std::string& executable)
{
    if (executable.empty()) return {};

    std::error_code ec;
    const fs::path direct(executable);
    if (direct.has_parent_path()) {
        if (fs::exists(direct, ec) && !fs::is_directory(direct, ec)) return direct.string();
#if defined(RD_PLATFORM_WINDOWS)
        for (const char* ext : {".exe", ".cmd", ".bat", ".ps1"}) {
            const fs::path p = direct.string() + ext;
            if (fs::exists(p, ec)) return p.string();
        }
#endif
        return {};
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};

#if defined(RD_PLATFORM_WINDOWS)
    const char separator = ';';
    // Real executables first: npm installs a bare POSIX shell script next to
    // its .cmd and .ps1 shims, and CreateProcess cannot run that one.
    const std::vector<std::string> extensions = {".exe", ".cmd", ".bat", ".ps1", ""};
#else
    const char separator = ':';
    const std::vector<std::string> extensions = {""};
#endif

    for (const std::string& dir : split(path_env, separator)) {
        if (dir.empty()) continue;
        for (const std::string& ext : extensions) {
            const fs::path candidate = fs::path(dir) / (executable + ext);
            if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
                return candidate.string();
        }
    }
    return {};
}

#if defined(RD_PLATFORM_WINDOWS)
namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

struct Pipe {
    HANDLE read  = nullptr;
    HANDLE write = nullptr;

    ~Pipe()
    {
        if (read)  CloseHandle(read);
        if (write) CloseHandle(write);
    }

    bool create(bool inherit_read)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read, &write, &sa, 0)) return false;
        // Only the end the child uses may be inherited.
        SetHandleInformation(inherit_read ? write : read, HANDLE_FLAG_INHERIT, 0);
        return true;
    }
};

void drain(HANDLE handle, std::string& into)
{
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return;
        if (available == 0) return;

        char  buffer[8192];
        DWORD got = 0;
        const DWORD want = std::min<DWORD>(available, sizeof(buffer));
        if (!ReadFile(handle, buffer, want, &got, nullptr) || got == 0) return;
        into.append(buffer, got);
    }
}

} // namespace

ProcessResult run_process(const ProcessRequest& req)
{
    ProcessResult result;
    Stopwatch watch;

    const std::string resolved = which(req.executable);
    if (resolved.empty()) {
        result.error = "cannot find '" + req.executable + "' on PATH";
        return result;
    }

    // .cmd, .bat and .ps1 are not executables; they need a shell.
    std::string exe = resolved;
    std::vector<std::string> args = req.arguments;
    const std::string ext = to_lower(fs::path(resolved).extension().string());
    if (ext == ".cmd" || ext == ".bat") {
        args.insert(args.begin(), resolved);
        args.insert(args.begin(), "/c");
        exe = "cmd.exe";
    } else if (ext == ".ps1") {
        args.insert(args.begin(), resolved);
        args.insert(args.begin(), "-File");
        args.insert(args.begin(), "Bypass");
        args.insert(args.begin(), "-ExecutionPolicy");
        args.insert(args.begin(), "-NoProfile");
        exe = "powershell.exe";
    }

    Pipe in_pipe, out_pipe, err_pipe;
    if (!in_pipe.create(true) || !out_pipe.create(false) || !err_pipe.create(false)) {
        result.error = "cannot create pipes";
        return result;
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in_pipe.read;
    si.hStdOutput = out_pipe.write;
    si.hStdError  = err_pipe.write;

    std::wstring command_line = widen(format_command(exe, args));
    command_line.push_back(L'\0');

    const std::wstring cwd = widen(req.working_directory);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok) {
        result.error = format("CreateProcess failed (%lu)", GetLastError());
        return result;
    }
    result.started = true;

    // The parent must let go of the child's ends or the reads never finish.
    CloseHandle(out_pipe.write); out_pipe.write = nullptr;
    CloseHandle(err_pipe.write); err_pipe.write = nullptr;
    CloseHandle(in_pipe.read);   in_pipe.read   = nullptr;

    if (!req.stdin_data.empty()) {
        size_t written = 0;
        while (written < req.stdin_data.size()) {
            DWORD put = 0;
            const DWORD chunk = DWORD(std::min<size_t>(req.stdin_data.size() - written, 32768));
            if (!WriteFile(in_pipe.write, req.stdin_data.data() + written, chunk, &put, nullptr) ||
                put == 0)
                break;
            written += put;
        }
    }
    CloseHandle(in_pipe.write);
    in_pipe.write = nullptr;

    const double timeout = req.timeout_seconds > 0 ? double(req.timeout_seconds) : 1e9;
    for (;;) {
        drain(out_pipe.read, result.out);
        drain(err_pipe.read, result.err);

        const DWORD state = WaitForSingleObject(pi.hProcess, 25);
        if (state == WAIT_OBJECT_0) break;

        if (req.cancel && req.cancel->load()) {
            result.cancelled = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
        if (watch.seconds() > timeout) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    drain(out_pipe.read, result.out);
    drain(err_pipe.read, result.err);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = int(code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.seconds = watch.seconds();
    return result;
}

#else   // POSIX

ProcessResult run_process(const ProcessRequest& req)
{
    ProcessResult result;
    Stopwatch watch;

    const std::string resolved = which(req.executable);
    if (resolved.empty()) {
        result.error = "cannot find '" + req.executable + "' on PATH";
        return result;
    }

    int in_fd[2], out_fd[2], err_fd[2];
    if (pipe(in_fd) || pipe(out_fd) || pipe(err_fd)) {
        result.error = "cannot create pipes";
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_fd[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_fd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_fd[1]);
    posix_spawn_file_actions_addclose(&actions, out_fd[0]);
    posix_spawn_file_actions_addclose(&actions, err_fd[0]);

    std::vector<std::string> storage;
    storage.push_back(resolved);
    for (const std::string& a : req.arguments) storage.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, resolved.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_fd[0]); close(out_fd[1]); close(err_fd[1]);

    if (rc != 0) {
        close(in_fd[1]); close(out_fd[0]); close(err_fd[0]);
        result.error = format("posix_spawn failed (%d)", rc);
        return result;
    }
    result.started = true;

    if (!req.stdin_data.empty()) {
        size_t written = 0;
        while (written < req.stdin_data.size()) {
            const ssize_t n = write(in_fd[1], req.stdin_data.data() + written,
                                    req.stdin_data.size() - written);
            if (n <= 0) break;
            written += size_t(n);
        }
    }
    close(in_fd[1]);

    fcntl(out_fd[0], F_SETFL, O_NONBLOCK);
    fcntl(err_fd[0], F_SETFL, O_NONBLOCK);

    const double timeout = req.timeout_seconds > 0 ? double(req.timeout_seconds) : 1e9;
    bool running = true;
    while (running) {
        pollfd fds[2] = {{out_fd[0], POLLIN, 0}, {err_fd[0], POLLIN, 0}};
        poll(fds, 2, 25);

        char buffer[8192];
        for (int i = 0; i < 2; ++i) {
            for (;;) {
                const ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
                if (n <= 0) break;
                (i == 0 ? result.out : result.err).append(buffer, size_t(n));
            }
        }

        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            running = false;
            break;
        }
        if (req.cancel && req.cancel->load()) {
            result.cancelled = true;
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
        if (watch.seconds() > timeout) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
    }

    close(out_fd[0]);
    close(err_fd[0]);
    result.seconds = watch.seconds();
    return result;
}

#endif

} // namespace rd
