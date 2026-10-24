// The inspector: one panel on the right with four tabs. Tune is the knob
// panel, Report says whether the result is good enough, Director is the
// conversation, Renders the images it was shown.

#include "ui/panels.h"

#include "core/paths.h"
#include "core/util.h"
#include "ui/file_dialog.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
// FindRenderedTextEnd, to hide a label's ## suffix like ImGui does.
#include <imgui_internal.h>

namespace rd::ui {
namespace {

float S(float v) { return v * dpi(); }

const char* const kBackendNames[]  = {"Auto", "Quad field", "Quadric"};
const char* const kFidelityNames[] = {"Geometry", "Balanced", "Texture"};

std::string thousands(size_t n)
{
    std::string digits = std::to_string(n);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out;
}

std::string tokens(int n)
{
    return n >= 1000 ? format("%.1fk", double(n) / 1000.0) : format("%d", n);
}

ImVec4 region_color(const Segmentation& seg, uint32_t id)
{
    if (const Region* r = seg.find(id))
        return {r->debug_color.x, r->debug_color.y, r->debug_color.z, 1.0f};
    return palette().text_faint;
}

bool knobs_differ(const RegionKnobs& a, const RegionKnobs& b)
{
    return a.share != b.share || a.detail_priority != b.detail_priority ||
           a.fidelity != b.fidelity || a.hard_edge_degrees != b.hard_edge_degrees ||
           a.preserve_silhouette != b.preserve_silhouette ||
           a.preserve_boundary != b.preserve_boundary || a.symmetry_lock != b.symmetry_lock ||
           a.curvature_bias != b.curvature_bias;
}

// A tab in the inspector's strip: glyph, label and an optional status dot.
bool tab_button(const char* label, Icon glyph, bool active, const ImVec4* dot, float w)
{
    const Palette& p = palette();
    const float    h = S(34);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, {w, h});
    const bool hov     = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hov)
        dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, col(active ? p.elevated : p.surface_alt),
                          S(8));
    const char*  end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts  = ImGui::CalcTextSize(label, end);
    const float  is  = S(15);
    const float  dw  = dot ? S(12) : 0.0f;
    const float  cw  = is + S(7) + ts.x + dw;
    float x = pos.x + (w - cw) * 0.5f;
    const ImVec4 tc = active ? p.text : (hov ? p.text_dim : p.text_faint);
    draw_icon(dl, glyph, {x + is * 0.5f, pos.y + h * 0.5f}, is, col(tc));
    x += is + S(7);
    dl->AddText({x, pos.y + (h - ts.y) * 0.5f}, col(tc), label, end);
    if (dot) dl->AddCircleFilled({x + ts.x + S(8), pos.y + h * 0.5f}, S(3), col(*dot));
    return pressed;
}

} // namespace

