// The settings sheet: everything decided once for a project or a machine -
// the target hardware, which model directs, how the mesh is split, where the
// run writes - plus the actions more than one surface shares.

#include "ui/panels.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "llm/http.h"
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
const char* const kLlmNames[]      = {"Disabled", "Claude CLI", "Codex CLI", "OpenAI API", "Replay"};
const char* const kSegmenterNames[] = {"Geometric", "SAM", "Auto"};
// The two ways of filling the knob panel. One control, two halves: a button
// that says "run the director" over a checkbox that can switch the director off
// is the same choice asked twice, and the second answer wins.
const char* const kRunModeNames[]   = {"Director", "Manual"};

} // namespace

// How far along the whole run is, 0..1. Lives here rather than in the status bar
// because both need it and neither owns it.
float overall_progress(const AppState& app)
{
    const int total = app.settings.max_iterations > 0
                          ? app.settings.max_iterations
                          : app.settings.profile.max_iterations;
    return stage_overall_progress(app.pipeline.stage(), app.pipeline.progress(),
                                  app.pipeline.iteration(), total);
}

ImVec4 feasibility_color(Feasibility f)
{
    const Palette& p = palette();
    switch (f) {
    case Feasibility::Comfortable: return p.success;
    case Feasibility::Tight:       return p.accent;
    case Feasibility::Strained:    return p.warning;
    case Feasibility::Impossible:  return p.danger;
    default:                       return p.text_faint;
    }
}

