// The frame around the viewport: the top bar, the status bar, the log drawer,
// and the sheets that open over everything - settings, about, shortcuts, the
// render lightbox.

#include "ui/panels.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "ui/file_dialog.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <imgui.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <vector>

namespace rd::ui {
namespace {

float S(float v) { return v * dpi(); }

} // namespace

std::string profile_display_name(const std::string& raw)
{
    std::string name = raw;
    for (char& c : name)
        if (c == '_' || c == '-') c = ' ';
    if (!name.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    if (name.size() >= 3 && iequals(name.substr(0, 3), "ps2")) name.replace(0, 3, "PS2");
    return name;
}

namespace {
std::string pretty_profile_name(const std::string& name) { return profile_display_name(name); }

std::string thousands(size_t n)
{
    std::string digits = std::to_string(n);
    std::string out;
    const int   lead = int(digits.size()) % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && int(i) % 3 == lead % 3 && (i != 0)) out += ' ';
        out += digits[i];
    }
    return out;
}

std::string short_count(size_t n)
{
    if (n >= 1000000) return format("%.1fM", double(n) / 1e6);
    if (n >= 10000)   return format("%zuk", (n + 500) / 1000);
    return thousands(n);
}

// A rounded chip in the top bar: a glyph, a label, a dimmer note and a chevron.
bool chip_button(const char* id, Icon glyph, const char* label, const char* note, bool filled)
{
    const Palette& p = palette();
    const float h   = S(34);
    const float pad = S(12);
    const float is  = S(17);
    const float gap = S(8);
    const ImVec2 lt = ImGui::CalcTextSize(label);
    const ImVec2 nt = note && *note ? ImGui::CalcTextSize(note) : ImVec2(0, 0);
    const float  w  = pad + is + gap + lt.x + (nt.x > 0 ? gap + nt.x : 0.0f) + gap + S(12) + pad;

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   pressed = ImGui::InvisibleButton(id, {w, h});
    const bool   hovered = ImGui::IsItemHovered();
    const float  alpha   = ImGui::GetStyle().Alpha;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (filled || hovered)
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h},
                          col(hovered ? p.elevated : p.surface_alt, alpha), S(8));
    const float cy = pos.y + h * 0.5f;
    float       x  = pos.x + pad;
    draw_icon(dl, glyph, {x + is * 0.5f, cy}, is, col(p.text_dim, alpha));
    x += is + gap;
    dl->AddText({x, cy - lt.y * 0.5f}, col(filled ? p.text : p.text_faint, alpha), label);
    x += lt.x + gap;
    if (nt.x > 0) {
        dl->AddText({x, cy - nt.y * 0.5f}, col(p.text_faint, alpha), note);
        x += nt.x + gap;
    }
    draw_icon(dl, Icon::ChevronDown, {x + S(6), cy}, S(12), col(p.text_faint, alpha));
    return pressed;
}

// The mark from option A of the icon sheet: a faceted low poly gem.
void draw_logo(ImDrawList* dl, ImVec2 pos, float size)
{
    const Palette& p = palette();
    dl->AddRectFilled(pos, {pos.x + size, pos.y + size}, col(p.accent), size * 0.24f);
    const float s = size / 64.0f;
    auto P = [&](float x, float y) { return ImVec2{pos.x + x * s, pos.y + y * s}; };
    const ImVec2 top = P(32, 12), ur = P(49, 22), lr = P(49, 42), bot = P(32, 52),
                 ll = P(15, 42), ul = P(15, 22), mid = P(32, 30);
    const ImU32 w = IM_COL32(255, 255, 255, 255);
    dl->AddTriangleFilled(ul, top, mid, IM_COL32(255, 255, 255, 215));
    dl->AddTriangleFilled(top, ur, mid, IM_COL32(255, 255, 255, 140));
    dl->AddTriangleFilled(ul, mid, ll, IM_COL32(255, 255, 255, 90));
    const ImVec2 hex[] = {top, ur, lr, bot, ll, ul};
    const float  t     = std::max(1.0f, size / 22.0f);
    dl->AddPolyline(hex, 6, w, t, ImDrawFlags_Closed);
    dl->AddLine(top, mid, w, t);
    dl->AddLine(ul, mid, w, t);
    dl->AddLine(ur, mid, w, t);
    dl->AddLine(mid, bot, w, t);
}

void recent_menu_items(AppState& app)
{
    // PushID per row because two files can share a name, and the label is what
    // ImGui hashes into the item id otherwise. Acting on the choice waits until
    // the loop is over: open_mesh rewrites the very vector being walked.
    fs::path chosen;
    for (size_t i = 0; i < app.recent_meshes.size(); ++i) {
        ImGui::PushID(int(i));
        if (ImGui::MenuItem(app.recent_label(i).c_str())) chosen = app.recent_meshes[i];
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", app.recent_meshes[i].string().c_str());
        ImGui::PopID();
    }
    if (!chosen.empty()) app.open_mesh(chosen);
}

