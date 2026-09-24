// Viewport, render gallery and the statistics readout.

#include "ui/panels.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "ui/file_dialog.h"
#include "ui/widgets.h"

#include <algorithm>
#include <imgui.h>

namespace rd::ui {
namespace {

const char* const kModeNames[] = {"Shaded", "Regions", "Silhouette",
                                  "Normals", "Curvature", "UV checker", "Clay"};
const char* const kSourceNames[] = {"Low poly", "High poly", "Compare"};

// Draws one mesh into `renderer` and blits it into the current window as an
// image, handling the empty case.
void draw_mesh_view(AppState& app, Renderer& renderer, const GpuMesh& mesh, bool valid,
                    const ImVec2& size, const char* caption, bool allow_input)
{
    const Palette& p = palette();

    if (size.x < 16.0f || size.y < 16.0f) return;

    if (!valid || !mesh.valid() || !renderer.ready()) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y},
                                                  col(p.canvas), 8.0f);
        const char* text = valid ? "GPU renderer unavailable"
                                 : (app.mesh_path.empty() ? "open a mesh to see it here"
                                                          : "not built yet");
        const ImVec2 ts  = ImGui::CalcTextSize(text);
        ImGui::GetWindowDrawList()->AddText(
            {pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f},
            col(p.text_faint), text);
        ImGui::Dummy(size);
        return;
    }

    ViewCamera cam;
    if (app.use_profile_camera) {
        std::vector<ViewCamera> cameras;
        app.pipeline.with_results([&](const PipelineResults& r) { cameras = r.cameras; });
        if (!cameras.empty()) {
            const int index = std::clamp(app.profile_camera_index, 0, int(cameras.size()) - 1);
            cam = cameras[size_t(index)];
        } else {
            cam = app.camera.to_view_camera(app.scene_bounds);
        }
    } else {
        cam = app.camera.to_view_camera(app.scene_bounds);
    }

    RenderOptions opts;
    opts.width   = std::max(16, int(size.x));
    opts.height  = std::max(16, int(size.y));
    opts.samples = 4;
    opts.mode    = app.mode;
    opts.wireframe_overlay = app.wireframe;
    opts.background = {p.canvas.x, p.canvas.y, p.canvas.z};
    opts.flip_y = false;

    const unsigned tex = renderer.render_to_gl_texture(mesh, cam, opts);
    const ImVec2   pos = ImGui::GetCursorScreenPos();

    if (tex) {
        // The framebuffer is bottom-up, so flip the v coordinate here.
        ImGui::Image(ImTextureRef(ImTextureID(tex)), size, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::Dummy(size);
    }

    if (caption && *caption) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(caption);
        dl->AddRectFilled({pos.x + 10.0f, pos.y + 10.0f},
                          {pos.x + 22.0f + ts.x, pos.y + 18.0f + ts.y},
                          col(p.canvas, 0.75f), 6.0f);
        dl->AddText({pos.x + 16.0f, pos.y + 14.0f}, col(p.text_dim), caption);
    }

    if (!allow_input || app.use_profile_camera) return;

    // Orbit / pan / zoom while the image is hovered.
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##orbit", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    if (active) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Drag left, the model turns left: the cursor takes the surface with
            // it, the way a hand on a turntable would. Increasing yaw walks the
            // camera toward +X, which sits on the left of the screen, so the
            // model sweeps right; a leftward drag therefore has to lower it.
            app.camera.yaw   += delta.x * 0.4f;
            app.camera.pitch  = clampf(app.camera.pitch + delta.y * 0.3f, -89.0f, 89.0f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                   ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            const float radius = std::max(app.scene_bounds.diagonal() * 0.5f, kEps);
            const Vec3  forward = normalize(cam.target - cam.eye);
            Vec3 right = normalize(cross(forward, Vec3{0, 1, 0}));
            if (length2(right) < 0.5f) right = Vec3{1, 0, 0};
            const Vec3 up = normalize(cross(right, forward));
            const float scale = radius * app.camera.distance * 0.0022f;
            app.camera.target -= right * (delta.x * scale);
            app.camera.target += up * (delta.y * scale);
        }
    }
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            app.camera.distance = clampf(app.camera.distance * (1.0f - wheel * 0.12f),
                                         0.15f, 40.0f);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            app.camera.frame(app.scene_bounds);
    }
}

} // namespace

