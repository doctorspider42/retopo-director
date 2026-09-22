#pragma once

// Runs a child process, feeds it stdin, captures stdout and stderr, and gives
// up after a timeout. Used for the CLI based LLM backends, where the agent
// binary is a black box that may well hang.

#include <atomic>
#include <string>
#include <vector>

namespace rd {

struct ProcessResult {
    bool        started    = false;
    bool        timed_out  = false;
    bool        cancelled  = false;
    int         exit_code  = -1;
    std::string out;
    std::string err;
    double      seconds    = 0.0;
    std::string error;     // why it could not be started at all
};

struct ProcessRequest {
    std::string              executable;
    std::vector<std::string> arguments;
    std::string              stdin_data;
    std::string              working_directory;   // empty keeps the current one
    int                      timeout_seconds = 300;
    // Set from another thread to abort early.
    const std::atomic<bool>* cancel = nullptr;
};

ProcessResult run_process(const ProcessRequest& req);

// Looks the executable up on PATH (and, on Windows, with the usual extensions).
// Returns an empty string when it is not there.
std::string which(const std::string& executable);

// Quotes a single argument the way the platform's command line parser expects.
// Exposed so the UI can show the exact command that will run.
std::string quote_argument(const std::string& arg);
std::string format_command(const std::string& exe, const std::vector<std::string>& args);

} // namespace rd
