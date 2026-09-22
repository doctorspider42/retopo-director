// Pipeline control, the knob panel, the target profile, the director and the
// validation report.

#include "ui/panels.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "segment/sam.h"
#include "ui/file_dialog.h"
#include "ui/widgets.h"

#include <algorithm>
#include <filesystem>
#include <imgui.h>

namespace rd::ui {
namespace {

const char* const kBackendNames[]  = {"Auto", "Quad field", "Quadric"};
const char* const kFidelityNames[] = {"Geometry", "Balanced", "Texture"};
const char* const kLlmNames[]      = {"Disabled", "Claude CLI", "Codex CLI", "OpenAI API"};
const char* const kSegmenterNames[] = {"Geometric", "SAM", "Auto"};

ImVec4 stage_color(Stage s)
{
    const Palette& p = palette();
    switch (s) {
    case Stage::Failed:    return p.danger;
    case Stage::Cancelled: return p.warning;
    case Stage::Done:      return p.success;
    case Stage::Idle:      return p.text_faint;
    default:               return p.accent;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void action_open_mesh(AppState& app)
{
    const fs::path chosen = open_file_dialog(
        "Open a high poly mesh",
        {{"Meshes", "obj;gltf;glb;fbx"},
         {"Wavefront OBJ", "obj"},
         {"glTF", "gltf;glb"},
         {"FBX", "fbx"}},
        app.mesh_path.empty() ? fs::path{} : app.mesh_path.parent_path());
    if (chosen.empty()) return;

    app.mesh_path = chosen;
    app.push_recent(chosen);
    app.notify("Loaded " + chosen.filename().string());
}

void action_run(AppState& app)
{
    if (app.mesh_path.empty()) {
        action_open_mesh(app);
        if (app.mesh_path.empty()) return;
    }
    if (app.pipeline.running()) return;

    app.panel_dirty = false;
    app.images.clear();
    if (!app.pipeline.start(app.mesh_path, app.settings))
        app.notify("A run is already in progress", 3.0);
}

void action_rebuild(AppState& app)
{
    if (app.pipeline.running()) return;
    action_apply_panel(app);

    PipelineSettings settings = app.settings;
    settings.use_llm = false;             // geometry only, no second opinion
    if (!app.pipeline.rebuild_geometry(settings))
        app.notify("Nothing to rebuild yet; run the full pipeline first", 4.0);
}

void action_apply_panel(AppState& app)
{
    if (!app.panel_dirty) return;
    app.panel.clamp();
    app.panel.resolve_budgets(app.settings.profile.max_triangles);
    app.pipeline.set_panel(app.panel);
    app.knobs_json_buffer = json_dump(app.panel.to_json());
    app.panel_dirty = false;
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
void panel_pipeline(AppState& app)
{
    if (!app.show_pipeline) return;
    if (!ImGui::Begin("Pipeline", &app.show_pipeline)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();
    const bool running = app.pipeline.running();

    // --- source ---------------------------------------------------------------
    if (card_begin("source_card", "Source",
                   app.mesh_path.empty() ? "no mesh loaded" : nullptr)) {
        if (!app.mesh_path.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, p.text);
            ImGui::TextUnformatted(app.mesh_path.filename().string().c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
            ImGui::TextWrapped("%s", app.mesh_path.parent_path().string().c_str());
            ImGui::PopStyleColor();

            app.pipeline.with_results([&](const PipelineResults& r) {
                if (r.highpoly.empty()) return;
                spacer(4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::Text("%zu triangles, %zu vertices%s", r.highpoly.triangle_count(),
                            r.highpoly.vertex_count(),
                            r.highpoly.armature.empty()
                                ? ", no armature"
                                : format(", %zu joints", r.highpoly.armature.size()).c_str());
                if (r.analysis.valid())
                    ImGui::Text("symmetry %s, score %.2f", r.analysis.symmetry.axis_name(),
                                r.analysis.symmetry.score);
                ImGui::PopStyleColor();
            });
        }
        spacer(6.0f);

        ImGui::BeginDisabled(running);
        if (secondary_button("Open mesh...")) action_open_mesh(app);
        if (!app.recent_meshes.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Recent")) ImGui::OpenPopup("recent_meshes");
            if (ImGui::BeginPopup("recent_meshes")) {
                for (const fs::path& path : app.recent_meshes) {
                    if (ImGui::MenuItem(path.filename().string().c_str())) {
                        app.mesh_path = path;
                        app.push_recent(path);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", path.string().c_str());
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndDisabled();
        card_end();
    }

    spacer(8.0f);

    // --- run controls ---------------------------------------------------------
    if (card_begin("run_card", "Run")) {
        const float full = ImGui::GetContentRegionAvail().x;
        if (running) {
            if (danger_button("Cancel", ImVec2(full, 0))) app.pipeline.cancel();
        } else {
            ImGui::BeginDisabled(app.mesh_path.empty());
            if (primary_button("Run the director", ImVec2(full, 0))) action_run(app);
            ImGui::EndDisabled();
        }

        spacer(6.0f);
        ImGui::BeginDisabled(running);
        if (secondary_button("Rebuild geometry only", ImVec2(full, 0))) action_rebuild(app);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Re-runs density, retopo, bake and validation with the "
                              "current knob panel and without asking the model anything.");
        ImGui::EndDisabled();

        spacer(10.0f);
        begin_form("run_form", 150.0f);
        toggle("Use the director", &app.settings.use_llm,
               "When off, the panel stays exactly as you set it and no request is sent.");
        toggle("Export on finish", &app.settings.auto_export);
        toggle("Write the report", &app.settings.write_report);
        slider_int("Max iterations", &app.settings.max_iterations, 0, 12, 0,
                   "0 takes the value from the target profile.");
        slider_int("Render size", &app.settings.render_size, 256, 1536, 640,
                   "Pixels per side for the images the director is shown.");
        end_form();
        card_end();
    }

    spacer(8.0f);

    // --- progress -------------------------------------------------------------
    {
        const Stage stage = app.pipeline.stage();
        if (card_begin("progress_card", "Progress")) {
            dot(stage_color(stage), 5.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, stage_color(stage));
            ImGui::TextUnformatted(stage_name(stage));
            ImGui::PopStyleColor();

            const int iteration = app.pipeline.iteration();
            if (iteration > 0) {
                ImGui::SameLine();
                badge(format("iteration %d", iteration).c_str(), p.accent);
            }

            spacer(6.0f);
            const std::string msg = app.pipeline.message();
            progress_bar(app.pipeline.progress(), msg.empty() ? nullptr : msg.c_str());

            const std::string err = app.pipeline.error();
            if (!err.empty()) {
                spacer(6.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
                ImGui::TextWrapped("%s", err.c_str());
                ImGui::PopStyleColor();
            }
            card_end();
        }
    }

    // --- iteration history ----------------------------------------------------
    std::vector<IterationRecord> iterations;
    app.pipeline.with_results([&](const PipelineResults& r) { iterations = r.iterations; });

    if (!iterations.empty()) {
        spacer(8.0f);
        if (card_begin("iterations_card", "Iterations")) {
            if (ImGui::BeginTable("iter", 5,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.0f);
                ImGui::TableSetupColumn("triangles");
                ImGui::TableSetupColumn("silhouette");
                ImGui::TableSetupColumn("checks");
                ImGui::TableSetupColumn("verdict");
                ImGui::TableHeadersRow();

                for (const IterationRecord& it : iterations) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", it.index);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", it.triangles);
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        it.silhouette.mean > app.settings.profile.max_silhouette_error
                            ? p.danger
                            : (it.silhouette.mean > app.settings.profile.target_silhouette_error
                                   ? p.warning : p.success));
                    ImGui::Text("%.4f", it.silhouette.mean);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          it.validation_passed ? p.success : p.danger);
                    ImGui::TextUnformatted(it.validation_passed ? "passed" : "failed");
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                    ImGui::TextUnformatted(it.verdict.empty() ? "-" : it.verdict.c_str());
                    ImGui::PopStyleColor();
                    if (!it.critique.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", it.critique.c_str());
                }
                ImGui::EndTable();
            }
            card_end();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Knob panel
// ---------------------------------------------------------------------------
void panel_regions(AppState& app)
{
    if (!app.show_regions) return;
    if (!ImGui::Begin("Knob panel", &app.show_regions)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();

    Segmentation seg;
    std::vector<int> actual;
    app.pipeline.with_results([&](const PipelineResults& r) {
        seg    = r.segmentation;
        actual = r.retopo.region_triangles;
    });

    if (app.panel.regions.empty()) {
        empty_state("No regions yet",
                    "The panel fills in once the mesh has been segmented. Everything "
                    "here is what the director writes and what you can overrule; the "
                    "geometry engine reads nothing else.");
        ImGui::End();
        return;
    }

    // --- global knobs ---------------------------------------------------------
    if (ImGui::BeginTabBar("knob_tabs")) {
        if (ImGui::BeginTabItem("Global")) {
            spacer(6.0f);
            GlobalKnobs& g = app.panel.global;
            bool changed = false;

            begin_form("global_form", 190.0f);
            int backend = int(g.backend);
            if (combo("Backend", &backend, kBackendNames, 3,
                      "Auto picks the quad field for skinned meshes and the quadric "
                      "collapse for everything else.")) {
                g.backend = RetopoBackend(backend);
                changed = true;
            }
            changed |= toggle("Enforce symmetry", &g.enforce_symmetry,
                              "Mirrors the result across the detected plane. The profile "
                              "can require this regardless.");
            changed |= slider_float("Joint loop density", &g.joint_loop_density, 0.0f, 3.0f,
                                    1.0f, "Extra edge rings around deforming joints.");
            changed |= slider_float("Merge aggressiveness", &g.merge_aggressiveness, 0.0f, 1.0f,
                                    0.5f, "How eagerly flat areas collapse.");
            changed |= slider_float("Curvature influence", &g.curvature_influence, 0.0f, 1.0f,
                                    0.6f, "Global weight of the curvature term in the "
                                          "density field.");
            changed |= slider_float("Silhouette weight", &g.silhouette_weight, 0.0f, 1.0f, 0.7f,
                                    "Protection for triangles that form an outline in the "
                                    "profile cameras.");
            changed |= slider_int("Smoothing passes", &g.smoothing_iterations, 0, 16, 3,
                                  "Tangential relaxation passes in the quad field backend.");
            changed |= slider_float("Quad dominance", &g.quad_dominance, 0.0f, 1.0f, 0.7f,
                                    "How hard to pair triangles into quads before "
                                    "re-triangulating. Higher means longer strips.");
            end_form();

            divider("Bake");
            begin_form("bake_form_knobs", 190.0f);
            changed |= toggle("Ambient occlusion", &g.bake_ambient_occlusion);
            changed |= slider_float("AO intensity", &g.ao_intensity, 0.0f, 1.0f, 0.75f);
            changed |= toggle("Vertex colours", &g.bake_vertex_colors);
            changed |= slider_float("UV padding", &g.uv_padding_texels, 0.0f, 16.0f, 4.0f,
                                    nullptr, "%.0f texels");
            changed |= slider_float("UV stretch tolerance", &g.uv_stretch_tolerance, 0.0f, 1.0f,
                                    0.15f);
            end_form();

            if (!g.notes.empty()) {
                divider("Director notes");
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextWrapped("%s", g.notes.c_str());
                ImGui::PopStyleColor();
            }

            if (changed) app.panel_dirty = true;
            ImGui::EndTabItem();
        }

        // --- per region -------------------------------------------------------
        if (ImGui::BeginTabItem("Regions")) {
            spacer(6.0f);

            const int total = app.panel.total_budget();
            meter("Allocated", float(total), float(app.settings.profile.max_triangles),
                  format("%d / %d", total, app.settings.profile.max_triangles).c_str());
            spacer(8.0f);

            const float list_width = std::min(240.0f, ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::BeginChild("region_list", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
            for (int i = 0; i < int(app.panel.regions.size()); ++i) {
                const RegionKnobs& k = app.panel.regions[size_t(i)];
                ImGui::PushID(i);

                const Region* region = seg.find(k.id);
                dot(region ? ImVec4{region->debug_color.x, region->debug_color.y,
                                    region->debug_color.z, 1.0f}
                           : p.text_faint);

                const bool selected = (app.selected_region == i);
                if (ImGui::Selectable(k.name.c_str(), selected)) app.selected_region = i;

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                const std::string budget = format("%d", k.triangle_budget);
                const float tw = ImGui::CalcTextSize(budget.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(4.0f, ImGui::GetContentRegionAvail().x - tw));
                ImGui::TextUnformatted(budget.c_str());
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("region_detail", ImVec2(0, 0));

            if (app.selected_region >= 0 &&
                app.selected_region < int(app.panel.regions.size())) {
                RegionKnobs& k = app.panel.regions[size_t(app.selected_region)];
                bool changed = false;

                heading(k.name.c_str());
                if (const Region* region = seg.find(k.id)) {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                    ImGui::TextWrapped("%s - %.1f%% of the surface, curvature %.2f, "
                                       "%u source triangles",
                                       region->auto_label.c_str(), region->area_share * 100.0f,
                                       region->mean_curvature, region->triangle_count);
                    ImGui::PopStyleColor();
                }

                if (k.id < actual.size()) {
                    spacer(6.0f);
                    meter("Budget", float(actual[k.id]), float(std::max(1, k.triangle_budget)),
                          format("%d built / %d allocated", actual[k.id],
                                 k.triangle_budget).c_str());
                }

                divider();
                begin_form("region_form", 180.0f);
                changed |= slider_float("Share", &k.share, 0.0f, 20.0f, 1.0f,
                                        "Relative slice of the triangle budget. Doubling "
                                        "this doubles the region's share of what is left "
                                        "after every other region is weighed.");
                changed |= slider_float("Detail priority", &k.detail_priority, 0.0f, 1.0f, 0.5f,
                                        "Breaks ties when the budget runs short.");

                int fidelity = int(k.fidelity);
                if (combo("Fidelity", &fidelity, kFidelityNames, 3,
                          "Geometry: the shape must exist in the silhouette.\n"
                          "Texture: flatten it and let the bake carry the detail.")) {
                    k.fidelity = Fidelity(fidelity);
                    changed = true;
                }
                changed |= slider_float("Hard edge angle", &k.hard_edge_degrees, 0.0f, 180.0f,
                                        40.0f, "Creases sharper than this stay sharp.",
                                        "%.0f deg");
                changed |= toggle("Preserve silhouette", &k.preserve_silhouette);
                changed |= toggle("Preserve boundary", &k.preserve_boundary);
                changed |= slider_float("Symmetry lock", &k.symmetry_lock, 0.0f, 1.0f, 1.0f);
                changed |= slider_float("Curvature bias", &k.curvature_bias, 0.0f, 1.0f, 0.5f,
                                        "Follow curvature, or stay uniform.");
                end_form();

                if (!k.rationale.empty()) {
                    divider("Why the director set it this way");
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                    ImGui::TextWrapped("%s", k.rationale.c_str());
                    ImGui::PopStyleColor();
                }

                if (changed) {
                    app.panel_dirty = true;
                    app.panel.resolve_budgets(app.settings.profile.max_triangles);
                }
            } else {
                empty_state("Pick a region", "Select one on the left to see and change "
                                             "what it was given.");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // --- raw JSON ----------------------------------------------------------
        if (ImGui::BeginTabItem("JSON")) {
            spacer(6.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
            ImGui::TextWrapped("This is exactly what the director fills in and what the "
                               "geometry engine reads. Edit it here and press Apply to "
                               "overrule the model.");
            ImGui::PopStyleColor();
            spacer(6.0f);

            if (app.knobs_json_buffer.empty())
                app.knobs_json_buffer = json_dump(app.panel.to_json());

            const Fonts& f = fonts();
            ImGui::PushFont(f.mono, f.size_mono);
            std::vector<char> buffer(std::max<size_t>(app.knobs_json_buffer.size() * 2, 8192));
            std::snprintf(buffer.data(), buffer.size(), "%s", app.knobs_json_buffer.c_str());
            if (ImGui::InputTextMultiline("##json", buffer.data(), buffer.size(),
                                          ImVec2(-FLT_MIN, -60.0f))) {
                app.knobs_json_buffer = buffer.data();
                std::string error;
                const Json parsed = json_parse_lenient(app.knobs_json_buffer, error);
                app.knobs_json_valid = !parsed.is_null() && error.empty();
                app.knobs_json_error = error;
            }
            ImGui::PopFont();

            spacer(4.0f);
            ImGui::BeginDisabled(!app.knobs_json_valid);
            if (primary_button("Apply JSON")) {
                std::string error;
                const Json parsed = json_parse_lenient(app.knobs_json_buffer, error);
                if (!parsed.is_null()) {
                    app.panel = KnobPanel::from_json(parsed);
                    app.panel.clamp();
                    app.panel.resolve_budgets(app.settings.profile.max_triangles);
                    app.panel_dirty = true;
                    app.notify("Knob panel replaced from JSON");
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (secondary_button("Reload from run"))
                app.knobs_json_buffer = json_dump(app.panel.to_json());

            if (!app.knobs_json_valid) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
                ImGui::TextUnformatted(app.knobs_json_error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (app.panel_dirty) {
        spacer(6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, p.warning);
        ImGui::TextUnformatted("Unapplied changes");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (primary_button("Apply")) action_apply_panel(app);
        ImGui::SameLine();
        if (secondary_button("Apply and rebuild")) action_rebuild(app);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Target profile
// ---------------------------------------------------------------------------
void panel_profile(AppState& app)
{
    if (!app.show_profile) return;
    if (!ImGui::Begin("Target profile", &app.show_profile)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();
    TargetProfile& profile = app.settings.profile;

    // --- file row -------------------------------------------------------------
    {
        std::vector<std::string> labels;
        std::vector<const char*> label_ptrs;
        for (const fs::path& f : app.profile_files) labels.push_back(f.stem().string());
        for (const std::string& s : labels) label_ptrs.push_back(s.c_str());

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        if (!label_ptrs.empty() &&
            ImGui::Combo("##profile", &app.profile_index, label_ptrs.data(),
                         int(label_ptrs.size()))) {
            app.load_profile(app.profile_files[size_t(app.profile_index)]);
        }
        ImGui::SameLine();
        if (secondary_button("Load...")) {
            const fs::path chosen = open_file_dialog("Load a target profile",
                                                     {{"Profiles", "json"}},
                                                     paths::profiles_dir());
            if (!chosen.empty()) app.load_profile(chosen);
        }
        ImGui::SameLine();
        if (secondary_button("Save as...")) {
            const fs::path chosen = save_file_dialog("Save the target profile",
                                                     {{"Profiles", "json"}},
                                                     profile.name + ".json",
                                                     paths::config_dir() / "profiles");
            if (!chosen.empty()) {
                std::string error;
                paths::ensure_dir(chosen.parent_path());
                if (profile.save(chosen.string(), &error)) {
                    app.refresh_profiles();
                    app.notify("Saved " + chosen.filename().string());
                } else {
                    app.notify("Save failed: " + error, 6.0);
                }
            }
        }
    }

    divider();

    bool changed = false;

    if (card_begin("geometry_limits", "Geometry limits",
                   "Hard. The validator rejects anything past these.")) {
        begin_form("geo_form", 200.0f);
        changed |= slider_int("Max triangles", &profile.max_triangles, 32, 20000, 1400);
        changed |= slider_int("Max vertices", &profile.max_vertices, 32, 20000, 1000);
        changed |= slider_int("Max shells", &profile.max_shells, 0, 32, 1,
                              "0 means unlimited.");
        changed |= slider_int("Bone influences", &profile.max_bone_influences, 0, 4, 2);
        changed |= toggle("Require manifold", &profile.require_manifold);
        changed |= toggle("Require symmetry", &profile.require_symmetry);
        changed |= toggle("Require strips", &profile.require_strips);
        changed |= slider_float("Min strip length", &profile.min_average_strip_len, 1.0f, 12.0f,
                                3.5f, nullptr, "%.1f triangles");
        changed |= slider_int("Vertex cache", &profile.vertex_cache_size, 4, 64, 16,
                              "The PS2 VU1 input buffer holds about 16 vertices.");
        end_form();
        card_end();
    }

    spacer(8.0f);

    if (card_begin("texture_limits", "Texture")) {
        begin_form("tex_form", 200.0f);
        changed |= slider_int("Width", &profile.texture.width, 8, 1024, 256);
        changed |= slider_int("Height", &profile.texture.height, 8, 1024, 256);
        changed |= slider_int("Palette colours", &profile.texture.palette_colors, 0, 256, 256,
                              "0 keeps truecolor. 16 is a 4 bit CLUT, 256 an 8 bit one.");
        changed |= slider_int("Pages", &profile.texture.count, 1, 8, 1);
        changed |= toggle("Dither", &profile.texture.dithering);
        changed |= toggle("Require UVs", &profile.require_uvs);
        changed |= toggle("Require vertex colours", &profile.require_vertex_colors);
        changed |= toggle("Bake lighting in", &profile.bake_lighting_to_diffuse,
                          "On hardware without per pixel lighting this is the shading model.");
        end_form();
        card_end();
    }

    spacer(8.0f);

    if (card_begin("framing_card", "Framing",
                   "Distances are in metres; the mesh is scaled to match.")) {
        begin_form("framing_form", 200.0f);
        changed |= slider_float("Reference height", &profile.reference_height_m, 0.1f, 5.0f,
                                1.8f, "How tall the subject is in the real world.",
                                "%.2f m");
        changed |= slider_int("Turntable views", &profile.turntable_views, 0, 24, 8);
        changed |= slider_float("Target silhouette error", &profile.target_silhouette_error,
                                0.0f, 0.2f, 0.02f, nullptr, "%.3f");
        changed |= slider_float("Max silhouette error", &profile.max_silhouette_error,
                                0.0f, 0.5f, 0.05f, nullptr, "%.3f");
        end_form();

        spacer(6.0f);
        for (size_t i = 0; i < profile.cameras.size(); ++i) {
            ProfileCamera& c = profile.cameras[i];
            ImGui::PushID(int(i));
            if (ImGui::TreeNode(c.name.c_str())) {
                begin_form("cam_form", 180.0f);
                changed |= text_input("Name", c.name);
                changed |= text_input("Description", c.description);
                changed |= slider_float("Yaw", &c.yaw_degrees, 0.0f, 360.0f, 180.0f, nullptr,
                                        "%.0f deg");
                changed |= slider_float("Pitch", &c.pitch_degrees, -89.0f, 89.0f, 10.0f,
                                        nullptr, "%.0f deg");
                changed |= slider_float("Distance", &c.distance_m, 0.0f, 20.0f, 3.0f,
                                        "0 auto frames the whole model.", "%.2f m");
                changed |= slider_float("Pivot height", &c.height_m, 0.0f, 3.0f, 1.1f, nullptr,
                                        "%.2f m");
                changed |= slider_float("Field of view", &c.fov_degrees, 10.0f, 120.0f, 40.0f,
                                        nullptr, "%.0f deg");
                changed |= toggle("Primary", &c.primary,
                                  "Primary cameras count double in the silhouette score.");
                end_form();
                spacer(4.0f);
                if (danger_button("Remove camera")) {
                    profile.cameras.erase(profile.cameras.begin() + long(i));
                    changed = true;
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        spacer(4.0f);
        if (secondary_button("Add camera")) {
            ProfileCamera c;
            c.name = format("camera_%zu", profile.cameras.size());
            profile.cameras.push_back(c);
            changed = true;
        }
        card_end();
    }

    spacer(8.0f);

    if (card_begin("direction_card", "Art direction",
                   "Handed to the director verbatim.")) {
        changed |= text_area("##art", profile.art_direction, 110.0f);
        spacer(4.0f);
        begin_form("iter_form", 200.0f);
        changed |= slider_int("Iterations", &profile.max_iterations, 0, 12, 4);
        end_form();
        card_end();
    }

    const std::vector<std::string> warnings = profile.warnings();
    if (!warnings.empty()) {
        spacer(8.0f);
        for (const std::string& w : warnings) {
            ImGui::PushStyleColor(ImGuiCol_Text, p.warning);
            ImGui::TextWrapped("%s", w.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (changed) profile.clamp();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Director
// ---------------------------------------------------------------------------
void panel_director(AppState& app)
{
    if (!app.show_director) return;
    if (!ImGui::Begin("Director", &app.show_director)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();
    LlmConfig& cfg = app.settings.llm;

    if (card_begin("backend_card", "Backend")) {
        int kind = int(cfg.kind);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (segmented("llm_kind", &kind, kLlmNames, 4)) cfg.kind = LlmBackendKind(kind);

        spacer(8.0f);
        begin_form("llm_form", 170.0f);
        switch (cfg.kind) {
        case LlmBackendKind::ClaudeCli:
            text_input("Executable", cfg.claude_path, "claude");
            text_input("Model", cfg.claude_model, "leave empty for the CLI default");
            break;
        case LlmBackendKind::CodexCli:
            text_input("Executable", cfg.codex_path, "codex");
            text_input("Model", cfg.codex_model, "leave empty for the CLI default");
            break;
        case LlmBackendKind::OpenAiApi:
            text_input("Base URL", cfg.openai_base_url, "https://api.openai.com/v1");
            text_input("Model", cfg.openai_model, "gpt-4o");
            text_input("API key", cfg.openai_api_key,
                       "empty reads OPENAI_API_KEY from the environment",
                       "The key is never written to the settings file.");
            toggle("Send images", &cfg.openai_send_images,
                   "Off saves tokens but leaves the model judging by numbers alone.");
            break;
        default:
            break;
        }
        slider_int("Timeout", &cfg.timeout_seconds, 30, 1800, 300, nullptr);
        end_form();

        spacer(8.0f);
        {
            const std::unique_ptr<ILlmBackend> probe = make_llm_backend(cfg);
            std::string reason;
            const bool ok = probe && probe->available(&reason);
            dot(ok ? p.success : p.danger);
            ImGui::PushStyleColor(ImGuiCol_Text, ok ? p.success : p.danger);
            ImGui::TextUnformatted(ok ? "ready" : reason.c_str());
            ImGui::PopStyleColor();
            if (probe) {
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                ImGui::TextWrapped("%s", probe->describe().c_str());
                ImGui::PopStyleColor();
            }
        }
        card_end();
    }

    spacer(8.0f);

    // --- region split ---------------------------------------------------------
    // The segmenter decides what the model is asked to name, so it belongs next
    // to the backend rather than in the knob panel: it makes no geometry
    // decisions, it only draws the patches.
    if (card_begin("segmenter_card", "Region split")) {
        SegmenterOptions& seg = app.settings.segmenter;

        int kind = int(seg.kind);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (segmented("segmenter_kind", &kind, kSegmenterNames, 3))
            seg.kind = SegmenterKind(kind);

        spacer(8.0f);
        if (seg.kind == SegmenterKind::Geometric) {
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
            ImGui::TextWrapped(
                "A multi source Dijkstra over the dual graph. No model, no download, "
                "single digit milliseconds, and every region maps onto source triangles.");
            ImGui::PopStyleColor();
        } else {
            begin_form("sam_form", 170.0f);
            text_input("Checkpoint", seg.sam.checkpoint, "sam_vit_b_01ec64.pth",
                       "The weights file. Nothing is downloaded on your behalf.");
            text_input("Model type", seg.sam.model_type, "vit_b");
            text_input("Device", seg.sam.device, "auto",
                       "auto | cuda | cpu | mps. On the CPU this is roughly "
                       "fifteen seconds a view.");
            text_input("Python", seg.sam.python, "empty picks python3 off PATH");
            slider_int("Sample grid", &seg.sam.points_per_side, 4, 32, 16,
                       "Points per side the mask generator starts from. "
                       "Doubling it roughly quadruples the work.");
            slider_int("Pixel stride", &seg.sam.pixel_stride, 1, 8, 2,
                       "One projection ray per Nth pixel. The Dijkstra fill "
                       "cleans up whatever the rays miss.");
            slider_int("Timeout", &seg.sam.timeout_seconds, 30, 3600, 900, nullptr);
            end_form();

            spacer(8.0f);
            {
                std::error_code ec;
                std::string reason;
                if (seg.sam.checkpoint.empty())
                    reason = "no checkpoint set";
                else if (!std::filesystem::exists(seg.sam.checkpoint, ec))
                    reason = "the checkpoint is not there";
                else if (seg.sam.script.empty() && find_sam_script().empty())
                    reason = "tools/sam_server.py not found";

                const bool ok = reason.empty();
                dot(ok ? p.success : p.warning);
                ImGui::PushStyleColor(ImGuiCol_Text, ok ? p.success : p.warning);
                ImGui::TextUnformatted(ok ? "sidecar configured" : reason.c_str());
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                ImGui::TextWrapped(
                    "Whatever goes wrong - no checkpoint, no CUDA device, a sidecar "
                    "that falls over - costs a warning in the log, and the run "
                    "continues on the geometric split.");
                ImGui::PopStyleColor();
            }
        }
        card_end();
    }

    spacer(8.0f);

    // --- transcript -----------------------------------------------------------
    std::vector<LlmExchange> transcript;
    app.pipeline.with_results([&](const PipelineResults& r) { transcript = r.transcript; });

    subheading("Transcript");
    spacer(4.0f);

    if (transcript.empty()) {
        empty_state("Nothing said yet",
                    "Every request and reply lands here, and a copy of each goes into "
                    "the reports folder so a run can be audited after the fact.");
        ImGui::End();
        return;
    }

    for (size_t i = 0; i < transcript.size(); ++i) {
        const LlmExchange& e = transcript[i];
        ImGui::PushID(int(i));

        const ImVec4 tint = e.ok ? p.accent : p.danger;
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        const bool open = ImGui::TreeNodeEx(
            "##node", ImGuiTreeNodeFlags_SpanAvailWidth,
            "%s  -  %s, %d in / %d out tokens", e.stage.c_str(),
            format_duration(e.seconds).c_str(), e.prompt_tokens, e.completion_tokens);
        ImGui::PopStyleColor();

        if (open) {
            if (!e.image_labels.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                ImGui::TextWrapped("%zu images attached", e.image_labels.size());
                ImGui::PopStyleColor();
            }
            if (!e.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
                ImGui::TextWrapped("%s", e.error.c_str());
                ImGui::PopStyleColor();
            }

            const Fonts& f = fonts();
            if (ImGui::BeginTabBar("exchange")) {
                if (ImGui::BeginTabItem("Reply")) {
                    ImGui::PushFont(f.mono, f.size_mono);
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                    ImGui::TextWrapped("%s", e.reply.empty() ? "(empty)" : e.reply.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Prompt")) {
                    ImGui::PushFont(f.mono, f.size_mono);
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                    ImGui::TextWrapped("%s", e.prompt.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
void panel_validation(AppState& app)
{
    if (!app.show_validation) return;
    if (!ImGui::Begin("Validation", &app.show_validation)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();
    ValidationReport report;
    app.pipeline.with_results([&](const PipelineResults& r) { report = r.validation; });

    if (report.checks.empty()) {
        empty_state("Nothing validated yet",
                    "Once an iteration finishes, every hard check from the target "
                    "profile is listed here, pass or fail, with the number that "
                    "decided it.");
        ImGui::End();
        return;
    }

    const ImVec4 banner = report.passed ? p.success : p.danger;
    ImGui::PushStyleColor(ImGuiCol_Text, banner);
    heading(report.passed ? "Passed" : "Failed");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    badge(format("%d error%s", report.errors, report.errors == 1 ? "" : "s").c_str(),
          report.errors ? p.danger : p.text_faint);
    ImGui::SameLine();
    badge(format("%d warning%s", report.warnings, report.warnings == 1 ? "" : "s").c_str(),
          report.warnings ? p.warning : p.text_faint);

    divider();

    static bool failures_only = false;
    ImGui::Checkbox("Failures only", &failures_only);
    ImGui::SameLine();
    if (secondary_button("Open reports")) reveal_in_file_manager(paths::reports_dir());

    spacer(6.0f);

    for (const Check& c : report.checks) {
        if (failures_only && c.passed) continue;

        ImVec4 tint = p.success;
        const char* mark = "ok";
        if (!c.passed) {
            switch (c.severity) {
            case Severity::Error:   tint = p.danger;  mark = "fail"; break;
            case Severity::Warning: tint = p.warning; mark = "warn"; break;
            default:                tint = p.info;    mark = "note"; break;
            }
        }

        dot(tint);
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        ImGui::TextUnformatted(mark);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(c.title.c_str());

        ImGui::Indent(26.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, c.passed ? p.text_faint : p.text_dim);
        ImGui::TextWrapped("%s", c.detail.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(26.0f);
        spacer(4.0f);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
void panel_log(AppState& app)
{
    if (!app.show_log) return;
    if (!ImGui::Begin("Log", &app.show_log)) {
        ImGui::End();
        return;
    }

    const Palette& p = palette();
    static const char* const kLevels[] = {"Trace", "Debug", "Info", "Warn", "Error"};

    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##level", &app.log_min_level, kLevels, 5))
        log::set_min_level(log::Level(app.log_min_level));

    ImGui::SameLine();
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", app.log_filter.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
        if (ImGui::InputTextWithHint("##filter", "filter", buf, sizeof(buf)))
            app.log_filter = buf;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &app.log_autoscroll);
    ImGui::SameLine();
    if (secondary_button("Clear")) log::clear();

    divider();

    static std::vector<log::Entry> entries;
    log::snapshot(entries);

    const Fonts& f = fonts();
    ImGui::PushFont(f.mono, f.size_mono);
    ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const std::string filter = to_lower(app.log_filter);
    for (const log::Entry& e : entries) {
        if (int(e.level) < app.log_min_level) continue;
        if (!filter.empty() && to_lower(e.text).find(filter) == std::string::npos) continue;

        ImVec4 tint = p.text_dim;
        switch (e.level) {
        case log::Level::Error: tint = p.danger; break;
        case log::Level::Warn:  tint = p.warning; break;
        case log::Level::Info:  tint = p.text; break;
        case log::Level::Debug: tint = p.text_dim; break;
        default:                tint = p.text_faint; break;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
        ImGui::Text("%8.3f", e.time_s);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        ImGui::TextUnformatted(e.text.c_str());
        ImGui::PopStyleColor();
    }

    if (app.log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------------------
void draw_about_modal(AppState& app)
{
    if (app.show_about) {
        ImGui::OpenPopup("About Retopo Director");
        app.show_about = false;
    }

    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("About Retopo Director", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    const Palette& p = palette();
    heading("Retopo Director " RD_VERSION_STRING);
    spacer(6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    ImGui::TextWrapped(
        "High poly in, console budget low poly out. Deterministic code does every "
        "piece of geometry, UV and bake work; a language model acts as art "
        "director, deciding how the triangle budget is split and judging the "
        "result from renders. It never touches a vertex.");
    ImGui::PopStyleColor();

    divider("Third party");
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
    ImGui::TextUnformatted(
        "meshoptimizer (MIT) - simplification, vertex cache, strips\n"
        "xatlas (MIT) - uv unwrap\n"
        "Dear ImGui (MIT) - interface\n"
        "GLFW (zlib) - window and input\n"
        "cgltf (MIT) - glTF loading\n"
        "stb_image / stb_image_write (MIT / public domain)\n"
        "nlohmann/json (MIT)");
    ImGui::PopStyleColor();

    spacer(10.0f);
    if (primary_button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace rd::ui
