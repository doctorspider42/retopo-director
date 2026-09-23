#pragma once

// Everything the UI owns. The pipeline owns the data; this owns the way it is
// looked at.

#include "core/dispatcher.h"
#include "pipeline/pipeline.h"
#include "render/renderer.h"
#include "segment/sam.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow;

namespace rd::ui {

namespace fs = std::filesystem;

enum class ViewSource : int { LowPoly = 0, HighPoly, SideBySide };

struct OrbitCamera {
    float yaw      = 200.0f;
    float pitch    = 14.0f;
    float distance = 2.8f;
    Vec3  target{0.0f, 0.0f, 0.0f};
    float fov_degrees = 38.0f;

    ViewCamera to_view_camera(const Aabb& bounds) const;
    void       frame(const Aabb& bounds);
};

// Lazily uploaded GL textures for the render gallery.
class ImageCache {
public:
    ~ImageCache();
    // Returns a GL texture id, 0 while it cannot be loaded.
    unsigned get(const fs::path& path, int* width = nullptr, int* height = nullptr);
    void     invalidate(const fs::path& path);
    void     clear();
    size_t   size() const { return entries_.size(); }

private:
    struct Entry {
        unsigned texture = 0;
        int      width = 0, height = 0;
        bool     failed = false;
    };
    std::map<std::string, Entry> entries_;
};

// Fetching a model checkpoint, off the interface thread.
//
// Hundreds of megabytes over a network connection cannot happen between two
// frames, so the work runs on its own thread and the panel reads the atomics
// every frame. The worker never touches AppState, which is what keeps this
// free of locks the interface would have to wait on.
class CheckpointDownload {
public:
    ~CheckpointDownload();

    // False when one is already in flight.
    bool start(const SamCheckpoint& entry);
    // Asks the transfer to stop; it ends on its own a moment later.
    void cancel();
    // Cancels and waits. Called on the way out, so closing the window during a
    // download does not leave a thread writing to a file nobody owns any more.
    void join();

    bool     running()  const { return running_.load(); }
    uint64_t received() const { return received_.load(); }
    uint64_t total()    const { return total_.load(); }
    // 0..1, or -1 when the server never said how big the file is.
    float    fraction() const;

    std::string label() const;
    std::string error() const;
    // The path of a download that just finished, returned once and then
    // forgotten, so the panel applies it exactly one time.
    std::string take_finished(std::string* model_type = nullptr);

private:
    std::thread           worker_;
    std::atomic<bool>     running_{false};
    std::atomic<bool>     cancel_{false};
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> total_{0};

    mutable std::mutex    mutex_;
    std::string           label_;
    std::string           error_;
    std::string           finished_;
    std::string           finished_model_type_;
};

struct AppState {
    // --- engine --------------------------------------------------------------
    GLFWwindow*          window = nullptr;
    Renderer             renderer;
    // A second renderer so the split view can show two different images
    // in the same frame; one renderer reuses its target every call.
    Renderer             renderer_alt;
    MainThreadDispatcher dispatcher;
    Pipeline             pipeline;
    PipelineSettings     settings;

    // --- scene ---------------------------------------------------------------
    GpuMesh     gpu_high;
    GpuMesh     gpu_low;
    uint64_t    gpu_version   = 0;
    bool        gpu_high_ok   = false;
    bool        gpu_low_ok    = false;
    Aabb        scene_bounds;
    ImageCache  images;
    CheckpointDownload checkpoint_download;

    // --- viewport ------------------------------------------------------------
    OrbitCamera camera;
    RenderMode  mode        = RenderMode::Shaded;
    ViewSource  source      = ViewSource::LowPoly;
    bool        wireframe   = true;
    bool        auto_rotate = false;
    bool        use_profile_camera = false;
    // Puts the high poly on screen the moment a file is picked, and swaps to the
    // low poly the first time a run produces one. Touching the source control
    // turns it off: after that the choice is the user's.
    bool        auto_view_source = true;
    // Set when a new file arrives. sync_from_pipeline refreshes scene_bounds the
    // moment the preview lands, so "have we framed this yet" cannot be inferred
    // from the bounds alone; it has to be asked for.
    bool        want_frame = false;
    int         profile_camera_index = 0;
    bool        show_baked_texture = true;

    // --- documents -----------------------------------------------------------
    fs::path                 mesh_path;
    std::vector<fs::path>    recent_meshes;
    std::vector<fs::path>    profile_files;
    int                      profile_index = 0;

    // --- editing -------------------------------------------------------------
    KnobPanel   panel;              // editable copy, synced from the pipeline
    int         selected_region = -1;
    bool        panel_dirty  = false;
    uint64_t    panel_version = 0;
    std::string knobs_json_buffer;
    bool        knobs_json_valid = true;
    std::string knobs_json_error;

    // --- director advice -----------------------------------------------------
    // What the director said about the brief, and whether the user has already
    // said they do not want to hear it again for this run.
    ProfileAdvice advice;
    bool          advice_dismissed = false;
    // Set once when an objection worth interrupting for arrives, cleared by the
    // panel that raises itself. An objection nobody sees is the thing this
    // whole channel exists to avoid.
    bool          advice_wants_attention = false;

    // --- panels --------------------------------------------------------------
    bool show_viewport   = true;
    bool show_pipeline   = true;
    bool show_regions    = true;
    bool show_director   = true;
    bool show_validation = true;
    bool show_gallery    = true;
    bool show_log        = true;
    bool show_stats      = true;
    bool show_demo       = false;
    bool show_about      = false;
    // The second window: everything that is set once and then left alone.
    bool show_options    = false;

    // --- log view ------------------------------------------------------------
    int         log_min_level = 1;   // Debug
    std::string log_filter;
    bool        log_autoscroll = true;

    // --- status --------------------------------------------------------------
    std::string toast;
    double      toast_until = 0.0;
    void        notify(const std::string& text, double seconds = 4.0);

    // --- helpers -------------------------------------------------------------
    void refresh_profiles();
    bool load_profile(const fs::path& path);
    void sync_from_pipeline();      // pulls results into the editable copies
    void upload_meshes();           // (re)uploads GPU copies when data changed
    void save_settings() const;
    void load_settings();
    void push_recent(const fs::path& p);
    // What to call recent_meshes[index] in a menu. Normally the filename; the
    // parent folder is appended when another entry has the same filename, which
    // is the usual case for an asset pack that exports one character per engine.
    std::string recent_label(size_t index) const;
    // Points the application at a file and asks the pipeline for a preview, so
    // the viewport fills in without anybody pressing anything.
    void open_mesh(const fs::path& p);
};

} // namespace rd::ui