// Which of the eight steps a stage belongs to. The pipeline has fifteen
// stages; nobody watching needs to tell "reference renders" from "analysing".
int phase_of(Stage s)
{
    switch (s) {
    case Stage::Loading:          return 0;
    case Stage::Analysing:
    case Stage::ReferenceRenders: return 1;
    case Stage::Segmenting:       return 2;
    case Stage::NamingRegions:
    case Stage::AllocatingBudget: return 3;
    case Stage::BuildingDensity:
    case Stage::Retopologising:   return 4;
    case Stage::Baking:           return 5;
    case Stage::Validating:
    case Stage::CandidateRenders:
    case Stage::Reviewing:        return 6;
    case Stage::Exporting:
    case Stage::Reporting:        return 7;
    default:                      return -1;
    }
}

std::string clock_text(double seconds)
{
    const int s = std::max(0, int(seconds));
    return format("%d:%02d", s / 60, s % 60);
}

// A frame of a modal sheet: dims the app and centres a rounded panel.
bool begin_sheet(const char* name, ImVec2 size)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    size.x = std::min(size.x, vp->WorkSize.x - S(48));
    size.y = std::min(size.y, vp->WorkSize.y - S(48));
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                             vp->WorkPos.y + vp->WorkSize.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, palette().surface);
    const bool open = ImGui::BeginPopupModal(
        name, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return open;
}

} // namespace

// ---------------------------------------------------------------------------
bool begin_surface(const char* name, const ImVec2& pos, const ImVec2& size, const ImVec4& bg,
                   const ImVec2& padding, ImGuiWindowFlags extra)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    const bool open = ImGui::Begin(
        name, nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse | extra);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return open;
}