// ---------------------------------------------------------------------------
void panel_viewport(AppState& app)
{
    if (!app.show_viewport) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    if (!ImGui::Begin("Viewport", &app.show_viewport)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();

    // --- toolbar -------------------------------------------------------------
    {
        const float toolbar_width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();

        int source = int(app.source);
        ImGui::PushItemWidth(toolbar_width * 0.30f);
        if (ImGui::BeginChild("##src", ImVec2(toolbar_width * 0.30f, 0),
                              ImGuiChildFlags_AutoResizeY)) {
            // Choosing by hand ends the automatic switching for good: after this
            // the viewport shows what was asked for and nothing else.
            if (segmented("source", &source, kSourceNames, 3)) {
                app.source           = ViewSource(source);
                app.auto_view_source = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopItemWidth();
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        int mode = int(app.mode);
        if (ImGui::Combo("##mode", &mode, kModeNames, IM_ARRAYSIZE(kModeNames))) {
            app.mode    = RenderMode(mode);
            app.gpu_version = 0;    // region colours may need re-uploading
        }

        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &app.wireframe);

        ImGui::SameLine();
        std::vector<ViewCamera> cameras;
        app.pipeline.with_results([&](const PipelineResults& r) { cameras = r.cameras; });
        ImGui::BeginDisabled(cameras.empty());
        ImGui::Checkbox("Profile camera", &app.use_profile_camera);
        ImGui::EndDisabled();

        if (app.use_profile_camera && !cameras.empty()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(190.0f);
            std::vector<const char*> names;
            for (const ViewCamera& c : cameras) names.push_back(c.name.c_str());
            ImGui::Combo("##cam", &app.profile_camera_index, names.data(), int(names.size()));
        }

        ImGui::SameLine();
        const float right_width = 96.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, ImGui::GetContentRegionAvail().x - right_width));
        if (secondary_button("Frame", ImVec2(right_width, 0)))
            app.camera.frame(app.scene_bounds);
    }

    spacer(4.0f);

    // --- render surface -------------------------------------------------------
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (app.source == ViewSource::SideBySide) {
        const float half = (avail.x - 8.0f) * 0.5f;
        draw_mesh_view(app, app.renderer_alt, app.gpu_high, app.gpu_high_ok,
                       ImVec2(half, avail.y), "high poly", false);
        ImGui::SameLine(0.0f, 8.0f);
        draw_mesh_view(app, app.renderer, app.gpu_low, app.gpu_low_ok,
                       ImVec2(half, avail.y), "low poly", true);
    } else if (app.source == ViewSource::HighPoly) {
        draw_mesh_view(app, app.renderer, app.gpu_high, app.gpu_high_ok, avail, nullptr, true);
    } else {
        draw_mesh_view(app, app.renderer, app.gpu_low, app.gpu_low_ok, avail, nullptr, true);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
void panel_gallery(AppState& app)
{
    if (!app.show_gallery) return;
    if (!ImGui::Begin("Renders", &app.show_gallery)) {
        ImGui::End();
        return;
    }

    struct Row {
        std::string           title;
        std::vector<std::pair<std::string, fs::path>> images;
    };
    std::vector<Row> rows;

    app.pipeline.with_results([&](const PipelineResults& r) {
        auto collect = [&](const char* title, const ViewSet& set) {
            if (set.entries.empty()) return;
            Row row;
            row.title = title;
            for (const ViewSet::Entry& e : set.entries) row.images.emplace_back(e.name, e.file);
            rows.push_back(std::move(row));
        };
        collect("High poly reference", r.reference_views);
        collect("Region overlay", r.region_views);
        collect("Low poly, latest iteration", r.candidate_views);

        if (r.bake.ok && !r.bake.diffuse.empty()) {
            Row row;
            row.title = "Baked texture";
            row.images.emplace_back("diffuse", paths::iteration_dir(
                                                   r.iterations.empty()
                                                       ? 1 : r.iterations.back().index) /
                                                   "diffuse.png");
            rows.push_back(std::move(row));
        }
    });

    if (rows.empty()) {
        empty_state("No renders yet",
                    "Run the pipeline and the reference shots, the region overlay and "
                    "each iteration of the low poly will collect here. These are the "
                    "exact images the director is shown.");
        ImGui::End();
        return;
    }

    static float thumb = 200.0f;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("##thumb", &thumb, 96.0f, 420.0f, "size %.0f px");
    ImGui::SameLine();
    if (secondary_button("Open folder")) reveal_in_file_manager(paths::renders_dir());
    ImGui::SameLine();
    if (secondary_button("Reload")) app.images.clear();

    divider();

    for (const Row& row : rows) {
        subheading(row.title.c_str());
        spacer(4.0f);

        const float available = ImGui::GetContentRegionAvail().x;
        float       used      = 0.0f;
        bool        first     = true;

        for (const auto& [name, path] : row.images) {
            int w = 0, h = 0;
            const unsigned tex = app.images.get(path, &w, &h);
            const float aspect = (w > 0 && h > 0) ? float(w) / float(h) : 1.0f;
            const ImVec2 size{thumb * aspect, thumb};

            if (!first && used + size.x + 10.0f > available) {
                used  = 0.0f;
                first = true;
            } else if (!first) {
                ImGui::SameLine(0.0f, 10.0f);
            }

            ImGui::BeginGroup();
            if (tex) {
                ImGui::Image(ImTextureRef(ImTextureID(tex)), size);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s", name.c_str(), path.filename().string().c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        reveal_in_file_manager(path);
                }
            } else {
                ImGui::Dummy(size);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            used += size.x + 10.0f;
            first = false;
        }
        spacer(10.0f);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
void panel_stats(AppState& app)
{
    if (!app.show_stats) return;
    if (!ImGui::Begin("Statistics", &app.show_stats)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();

    size_t high_tris = 0, low_tris = 0, low_verts = 0;
    size_t regions = 0;
    float  sil_mean = 0.0f, sil_worst = 0.0f;
    int    errors = 0, warnings = 0;
    bool   validated = false;
    double total_seconds = 0.0;
    Mesh::Stats low_stats;
    BakeResult  bake;
    ExportResult exported;
    int          max_triangles = app.settings.profile.max_triangles;
    int          max_vertices  = app.settings.profile.max_vertices;

    app.pipeline.with_results([&](const PipelineResults& r) {
        high_tris = r.highpoly.triangle_count();
        low_tris  = r.lowpoly.triangle_count();
        low_verts = r.lowpoly.vertex_count();
        regions   = r.segmentation.size();
        sil_mean  = r.silhouette.mean;
        sil_worst = r.silhouette.worst;
        errors    = r.validation.errors;
        warnings  = r.validation.warnings;
        validated = !r.validation.checks.empty();
        total_seconds = r.total_seconds;
        if (!r.lowpoly.empty()) low_stats = r.lowpoly.compute_stats();
        bake     = r.bake;
        exported = r.exported;
    });

    if (low_tris == 0 && high_tris == 0) {
        empty_state("Nothing measured yet",
                    "Load a high poly and run the pipeline. Everything the validator "
                    "looks at shows up here as you go.");
        ImGui::End();
        return;
    }

    // --- headline numbers ----------------------------------------------------
    {
        const float width = ImGui::GetContentRegionAvail().x / 4.0f;
        stat("high poly triangles", format("%zu", high_tris).c_str(), p.text_dim, width);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width);
        stat("low poly triangles",
             format("%zu", low_tris).c_str(),
             int(low_tris) > max_triangles ? p.danger : p.accent, width);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width);
        stat("reduction",
             high_tris ? format("%.0f%%", 100.0 * (1.0 - double(low_tris) / double(high_tris))).c_str()
                       : "-",
             p.success, width);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width);
        stat("silhouette error", format("%.4f", sil_mean).c_str(),
             sil_mean > app.settings.profile.max_silhouette_error ? p.danger
             : sil_mean > app.settings.profile.target_silhouette_error ? p.warning : p.success,
             width);
    }

    divider();

    // --- budgets -------------------------------------------------------------
    if (card_begin("budgets", "Against the profile")) {
        meter("Triangles", float(low_tris), float(max_triangles),
              format("%zu / %d", low_tris, max_triangles).c_str());
        spacer(8.0f);
        meter("Vertices", float(low_verts), float(max_vertices),
              format("%zu / %d", low_verts, max_vertices).c_str());
        spacer(8.0f);
        meter("Silhouette", sil_mean, app.settings.profile.max_silhouette_error,
              format("%.4f / %.4f", sil_mean, app.settings.profile.max_silhouette_error).c_str());
        if (!bake.diffuse.empty()) {
            spacer(8.0f);
            meter("Palette", float(bake.palette.size()),
                  float(std::max(1, app.settings.profile.texture.palette_colors)),
                  format("%zu / %d", bake.palette.size(),
                         app.settings.profile.texture.palette_colors).c_str());
        }
        card_end();
    }

    spacer(8.0f);

    // --- mesh health ---------------------------------------------------------
    if (card_begin("health", "Low poly health")) {
        begin_form("health_form", 190.0f);
        key_value("Shells", "%zu", low_stats.shells);
        key_value("Boundary edges", "%zu", low_stats.boundary_edges);
        key_value("Non manifold edges", "%zu", low_stats.nonmanifold_edges);
        key_value("Worst triangle quality", "%.3f", low_stats.min_quality);
        key_value("Mean edge length", "%.5f", low_stats.mean_edge);
        key_value("Regions", "%zu", regions);
        key_value("Worst silhouette view", "%.4f", sil_worst);
        end_form();
        card_end();
    }

    spacer(8.0f);

    if (bake.ok && card_begin("bake_stats", "Bake")) {
        begin_form("bake_form", 190.0f);
        key_value("Atlas", "%dx%d", bake.diffuse.width, bake.diffuse.height);
        key_value("Charts", "%d", bake.charts);
        key_value("Atlas pages", "%d", bake.atlas_count);
        key_value("Utilisation", "%.1f%%", bake.uv_utilisation * 100.0f);
        key_value("Worst uv stretch", "%.2f", bake.uv_max_stretch);
        key_value("Texels baked", "%zu", bake.texels_baked);
        key_value("Rays cast", "%zu", bake.rays_cast);
        end_form();
        card_end();
    }

    if (!exported.files.empty()) {
        spacer(8.0f);
        if (card_begin("export_stats", "Exported")) {
            begin_form("export_form", 190.0f);
            key_value("Files", "%zu", exported.files.size());
            key_value("Total size", "%s", paths::format_bytes(exported.total_bytes).c_str());
            key_value("Strips", "%zu", exported.strips.strip_count);
            key_value("Average strip", "%.2f triangles", exported.strips.average_length);
            key_value("ACMR", "%.3f -> %.3f", exported.optimisation.acmr_before,
                      exported.optimisation.acmr_after);
            end_form();
            spacer(6.0f);
            if (secondary_button("Open export folder"))
                reveal_in_file_manager(paths::export_dir());
            card_end();
        }
    }

    if (validated) {
        spacer(8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
        ImGui::Text("%d error%s, %d warning%s, run took %s", errors, errors == 1 ? "" : "s",
                    warnings, warnings == 1 ? "" : "s",
                    format_duration(total_seconds).c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

} // namespace rd::ui
