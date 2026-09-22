// Retopo Director - entry point.
// The real application object lives in ui/app.h; this file only owns process
// level concerns: working directory, crash-friendly logging, argument parsing.

#include "core/log.h"
#include "core/paths.h"
#include "ui/app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

void print_usage()
{
    std::printf(
        "Retopo Director %s\n"
        "\n"
        "  retopo-director [options]\n"
        "\n"
        "  --project <dir>   Working directory for renders, bakes and reports\n"
        "  --mesh <file>     Load a high poly mesh on startup (.obj/.gltf/.glb)\n"
        "  --profile <file>  Load a target profile on startup (.json)\n"
        "  --headless        Run the pipeline without a window and exit\n"
        "  --no-llm          Skip the director; run the deterministic half only\n"
        "  --backend <name>  Force auto | quad_field | quadric\n"
        "  --run             Start the pipeline as soon as the window opens\n"
        "  --verbose         Mirror the log to stderr\n"
        "  --no-gpu          Do not create a GL context, even in headless mode\n"
        "\n"
        "  --screenshot <file>       Write a png of the window (no focus needed)\n"
        "  --screenshot-delay <sec>  When to take it; 0 waits for the run to end\n"
        "  --exit-after <sec>        Close the window automatically\n"
        "  --help            This text\n",
        RD_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
    rd::AppOptions opts;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs an argument\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            print_usage();
            return 0;
        } else if (!std::strcmp(a, "--project")) {
            opts.project_dir = next("--project");
        } else if (!std::strcmp(a, "--mesh")) {
            opts.startup_mesh = next("--mesh");
        } else if (!std::strcmp(a, "--profile")) {
            opts.startup_profile = next("--profile");
        } else if (!std::strcmp(a, "--headless")) {
            opts.headless = true;
        } else if (!std::strcmp(a, "--no-llm")) {
            opts.no_llm = true;
        } else if (!std::strcmp(a, "--verbose")) {
            opts.verbose = true;
        } else if (!std::strcmp(a, "--run")) {
            opts.autorun = true;
        } else if (!std::strcmp(a, "--backend")) {
            opts.backend = next("--backend");
        } else if (!std::strcmp(a, "--no-gpu")) {
            opts.no_gpu = true;
        } else if (!std::strcmp(a, "--screenshot")) {
            opts.screenshot = next("--screenshot");
        } else if (!std::strcmp(a, "--screenshot-delay")) {
            opts.screenshot_delay = std::atof(next("--screenshot-delay"));
        } else if (!std::strcmp(a, "--exit-after")) {
            opts.exit_after = std::atof(next("--exit-after"));
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", a);
            print_usage();
            return 2;
        }
    }

    rd::log::init();
    rd::paths::init(argv[0]);

    int rc = 0;
    try {
        rc = rd::run_application(opts);
    } catch (const std::exception& e) {
        RD_ERROR("fatal: %s", e.what());
        std::fprintf(stderr, "fatal: %s\n", e.what());
        rc = 1;
    } catch (...) {
        RD_ERROR("fatal: unknown exception");
        rc = 1;
    }

    rd::log::shutdown();
    return rc;
}