// ---------------------------------------------------------------------------
void draw_inspector(AppState& app, const ImVec2& pos, const ImVec2& size)
{
    const Palette& p = palette();
    const bool open = begin_surface("##inspector", pos, size, p.surface, ImVec2(0, 0),
                                    ImGuiWindowFlags_NoScrollbar);
    if (!open) {
        end_surface();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(pos, {pos.x, pos.y + size.y}, col(p.line), 1.0f);

    // --- the edge drags to resize ------------------------------------------------
    ImGui::SetCursorScreenPos({pos.x - S(3), pos.y});
    ImGui::InvisibleButton("##resize", {S(7), size.y});
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        dl->AddLine(pos, {pos.x, pos.y + size.y}, col(p.accent), S(2));
    }
    if (ImGui::IsItemActive())
        app.inspector_width = std::clamp(app.inspector_width - ImGui::GetIO().MouseDelta.x / dpi(),
                                         320.0f, 720.0f);

    // --- tabs -------------------------------------------------------------------
    bool validated = false, passed = false, dirty = app.panel_dirty;
    app.pipeline.with_results([&](const PipelineResults& r) {
        validated = !r.validation.checks.empty();
        passed    = r.validation.passed;
    });
    {
        ImGui::SetCursorPos({S(12), S(12)});
        const float gap = S(4);
        const float w   = (size.x - S(24) - gap * 3.0f) / 4.0f;
        const ImVec4 report_dot = passed ? p.success : p.danger;
        const ImVec4 live_dot   = p.accent;
        const ImVec4 dirty_dot  = p.warning;
        const bool   live = app.pipeline.running() && app.settings.use_llm;

        struct T { const char* label; Icon glyph; AppState::Tab tab; const ImVec4* dot; };
        const T tabs[] = {
            {"Tune", Icon::Sliders, AppState::Tab::Tune, dirty ? &dirty_dot : nullptr},
            {"Report", Icon::Report, AppState::Tab::Report, validated ? &report_dot : nullptr},
            {"Director", Icon::Chat, AppState::Tab::Director, live ? &live_dot : nullptr},
            {"Renders", Icon::Image, AppState::Tab::Renders, nullptr},
        };
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine(0.0f, gap);
            if (tab_button(tabs[i].label, tabs[i].glyph, app.tab == tabs[i].tab, tabs[i].dot, w))
                app.tab = tabs[i].tab;
        }
    }

    // --- the footer the Tune tab keeps in one place -----------------------------------
    const bool  tune_footer = app.tab == AppState::Tab::Tune && can_rebuild(app);
    const float footer_h    = tune_footer ? S(58) : 0.0f;
    const float top         = S(12) + S(34) + S(8);

    ImGui::SetCursorPos({0, top});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(10)));
    ImGui::BeginChild("tab_body", {size.x, size.y - top - footer_h},
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (app.tab) {
    case AppState::Tab::Tune:     tab_tune(app); break;
    case AppState::Tab::Report:   tab_report(app); break;
    case AppState::Tab::Director: tab_director(app); break;
    case AppState::Tab::Renders:  tab_renders(app); break;
    }
    spacer(12.0f);
    ImGui::EndChild();

    if (tune_footer) {
        const ImVec2 fa{pos.x, pos.y + size.y - footer_h};
        dl->AddRectFilled(fa, {pos.x + size.x, pos.y + size.y},
                          col(dirty ? p.surface_alt : p.surface));
        dl->AddLine(fa, {fa.x + size.x, fa.y}, col(dirty ? p.overlay : p.line));
        const float cy = size.y - footer_h * 0.5f;
        const bool  running = app.pipeline.running();

        const float rb_w = S(128);
        ImGui::SetCursorPos({S(16), cy - ImGui::GetTextLineHeight() * 0.5f});
        if (dirty) {
            const ImVec2 dp = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled({dp.x + S(4), dp.y + ImGui::GetTextLineHeight() * 0.5f}, S(4),
                                col(p.warning));
            ImGui::Dummy({S(8), 1});
            ImGui::SameLine(0.0f, S(10));
            ImGui::TextUnformatted("Changes not built yet");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
            ImGui::TextUnformatted("Rebuild reuses the regions");
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(running);
        if (dirty) {
            ImGui::SetCursorPos({size.x - S(16) - rb_w - S(8) - S(76), cy - S(17)});
            if (ghost_button("Revert", {S(76), S(34)})) action_revert_panel(app);
            ImGui::SameLine(0.0f, S(8));
        } else {
            ImGui::SetCursorPos({size.x - S(16) - rb_w, cy - S(17)});
        }
        if (icon_label_button("Rebuild  F6", Icon::Rebuild,
                              dirty ? ButtonKind::Primary : ButtonKind::Secondary,
                              {rb_w, S(34)}))
            action_rebuild(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Density, retopology, bake and validation again from these "
                              "knobs. Seconds, not a whole run.");
    }

    end_surface();
}

// ---------------------------------------------------------------------------
// Tune
// ---------------------------------------------------------------------------
void tab_tune(AppState& app)
{
    const Palette& p = palette();

    Segmentation     seg;
    std::vector<int> actual;
    KnobPanel        built;
    app.pipeline.with_results([&](const PipelineResults& r) {
        seg    = r.segmentation;
        actual = r.retopo.region_triangles;
        built  = r.panel;
    });

    if (app.panel.regions.empty()) {
        empty_state("Nothing to tune yet",
                    "The knobs fill in once the mesh has been split into regions. They are "
                    "what the director writes and what you can overrule; the geometry engine "
                    "reads nothing else.",
                    Icon::Sliders);
        if (!app.mesh_path.empty() && !app.pipeline.running()) {
            spacer(14.0f);
            const float w = S(140);
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f + S(16));
            if (icon_label_button("Run", Icon::Play, ButtonKind::Primary, {w, S(38)}))
                action_run(app);
        }
        return;
    }

    // --- budget ----------------------------------------------------------------------
    const int max_tris = app.settings.profile.max_triangles;
    const int total    = app.panel.total_budget();
    if (card_begin("budget_card")) {
        ImGui::AlignTextToFramePadding();
        muted("Triangle budget");
        ImGui::SameLine();
        {
            const Fonts& f = fonts();
            const std::string big = thousands(size_t(total));
            const std::string sml = " / " + thousands(size_t(max_tris));
            ImGui::PushFont(f.medium, f.size_title + S(2));
            const float bw = ImGui::CalcTextSize(big.c_str()).x;
            ImGui::PopFont();
            const float sw = ImGui::CalcTextSize(sml.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 std::max(4.0f, ImGui::GetContentRegionAvail().x - bw - sw));
            ImGui::PushFont(f.medium, f.size_title + S(2));
            ImGui::PushStyleColor(ImGuiCol_Text, total > max_tris ? p.danger : p.text);
            ImGui::TextUnformatted(big.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
            faint("%s", sml.c_str());
        }
        spacer(2.0f);
        // One bar, one segment per region in its own colour: the split at a glance.
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float  bw = ImGui::GetContentRegionAvail().x;
        const float  bh = S(8);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(bp, {bp.x + bw, bp.y + bh}, col(p.overlay), S(4));
        float x = bp.x;
        const float denom = float(std::max(total, max_tris));
        for (const RegionKnobs& k : app.panel.regions) {
            const float w = bw * float(k.triangle_budget) / std::max(1.0f, denom);
            if (w < 1.0f) continue;
            dl->AddRectFilled({x, bp.y}, {x + std::max(1.0f, w - S(2)), bp.y + bh},
                              col(region_color(seg, k.id)), S(2));
            x += w;
        }
        ImGui::Dummy({bw, bh});
        card_end();
    }

    spacer(8.0f);

    // --- regions --------------------------------------------------------------------
    {
        const bool  shown = app.mode == RenderMode::Regions;
        const char* t     = shown ? "Shown in viewport" : "Show in viewport";
        const float tw    = ImGui::CalcTextSize(t).x + S(40);
        const float y     = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (S(26) - ImGui::GetTextLineHeight()) * 0.5f);
        section_label(format("Regions - %zu", app.panel.regions.size()).c_str());
        ImGui::SetCursorPos({ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - tw, y});
        if (icon_label_button(t, Icon::Eye, shown ? ButtonKind::Secondary : ButtonKind::Ghost,
                              {tw, S(26)})) {
            app.mode        = shown ? RenderMode::Shaded : RenderMode::Regions;
            app.gpu_version = 0;
        }
        ImGui::SetCursorPosY(y + S(32));
    }
    // Rows sit close; the spacing between them is in the row height.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(2)));

    int largest = 1;
    for (const RegionKnobs& k : app.panel.regions) largest = std::max(largest, k.triangle_budget);

    for (int i = 0; i < int(app.panel.regions.size()); ++i) {
        RegionKnobs& k = app.panel.regions[size_t(i)];
        ImGui::PushID(i);
        const bool selected = app.selected_region == i;
        const ImVec4 rc     = region_color(seg, k.id);

        const RegionKnobs* was = nullptr;
        for (const RegionKnobs& b : built.regions)
            if (b.id == k.id) was = &b;
        const bool edited = was && knobs_differ(k, *was);

        // --- the row -----------------------------------------------------------
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float  w   = ImGui::GetContentRegionAvail().x;
        const float  h   = S(36);
        if (ImGui::InvisibleButton("##row", {w, h})) app.selected_region = selected ? -1 : i;
        if (selected && app.scroll_to_region) {
            ImGui::SetScrollHereY(0.15f);
            app.scroll_to_region = false;
        }
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected)
            dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, col(p.surface_alt), S(10),
                              ImDrawFlags_RoundCornersTop);
        else if (hov)
            dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, col(p.surface_alt), S(8));
        const float cy = pos.y + h * 0.5f;
        dl->AddRectFilled({pos.x + S(10), cy - S(5)}, {pos.x + S(20), cy + S(5)}, col(rc), S(3));
        const float name_w = std::min(S(130), w * 0.38f);
        dl->PushClipRect({pos.x + S(30), pos.y}, {pos.x + S(30) + name_w, pos.y + h}, true);
        ImGui::PushFont(selected ? fonts().medium : fonts().regular, fonts().size_body);
        dl->AddText({pos.x + S(30), cy - ImGui::GetTextLineHeight() * 0.5f}, col(p.text),
                    k.name.c_str());
        ImGui::PopFont();
        dl->PopClipRect();

        const std::string num = thousands(size_t(std::max(0, k.triangle_budget)));
        ImGui::PushFont(fonts().mono, fonts().size_small);
        const ImVec2 ns = ImGui::CalcTextSize(num.c_str());
        dl->AddText({pos.x + w - S(10) - ns.x, cy - ns.y * 0.5f},
                    col(edited ? p.warning : p.text_dim), num.c_str());
        ImGui::PopFont();
        const float bx0 = pos.x + S(30) + name_w + S(8);
        const float bx1 = pos.x + w - S(10) - S(44);
        if (bx1 > bx0 + S(10)) {
            dl->AddRectFilled({bx0, cy - S(2)}, {bx1, cy + S(2)}, col(p.overlay), S(2));
            const float fx = bx0 + (bx1 - bx0) * float(k.triangle_budget) / float(largest);
            dl->AddRectFilled({bx0, cy - S(2)}, {std::max(bx0 + S(2), fx), cy + S(2)},
                              col(selected ? rc : p.text_faint), S(2));
        }

        // --- the open one ----------------------------------------------------------
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface_alt);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(6)));
            ImGui::SetCursorScreenPos({pos.x, pos.y + h - S(2)});
            ImGui::BeginChild("detail", {w, 0},
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
            bool changed = false;

            if (const Region* region = seg.find(k.id))
                hint(format("%s - %.1f%% of the surface, %u source triangles",
                            region->auto_label.c_str(), region->area_share * 100.0f,
                            region->triangle_count).c_str());
            if (k.id < actual.size()) {
                if (edited) {
                    badge("edited", p.warning);
                    ImGui::SameLine();
                }
                faint("built %d, now %d", actual[k.id], k.triangle_budget);
            }
            spacer(4.0f);

            begin_form("region_form", 118.0f);
            {
                // Shown as the region's percentage of the budget, not as the raw
                // weight. The weights add up to about one, so a weight slider
                // running to twenty handed half the budget to one region within
                // a few pixels of the left end. The others keep their
                // proportions and make room.
                double others = 0.0, built_others = 0.0;
                float  built_share = k.share;
                for (const RegionKnobs& o : app.panel.regions)
                    if (o.id != k.id) others += std::max(0.0f, o.share);
                for (const RegionKnobs& o : built.regions) {
                    if (o.id == k.id) built_share = o.share;
                    else              built_others += std::max(0.0f, o.share);
                }
                auto percent = [](double mine, double rest) {
                    return mine + rest > 1e-9 ? float(100.0 * mine / (mine + rest)) : 0.0f;
                };
                float pct = percent(std::max(0.0f, k.share), others);
                const float built_pct =
                    percent(std::max(0.0f, built_share), was ? built_others : others);
                if (slider_float("Budget", &pct, 0.0f, 60.0f, built_pct,
                                 "This region's slice of the triangle budget. The other "
                                 "regions keep their proportions and share what is left. "
                                 "Right click goes back to what was built.",
                                 "%.1f%%")) {
                    const double p = std::clamp(double(pct), 0.0, 95.0) / 100.0;
                    k.share = others > 1e-9 ? float(p * others / (1.0 - p)) : 1.0f;
                    changed = true;
                }
            }
            changed |= slider_float("Detail priority", &k.detail_priority, 0.0f, 1.0f, 0.5f,
                                    "Breaks ties when the budget runs short.");
            {
                ImGui::PushID("fidelity");
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextUnformatted("Fidelity");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("Geometry: the shape must exist in the silhouette.\n"
                                      "Texture: flatten it and let the bake carry the detail.");
                ImGui::SameLine();
                ImGui::SetCursorPosX(S(12) + S(118));
                int fidelity = int(k.fidelity);
                if (segmented("seg", &fidelity, kFidelityNames, 3)) {
                    k.fidelity = Fidelity(fidelity);
                    changed    = true;
                }
                ImGui::PopID();
            }
            changed |= toggle("Keep silhouette", &k.preserve_silhouette,
                              "Protect the triangles that form this region's outline.");
            end_form();

            if (section("More knobs", false)) {
                begin_form("region_more", 118.0f);
                changed |= slider_float("Hard edge", &k.hard_edge_degrees, 0.0f, 180.0f, 40.0f,
                                        "Creases sharper than this stay sharp.", "%.0f deg");
                changed |= toggle("Keep boundary", &k.preserve_boundary);
                changed |= slider_float("Symmetry lock", &k.symmetry_lock, 0.0f, 1.0f, 1.0f);
                changed |= slider_float("Curvature bias", &k.curvature_bias, 0.0f, 1.0f, 0.5f,
                                        "Follow curvature, or stay uniform.");
                end_form();
            }

            if (!k.rationale.empty()) {
                spacer(2.0f);
                const ImVec2 qa = ImGui::GetCursorScreenPos();
                const float  qw = ImGui::GetContentRegionAvail().x;
                ImGui::GetWindowDrawList()->ChannelsSplit(2);
                ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
                ImGui::SetCursorScreenPos({qa.x + S(10), qa.y + S(8)});
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + qw - S(20));
                ImGui::PushStyleColor(ImGuiCol_Text, p.accent_hot);
                ImGui::TextUnformatted("Director");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextWrapped("%s", k.rationale.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                const float qb = ImGui::GetCursorScreenPos().y + S(4);
                ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
                ImGui::GetWindowDrawList()->AddRectFilled(qa, {qa.x + qw, qb}, col(p.surface),
                                                          S(8));
                ImGui::GetWindowDrawList()->ChannelsMerge();
                ImGui::SetCursorScreenPos({qa.x, qb});
                ImGui::Dummy({qw, S(4)});
            }

            if (changed) {
                app.panel_dirty = true;
                app.panel.resolve_budgets(max_tris);
            }
            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            spacer(4.0f);
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    spacer(10.0f);

    // --- global knobs ---------------------------------------------------------------
    GlobalKnobs& g = app.panel.global;
    bool changed = false;

    if (section("Shape", false, format("%s - symmetry %s", kBackendNames[int(g.backend)],
                                       g.enforce_symmetry ? "on" : "off").c_str())) {
        begin_form("global_form", 150.0f);
        int backend = int(g.backend);
        if (combo("Backend", &backend, kBackendNames, 3,
                  "Auto picks the quad field for skinned meshes and the quadric collapse for "
                  "everything else.")) {
            g.backend = RetopoBackend(backend);
            changed   = true;
        }
        changed |= toggle("Enforce symmetry", &g.enforce_symmetry,
                          "Mirrors the result across the detected plane. The profile can "
                          "require this regardless.");
        changed |= slider_float("Joint loops", &g.joint_loop_density, 0.0f, 3.0f, 1.0f,
                                "Extra edge rings around deforming joints.");
        changed |= slider_float("Merge", &g.merge_aggressiveness, 0.0f, 1.0f, 0.5f,
                                "How eagerly flat areas collapse.");
        changed |= slider_float("Curvature", &g.curvature_influence, 0.0f, 1.0f, 0.6f,
                                "Global weight of the curvature term in the density field.");
        changed |= slider_float("Silhouette", &g.silhouette_weight, 0.0f, 1.0f, 0.7f,
                                "Protection for triangles that form an outline in the profile "
                                "cameras.");
        changed |= slider_int("Smoothing", &g.smoothing_iterations, 0, 16, 3,
                              "Tangential relaxation passes in the quad field backend.");
        changed |= slider_float("Quad dominance", &g.quad_dominance, 0.0f, 1.0f, 0.7f,
                                "How hard to pair triangles into quads before re-triangulating. "
                                "Higher means longer strips.");
        end_form();
        spacer(6.0f);
    }

    if (section("Bake", false, format("AO %.2f%s", g.bake_ambient_occlusion ? g.ao_intensity : 0.0f,
                                      g.bake_vertex_colors ? " - vertex colours" : "").c_str())) {
        begin_form("bake_form_knobs", 150.0f);
        changed |= toggle("Ambient occlusion", &g.bake_ambient_occlusion);
        changed |= slider_float("AO intensity", &g.ao_intensity, 0.0f, 1.0f, 0.75f);
        changed |= toggle("Vertex colours", &g.bake_vertex_colors);
        changed |= slider_float("UV padding", &g.uv_padding_texels, 0.0f, 16.0f, 4.0f, nullptr,
                                "%.0f px");
        end_form();
        spacer(6.0f);
    }

    if (changed) app.panel_dirty = true;

    if (section("Raw JSON", false, "what the engine reads")) {
        hint("Exactly what the director fills in and the geometry engine reads. Edit it and "
             "apply to overrule the model.");
        spacer(4.0f);
        if (app.knobs_json_buffer.empty()) app.knobs_json_buffer = json_dump(app.panel.to_json());

        const Fonts& f = fonts();
        ImGui::PushFont(f.mono, f.size_mono);
        std::vector<char> buffer(std::max<size_t>(app.knobs_json_buffer.size() * 2, 8192));
        std::snprintf(buffer.data(), buffer.size(), "%s", app.knobs_json_buffer.c_str());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.canvas);
        if (ImGui::InputTextMultiline("##json", buffer.data(), buffer.size(),
                                      ImVec2(-FLT_MIN, S(320)))) {
            app.knobs_json_buffer = buffer.data();
            std::string error;
            const Json parsed = json_parse_lenient(app.knobs_json_buffer, error);
            app.knobs_json_valid = !parsed.is_null() && error.empty();
            app.knobs_json_error = error;
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();

        spacer(4.0f);
        ImGui::BeginDisabled(!app.knobs_json_valid);
        if (secondary_button("Apply JSON")) {
            std::string error;
            const Json parsed = json_parse_lenient(app.knobs_json_buffer, error);
            if (!parsed.is_null()) {
                app.panel = KnobPanel::from_json(parsed);
                app.panel.clamp();
                app.panel.resolve_budgets(max_tris);
                app.panel_dirty = true;
                app.notify("Knob panel replaced from JSON");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ghost_button("Reload")) app.knobs_json_buffer = json_dump(app.panel.to_json());
        if (!app.knobs_json_valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
            ImGui::TextWrapped("%s", app.knobs_json_error.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (!g.notes.empty()) {
        spacer(8.0f);
        callout(Icon::Sparkle, p.accent, "Director's notes", g.notes.c_str());
    }
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
void tab_report(AppState& app)
{
    const Palette& p = palette();

    size_t high_tris = 0, low_tris = 0, low_verts = 0, regions = 0;
    SilhouetteError sil;
    ValidationReport report;
    std::vector<IterationRecord> iterations;
    int          kept = 0;
    double       total_seconds = 0.0;
    Mesh::Stats  low_stats;
    BakeResult   bake;
    ExportResult exported;
    app.pipeline.with_results([&](const PipelineResults& r) {
        high_tris  = r.highpoly.triangle_count();
        low_tris   = r.lowpoly.triangle_count();
        low_verts  = r.lowpoly.vertex_count();
        regions    = r.segmentation.size();
        sil        = r.silhouette;
        report     = r.validation;
        iterations = r.iterations;
        kept       = r.kept_iteration;
        total_seconds = r.total_seconds;
        if (!r.lowpoly.empty()) low_stats = r.lowpoly.compute_stats();
        bake     = r.bake;
        exported = r.exported;
    });

    if (report.checks.empty()) {
        empty_state("Nothing to report yet",
                    "When a pass finishes, every hard check from the target is listed here with "
                    "the number that decided it.",
                    Icon::Report);
        return;
    }

    const TargetProfile& profile = app.settings.profile;

    // --- verdict --------------------------------------------------------------------
    {
        int passed_checks = 0;
        for (const Check& c : report.checks) passed_checks += c.passed ? 1 : 0;
        const ImVec4 tint = report.passed ? p.success : p.danger;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, with_alpha(tint, 0.09f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
        ImGui::BeginChild("verdict", {0, 0},
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float  r  = S(18);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled({cp.x + r, cp.y + r}, r, col(tint, 0.18f));
        draw_icon(dl, report.passed ? Icon::Check : Icon::Cross, {cp.x + r, cp.y + r}, S(20),
                  col(tint), S(2.2f));
        ImGui::Dummy({r * 2.0f, r * 2.0f});
        ImGui::SameLine(0.0f, S(12));
        ImGui::BeginGroup();
        const std::string target = profile.name.empty() ? std::string("the target")
                                                        : profile_display_name(profile.name);
        subheading(report.passed ? format("Ready for %s", target.c_str()).c_str()
                                 : format("%d check%s failed", report.errors,
                                          report.errors == 1 ? "" : "s").c_str());
        std::string sub = format("%d passed, %d warning%s", passed_checks, report.warnings,
                                 report.warnings == 1 ? "" : "s");
        if (iterations.size() > 1 && kept > 0)
            sub += format(" - pass %d of %zu kept", kept, iterations.size());
        if (total_seconds > 0.0) sub += " - " + format_duration(total_seconds);
        muted("%s", sub.c_str());
        ImGui::EndGroup();
        spacer(6.0f);
        if (!exported.files.empty()) {
            if (icon_label_button("Open export", Icon::Folder, ButtonKind::Secondary))
                reveal_in_file_manager(paths::export_dir());
            ImGui::SameLine();
        }
        if (ghost_button("Open reports")) reveal_in_file_manager(paths::reports_dir());
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    spacer(8.0f);

    // --- the numbers ------------------------------------------------------------------
    {
        const float gap = S(8);
        const float w   = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        // Over the limit is red; a budget used to the last few triangles is
        // what the run aims for, so a full bar there is still green.
        auto tint_for = [&](float ratio) { return ratio > 1.0f ? p.danger : p.success; };
        const ImVec4 text = p.text;
        const float tr = float(low_tris) / float(std::max(1, profile.max_triangles));
        stat_tile("tris", "Triangles", thousands(low_tris).c_str(),
                  format("of %s - from %s", thousands(size_t(profile.max_triangles)).c_str(),
                         thousands(high_tris).c_str()).c_str(),
                  tint_for(tr), w, std::min(tr, 1.0f), tr > 1.0f ? nullptr : &text);
        ImGui::SameLine(0.0f, gap);
        const float vr = float(low_verts) / float(std::max(1, profile.max_vertices));
        stat_tile("verts", "Vertices", thousands(low_verts).c_str(),
                  format("of %s", thousands(size_t(profile.max_vertices)).c_str()).c_str(),
                  tint_for(vr), w, std::min(vr, 1.0f), vr > 1.0f ? nullptr : &text);

        spacer(gap - ImGui::GetStyle().ItemSpacing.y);
        const float sr = sil.mean / std::max(1e-6f, profile.max_silhouette_error);
        const ImVec4 sc = sil.mean > profile.max_silhouette_error ? p.danger
                        : sil.mean > profile.target_silhouette_error ? p.warning : p.success;
        stat_tile("outline", "Outline offset", format("%.2f px", sil.outline_px).c_str(),
                  format("silhouette %.3f of %.3f", sil.mean, profile.max_silhouette_error).c_str(),
                  sc, w, std::min(sr, 1.0f), sc.x == p.success.x ? &text : nullptr);
        ImGui::SameLine(0.0f, gap);
        if (bake.ok && !bake.diffuse.empty()) {
            const int   pc = std::max(1, profile.texture.palette_colors);
            const float pr = float(bake.palette.size()) / float(pc);
            stat_tile("tex", "Texture",
                      format("%dx%d", bake.diffuse.width, bake.diffuse.height).c_str(),
                      profile.texture.palette_colors > 0
                          ? format("%zu of %d colours", bake.palette.size(), pc).c_str()
                          : "truecolor",
                      tint_for(pr), w,
                      profile.texture.palette_colors > 0 ? std::min(pr, 1.0f) : -1.0f, &text);
        } else {
            stat_tile("reduction", "Reduction",
                      high_tris ? format("%.1f%%", 100.0 * (1.0 - double(low_tris) /
                                                                  double(high_tris))).c_str()
                                : "-",
                      format("%zu regions", regions).c_str(), p.text, w, -1.0f, &text);
        }
    }

    // --- passes ------------------------------------------------------------------------
    if (!iterations.empty()) {
        spacer(12.0f);
        section_label("Passes");
        for (const IterationRecord& it : iterations) {
            ImGui::PushID(it.index);
            const bool is_kept = it.index == kept;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float  w   = ImGui::GetContentRegionAvail().x;
            const float  h   = S(34);
            ImGui::InvisibleButton("##pass", {w, h});
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (is_kept || hov)
                dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, col(p.surface_alt), S(8));
            const Fonts& f = fonts();
            ImGui::PushFont(f.mono, f.size_small);
            const float ty = pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText({pos.x + S(10), ty}, col(p.text_faint), format("%d", it.index).c_str());
            dl->AddText({pos.x + S(34), ty}, col(p.text), thousands(it.triangles).c_str());
            const ImVec4 pxc = it.silhouette.mean > profile.max_silhouette_error ? p.danger
                             : it.silhouette.mean > profile.target_silhouette_error ? p.warning
                                                                                     : p.success;
            dl->AddText({pos.x + S(92), ty}, col(pxc),
                        format("%.2f px", it.silhouette.outline_px).c_str());
            ImGui::PopFont();
            std::string verdict = it.verdict.empty()
                ? std::string(it.validation_passed ? "passed" : "failed checks")
                : it.verdict;
            const float vx = pos.x + S(160);
            const float vr = pos.x + w - (is_kept ? S(56) : S(10));
            dl->PushClipRect({vx, pos.y}, {vr, pos.y + h}, true);
            dl->AddText({vx, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, col(p.text_dim),
                        verdict.c_str());
            dl->PopClipRect();
            if (is_kept) {
                ImGui::PushFont(f.medium, f.size_small);
                const ImVec2 ks = ImGui::CalcTextSize("kept");
                const ImVec2 kp{pos.x + w - S(10) - ks.x - S(14), pos.y + (h - ks.y) * 0.5f - S(2)};
                dl->AddRectFilled(kp, {kp.x + ks.x + S(14), kp.y + ks.y + S(4)},
                                  col(p.accent, 0.18f), S(8));
                dl->AddText({kp.x + S(7), kp.y + S(2)}, col(p.accent_hot), "kept");
                ImGui::PopFont();
            }
            if (hov && !it.critique.empty()) {
                ImGui::SetNextWindowSize({S(360), 0});
                ImGui::BeginTooltip();
                ImGui::TextWrapped("%s", it.critique.c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }

    // --- checks --------------------------------------------------------------------------
    spacer(12.0f);
    // Errors and warnings are what needs a look. Notes are the validator
    // thinking aloud - often a dozen of them - so they fold away with the passes.
    auto is_note = [](const Check& c) {
        return !c.passed && c.severity != Severity::Error && c.severity != Severity::Warning;
    };
    int failing = 0, passing = 0, notes = 0;
    for (const Check& c : report.checks) {
        if (c.passed)        ++passing;
        else if (is_note(c)) ++notes;
        else                 ++failing;
    }
    section_label(failing ? "Needs a look" : "Checks");
    bool* show_all   = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("show_passed"), false);
    bool* show_notes = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("show_notes"), false);
    if (!failing) faint("Nothing failed and nothing to warn about.");

    auto draw_check = [&](const Check& c) {
        ImVec4 tint = p.success;
        Icon   g    = Icon::Check;
        if (!c.passed) {
            switch (c.severity) {
            case Severity::Error:   tint = p.danger;  g = Icon::Cross; break;
            case Severity::Warning: tint = p.warning; g = Icon::Warning; break;
            default:                tint = p.info;    g = Icon::Info; break;
            }
        }
        ImGui::PushID(&c);
        if (c.passed) {
            icon(g, ImGui::GetTextLineHeight() * 0.9f, tint);
            ImGui::SameLine(0.0f, S(8));
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
            ImGui::TextUnformatted(c.title.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !c.detail.empty()) ImGui::SetTooltip("%s", c.detail.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface_alt);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
            ImGui::BeginChild("check", {0, 0},
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
            icon(g, ImGui::GetTextLineHeight() * 0.95f, tint);
            ImGui::SameLine(0.0f, S(10));
            ImGui::BeginGroup();
            subheading(c.title.c_str());
            if (!c.detail.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextWrapped("%s", c.detail.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };
    for (const Check& c : report.checks)
        if (!c.passed && !is_note(c)) draw_check(c);
    if (notes) {
        spacer(2.0f);
        if (ghost_button(*show_notes ? "Hide the notes"
                                     : format("Show %d note%s", notes, notes == 1 ? "" : "s").c_str()))
            *show_notes = !*show_notes;
        if (*show_notes)
            for (const Check& c : report.checks)
                if (is_note(c)) draw_check(c);
    }
    if (passing) {
        spacer(2.0f);
        if (ghost_button(*show_all ? "Hide the passed checks"
                                   : format("Show the %d passed checks", passing).c_str()))
            *show_all = !*show_all;
        if (*show_all) {
            spacer(2.0f);
            for (const Check& c : report.checks)
                if (c.passed) draw_check(c);
        }
    }

    // --- detail ----------------------------------------------------------------------------
    spacer(10.0f);
    if (section("Mesh health", false)) {
        begin_form("health_form", 190.0f);
        key_value("Shells", "%zu", low_stats.shells);
        key_value("Boundary edges", "%zu", low_stats.boundary_edges);
        key_value("Non manifold edges", "%zu", low_stats.nonmanifold_edges);
        key_value("Worst triangle quality", "%.3f", low_stats.min_quality);
        key_value("Regions", "%zu", regions);
        key_value("Worst view", "%s %.4f", sil.worst_view.c_str(), sil.worst);
        end_form();
        spacer(6.0f);
    }
    if (bake.ok && section("Bake", false)) {
        key_value("Atlas", "%dx%d, %d page%s", bake.diffuse.width, bake.diffuse.height,
                  bake.atlas_count, bake.atlas_count == 1 ? "" : "s");
        key_value("Charts", "%d", bake.charts);
        key_value("Utilisation", "%.1f%%", bake.uv_utilisation * 100.0f);
        key_value("Worst uv stretch", "%.2f", bake.uv_max_stretch);
        key_value("Rays cast", "%zu", bake.rays_cast);
        spacer(6.0f);
    }
    if (!exported.files.empty() && section("Export", false)) {
        key_value("Files", "%zu, %s", exported.files.size(),
                  paths::format_bytes(exported.total_bytes).c_str());
        key_value("Strips", "%zu, %.2f triangles average", exported.strips.strip_count,
                  exported.strips.average_length);
        key_value("ACMR", "%.3f -> %.3f", exported.optimisation.acmr_before,
                  exported.optimisation.acmr_after);
        spacer(6.0f);
    }
}

// ---------------------------------------------------------------------------
// Director: everything it said, so a run can be argued with.
// ---------------------------------------------------------------------------
void tab_director(AppState& app)
{
    const Palette& p = palette();

    std::vector<LlmExchange> transcript;
    app.pipeline.with_results([&](const PipelineResults& r) { transcript = r.transcript; });

    if (!app.settings.use_llm && transcript.empty()) {
        callout(Icon::Info, p.info, "The director is off",
                "Runs use the knob panel exactly as it stands and nothing is sent anywhere.");
        spacer(8.0f);
        if (icon_label_button("Turn the director on", Icon::Sparkle, ButtonKind::Secondary))
            app.settings.use_llm = true;
        return;
    }

    if (draw_profile_advice(app, app.settings.profile)) app.settings.profile.clamp();
    if (!app.advice.empty() && !app.advice_dismissed) spacer(10.0f);

    if (transcript.empty() && !app.pipeline.running()) {
        empty_state("Nothing said yet",
                    "Every request and reply lands here, and a copy of each goes into the "
                    "reports folder so a run can be audited after the fact.",
                    Icon::Chat);
        return;
    }

    {
        const std::string right = format("%s - %zu call%s",
                                         llm_backend_name(app.settings.llm.kind),
                                         transcript.size(), transcript.size() == 1 ? "" : "s");
        section_label("Conversation", right.c_str());
    }

    const Fonts& f = fonts();
    for (size_t i = 0; i < transcript.size(); ++i) {
        const LlmExchange& e = transcript[i];
        ImGui::PushID(int(i));
        bool* show_reply  = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("reply"), false);
        bool* show_prompt = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("prompt"), false);

        if (card_begin("exchange")) {
            const ImVec4 tint = e.ok ? p.success : p.danger;
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            const float  th = ImGui::GetTextLineHeight();
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled({cp.x + S(9), cp.y + th * 0.5f}, S(9), col(tint, 0.16f));
            draw_icon(dl, e.ok ? Icon::Check : Icon::Cross, {cp.x + S(9), cp.y + th * 0.5f},
                      S(12), col(tint), S(2));
            ImGui::Dummy({S(18), th});
            ImGui::SameLine(0.0f, S(8));
            subheading(e.stage.c_str());
            ImGui::SameLine();
            ImGui::PushFont(f.mono, f.size_small);
            std::string meta = format("%s - %s>%s", format_duration(e.seconds).c_str(),
                                      tokens(e.prompt_tokens).c_str(),
                                      tokens(e.completion_tokens).c_str());
            if (!e.image_labels.empty()) meta += format(" - %zu img", e.image_labels.size());
            const float mw = ImGui::CalcTextSize(meta.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 std::max(4.0f, ImGui::GetContentRegionAvail().x - mw));
            ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
            ImGui::TextUnformatted(meta.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();

            if (!e.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
                ImGui::TextWrapped("%s", e.error.c_str());
                ImGui::PopStyleColor();
            }

            // The first few lines, so the conversation reads without clicking.
            std::string preview = e.reply;
            for (char& c : preview)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (preview.size() > 260) preview = preview.substr(0, 257) + "...";
            if (!*show_reply && !preview.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, mix(p.text, p.text_dim, 0.4f));
                ImGui::TextWrapped("%s", preview.c_str());
                ImGui::PopStyleColor();
            }
            if (*show_reply) {
                ImGui::PushFont(f.mono, f.size_mono);
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
                ImGui::TextWrapped("%s", e.reply.empty() ? "(empty)" : e.reply.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            if (*show_prompt) {
                spacer(4.0f);
                section_label("Prompt");
                ImGui::PushFont(f.mono, f.size_mono);
                ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
                ImGui::TextWrapped("%s", e.prompt.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            spacer(2.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), 0));
            if (ghost_button(*show_reply ? "Short reply" : "Full reply", {0, S(26)}))
                *show_reply = !*show_reply;
            ImGui::SameLine();
            if (ghost_button(*show_prompt ? "Hide prompt" : "Prompt", {0, S(26)}))
                *show_prompt = !*show_prompt;
            ImGui::PopStyleVar();
            card_end();
        }
        spacer(6.0f);
        ImGui::PopID();
    }

    // The question in flight, so a quiet minute does not look like a hang.
    const Stage stage = app.pipeline.stage();
    if (app.pipeline.running() && app.settings.use_llm &&
        (stage == Stage::NamingRegions || stage == Stage::AllocatingBudget ||
         stage == Stage::Reviewing)) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface_alt);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
        ImGui::BeginChild("live", {0, 0},
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float  th = ImGui::GetTextLineHeight();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddCircle({cp.x + S(9), cp.y + th * 0.5f}, S(8), col(p.overlay), 0, S(2.5f));
        draw_spinner(dl, {cp.x + S(9), cp.y + th * 0.5f}, S(8), col(p.accent), S(2.5f));
        ImGui::Dummy({S(18), th});
        ImGui::SameLine(0.0f, S(8));
        subheading(stage_name(stage));
        ImGui::SameLine();
        const char* waiting = "waiting for the model";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(4.0f, ImGui::GetContentRegionAvail().x -
                                                ImGui::CalcTextSize(waiting).x));
        faint("%s", waiting);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(80)) ImGui::SetScrollHereY(1.0f);
    }
}

} // namespace rd::ui
