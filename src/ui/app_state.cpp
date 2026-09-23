#include "ui/app_state.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "llm/http.h"
#include "render/gl.h"

#include <stb_image.h>

#include <algorithm>
#include <imgui.h>

namespace rd::ui {
namespace {

// Two spellings of one file - a different case, a forward slash where the
// dialog gave a backslash, a stray "." - are one file to the filesystem and two
// entries to a vector of paths. Everything entering the recent list is flattened
// to a single spelling first.
fs::path normalise_mesh_path(const fs::path& p)
{
    std::error_code ec;
    fs::path        out = p.is_absolute() ? p : fs::absolute(p, ec);
    if (ec) out = p;
    out = out.lexically_normal();
#if defined(RD_PLATFORM_WINDOWS)
    out.make_preferred();
#endif
    return out;
}

// Windows does not distinguish two names by case, so neither does the list.
bool same_name(const std::string& a, const std::string& b)
{
#if defined(RD_PLATFORM_WINDOWS)
    return iequals(a, b);
#else
    return a == b;
#endif
}

bool same_mesh_path(const fs::path& a, const fs::path& b)
{
    return same_name(a.string(), b.string());
}

} // namespace

// ---------------------------------------------------------------------------
ViewCamera OrbitCamera::to_view_camera(const Aabb& bounds) const
{
    ViewCamera cam;
    cam.fov_radians = clampf(fov_degrees, 5.0f, 120.0f) * kDeg2Rad;
    cam.target      = target;

    const float radius = bounds.valid() ? std::max(0.5f * bounds.diagonal(), kEps) : 1.0f;
    const float yaw_r   = yaw * kDeg2Rad;
    const float pitch_r = clampf(pitch, -89.0f, 89.0f) * kDeg2Rad;
    const float cp      = std::cos(pitch_r);

    const Vec3 dir{std::sin(yaw_r) * cp, std::sin(pitch_r), -std::cos(yaw_r) * cp};
    cam.eye   = target + normalize(dir) * (radius * std::max(distance, 0.05f));
    cam.znear = std::max(radius * 0.01f, 1e-4f);
    cam.zfar  = radius * (distance + 6.0f);
    return cam;
}

void OrbitCamera::frame(const Aabb& bounds)
{
    if (!bounds.valid()) return;
    target   = bounds.center();
    distance = 2.4f;
}

// ---------------------------------------------------------------------------
ImageCache::~ImageCache() { clear(); }

unsigned ImageCache::get(const fs::path& path, int* out_w, int* out_h)
{
    const std::string key = path.string();
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        if (out_w) *out_w = it->second.width;
        if (out_h) *out_h = it->second.height;
        return it->second.failed ? 0u : it->second.texture;
    }

    Entry entry;
    int   channels = 0;
    stbi_uc* data = stbi_load(key.c_str(), &entry.width, &entry.height, &channels, 4);
    if (!data) {
        entry.failed = true;
        entries_.emplace(key, entry);
        return 0;
    }