void end_surface() { ImGui::End(); }

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------
void draw_topbar(AppState& app, const ImVec2& pos, const ImVec2& size, bool& want_quit,
                 bool& want_screenshot)
{
    const Palette& p = palette();
    const bool open = begin_surface("##topbar", pos, size, p.surface, ImVec2(S(16), 0));
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine({pos.x, pos.y + size.y - 1.0f}, {pos.x + size.x, pos.y + size.y - 1.0f},
                    col(p.line), 1.0f);

        const float cy      = size.y * 0.5f;
        const bool  running = app.pipeline.running();

        // --- brand -------------------------------------------------------------
        const float logo = S(28);
        ImGui::SetCursorPos({S(16), cy - logo * 0.5f});
        draw_logo(dl, ImGui::GetCursorScreenPos(), logo);
        ImGui::Dummy({logo, logo});
        ImGui::SameLine(0.0f, S(10));
        {
            const Fonts& f = fonts();
            ImGui::PushFont(f.medium, f.size_body);
            ImGui::SetCursorPosY(cy - ImGui::GetTextLineHeight() * 0.5f);
            ImGui::TextUnformatted("PS2 Retopo Director");
            ImGui::PopFont();
        }
        ImGui::SameLine(0.0f, S(16));
        {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            dl->AddLine({c.x, pos.y + cy - S(12)}, {c.x, pos.y + cy + S(12)}, col(p.overlay));
            ImGui::Dummy({1.0f, 1.0f});
        }

        // --- the mesh --------------------------------------------------------
        ImGui::SameLine(0.0f, S(12));
        ImGui::SetCursorPosY(cy - S(17));
        size_t high_tris = 0;
        app.pipeline.with_results([&](const PipelineResults& r) {
            high_tris = r.highpoly.triangle_count();
        });
        const std::string mesh_label = app.mesh_path.empty() ? std::string("No mesh")
                                                             : app.mesh_path.filename().string();
        const std::string mesh_note  = high_tris ? short_count(high_tris) + " tris" : "";
        ImGui::BeginDisabled(running);
        if (chip_button("##mesh", Icon::Folder, mesh_label.c_str(), mesh_note.c_str(),
                        !app.mesh_path.empty()))
            ImGui::OpenPopup("mesh_menu");
        ImGui::EndDisabled();
        if (!app.mesh_path.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                                           ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", app.mesh_path.string().c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
        if (ImGui::BeginPopup("mesh_menu")) {
            if (ImGui::MenuItem("Open mesh...", "Ctrl+O")) action_open_mesh(app);
            if (!app.recent_meshes.empty()) {
                ImGui::Separator();
                faint("Recent");
                recent_menu_items(app);
            }
            if (!app.mesh_path.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Show in folder")) reveal_in_file_manager(app.mesh_path);
            }
            ImGui::EndPopup();
        }

        // --- the target --------------------------------------------------------
        ImGui::SameLine(0.0f, S(6));
        ImGui::SetCursorPosY(cy - S(17));
        const TargetProfile& profile = app.settings.profile;
        const std::string target_label = pretty_profile_name(
            profile.name.empty() ? std::string("Custom target") : profile.name);
        const std::string target_note = format("%s tris - %dx%d",
                                               thousands(size_t(profile.max_triangles)).c_str(),
                                               profile.texture.width, profile.texture.height);
        ImGui::BeginDisabled(running);
        if (chip_button("##target", Icon::Target, target_label.c_str(), target_note.c_str(), true))
            ImGui::OpenPopup("target_menu");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("target_menu")) {
            faint("Target hardware");
            for (size_t i = 0; i < app.profile_files.size(); ++i) {
                const std::string stem = app.profile_files[i].stem().string();
                if (ImGui::MenuItem(pretty_profile_name(stem).c_str(), nullptr,
                                    stem == profile.name)) {
                    app.profile_index = int(i);
                    app.load_profile(app.profile_files[i]);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Edit the target...")) {
                app.options_page = 0;
                app.show_options = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        // --- right hand side ---------------------------------------------------
        static const char* const kModes[] = {"Director", "Manual"};
        const float icon_w = ImGui::GetFrameHeight() + S(4);
        bool has_result = false;
        app.pipeline.with_results([&](const PipelineResults& r) {
            has_result = !r.iterations.empty();
        });
        const char* run_label = running ? "Stop" : (has_result ? "Run again" : "Run");
        const float run_w = ImGui::CalcTextSize(run_label).x + S(14) * 2 + S(26);
        float seg_w = S(6);
        for (const char* m : kModes) seg_w += ImGui::CalcTextSize(m).x + S(28);
        // An update, when there is one, sits to the left of everything else:
        // noticeable, never in the way of Run.
        const UpdateState us = app.updater.state();
        const bool show_update = us == UpdateState::Available ||
                                 us == UpdateState::Downloading || us == UpdateState::Ready;
        std::string update_label;
        if (us == UpdateState::Available)
            update_label = format("Build %d is out", app.updater.latest_build());
        else if (us == UpdateState::Downloading)
            update_label = format("Downloading %.0f%%", app.updater.fraction() * 100.0f);
        else if (us == UpdateState::Ready)
            update_label = "Restart to update";
        const float update_w = show_update
            ? ImGui::CalcTextSize(update_label.c_str()).x + S(14) * 2 + S(26) : 0.0f;

        const float right = (show_update ? update_w + S(12) : 0.0f) + seg_w + S(10) + run_w +
                            S(8) + icon_w * 2 + S(4);

        ImGui::SameLine();
        ImGui::SetCursorPos({size.x - S(16) - right, cy - (ImGui::GetFrameHeight() + S(6)) * 0.5f});
        if (show_update) {
            ImGui::SetCursorPosY(cy - S(17));
            if (icon_label_button(format("%s##update", update_label.c_str()).c_str(),
                                  us == UpdateState::Ready ? Icon::Rebuild : Icon::Upload,
                                  ButtonKind::Ghost, {update_w, S(34)}))
                ImGui::OpenPopup("update_menu");
            {
                // Tinted so it reads as news rather than as one more control.
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                dl->AddRect(a, b, col(p.accent, 0.55f), S(8), 1.0f);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
            ImGui::SetNextWindowSize({S(380), 0});
            if (ImGui::BeginPopup("update_menu")) {
                subheading(format("PS2 Retopo Director, build %d", app.updater.latest_build()).c_str());
                faint("You have build %d.", Updater::current_build());
                const std::string notes = app.updater.notes();
                if (!notes.empty()) {
                    spacer(4.0f);
                    std::string shown = notes.size() > 700 ? notes.substr(0, 697) + "..." : notes;
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(348));
                    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                    ImGui::TextWrapped("%s", shown.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                }
                spacer(8.0f);
                if (us == UpdateState::Downloading) {
                    progress_bar(app.updater.fraction());
                    spacer(2.0f);
                    faint("Downloading the installer...");
                } else if (!Updater::installed()) {
                    // A copy unpacked from the zip: the installer would put a
                    // second copy somewhere else, so send them to the page.
                    hint("This copy was unpacked from a zip, so it cannot replace itself.");
                    spacer(4.0f);
                    if (primary_button("Open the release page")) {
                        open_url(app.updater.page_url());
                        ImGui::CloseCurrentPopup();
                    }
                } else if (us == UpdateState::Ready) {
                    hint("The window closes, the installer replaces it and opens the new build.");
                    spacer(4.0f);
                    ImGui::BeginDisabled(running);
                    if (primary_button("Restart and update")) {
                        std::string error;
                        if (app.updater.install(&error)) want_quit = true;
                        else app.notify("Update failed: " + error, 6.0);
                    }
                    ImGui::EndDisabled();
                    if (running) faint("Waiting for the run to finish.");
                } else {
                    if (primary_button("Download and install")) app.updater.download();
                }
                ImGui::SameLine();
                if (ghost_button("Release notes")) open_url(app.updater.page_url());
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0.0f, S(12));
            ImGui::SetCursorPosY(cy - (ImGui::GetFrameHeight() + S(6)) * 0.5f);
        }
        int run_mode = app.settings.use_llm ? 0 : 1;
        ImGui::BeginDisabled(running);
        if (segmented("run_mode", &run_mode, kModes, 2, -1.0f))
            app.settings.use_llm = (run_mode == 0);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(app.settings.use_llm
                ? "A language model names the parts, splits the budget and judges each "
                  "pass from renders."
                : "The knob panel is used exactly as it stands. Nothing is sent anywhere.");

        ImGui::SameLine(0.0f, S(10));
        const float btn_h = S(36);
        ImGui::SetCursorPosY(cy - btn_h * 0.5f);
        if (running) {
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            if (icon_label_button("Stop##run", Icon::Stop, ButtonKind::Danger, {run_w, btn_h}))
                app.pipeline.cancel();
            // A ring around the glyph says how far along the run is.
            const ImVec2 c{bp.x + run_w * 0.5f - ImGui::CalcTextSize("Stop").x * 0.5f - S(4),
                           bp.y + btn_h * 0.5f};
            dl->PathArcTo(c, S(10), -3.14159265f * 0.5f,
                          -3.14159265f * 0.5f + 3.14159265f * 2.0f * overall_progress(app), 32);
            dl->PathStroke(col(p.danger), S(2), ImDrawFlags_None);
        } else {
            ImGui::BeginDisabled(app.mesh_path.empty());
            const bool go = has_result
                ? icon_label_button("Run again##run", Icon::Rebuild, ButtonKind::Secondary,
                                    {run_w, btn_h})
                : icon_label_button("Run##run", Icon::Play, ButtonKind::Primary, {run_w, btn_h});
            ImGui::EndDisabled();
            if (go) action_run(app);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                     ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(app.mesh_path.empty() ? "Open a high poly mesh first."
                                                        : "Run the whole pipeline (F5)");
        }

        ImGui::SameLine(0.0f, S(8));
        ImGui::SetCursorPosY(cy - icon_w * 0.5f);
        if (icon_button("##settings", Icon::Gear, "Settings (Ctrl+,)", app.show_options))
            app.show_options = true;
        ImGui::SameLine(0.0f, S(4));
        ImGui::SetCursorPosY(cy - icon_w * 0.5f);
        if (icon_button("##more", Icon::More, "More")) ImGui::OpenPopup("more_menu");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
        if (ImGui::BeginPopup("more_menu")) {
            if (ImGui::MenuItem("Open the project folder"))
                reveal_in_file_manager(paths::project_dir());
            if (ImGui::MenuItem("Take a screenshot", "F12")) want_screenshot = true;
            if (ImGui::MenuItem("Show the log", "Ctrl+L", app.show_log))
                app.show_log = !app.show_log;
            ImGui::Separator();
            if (ImGui::MenuItem("Keyboard shortcuts", "?")) app.show_shortcuts = true;
            if (ImGui::MenuItem("Check for updates", nullptr, false,
                                Updater::enabled() &&
                                    app.updater.state() != UpdateState::Checking &&
                                    app.updater.state() != UpdateState::Downloading))
                app.updater.check(false);
            if (ImGui::MenuItem("About")) app.show_about = true;
            ImGui::MenuItem("ImGui demo", nullptr, &app.show_demo);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) want_quit = true;
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    end_surface();
}

// ---------------------------------------------------------------------------
// Status bar: where the run is, how long it has taken, and the log.
// ---------------------------------------------------------------------------
void draw_statusbar(AppState& app, const ImVec2& pos, const ImVec2& size)
{
    const Palette& p = palette();
    const bool open = begin_surface("##statusbar", pos, size, p.surface, ImVec2(S(16), 0));
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(pos, {pos.x + size.x, pos.y}, col(p.line), 1.0f);

        const Fonts& f = fonts();
        ImGui::PushFont(f.regular, f.size_small);
        const float cy = size.y * 0.5f;
        const float th = ImGui::GetTextLineHeight();

        const Stage stage   = app.pipeline.stage();
        const bool  running = app.pipeline.running();

        ImVec4      tint = p.text_faint;
        std::string label;
        if (running)                        { tint = p.accent;  label = "Running"; }
        else if (stage == Stage::Failed)    { tint = p.danger;  label = "Failed"; }
        else if (stage == Stage::Cancelled) { tint = p.warning; label = "Stopped"; }
        else if (stage == Stage::Done)      { tint = p.success; label = "Done"; }
        else label = app.mesh_path.empty() ? "Waiting for a mesh" : "Ready";

        bool   passed = false, have_report = false;
        size_t passes = 0;
        int    kept   = 0;
        size_t exported = 0;
        double took   = 0.0;
        app.pipeline.with_results([&](const PipelineResults& r) {
            have_report = !r.validation.checks.empty();
            passed      = r.validation.passed;
            passes      = r.iterations.size();
            kept        = r.kept_iteration;
            exported    = r.exported.files.size();
            took        = r.total_seconds;
        });
        if (!running && stage == Stage::Done && have_report) {
            label = passed ? "Passed" : "Failed checks";
            tint  = passed ? p.success : p.danger;
        }

        ImGui::SetCursorPos({S(16), cy - th * 0.5f});
        const ImVec2 dp = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled({dp.x + S(4), dp.y + th * 0.5f}, S(4), col(tint));
        ImGui::Dummy({S(8), th});
        ImGui::SameLine(0.0f, S(8));
        ImGui::TextUnformatted(label.c_str());

        if (running) {
            // Eight steps, the current one lit, the finished ones ticked.
            const int now = phase_of(stage);
            const char* names[] = {"Load", "Analyse", "Split",
                                   app.settings.use_llm ? "Direct" : "Budget",
                                   "Build", "Bake", "Check", "Export"};
            ImGui::SameLine(0.0f, S(14));
            for (int i = 0; i < 8; ++i) {
                const char*  shown = names[i];
                const ImVec2 ts    = ImGui::CalcTextSize(shown);
                const float  extra = i < now ? S(12) : 0.0f;
                const ImVec2 cp    = ImGui::GetCursorScreenPos();
                const float  w     = ts.x + extra + S(16);
                const ImVec2 a{cp.x, pos.y + cy - th * 0.5f - S(2)};
                const ImVec2 b{cp.x + w, pos.y + cy + th * 0.5f + S(2)};
                ImVec4 fg = i < now ? p.text_faint : p.overlay;
                if (i == now) {
                    dl->AddRectFilled(a, b, col(p.accent, 0.18f), (b.y - a.y) * 0.5f);
                    fg = p.accent_hot;
                } else if (i > now) {
                    fg = mix(p.text_faint, p.surface, 0.35f);
                }
                float x = a.x + S(8);
                if (i < now) {
                    draw_icon(dl, Icon::Check, {x + S(5), pos.y + cy}, S(11), col(fg), S(1.6f));
                    x += extra;
                }
                dl->AddText({x, pos.y + cy - ts.y * 0.5f}, col(fg), shown);
                ImGui::Dummy({w, th});
                ImGui::SameLine(0.0f, S(2));
            }
            const int iteration = app.pipeline.iteration();
            if (iteration > 0) {
                ImGui::SameLine(0.0f, S(10));
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextUnformatted(format("pass %d", iteration).c_str());
                ImGui::PopStyleColor();
            }
        } else if (stage == Stage::Failed) {
            ImGui::SameLine(0.0f, S(8));
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
            ImGui::TextUnformatted(app.pipeline.error().c_str());
            ImGui::PopStyleColor();
        } else if (have_report && stage == Stage::Done) {
            ImGui::SameLine(0.0f, S(8));
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
            std::string detail = format("- %zu pass%s", passes, passes == 1 ? "" : "es");
            if (passes > 1 && kept > 0) detail += format(", kept pass %d", kept);
            if (exported) detail += format(", %zu files exported", exported);
            ImGui::TextUnformatted(detail.c_str());
            ImGui::PopStyleColor();
        }

        // --- right: clock and log -------------------------------------------
        int warns = 0, errors = 0;
        {
            static std::vector<log::Entry> entries;
            log::snapshot(entries);
            for (const log::Entry& e : entries) {
                if (e.level == log::Level::Warn)  ++warns;
                if (e.level == log::Level::Error) ++errors;
            }
        }
        const double seconds = running ? ImGui::GetTime() - app.run_started : took;
        const std::string clock = seconds > 0.0 ? clock_text(seconds) : std::string();
        const std::string count = errors ? format("%d", errors)
                                         : (warns ? format("%d", warns) : std::string());
        const float log_w = S(16) + S(14) + ImGui::CalcTextSize("Log").x +
                            (count.empty() ? 0.0f : ImGui::CalcTextSize(count.c_str()).x + S(18));
        const float clock_w = clock.empty() ? 0.0f : ImGui::CalcTextSize(clock.c_str()).x + S(14);

        ImGui::SameLine();
        ImGui::SetCursorPosX(size.x - S(16) - log_w - clock_w);
        if (!clock.empty()) {
            ImGui::SetCursorPosY(cy - th * 0.5f);
            ImGui::PushFont(f.mono, f.size_small);
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
            ImGui::TextUnformatted(clock.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0.0f, S(14));
        }
        ImGui::SetCursorPosY(cy - S(12));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##log", {log_w, S(24)})) app.show_log = !app.show_log;
        const bool hov = ImGui::IsItemHovered();
        if (hov || app.show_log)
            dl->AddRectFilled(bp, {bp.x + log_w, bp.y + S(24)},
                              col(app.show_log ? p.accent_dim : p.elevated), S(6));
        float x = bp.x + S(8);
        draw_icon(dl, Icon::Terminal, {x + S(7), bp.y + S(12)}, S(14),
                  col(app.show_log ? p.accent_hot : p.text_dim));
        x += S(14) + S(6);
        dl->AddText({x, bp.y + S(12) - th * 0.5f}, col(hov ? p.text : p.text_dim), "Log");
        x += ImGui::CalcTextSize("Log").x + S(6);
        if (!count.empty()) {
            const ImVec4 c  = errors ? p.danger : p.warning;
            const float  cw = ImGui::CalcTextSize(count.c_str()).x + S(12);
            dl->AddRectFilled({x, bp.y + S(4)}, {x + cw, bp.y + S(20)}, col(c, 0.18f), S(8));
            dl->AddText({x + S(6), bp.y + S(12) - th * 0.5f}, col(c), count.c_str());
        }
        if (hov) ImGui::SetTooltip("Log (Ctrl+L)");
        ImGui::PopFont();
    }
    end_surface();
}

// ---------------------------------------------------------------------------
// Log drawer
// ---------------------------------------------------------------------------
void draw_log_drawer(AppState& app, const ImVec2& pos, const ImVec2& size)
{
    const Palette& p = palette();
    static const char* const kLevels[] = {"Trace", "Debug", "Info", "Warn", "Error"};

    const bool open = begin_surface("##log_drawer", pos, size, p.surface,
                                    ImVec2(S(16), S(12)));
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(pos, {pos.x + size.x, pos.y}, col(p.line), 1.0f);

        ImGui::AlignTextToFramePadding();
        subheading("Log");
        ImGui::SameLine(0.0f, S(16));
        ImGui::SetNextItemWidth(S(100));
        if (ImGui::BeginCombo("##level", kLevels[std::clamp(app.log_min_level, 0, 4)],
                              ImGuiComboFlags_NoArrowButton)) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::Selectable(kLevels[i], app.log_min_level == i)) {
                    app.log_min_level = i;
                    log::set_min_level(log::Level(i));
                }
            ImGui::EndCombo();
        }
        {
            const ImVec2 mx = ImGui::GetItemRectMax();
            draw_icon(ImGui::GetWindowDrawList(), Icon::ChevronDown,
                      {mx.x - S(14), (ImGui::GetItemRectMin().y + mx.y) * 0.5f}, S(12),
                      col(p.text_faint));
        }
        ImGui::SameLine();
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", app.log_filter.c_str());
            ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x - S(260)));
            if (ImGui::InputTextWithHint("##filter", "Filter", buf, sizeof(buf)))
                app.log_filter = buf;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(size.x - S(16) - S(250));
        ImGui::BeginGroup();
        ImGui::PushID("follow");
        begin_form("follow_form", 50.0f);
        toggle("Follow", &app.log_autoscroll);
        end_form();
        ImGui::PopID();
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, S(12));
        if (ghost_button("Clear")) log::clear();
        ImGui::SameLine(0.0f, S(4));
        if (icon_button("##close_log", Icon::Close, "Close (Ctrl+L)")) app.show_log = false;

        spacer(4.0f);

        static std::vector<log::Entry> entries;
        log::snapshot(entries);

        const Fonts& f = fonts();
        ImGui::PushFont(f.mono, f.size_mono);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, p.canvas);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
        ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
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
            ImGui::SameLine(0.0f, S(12));
            ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }

        if (app.log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    end_surface();
}

