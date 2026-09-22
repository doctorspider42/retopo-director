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
    toast       = text;
    toast_until = ImGui::GetTime() + seconds;
}

void AppState::push_recent(const fs::path& p)
{
    if (p.empty()) return;
    recent_meshes.erase(std::remove(recent_meshes.begin(), recent_meshes.end(), p),
                        recent_meshes.end());
    recent_meshes.insert(recent_meshes.begin(), p);
    if (recent_meshes.size() > 10) recent_meshes.resize(10);
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
    });
    panel_version = version;
}

void AppState::upload_meshes()
{
    const uint64_t version = pipeline.version();
    if (version == gpu_version) return;
    gpu_version = version;

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
        if (bounds.valid() && !scene_bounds.valid()) camera.frame(bounds);
        if (bounds.valid()) scene_bounds = bounds;
    });
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

    for (const Json& p : json_array_or_empty(j, "recent")) {
        if (!p.is_string()) continue;
        const fs::path path = p.get<std::string>();
        std::error_code ec;
        if (fs::exists(path, ec)) recent_meshes.push_back(path);
    }

    const std::string project = json_get<std::string>(j, "project_dir", "");
    if (!project.empty()) paths::set_project_dir(project);
}

} // namespace rd::ui