    if (gl::loaded()) {
        glGenTextures(1, &entry.texture);
        glBindTexture(GL_TEXTURE_2D, entry.texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, entry.width, entry.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        entry.failed = true;
    }
    stbi_image_free(data);

    if (out_w) *out_w = entry.width;
    if (out_h) *out_h = entry.height;
    entries_.emplace(key, entry);
    return entry.failed ? 0u : entry.texture;
}

void ImageCache::invalidate(const fs::path& path)
{
    const auto it = entries_.find(path.string());
    if (it == entries_.end()) return;
    if (it->second.texture && gl::loaded()) glDeleteTextures(1, &it->second.texture);
    entries_.erase(it);
}

void ImageCache::clear()
{
    if (gl::loaded())
        for (auto& [key, entry] : entries_)
            if (entry.texture) glDeleteTextures(1, &entry.texture);
    entries_.clear();
}

// ---------------------------------------------------------------------------
void AppState::notify(const std::string& text, double seconds)
{
    toast = text;
    // Startup notifies before there is an interface to notify. `--profile`
    // loads a profile from run_application, which is a long way above
    // ImGui::CreateContext, and headless never creates a context at all;
    // GetTime() dereferences the context unconditionally, so the option
    // segfaulted on its way in, windowed and headless alike. Without a context
    // the toast has nowhere to appear, so it is left already expired.
    toast_until = ImGui::GetCurrentContext() ? ImGui::GetTime() + seconds : 0.0;
}

void AppState::push_recent(const fs::path& p)
{
    if (p.empty()) return;
    // Copied before anything is erased: callers pass a reference straight out of
    // this vector, and removing the element underneath the argument would leave
    // us inserting a dangling path.
    const fs::path path = normalise_mesh_path(p);

    recent_meshes.erase(std::remove_if(recent_meshes.begin(), recent_meshes.end(),
                                       [&](const fs::path& e) {
                                           return same_mesh_path(e, path);
                                       }),
                        recent_meshes.end());
    recent_meshes.insert(recent_meshes.begin(), path);
    if (recent_meshes.size() > 10) recent_meshes.resize(10);
}

std::string AppState::recent_label(size_t index) const
{
    if (index >= recent_meshes.size()) return {};
    const fs::path& path = recent_meshes[index];

    // Take the filename, and keep adding parent folders until no other entry
    // ends the same way. One level is not always enough: two runs of the same
    // asset pack give ".../unity/Body.fbx" and ".../unreal/Body.fbx", but two
    // scratch folders give ".../scratchpad/high.obj" twice over.
    std::vector<std::string> parts;
    for (const fs::path& part : path) parts.push_back(part.string());
    if (parts.empty()) return path.string();

    auto ends_with_tail = [&](const fs::path& other, size_t depth) {
        std::vector<std::string> theirs;
        for (const fs::path& part : other) theirs.push_back(part.string());
        if (theirs.size() < depth) return false;
        for (size_t k = 0; k < depth; ++k)
            if (!same_name(theirs[theirs.size() - 1 - k], parts[parts.size() - 1 - k]))
                return false;
        return true;
    };

    size_t depth = 1;
    for (; depth < parts.size(); ++depth) {
        bool clash = false;
        for (size_t i = 0; i < recent_meshes.size() && !clash; ++i)
            clash = i != index && ends_with_tail(recent_meshes[i], depth);
        if (!clash) break;
    }

    const std::string name = parts.back();
    if (depth <= 1) return name;

    // The folders, outermost first, so it reads the way a path does.
    std::string where;
    for (size_t k = depth - 1; k >= 1; --k) {
        if (!where.empty()) where += "/";
        where += parts[parts.size() - 1 - k];
    }
    return where.empty() ? name : name + "  -  " + where;
}

void AppState::open_mesh(const fs::path& p)
{
    if (p.empty()) return;
    push_recent(p);
    mesh_path = recent_meshes.front();   // the one spelling the list agreed on

    // A new subject deserves a new framing, and the preview is of the high poly
    // because that is the only thing there is until a run finishes.
    want_frame       = true;
    auto_view_source = true;
    source           = ViewSource::HighPoly;
    selected_region  = -1;
    panel_dirty      = false;
    images.clear();

    if (!pipeline.preview(p, settings))
        notify("Busy; the preview will have to wait for the run to finish", 4.0);
}

void AppState::refresh_profiles()
{
    profile_files.clear();
    std::error_code ec;

    for (const fs::path& dir : {paths::profiles_dir(), paths::config_dir() / "profiles"}) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (paths::extension_of(entry.path()) != "json") continue;
            profile_files.push_back(entry.path());
        }
    }
    std::sort(profile_files.begin(), profile_files.end());

    // Keep the current selection pointing at the same file if we can.
    profile_index = std::min(profile_index, int(profile_files.size()) - 1);
    if (profile_index < 0 && !profile_files.empty()) profile_index = 0;
}

bool AppState::load_profile(const fs::path& path)
{
    std::string error;
    TargetProfile loaded;
    if (!loaded.load(path.string(), &error)) {
        RD_ERROR("cannot load profile %s: %s", path.string().c_str(), error.c_str());
        notify("Profile failed to load: " + error, 6.0);
        return false;
    }
    settings.profile = std::move(loaded);
    settings.profile.clamp();
    notify("Loaded profile " + settings.profile.name);
    return true;
}