// The director arguing with the brief. It is shown here, next to the sliders it
// is talking about, and every change still costs a human one click: the profile
// is a promise about hardware, and nothing that has only seen one mesh gets to
// make that promise on somebody's behalf.
bool draw_profile_advice(AppState& app, TargetProfile& profile)
{
    const Palette&       p      = palette();
    const ProfileAdvice& advice = app.advice;
    if (advice.empty() || app.advice_dismissed) return false;

    bool changed = false;
    const ImVec4 tint = feasibility_color(advice.feasibility);

    ImGui::PushID("advice");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, with_alpha(tint, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metrics().rounding_card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (ImGui::BeginChild("card", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        icon(feasibility_is_alarming(advice.feasibility) ? Icon::Warning : Icon::Sparkle,
             ImGui::GetTextLineHeight(), tint);
        ImGui::SameLine(0.0f, 10.0f);
        subheading(format("The director calls this brief %s",
                          to_lower(feasibility_name(advice.feasibility)).c_str()).c_str());
        if (!advice.headline.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
            ImGui::TextWrapped("%s", advice.headline.c_str());
            ImGui::PopStyleColor();
        }

        const int applicable = advice.applicable_count();
        if (applicable > 0) {
            spacer(4.0f);
            for (size_t i = 0; i < advice.proposals.size(); ++i) {
                const ProfileProposal& prop = advice.proposals[i];
                if (!prop.applicable) continue;
                ImGui::PushID(int(i));
                mono("%s  %s", prop.field.c_str(), prop.change_text().c_str());
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(4.0f, ImGui::GetContentRegionAvail().x - 70.0f));
                if (ghost_button("Apply", ImVec2(70, 0)) && apply_proposal(profile, prop)) {
                    changed = true;
                    app.notify(prop.field + " set to " + prop.change_text());
                }
                if (!prop.reason.empty() || !prop.note.empty())
                    hint(format("%s%s%s", prop.reason.c_str(),
                                prop.reason.empty() || prop.note.empty() ? "" : " - ",
                                prop.note.c_str()).c_str());
                ImGui::PopID();
            }
        }

        spacer(4.0f);
        hint("Advisory. Nothing here is applied until you say so.");
        spacer(4.0f);
        if (applicable > 1 && primary_button("Apply all")) {
            for (const ProfileProposal& prop : advice.proposals)
                changed |= apply_proposal(profile, prop);
            if (changed) {
                app.advice_dismissed = true;
                app.notify("Profile updated. Run again to build against it.", 5.0);
            }
        }
        if (applicable > 1) ImGui::SameLine();
        if (ghost_button("Dismiss")) app.advice_dismissed = true;

        // A proposal the director was not allowed to make is still worth
        // reading: it is usually the clearest statement of what it wanted.
        const int ignored = int(advice.proposals.size()) - applicable;
        if (ignored > 0) {
            spacer(4.0f);
            if (ImGui::TreeNode("ignored_proposals", "%d proposal%s could not be applied",
                                ignored, ignored == 1 ? "" : "s")) {
                for (const ProfileProposal& prop : advice.proposals) {
                    if (prop.applicable) continue;
                    muted("%s - %s", prop.field.c_str(),
                          prop.note.empty() ? "unusable" : prop.note.c_str());
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
    return changed;
}

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

    app.open_mesh(chosen);
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
    if (!app.pipeline.start(app.mesh_path, app.settings)) {
        app.notify("A run is already in progress", 3.0);
        return;
    }
    // With the director on, the conversation is the thing worth watching while
    // the bar fills; without it there is nothing new until the report.
    if (app.settings.use_llm) app.tab = AppState::Tab::Director;
    app.jump_to_report = true;
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

bool can_rebuild(const AppState& app)
{
    bool ok = false;
    app.pipeline.with_results([&](const PipelineResults& r) {
        ok = !r.highpoly.empty() && r.segmentation.valid();
    });
    return ok;
}

// Throws the edits away by pretending the results changed: sync_from_pipeline
// then copies the run's panel back over the edited one.
void action_revert_panel(AppState& app)
{
    app.panel_dirty   = false;
    app.panel_version = 0;
    app.sync_from_pipeline();
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
// Target profile - what the hardware allows. Set once per project.
// ---------------------------------------------------------------------------
void draw_profile_settings(AppState& app)
{
    const Palette& p = palette();
    TargetProfile& profile = app.settings.profile;

    // --- which profile --------------------------------------------------------
    // Cards rather than a combo: there are two or three of these, and the line
    // under each name is what tells them apart.
    {
        const float gap  = 8.0f;
        const int   n    = int(app.profile_files.size()) + 1;
        const int   cols = std::max(1, std::min(3, n));
        const float w    = (ImGui::GetContentRegionAvail().x - gap * float(cols - 1)) / float(cols);
        const float h    = ImGui::GetTextLineHeight() * 2.0f + 28.0f;

        for (int i = 0; i < n; ++i) {
            if (i % cols) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(i);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList*  dl  = ImGui::GetWindowDrawList();

            if (i + 1 == n) {
                // The last card loads or saves a file.
                if (ImGui::InvisibleButton("##more", {w, h})) ImGui::OpenPopup("profile_file");
                const bool hov = ImGui::IsItemHovered();
                dl->AddRect(pos, {pos.x + w, pos.y + h}, col(hov ? p.text_faint : p.overlay),
                            metrics().rounding_card, 1.5f);
                const char*  t  = "Load or save as...";
                const ImVec2 ts = ImGui::CalcTextSize(t);
                dl->AddText({pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f},
                            col(hov ? p.text : p.text_dim), t);
                if (ImGui::BeginPopup("profile_file")) {
                    if (ImGui::MenuItem("Load a profile...")) {
                        const fs::path chosen = open_file_dialog(
                            "Load a target profile", {{"Profiles", "json"}}, paths::profiles_dir());
                        if (!chosen.empty()) app.load_profile(chosen);
                    }
                    if (ImGui::MenuItem("Save this profile as...")) {
                        const fs::path chosen = save_file_dialog(
                            "Save the target profile", {{"Profiles", "json"}},
                            profile.name + ".json", paths::config_dir() / "profiles");
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
                    ImGui::EndPopup();
                }
            } else {
                const fs::path& file    = app.profile_files[size_t(i)];
                const bool      current = profile.name == file.stem().string();
                if (ImGui::InvisibleButton("##card", {w, h})) {
                    app.profile_index = i;
                    app.load_profile(file);
                }
                const bool hov = ImGui::IsItemHovered();
                dl->AddRectFilled(pos, {pos.x + w, pos.y + h},
                                  col(hov ? p.elevated : p.surface_alt), metrics().rounding_card);
                if (current)
                    dl->AddRect(pos, {pos.x + w, pos.y + h}, col(p.accent),
                                metrics().rounding_card, 1.5f);
                const Fonts& f = fonts();
                ImGui::PushFont(f.medium, f.size_body);
                dl->AddText({pos.x + 14.0f, pos.y + 11.0f}, col(p.text),
                            profile_display_name(file.stem().string()).c_str());
                ImGui::PopFont();
                const std::string sub = current
                    ? format("%d tris - %dx%d - in use", profile.max_triangles,
                             profile.texture.width, profile.texture.height)
                    : std::string("click to use");
                dl->AddText({pos.x + 14.0f, pos.y + 15.0f + ImGui::GetTextLineHeight()},
                            col(p.text_dim), sub.c_str());
            }
            ImGui::PopID();
        }
    }

    spacer(10.0f);
    bool changed = draw_profile_advice(app, profile);
    if (!app.advice.empty() && !app.advice_dismissed) spacer(10.0f);

    // --- limits, two columns --------------------------------------------------
    if (ImGui::BeginTable("limits", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        section_label("Geometry");
        begin_form("geo_form", 130.0f);
        changed |= slider_int("Max triangles", &profile.max_triangles, 32, 20000, 1400);
        changed |= slider_int("Max vertices", &profile.max_vertices, 32, 20000, 1000);
        changed |= slider_int("Max shells", &profile.max_shells, 0, 64, 32,
                              "Not a check: pieces past this are deleted, smallest first. "
                              "0 means unlimited.");
        changed |= slider_int("Bone influences", &profile.max_bone_influences, 0, 4, 2);
        changed |= toggle("Require manifold", &profile.require_manifold);
        changed |= toggle("Require symmetry", &profile.require_symmetry);
        changed |= toggle("Triangle strips", &profile.require_strips);
        changed |= slider_float("Min strip length", &profile.min_average_strip_len, 1.0f, 12.0f,
                                3.5f, nullptr, "%.1f");
        changed |= slider_int("Vertex cache", &profile.vertex_cache_size, 4, 64, 16,
                              "The PS2 VU1 input buffer holds about 16 vertices.");
        end_form();

        ImGui::TableNextColumn();
        section_label("Texture");
        begin_form("tex_form", 130.0f);
        changed |= slider_int("Width", &profile.texture.width, 8, 1024, 256);
        changed |= slider_int("Height", &profile.texture.height, 8, 1024, 256);
        changed |= slider_int("Colours", &profile.texture.palette_colors, 0, 256, 256,
                              "0 keeps truecolor. 16 is a 4 bit CLUT, 256 an 8 bit one.");
        changed |= slider_int("Pages", &profile.texture.count, 1, 8, 1);
        changed |= toggle("Dither", &profile.texture.dithering);
        changed |= toggle("Bilinear filtering", &profile.texture.bilinear,
                          "How the target magnifies the texture. The PS2 filters "
                          "bilinearly; off shows every texel as a hard square.");
        changed |= toggle("Require UVs", &profile.require_uvs);
        changed |= toggle("Vertex colours", &profile.require_vertex_colors);
        changed |= toggle("Bake lighting in", &profile.bake_lighting_to_diffuse,
                          "On hardware without per pixel lighting this is the shading model.");
        end_form();
        ImGui::EndTable();
    }

    spacer(8.0f);
    if (section("Framing and cameras", false,
                format("%zu cameras", profile.cameras.size()).c_str())) {
        begin_form("framing_form", 170.0f);
        changed |= slider_float("Reference height", &profile.reference_height_m, 0.1f, 5.0f,
                                1.8f, "How tall the subject is in the real world.", "%.2f m");
        changed |= slider_int("Turntable views", &profile.turntable_views, 0, 24, 8);
        changed |= slider_float("Target silhouette", &profile.target_silhouette_error,
                                0.0f, 0.2f, 0.02f, nullptr, "%.3f");
        changed |= slider_float("Max silhouette", &profile.max_silhouette_error,
                                0.0f, 0.5f, 0.05f, nullptr, "%.3f");
        end_form();

        spacer(6.0f);
        for (size_t i = 0; i < profile.cameras.size(); ++i) {
            ProfileCamera& c = profile.cameras[i];
            ImGui::PushID(int(i));
            if (ImGui::TreeNode(c.name.c_str())) {
                begin_form("cam_form", 150.0f);
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
        spacer(6.0f);
    }

    if (section("Art direction", true)) {
        hint("Handed to the director verbatim.");
        spacer(2.0f);
        changed |= text_area("##art", profile.art_direction, 110.0f);
        spacer(4.0f);
        begin_form("iter_form", 170.0f);
        changed |= slider_int("Passes", &profile.max_iterations, 0, 12, 4,
                              "How many times the director may look and ask for another pass.");
        end_form();
    }

    const std::vector<std::string> warnings = profile.warnings();
    if (!warnings.empty()) {
        spacer(8.0f);
        for (const std::string& w : warnings) callout(Icon::Warning, p.warning, w.c_str());
    }

    if (changed) profile.clamp();
}

// ---------------------------------------------------------------------------
// The weights, fetched on request.
//
// Nothing is downloaded unless somebody presses the button: it is hundreds of
// megabytes from a third party, and that is a decision for the person sitting
// here, not for the application on first run. The licence and the source are on
// screen next to it for the same reason.
void draw_checkpoint_download(AppState& app, SegmenterOptions& seg)
{
    const Palette&      p  = palette();
    CheckpointDownload& dl = app.checkpoint_download;

    // A finished download points the settings at what it just wrote.
    std::string model_type;
    if (const std::string done = dl.take_finished(&model_type); !done.empty()) {
        seg.sam.checkpoint = done;
        if (!model_type.empty()) seg.sam.model_type = model_type;
        app.notify("Checkpoint ready: " + fs::path(done).filename().string(), 6.0);
    }

    if (dl.running()) {
        const float    fraction = dl.fraction();
        const uint64_t received = dl.received();
        const uint64_t total    = dl.total();

        const std::string overlay =
            total ? format("%s  -  %s of %s", dl.label().c_str(),
                           paths::format_bytes(received).c_str(),
                           paths::format_bytes(total).c_str())
                  : format("%s  -  %s", dl.label().c_str(),
                           paths::format_bytes(received).c_str());
        progress_bar(fraction < 0.0f ? 0.0f : fraction, overlay.c_str());

        spacer(4.0f);
        if (ImGui::Button("Cancel download", ImVec2(160, 0))) dl.cancel();
        return;
    }

    const std::vector<SamCheckpoint>& table = sam_checkpoints();
    static int selected = 0;
    selected = std::clamp(selected, 0, int(table.size()) - 1);

    std::vector<std::string> labels;
    std::vector<const char*> label_ptrs;
    labels.reserve(table.size());
    for (const SamCheckpoint& c : table)
        labels.push_back(format("%s  -  %s", c.label, paths::format_bytes(c.bytes).c_str()));
    for (const std::string& l : labels) label_ptrs.push_back(l.c_str());

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::Combo("##checkpoint_pick", &selected, label_ptrs.data(), int(label_ptrs.size()));
    ImGui::SameLine();

    const SamCheckpoint& entry = table[size_t(selected)];
    std::error_code ec;
    const fs::path  destination = sam_checkpoint_path(entry);
    const bool      already     = std::filesystem::exists(destination, ec);

    if (already) {
        if (ImGui::Button("Use it", ImVec2(150, 0))) {
            seg.sam.checkpoint = destination.string();
            seg.sam.model_type = entry.model_type;
            app.notify("Using " + destination.filename().string());
        }
    } else {
        std::string reason;
        const bool  online = http_available(&reason);
        ImGui::BeginDisabled(!online);
        if (ImGui::Button("Download", ImVec2(150, 0))) dl.start(entry);
        ImGui::EndDisabled();
        if (!online && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reason.c_str());
    }

    spacer(4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
    ImGui::TextWrapped("%s", entry.note);
    ImGui::TextWrapped("Apache-2.0, from Meta's dl.fbaipublicfiles.com, kept beside your "
                       "settings.");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", entry.url, sam_checkpoint_dir().string().c_str());

    if (const std::string err = dl.error(); !err.empty()) {
        spacer(4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
        ImGui::TextWrapped("%s", err.c_str());
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------
// Which model directs, and who draws the regions it is asked to name. Both are
// set up once for a machine, so both live in the Options window.
// ---------------------------------------------------------------------------
void draw_director_settings(AppState& app)
{
    const Palette& p = palette();
    LlmConfig& cfg = app.settings.llm;

    if (card_begin("backend_card", "Which model directs")) {
        int kind = int(cfg.kind);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (segmented("llm_kind", &kind, kLlmNames, 5)) cfg.kind = LlmBackendKind(kind);
        spacer(2.0f);
        hint("The director only fills in the knob panel and judges renders. "
             "It never touches a vertex.");

        spacer(8.0f);
        begin_form("llm_form", 170.0f);
        switch (cfg.kind) {
        case LlmBackendKind::ClaudeCli:
            text_input("Executable", cfg.claude_path, "claude");
            text_input("Model", cfg.claude_model, "claude-opus-5-5; \"default\" for the CLI's own");
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
        case LlmBackendKind::Replay:
            text_input("Recorded run", cfg.replay_dir, "<run>/reports/prompts",
                       "Answers every request with what an earlier run's director said, "
                       "in the same order. For repeating a run exactly, without the model.");
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

}

// ---------------------------------------------------------------------------
// The segmenter decides what the model is asked to name. It makes no geometry
// decisions, it only draws the patches.
// ---------------------------------------------------------------------------
void draw_segmenter_settings(AppState& app)
{
    const Palette& p = palette();
    if (card_begin("segmenter_card", "Who draws the regions")) {
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
            draw_checkpoint_download(app, seg);

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
}

// ---------------------------------------------------------------------------
// What a run does with itself: where it writes and how hard it tries.
// ---------------------------------------------------------------------------
void draw_run_settings(AppState& app)
{
    const Palette& p = palette();

    if (card_begin("run_output_card", "What a run leaves behind")) {
        begin_form("run_output_form", 190.0f);
        toggle("Export on finish", &app.settings.auto_export,
               "Writes the low poly and its texture into the project's export folder "
               "as soon as validation passes.");
        toggle("Write the report", &app.settings.write_report,
               "A markdown summary of every iteration, next to the renders.");
        end_form();

        spacer(8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
        ImGui::TextWrapped("%s", paths::project_dir().string().c_str());
        ImGui::PopStyleColor();
        spacer(4.0f);
        if (secondary_button("Open the folder")) reveal_in_file_manager(paths::project_dir());
        ImGui::SameLine();
        if (secondary_button("Change...")) {
            const fs::path chosen = pick_folder_dialog("Where should this run write?",
                                                       paths::project_dir());
            if (!chosen.empty()) {
                paths::set_project_dir(chosen);
                app.notify("Project folder: " + chosen.string());
            }
        }
        card_end();
    }

    spacer(8.0f);

    if (card_begin("run_effort_card", "How hard it tries")) {
        begin_form("run_effort_form", 190.0f);
        slider_int("Max iterations", &app.settings.max_iterations, 0, 12, 0,
                   "How many passes the director may ask for. 0 takes the value from "
                   "the target profile.");
        slider_int("Render size", &app.settings.render_size, 256, 1536, 640,
                   "Pixels per side for the images the director is shown. Bigger is "
                   "not better: it reads a 640px silhouette as well as a 2048px one "
                   "and the upload costs real time.");
        end_form();
        card_end();
    }
}

} // namespace rd::ui
