#pragma once

// Everything the UI owns. The pipeline owns the data; this owns the way it is
// looked at.

#include "core/dispatcher.h"
#include "pipeline/pipeline.h"
#include "render/renderer.h"

#include <filesystem>
#include <map>
#include <string>
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

    // --- viewport ------------------------------------------------------------
    OrbitCamera camera;
    RenderMode  mode        = RenderMode::Shaded;
    ViewSource  source      = ViewSource::LowPoly;
    bool        wireframe   = true;
    bool        auto_rotate = false;
    bool        use_profile_camera = false;
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

    // --- panels --------------------------------------------------------------
    bool show_viewport   = true;
    bool show_pipeline   = true;
    bool show_regions    = true;
    bool show_profile    = true;
    bool show_director   = true;
    bool show_validation = true;
    bool show_gallery    = true;
    bool show_log        = true;
    bool show_stats      = true;
    bool show_demo       = false;
    bool show_about      = false;

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
};

} // namespace rd::ui