void AppState::sync_from_pipeline()
{
    const uint64_t version = pipeline.version();
    if (version == panel_version) return;

    pipeline.with_results([&](const PipelineResults& r) {
        // A panel the user is editing must not be stomped mid-edit.
        if (!panel_dirty) {
            panel = r.panel;
            knobs_json_buffer = json_dump(panel.to_json());
        }
        scene_bounds = r.lowpoly.empty() ? r.highpoly.bounds() : r.lowpoly.bounds();

        // A dismissal covers the objection that was on screen, not every
        // objection the director will ever raise: a later pass that has seen the
        // render has earned the right to interrupt again.
        if (r.advice.headline != advice.headline ||
            r.advice.feasibility != advice.feasibility) {
            advice_dismissed = false;
            advice_wants_attention |= feasibility_is_alarming(r.advice.feasibility);
        }
        advice = r.advice;
    });
    panel_version = version;
}

void AppState::upload_meshes()
{
    const uint64_t version = pipeline.version();
    if (version == gpu_version) return;
    gpu_version = version;

    const bool had_low = gpu_low_ok;

    pipeline.with_results([&](const PipelineResults& r) {
        if (!r.highpoly.empty()) {
            gpu_high.upload(r.highpoly);
            gpu_high_ok = gpu_high.valid();
            if (!r.segmentation.tri_region.empty() &&
                r.segmentation.tri_region.size() == r.highpoly.triangle_count()) {
                std::vector<Vec4> colors;
                region_colors(r.segmentation, colors);
                gpu_high.upload_face_attribute(r.highpoly, colors);
            }
        } else {
            gpu_high_ok = false;
        }

        if (!r.lowpoly.empty()) {
            gpu_low.upload(r.lowpoly);
            gpu_low_ok = gpu_low.valid();
            if (r.bake.ok && !r.bake.diffuse.empty() && show_baked_texture)
                gpu_low.set_texture(r.bake.diffuse);
            if (!r.lowpoly.tri_region.empty() &&
                r.lowpoly.tri_region.size() == r.lowpoly.triangle_count()) {
                // Region colours for the low poly come from its own map.
                Segmentation view_seg = r.segmentation;
                view_seg.tri_region   = r.lowpoly.tri_region;
                std::vector<Vec4> colors;
                region_colors(view_seg, colors);
                if (mode == RenderMode::Regions) {
                    gpu_low.upload_face_attribute(r.lowpoly, colors);
                    gpu_low.set_baked_colors(false);
                }
            }
        } else {
            gpu_low_ok = false;
        }

        const Aabb bounds = gpu_low_ok ? r.lowpoly.bounds() : r.highpoly.bounds();
        if (bounds.valid() && (want_frame || !scene_bounds.valid())) {
            camera.frame(bounds);
            want_frame = false;
        }
        if (bounds.valid()) scene_bounds = bounds;
    });

    // The first low poly of a run is what everyone came to see, so show it
    // without being asked. Only once: after that the source control is obeyed.
    if (auto_view_source && gpu_low_ok && !had_low) source = ViewSource::LowPoly;
    if (auto_view_source && !gpu_low_ok && gpu_high_ok) source = ViewSource::HighPoly;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CheckpointDownload
// ---------------------------------------------------------------------------
CheckpointDownload::~CheckpointDownload() { join(); }

bool CheckpointDownload::start(const SamCheckpoint& entry)
{
    if (running_.load()) return false;
    if (worker_.joinable()) worker_.join();   // a finished one, not yet reaped

    const fs::path destination = sam_checkpoint_path(entry);
    {
        std::lock_guard lock(mutex_);
        label_ = entry.label;
        error_.clear();
        finished_.clear();
        finished_model_type_.clear();
    }
    cancel_.store(false);
    received_.store(0);
    total_.store(entry.bytes);
    running_.store(true);

    const std::string url        = entry.url;
    const std::string model_type = entry.model_type;
    const std::string path       = destination.string();

    RD_INFO("downloading %s (%s) to %s", entry.label,
            paths::format_bytes(entry.bytes).c_str(), path.c_str());

    worker_ = std::thread([this, url, path, model_type] {
        HttpDownloadRequest req;
        req.url         = url;
        req.destination = path;
        req.cancel      = &cancel_;
        req.progress    = [this](uint64_t received, uint64_t total) {
            received_.store(received);
            if (total) total_.store(total);
        };

        const HttpDownloadResult res = http_download(req);

        {
            std::lock_guard lock(mutex_);
            if (res.ok) {
                finished_            = path;
                finished_model_type_ = model_type;
            } else {
                error_ = res.error;
            }
        }
        if (res.ok)
            RD_INFO("downloaded %s in %s", path.c_str(), format_duration(res.seconds).c_str());
        else
            RD_WARN("download failed: %s", res.error.c_str());

        running_.store(false);
    });
    return true;
}

void CheckpointDownload::cancel() { cancel_.store(true); }

void CheckpointDownload::join()
{
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

float CheckpointDownload::fraction() const
{
    const uint64_t total = total_.load();
    if (total == 0) return -1.0f;
    return std::clamp(float(double(received_.load()) / double(total)), 0.0f, 1.0f);
}

std::string CheckpointDownload::label() const
{
    std::lock_guard lock(mutex_);
    return label_;
}

std::string CheckpointDownload::error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

std::string CheckpointDownload::take_finished(std::string* model_type)
{
    std::lock_guard lock(mutex_);
    if (finished_.empty()) return {};
    if (model_type) *model_type = finished_model_type_;
    std::string out;
    out.swap(finished_);
    finished_model_type_.clear();
    return out;
}

// ---------------------------------------------------------------------------
void AppState::save_settings() const
{
    Json j;
    j["llm"]     = settings.llm.to_json();
    j["profile"] = settings.profile.to_json();
    j["segmenter"] = settings.segmenter.to_json();

    Json ui;
    ui["render_size"]    = settings.render_size;
    ui["render_samples"] = settings.render_samples;
    ui["use_llm"]        = settings.use_llm;
    ui["auto_export"]    = settings.auto_export;
    ui["write_report"]   = settings.write_report;
    ui["max_iterations"] = settings.max_iterations;
    ui["wireframe"]      = wireframe;
    ui["mode"]           = int(mode);
    ui["log_min_level"]  = log_min_level;
    j["ui"] = ui;

    Json recents = Json::array();
    for (const fs::path& p : recent_meshes) recents.push_back(p.string());
    j["recent"] = recents;

    j["project_dir"] = paths::project_dir().string();

    std::string error;
    paths::ensure_dir(paths::config_dir());
    if (!json_save_file(paths::config_file().string(), j, error))
        RD_WARN("cannot save settings: %s", error.c_str());
}

void AppState::load_settings()
{
    Json        j;
    std::string error;
    if (!json_load_file(paths::config_file().string(), j, error)) return;

    settings.llm     = LlmConfig::from_json(json_object_or_empty(j, "llm"));
    settings.profile = TargetProfile::from_json(json_object_or_empty(j, "profile"));
    settings.segmenter = SegmenterOptions::from_json(json_object_or_empty(j, "segmenter"));

    const Json& ui = json_object_or_empty(j, "ui");
    settings.render_size    = std::clamp(json_get<int>(ui, "render_size", settings.render_size), 128, 2048);
    settings.render_samples = std::clamp(json_get<int>(ui, "render_samples", settings.render_samples), 0, 8);
    settings.use_llm        = json_get<bool>(ui, "use_llm", settings.use_llm);
    settings.auto_export    = json_get<bool>(ui, "auto_export", settings.auto_export);
    settings.write_report   = json_get<bool>(ui, "write_report", settings.write_report);
    settings.max_iterations = std::clamp(json_get<int>(ui, "max_iterations", 0), 0, 20);
    wireframe               = json_get<bool>(ui, "wireframe", wireframe);
    mode                    = RenderMode(std::clamp(json_get<int>(ui, "mode", 0), 0, 6));
    log_min_level           = std::clamp(json_get<int>(ui, "log_min_level", 1), 0, 4);

    // Oldest first, because push_recent inserts at the front. Going through it
    // rather than push_back is what collapses entries a previous build wrote
    // twice under two spellings.
    const Json& recents = json_array_or_empty(j, "recent");
    for (auto it = recents.rbegin(); it != recents.rend(); ++it) {
        if (!it->is_string()) continue;
        const fs::path path = it->get<std::string>();
        std::error_code ec;
        if (fs::exists(path, ec)) push_recent(path);
    }

    const std::string project = json_get<std::string>(j, "project_dir", "");
    if (!project.empty()) paths::set_project_dir(project);
}

} // namespace rd::ui
