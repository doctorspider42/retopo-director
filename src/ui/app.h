#pragma once

#include <string>

namespace rd {

struct AppOptions {
    std::string project_dir;
    std::string startup_mesh;
    std::string startup_profile;
    bool        headless = false;
    bool        no_llm   = false;   // deterministic half only
    bool        verbose  = false;   // mirror the log to stderr
    bool        autorun  = false;   // start the pipeline as soon as the window opens
    std::string backend;            // auto | quad_field | quadric, overrides the panel

    // Region split. Empty keeps whatever settings.json holds; the rest override
    // it for this run only, which is how the two paths get compared on one mesh.
    std::string segmenter;          // geometric | sam | auto
    std::string sam_checkpoint;
    std::string sam_device;         // auto | cuda | cpu | mps
    // Fetch a checkpoint and exit. The same download the Director panel offers,
    // for a machine nobody is sitting at.
    std::string sam_download;       // vit_b | vit_l | vit_h

    // Headless still creates a hidden OpenGL context so the director gets its
    // renders and the silhouette metric means something. --no-gpu forces the
    // old behaviour, for a machine with no usable driver at all.
    bool        no_gpu = false;

    // Automation. The screenshot is read out of the back buffer before the
    // swap, so it does not need the window to be focused or even visible.
    std::string screenshot;          // where to write it
    double      screenshot_delay = 0.0;  // seconds; 0 waits for the run to finish
    double      exit_after       = 0.0;  // seconds; 0 means run until closed
};

// Owns the window, the GPU context and the pipeline. Returns a process exit code.
int run_application(const AppOptions& opts);

} // namespace rd