// ---------------------------------------------------------------------------
void draw_toast(AppState& app)
{
    if (app.toast.empty() || ImGui::GetTime() > app.toast_until) return;

    const Palette& p = palette();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                             vp->WorkPos.y + vp->WorkSize.y - S(56)},
                            ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.elevated);
    ImGui::PushStyleColor(ImGuiCol_Border, p.overlay);

    if (ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoDocking)) {
        icon(Icon::Info, ImGui::GetTextLineHeight(), p.accent_hot);
        ImGui::SameLine(0.0f, S(10));
        ImGui::TextUnformatted(app.toast.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// Settings: one sheet, four pages.
//
// Everything here is decided once for a project or once for a machine - the
// target hardware, which model directs, how the mesh is split, where the run
// writes. Keeping it out of the main window is the point: what is left there is
// load, look, run, tune.
// ---------------------------------------------------------------------------
void draw_options(AppState& app)
{
    // A brief the director calls strained or impossible is worth interrupting
    // for: open on the target page, where the advice sits next to the sliders
    // it is talking about.
    if (app.advice_wants_attention && !app.advice_dismissed) {
        app.advice_wants_attention = false;
        app.show_options = true;
        app.options_page = 0;
    }

    if (app.show_options && !ImGui::IsPopupOpen("##settings"))
        ImGui::OpenPopup("##settings");

    if (!begin_sheet("##settings", {S(980), S(720)})) {
        app.show_options = false;
        return;
    }

    const Palette& p = palette();
    struct Page {
        const char* title;
        const char* blurb;
        void (*draw)(AppState&);
    };
    static const Page kPages[] = {
        {"Target hardware", "Hard limits. The validator rejects anything past these, and the "
                            "director is told about them up front.", &draw_profile_settings},
        {"Director", "Which model directs. It fills in the knob panel and judges renders; "
                     "it never touches a vertex.", &draw_director_settings},
        {"Region split", "Who draws the patches the director is asked to name.",
         &draw_segmenter_settings},
        {"Output", "Where a run writes and how hard it tries.", &draw_run_settings},
    };
    const std::string subs[] = {
        pretty_profile_name(app.settings.profile.name),
        llm_backend_name(app.settings.llm.kind),
        segmenter_kind_name(app.settings.segmenter.kind),
        app.settings.auto_export ? "export on pass" : "no export",
    };
    app.options_page = std::clamp(app.options_page, 0, 3);

    const ImVec2 size = ImGui::GetWindowSize();
    const float  nav  = S(230);

    // --- navigation ----------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.canvas);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(20)));
    ImGui::BeginChild("nav", {nav, size.y}, ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        ImGui::SetCursorPosX(S(22));
        heading("Settings");
        spacer(10.0f);
        const Fonts& f = fonts();
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float  w   = ImGui::GetContentRegionAvail().x;
            const float  h   = ImGui::GetTextLineHeight() * 2.0f + S(18);
            if (ImGui::InvisibleButton("##page", {w, h})) app.options_page = i;
            const bool hov = ImGui::IsItemHovered();
            const bool cur = app.options_page == i;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (cur || hov)
                dl->AddRectFilled(pos, {pos.x + w, pos.y + h},
                                  col(cur ? p.elevated : p.surface_alt), S(8));
            ImGui::PushFont(f.medium, f.size_body);
            dl->AddText({pos.x + S(10), pos.y + S(8)}, col(cur ? p.text : p.text_dim),
                        kPages[i].title);
            ImGui::PopFont();
            ImGui::PushFont(f.regular, f.size_small);
            dl->AddText({pos.x + S(10), pos.y + S(10) + ImGui::GetTextLineHeight() * 1.15f},
                        col(p.text_faint), subs[i].c_str());
            ImGui::PopFont();
            ImGui::PopID();
            spacer(2.0f);
        }
        ImGui::SetCursorPosY(size.y - S(70));
        ImGui::SetCursorPosX(S(22));
        ImGui::PushTextWrapPos(nav - S(16));
        hint("Saved with the app. A run reads them when it starts.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // --- page ----------------------------------------------------------------
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginGroup();
    const Page& page   = kPages[app.options_page];
    const float footer = S(64);
    const float body_w = size.x - nav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(22)));
    ImGui::BeginChild("head", {body_w, 0}, ImGuiChildFlags_AlwaysUseWindowPadding |
                                               ImGuiChildFlags_AutoResizeY);
    heading(page.title);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(28) - ImGui::GetFrameHeight() - S(4));
    if (icon_button("##close", Icon::Close, "Close (Esc)")) {
        app.show_options = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    ImGui::TextWrapped("%s", page.blurb);
    ImGui::PopStyleColor();
    ImGui::EndChild();

    const float head_h = ImGui::GetItemRectSize().y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(4)));
    ImGui::BeginChild("body", {body_w, size.y - head_h - footer},
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    page.draw(app);
    spacer(12.0f);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // --- footer ----------------------------------------------------------------
    {
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(fp, {fp.x + body_w, fp.y}, col(p.line));
        ImGui::SetCursorPos({nav + S(28), size.y - footer + (footer - S(36)) * 0.5f +
                                              (S(36) - ImGui::GetTextLineHeight()) * 0.5f});
        hint("Changes apply to the next run - right click any slider to reset it");
        ImGui::SetCursorPos({size.x - S(28) - S(96), size.y - footer + (footer - S(36)) * 0.5f});
        if (primary_button("Done", {S(96), S(36)})) {
            app.show_options = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
        app.show_options = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
void draw_lightbox(AppState& app)
{
    if (!app.lightbox.empty() && !ImGui::IsPopupOpen("##lightbox"))
        ImGui::OpenPopup("##lightbox");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!begin_sheet("##lightbox", {vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.86f})) {
        app.lightbox.clear();
        return;
    }
    const Palette& p    = palette();
    const ImVec2   size = ImGui::GetWindowSize();

    ImGui::SetCursorPos({S(20), S(16)});
    ImGui::AlignTextToFramePadding();
    subheading(app.lightbox.filename().string().c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(size.x - S(20) - S(140) - ImGui::GetFrameHeight() - S(12));
    if (secondary_button("Show in folder", {S(140), 0})) reveal_in_file_manager(app.lightbox);
    ImGui::SameLine(0.0f, S(8));
    bool close = icon_button("##close", Icon::Close, "Close (Esc)");

    int w = 0, h = 0;
    const unsigned tex = app.images.get(app.lightbox, &w, &h);
    const float top = S(64);
    const ImVec2 room{size.x - S(40), size.y - top - S(20)};
    if (tex && w > 0 && h > 0) {
        const float  scale = std::min(room.x / float(w), room.y / float(h));
        const ImVec2 is{float(w) * scale, float(h) * scale};
        ImGui::SetCursorPos({(size.x - is.x) * 0.5f, top + (room.y - is.y) * 0.5f});
        const ImVec2 ip = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ip, {ip.x + is.x, ip.y + is.y},
                                                  col(p.viewport), S(8));
        ImGui::Image(ImTextureRef(ImTextureID(tex)), is);
    }

    if (close || (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ||
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(
             ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
        app.lightbox.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
void draw_about_modal(AppState& app)
{
    if (app.show_about) {
        ImGui::OpenPopup("##about");
        app.show_about = false;
    }
    if (!begin_sheet("##about", {S(560), S(470)})) return;

    const Palette& p = palette();
    ImGui::SetCursorPos({S(32), S(32)});
    ImGui::BeginGroup();
    draw_logo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), S(56));
    ImGui::Dummy({S(56), S(56)});
    spacer(8.0f);
    heading("PS2 Retopo Director");
    if (Updater::enabled())
        faint("version %s, build %d", RD_VERSION_STRING, Updater::current_build());
    else
        faint("version %s, local build", RD_VERSION_STRING);
    spacer(8.0f);
    ImGui::PushTextWrapPos(S(560) - S(32));
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    ImGui::TextWrapped(
        "High poly in, console budget low poly out. Deterministic code does every "
        "piece of geometry, UV and bake work; a language model acts as art "
        "director, deciding how the triangle budget is split and judging the "
        "result from renders. It never touches a vertex.");
    ImGui::PopStyleColor();
    spacer(10.0f);
    section_label("Third party");
    hint("meshoptimizer, xatlas, Dear ImGui, cgltf, ufbx, stb and nlohmann/json "
         "(MIT); GLFW (zlib).");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::SetCursorPos({S(560) - S(32) - S(100), S(470) - S(32) - S(36)});
    if (primary_button("Close", {S(100), S(36)}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_shortcuts_modal(AppState& app)
{
    if (app.show_shortcuts) {
        ImGui::OpenPopup("##shortcuts");
        app.show_shortcuts = false;
    }
    if (!begin_sheet("##shortcuts", {S(520), S(580)})) return;

    const Palette& p = palette();
    static const char* const kRows[][2] = {
        {"Ctrl O", "Open a mesh"},        {"F5", "Run"},
        {"F6", "Rebuild from the knobs"}, {"Esc", "Stop the run"},
        {"Ctrl ,", "Settings"},           {"Ctrl L", "Log"},
        {"Tab", "Low poly / source / compare"},
        {"1  2  3  4", "Shaded, regions, silhouette, normals"},
        {"W", "Wireframe"},               {"F", "Frame the mesh"},
        {"F12", "Screenshot"},
        {"Right drag", "Orbit"},          {"Middle drag", "Pan"},
        {"Wheel", "Zoom"},                {"Double click", "Frame the mesh"},
        {"Right click", "Reset a slider"},
    };

    ImGui::SetCursorPos({S(32), S(28)});
    ImGui::BeginGroup();
    heading("Keyboard and mouse");
    spacer(10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(4)));
    const Fonts& f = fonts();
    for (const auto& row : kRows) {
        ImGui::PushFont(f.mono, f.size_small);
        const ImVec2 ts  = ImGui::CalcTextSize(row[0]);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            pos, {pos.x + ts.x + S(14), pos.y + ts.y + S(8)}, col(p.elevated), S(6));
        ImGui::GetWindowDrawList()->AddText({pos.x + S(7), pos.y + S(4)}, col(p.text), row[0]);
        ImGui::PopFont();
        ImGui::Dummy({S(130), ts.y + S(8)});
        ImGui::SameLine(0.0f, S(12));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
        muted("%s", row[1]);
    }
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    ImGui::SetCursorPos({S(520) - S(32) - S(100), S(580) - S(28) - S(36)});
    if (primary_button("Close", {S(100), S(36)}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// The window icon: the same faceted mark as the top bar, rasterised in
// software at the sizes the taskbar and the title bar ask for. There is no
// image file to ship and nothing to load.
// ---------------------------------------------------------------------------
namespace {

struct IconPixels {
    int                  size = 0;
    std::vector<uint8_t> rgba;
};

float seg_distance(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float t  = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    const float dx = ax + vx * t - px, dy = ay + vy * t - py;
    return std::sqrt(dx * dx + dy * dy);
}

bool in_triangle(float px, float py, const float* a, const float* b, const float* c)
{
    auto side = [&](const float* p0, const float* p1) {
        return (p1[0] - p0[0]) * (py - p0[1]) - (p1[1] - p0[1]) * (px - p0[0]);
    };
    const float d0 = side(a, b), d1 = side(b, c), d2 = side(c, a);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

IconPixels rasterise_mark(int size)
{
    const Palette& pal = palette();
    IconPixels out;
    out.size = size;
    out.rgba.assign(size_t(size) * size_t(size) * 4, 0);

    // On the 64 unit grid of the icon sheet.
    const float top[2] = {32, 12}, ur[2] = {49, 22}, lr[2] = {49, 42}, bot[2] = {32, 52},
                ll[2] = {15, 42}, ul[2] = {15, 22}, mid[2] = {32, 30};
    struct Facet { const float *a, *b, *c; float alpha; };
    const Facet facets[] = {{ul, top, mid, 0.85f}, {top, ur, mid, 0.55f}, {ul, mid, ll, 0.35f},
                            {ur, lr, mid, 0.20f},  {ll, mid, bot, 0.15f}, {mid, lr, bot, 0.08f}};
    const float* edges[][2] = {{top, ur}, {ur, lr}, {lr, bot}, {bot, ll}, {ll, ul}, {ul, top},
                               {top, mid}, {ul, mid}, {ur, mid}, {mid, bot}};
    // Lines thicken as the icon shrinks, or they vanish at 16 px.
    const float line = std::max(1.6f, 64.0f / float(size) * 0.9f);
    const float radius = 15.0f;
    const int   ss = 4;

    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = (float(x) + (float(sx) + 0.5f) / ss) * 64.0f / float(size);
                    const float v = (float(y) + (float(sy) + 0.5f) / ss) * 64.0f / float(size);
                    // Rounded square.
                    const float qx = std::max(std::abs(u - 32.0f) - (32.0f - radius), 0.0f);
                    const float qy = std::max(std::abs(v - 32.0f) - (32.0f - radius), 0.0f);
                    if (qx * qx + qy * qy > radius * radius) continue;
                    float cr = pal.accent.x, cg = pal.accent.y, cb = pal.accent.z;
                    float white = 0.0f;
                    for (const Facet& f : facets)
                        if (in_triangle(u, v, f.a, f.b, f.c)) white = f.alpha;
                    for (const auto& e : edges)
                        if (seg_distance(u, v, e[0][0], e[0][1], e[1][0], e[1][1]) < line * 0.5f)
                            white = 1.0f;
                    cr += (1.0f - cr) * white;
                    cg += (1.0f - cg) * white;
                    cb += (1.0f - cb) * white;
                    r += cr; g += cg; b += cb; a += 1.0f;
                }
            const float n = float(ss * ss);
            uint8_t* px = &out.rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
            if (a > 0.0f) {
                px[0] = uint8_t(std::clamp(r / a, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[1] = uint8_t(std::clamp(g / a, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[2] = uint8_t(std::clamp(b / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            px[3] = uint8_t(a / n * 255.0f + 0.5f);
        }
    return out;
}

} // namespace

void set_window_icon(GLFWwindow* window)
{
    static std::vector<IconPixels> sizes;
    if (sizes.empty())
        for (int s : {16, 24, 32, 48, 64, 128}) sizes.push_back(rasterise_mark(s));
    std::vector<GLFWimage> images;
    for (IconPixels& ip : sizes) images.push_back({ip.size, ip.size, ip.rgba.data()});
    glfwSetWindowIcon(window, int(images.size()), images.data());
}

} // namespace rd::ui
