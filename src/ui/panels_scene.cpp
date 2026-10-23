// The viewport - with its floating toolbars, the compare wipe and the welcome
// screen - and the Renders tab.

#include "ui/panels.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "ui/file_dialog.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace rd::ui {
namespace {

float S(float v) { return v * dpi(); }

const char* const kModeNames[] = {"Shaded", "Regions", "Silhouette",
                                  "Normals", "Curvature", "UV checker", "Clay"};
const char* const kSourceNames[] = {"Low poly", "Source", "Compare"};

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

ViewCamera current_camera(AppState& app)
{
    if (app.use_profile_camera) {
        std::vector<ViewCamera> cameras;
        app.pipeline.with_results([&](const PipelineResults& r) { cameras = r.cameras; });
        if (!cameras.empty()) {
            const int index = std::clamp(app.profile_camera_index, 0, int(cameras.size()) - 1);
            return cameras[size_t(index)];
        }
    }
    return app.camera.to_view_camera(app.scene_bounds);
}

// Renders `mesh` at `size` and returns the GL texture, 0 when there is nothing
// to show. The image is bottom up; callers flip v.
unsigned render_view(AppState& app, Renderer& renderer, const GpuMesh& mesh,
                     const ViewCamera& cam, const ImVec2& size)
{
    if (!mesh.valid() || !renderer.ready() || size.x < 16.0f || size.y < 16.0f) return 0;
    const Palette& p = palette();
    RenderOptions opts;
    opts.width   = std::max(16, int(size.x));
    opts.height  = std::max(16, int(size.y));
    opts.samples = 4;
    opts.mode    = app.mode;
    opts.wireframe_overlay = app.wireframe;
    opts.background = {p.viewport.x, p.viewport.y, p.viewport.z};
    opts.flip_y = false;
    return renderer.render_to_gl_texture(mesh, cam, opts);
}

// Orbit, pan and zoom on whatever item was submitted last.
void handle_orbit(AppState& app, const ViewCamera& cam)
{
    if (app.use_profile_camera) return;
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    if (active) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        // Right drag turns, middle drag pans, the way most DCC viewports do;
        // the left button is left free so a click never moves the camera.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // Drag left, the model turns left: the cursor takes the surface with
            // it, the way a hand on a turntable would. Increasing yaw walks the
            // camera toward +X, which sits on the left of the screen, so the
            // model sweeps right; a leftward drag therefore has to lower it.
            app.camera.yaw   += delta.x * 0.4f;
            app.camera.pitch  = clampf(app.camera.pitch + delta.y * 0.3f, -89.0f, 89.0f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
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

// A translucent rounded plate for the floating toolbars and chips.
void plate(const ImVec2& a, const ImVec2& b, float rounding = 0.0f)
{
    const Palette& p = palette();
    if (rounding <= 0.0f) rounding = S(10);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, col(p.surface, 0.9f), rounding);
    dl->AddRect(a, b, col(p.line), rounding);
}

void hud_chip(const char* text, const ImVec4* dot_color, const ImVec4& text_color)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.mono, f.size_small);
    const ImVec2 ts  = ImGui::CalcTextSize(text);
    const float  dw  = dot_color ? S(12) : 0.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz{ts.x + dw + S(18), ts.y + S(10)};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + sz.x, pos.y + sz.y}, col(palette().surface, 0.88f), S(7));
    if (dot_color)
        dl->AddCircleFilled({pos.x + S(12), pos.y + sz.y * 0.5f}, S(3), col(*dot_color));
    dl->AddText({pos.x + S(9) + dw, pos.y + S(5)}, col(text_color), text);
    ImGui::Dummy(sz);
    ImGui::PopFont();
}

void dashed_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float rounding, float dash,
                 float gap, float thickness)
{
    // Walk the rounded outline as a path and stroke every other stretch of it.
    dl->PathRect(a, b, rounding);
    ImVector<ImVec2> pts = dl->_Path;
    dl->PathClear();
    if (pts.Size < 2) return;
    pts.push_back(pts[0]);
    float carried = 0.0f;
    bool  on      = true;
    for (int i = 0; i + 1 < pts.Size; ++i) {
        ImVec2 p0 = pts[i];
        const ImVec2 p1 = pts[i + 1];
        float seg = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
        while (seg > 0.0f) {
            const float want = (on ? dash : gap) - carried;
            const float step = std::min(want, seg);
            const float t    = step / seg;
            const ImVec2 q{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t};
            if (on) dl->AddLine(p0, q, color, thickness);
            seg -= step;
            p0 = q;
            carried += step;
            if (carried >= (on ? dash : gap) - 0.01f) {
                carried = 0.0f;
                on      = !on;
            }
        }
    }
}

// --- nothing loaded ----------------------------------------------------------
void draw_welcome(AppState& app, const ImVec2& size)
{
    const Palette& p = palette();
    const Fonts&   f = fonts();
    ImDrawList*    dl = ImGui::GetWindowDrawList();

    const float w = std::min(S(620), size.x - S(64));
    const size_t recent = std::min<size_t>(app.recent_meshes.size(), 5);
    const float drop_h  = S(250);
    const float row_h   = S(48);
    const float total_h = drop_h + S(28) +
                          (recent ? S(24) + float(recent) * (row_h + S(6)) + S(22) : 0.0f) +
                          S(86);
    const float x0 = (size.x - w) * 0.5f;
    float       y  = std::max(S(24), (size.y - total_h) * 0.5f);

    // --- the drop zone ----------------------------------------------------------
    {
        ImGui::SetCursorPos({x0, y});
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b{a.x + w, a.y + drop_h};
        dl->AddRectFilled(a, b, col(p.surface), S(20));
        dashed_rect(dl, a, b, col(p.overlay), S(20), S(7), S(5), 1.5f);

        const ImVec2 c{a.x + w * 0.5f, a.y + S(68)};
        dl->AddCircleFilled(c, S(32), col(p.accent, 0.14f));
        draw_icon(dl, Icon::Upload, c, S(28), col(p.accent_hot));

        auto centred = [&](const char* text, float yy, ImFont* font, float fs, const ImVec4& tc) {
            ImGui::PushFont(font, fs);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddText({a.x + (w - ts.x) * 0.5f, yy}, col(tc), text);
            ImGui::PopFont();
        };
        centred("Drop a high-poly mesh to begin", a.y + S(116), f.medium, f.size_title + S(4),
                p.text);
        centred("OBJ, glTF, GLB or FBX - one asset, any scale or up axis", a.y + S(148),
                f.regular, f.size_body, p.text_dim);

        const char* kbd  = "Ctrl O";
        ImGui::PushFont(f.mono, f.size_small);
        const float kbd_w = ImGui::CalcTextSize(kbd).x + S(14);
        ImGui::PopFont();
        const float or_w  = ImGui::CalcTextSize("or press").x;
        const float btn_w = S(150);
        const float row_w = btn_w + S(16) + or_w + S(8) + kbd_w;
        ImGui::SetCursorScreenPos({a.x + (w - row_w) * 0.5f, a.y + S(186)});
        if (icon_label_button("Open mesh...", Icon::Open, ButtonKind::Primary, {btn_w, S(40)}))
            action_open_mesh(app);
        ImGui::SameLine(0.0f, S(16));
        const ImVec2 op = ImGui::GetCursorScreenPos();
        const float  ty = op.y + (S(40) - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText({op.x, ty}, col(p.text_faint), "or press");
        const ImVec2 kp{op.x + or_w + S(8), ty - S(3)};
        ImGui::PushFont(f.mono, f.size_small);
        dl->AddRectFilled(kp, {kp.x + kbd_w, kp.y + ImGui::GetTextLineHeight() + S(6)},
                          col(p.elevated), S(6));
        dl->AddText({kp.x + S(7), kp.y + S(3)}, col(p.text_dim), kbd);
        ImGui::PopFont();
        ImGui::Dummy({or_w + S(8) + kbd_w, S(40)});
        y += drop_h + S(28);
    }

    // --- recent ------------------------------------------------------------------
    if (recent) {
        ImGui::SetCursorPos({x0 + S(4), y});
        section_label("Recent");
        y += S(24);
        fs::path chosen;
        for (size_t i = 0; i < recent; ++i) {
            const fs::path& path = app.recent_meshes[i];
            ImGui::SetCursorPos({x0, y});
            ImGui::PushID(int(i));
            const ImVec2 a = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##recent", {w, row_h})) chosen = path;
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetTooltip("%s", path.string().c_str());
            dl->AddRectFilled(a, {a.x + w, a.y + row_h}, col(hov ? p.surface_alt : p.surface),
                              S(10));
            const ImVec2 ic{a.x + S(14) + S(15), a.y + row_h * 0.5f};
            dl->AddRectFilled({ic.x - S(15), ic.y - S(15)}, {ic.x + S(15), ic.y + S(15)},
                              col(p.elevated), S(8));
            draw_icon(dl, Icon::Cube, ic, S(16), col(p.text_dim));

            const float ty = a.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
            float       x  = a.x + S(14) + S(30) + S(12);
            const std::string name = path.filename().string();
            ImGui::PushFont(f.medium, f.size_body);
            dl->AddText({x, ty}, col(p.text), name.c_str());
            x += ImGui::CalcTextSize(name.c_str()).x + S(10);
            ImGui::PopFont();
            const std::string where = path.parent_path().filename().string();
            dl->AddText({x, ty}, col(p.text_faint), where.c_str());

            std::string ext = to_lower(path.extension().string());
            if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
            ImGui::PushFont(f.mono, f.size_small);
            const ImVec2 es = ImGui::CalcTextSize(ext.c_str());
            dl->AddText({a.x + w - S(14) - es.x, a.y + (row_h - es.y) * 0.5f},
                        col(p.text_faint), ext.c_str());
            ImGui::PopFont();
            ImGui::PopID();
            y += row_h + S(6);
        }
        y += S(22);
        if (!chosen.empty()) app.open_mesh(chosen);
    }

    // --- three steps -------------------------------------------------------------
    {
        static const char* const kSteps[][2] = {
            {"1 - Load", "The source shows up at once, framed and lit."},
            {"2 - Run", "The director splits the budget and judges each pass."},
            {"3 - Tune", "Overrule any number and rebuild in seconds."},
        };
        const float gap = S(12);
        const float cw  = (w - gap * 2.0f) / 3.0f;
        const float ch  = S(80);
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPos({x0 + float(i) * (cw + gap), y});
            const ImVec2 a = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(a, {a.x + cw, a.y + ch}, col(p.surface), S(12));
            ImGui::PushFont(f.medium, f.size_small);
            dl->AddText({a.x + S(16), a.y + S(14)}, col(p.accent_hot), kSteps[i][0]);
            ImGui::PopFont();
            ImGui::PushFont(f.regular, f.size_small);
            dl->AddText(nullptr, 0.0f, {a.x + S(16), a.y + S(36)}, col(p.text_dim), kSteps[i][1],
                        nullptr, cw - S(32));
            ImGui::PopFont();
            ImGui::Dummy({cw, ch});
        }
    }
}

// The run, drawn where the eye already is.
void draw_run_card(AppState& app, const ImVec2& origin, const ImVec2& size)
{
    const Palette& p = palette();
    const Fonts&   f = fonts();
    const float    w = std::min(S(460), size.x - S(40));
    const float    h = S(92);
    const ImVec2   a{origin.x + (size.x - w) * 0.5f, origin.y + size.y - h - S(20)};
    const ImVec2   b{a.x + w, a.y + h};
    ImDrawList*    dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({a.x, a.y + S(6)}, {b.x, b.y + S(10)}, IM_COL32(0, 0, 0, 70), S(16));
    dl->AddRectFilled(a, b, col(p.surface_alt, 0.96f), S(14));
    dl->AddRect(a, b, col(p.overlay), S(14));

    const float  pad = S(16);
    const float  th  = ImGui::GetTextLineHeight();
    const ImVec2 sc{a.x + pad + S(9), a.y + pad + th * 0.5f};
    dl->AddCircle(sc, S(8), col(p.overlay), 0, S(2.5f));
    draw_spinner(dl, sc, S(8), col(p.accent), S(2.5f));

    ImGui::PushFont(f.medium, f.size_body);
    const char* stage = stage_name(app.pipeline.stage());
    dl->AddText({a.x + pad + S(28), a.y + pad}, col(p.text), stage);
    float x = a.x + pad + S(28) + ImGui::CalcTextSize(stage).x + S(10);
    ImGui::PopFont();

    const int iteration = app.pipeline.iteration();
    if (iteration > 0) {
        const int total = app.settings.max_iterations > 0 ? app.settings.max_iterations
                                                          : app.settings.profile.max_iterations;
        const std::string pass = total > 0 ? format("pass %d of %d", iteration, total)
                                           : format("pass %d", iteration);
        ImGui::PushFont(f.medium, f.size_small);
        const ImVec2 ps = ImGui::CalcTextSize(pass.c_str());
        dl->AddRectFilled({x, a.y + pad - S(1)}, {x + ps.x + S(16), a.y + pad + th + S(1)},
                          col(p.accent, 0.16f), th);
        dl->AddText({x + S(8), a.y + pad + (th - ps.y) * 0.5f}, col(p.accent_hot), pass.c_str());
        ImGui::PopFont();
    }

    const float overall = overall_progress(app);
    ImGui::PushFont(f.mono, f.size_small);
    const std::string pct = format("%.0f%%", overall * 100.0f);
    const ImVec2 pts = ImGui::CalcTextSize(pct.c_str());
    dl->AddText({b.x - pad - pts.x, a.y + pad + (th - pts.y) * 0.5f}, col(p.text_dim),
                pct.c_str());
    ImGui::PopFont();

    const float by = a.y + pad + th + S(12);
    dl->AddRectFilled({a.x + pad, by}, {b.x - pad, by + S(6)}, col(p.overlay), S(3));
    const float fill = (w - pad * 2.0f) * std::clamp(overall, 0.0f, 1.0f);
    if (fill > 1.0f)
        dl->AddRectFilled({a.x + pad, by}, {a.x + pad + std::max(fill, S(6)), by + S(6)},
                          col(p.accent), S(3));

    const std::string msg = app.pipeline.message();
    if (!msg.empty()) {
        ImGui::PushFont(f.regular, f.size_small);
        dl->PushClipRect({a.x + pad, by}, {b.x - pad, b.y}, true);
        dl->AddText({a.x + pad, by + S(14)}, col(p.text_faint), msg.c_str());
        dl->PopClipRect();
        ImGui::PopFont();
    }
}

} // namespace

// ---------------------------------------------------------------------------
void draw_viewport(AppState& app, const ImVec2& pos, const ImVec2& size)
{
    const Palette& p = palette();
    const bool open = begin_surface("##viewport", pos, size, p.viewport, ImVec2(0, 0),
                                    ImGuiWindowFlags_NoScrollbar);
    if (!open) {
        end_surface();
        return;
    }

    if (app.mesh_path.empty()) {
        draw_welcome(app, size);
        end_surface();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ViewCamera cam = current_camera(app);

    // --- the picture -----------------------------------------------------------
    // Compare is a wipe: both meshes rendered full size from one camera, the
    // source shown left of the split and the low poly right of it, so the eye
    // compares the same pixels instead of two pictures side by side.
    const bool compare = app.source == ViewSource::SideBySide;
    const bool show_low = app.source == ViewSource::LowPoly;
    const GpuMesh& main_mesh = show_low || compare ? app.gpu_low : app.gpu_high;
    const bool     main_ok   = show_low || compare ? app.gpu_low_ok : app.gpu_high_ok;

    unsigned tex_main = main_ok ? render_view(app, app.renderer, main_mesh, cam, size) : 0;
    unsigned tex_high = compare && app.gpu_high_ok
                            ? render_view(app, app.renderer_alt, app.gpu_high, cam, size) : 0;

    const ImVec2 a = pos;
    const ImVec2 b{pos.x + size.x, pos.y + size.y};
    if (tex_main) {
        if (compare && tex_high) {
            const float sx = a.x + size.x * app.compare_split;
            const float u  = app.compare_split;
            dl->AddImage(ImTextureRef(ImTextureID(tex_high)), a, {sx, b.y}, {0, 1}, {u, 0});
            dl->AddImage(ImTextureRef(ImTextureID(tex_main)), {sx, a.y}, b, {u, 1}, {1, 0});
        } else {
            dl->AddImage(ImTextureRef(ImTextureID(tex_main)), a, b, {0, 1}, {1, 0});
        }
    } else {
        // Nothing to draw yet: the preview is loading, or the low poly has not
        // been built.
        const bool loading = !app.gpu_high_ok && !app.pipeline.running();
        const char* text = !app.renderer.ready() ? "GPU renderer unavailable"
                         : (show_low || compare) && !app.gpu_low_ok
                               ? "No low poly yet - press Run"
                               : (loading || app.pipeline.running() ? "Loading the preview"
                                                                    : "Nothing to show");
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 c{a.x + size.x * 0.5f, a.y + size.y * 0.5f};
        if (!app.gpu_high_ok && app.renderer.ready())
            draw_spinner(dl, {c.x, c.y - S(28)}, S(10), col(p.accent), S(2.5f));
        dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, col(p.text_faint), text);
    }

    // --- input -----------------------------------------------------------------
    // Everything drawn after this button takes the mouse first, which is what
    // lets the toolbars float over the picture.
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##orbit", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    handle_orbit(app, cam);

    if (compare && tex_main && tex_high) {
        const float  sx = a.x + size.x * app.compare_split;
        const ImVec2 gc{sx, a.y + size.y * 0.5f};
        dl->AddLine({sx, a.y}, {sx, b.y}, col(p.text, 0.9f), S(2));
        ImGui::SetCursorScreenPos({sx - S(18), gc.y - S(18)});
        ImGui::InvisibleButton("##wipe", {S(36), S(36)});
        if (ImGui::IsItemActive())
            app.compare_split = std::clamp((ImGui::GetIO().MousePos.x - a.x) / size.x, 0.02f, 0.98f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        dl->AddCircleFilled({gc.x, gc.y + S(3)}, S(18), IM_COL32(0, 0, 0, 90));
        dl->AddCircleFilled(gc, S(18), col(p.text));
        draw_icon(dl, Icon::ChevronRight, {gc.x + S(5), gc.y}, S(13), col(p.surface), S(2));
        dl->AddLine({gc.x - S(3), gc.y - S(4)}, {gc.x - S(7), gc.y}, col(p.surface), S(2));
        dl->AddLine({gc.x - S(7), gc.y}, {gc.x - S(3), gc.y + S(4)}, col(p.surface), S(2));
    }

    // --- top left: what is on screen -------------------------------------------
    const float m = S(16);
    {
        ImGui::SetCursorScreenPos({a.x + m, a.y + m});
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        float sw = S(6);
        for (const char* n : kSourceNames) sw += ImGui::CalcTextSize(n).x + S(28);
        plate({sp.x - 1, sp.y - 1}, {sp.x + sw + 1, sp.y + ImGui::GetFrameHeight() + S(6) + 1});
        int source = int(app.source);
        // Choosing by hand ends the automatic switching for good: after this
        // the viewport shows what was asked for and nothing else.
        if (segmented("source", &source, kSourceNames, 3, -1.0f)) {
            app.source           = ViewSource(source);
            app.auto_view_source = false;
        }
    }
    if (compare) {
        const Fonts& f = fonts();
        ImGui::PushFont(f.regular, f.size_small);
        size_t ht = 0, lt = 0;
        app.pipeline.with_results([&](const PipelineResults& r) {
            ht = r.highpoly.triangle_count();
            lt = r.lowpoly.triangle_count();
        });
        const std::string l = "Source - " + thousands(ht) + " tris";
        const std::string r = "Low poly - " + thousands(lt) + " tris";
        ImGui::PopFont();
        const float y = a.y + m + ImGui::GetFrameHeight() + S(18);
        ImGui::SetCursorScreenPos({a.x + m, y});
        hud_chip(l.c_str(), nullptr, p.text_dim);
        // hud_chip sets in the mono face, so measure in it.
        ImGui::PushFont(f.mono, f.size_small);
        const float rw = ImGui::CalcTextSize(r.c_str()).x + S(18);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({b.x - m - rw, y});
        hud_chip(r.c_str(), nullptr, p.text_dim);
    }

    // --- top right: how it is drawn ----------------------------------------------
    {
        std::vector<ViewCamera> cameras;
        app.pipeline.with_results([&](const PipelineResults& r) { cameras = r.cameras; });
        const char* mode_name = kModeNames[std::clamp(int(app.mode), 0, 6)];
        const std::string cam_name =
            app.use_profile_camera && !cameras.empty()
                ? cameras[size_t(std::clamp(app.profile_camera_index, 0,
                                            int(cameras.size()) - 1))].name
                : std::string("Free orbit");

        const float bh   = ImGui::GetFrameHeight();
        const float ib   = bh;
        const float mode_w = ImGui::CalcTextSize(mode_name).x + S(36);
        const float cam_w  = ImGui::CalcTextSize(cam_name.c_str()).x + S(46);
        const float total  = S(3) + mode_w + S(9) + ib * 2 + S(2) + cam_w + S(2) + ib + S(3);
        const ImVec2 tp{b.x - m - total, a.y + m};
        plate({tp.x, tp.y}, {tp.x + total, tp.y + bh + S(6)});

        ImGui::SetCursorScreenPos({tp.x + S(3), tp.y + S(3)});
        if (ghost_button(format("%s##mode", mode_name).c_str(), {mode_w, bh}))
            ImGui::OpenPopup("mode_menu");
        {
            const ImVec2 mx = ImGui::GetItemRectMax();
            draw_icon(dl, Icon::ChevronDown, {mx.x - S(12), (ImGui::GetItemRectMin().y + mx.y) * 0.5f},
                      S(12), col(p.text_faint));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
        if (ImGui::BeginPopup("mode_menu")) {
            for (int i = 0; i < 7; ++i) {
                const std::string key = i < 4 ? format("%d", i + 1) : std::string();
                if (ImGui::MenuItem(kModeNames[i], key.empty() ? nullptr : key.c_str(),
                                    int(app.mode) == i)) {
                    app.mode        = RenderMode(i);
                    app.gpu_version = 0;    // region colours may need re-uploading
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, S(4));
        const ImVec2 sep = ImGui::GetCursorScreenPos();
        dl->AddLine({sep.x, sep.y + S(7)}, {sep.x, sep.y + bh - S(7)}, col(p.overlay));
        ImGui::Dummy({1, bh});
        ImGui::SameLine(0.0f, S(4));
        if (icon_button("##wire", Icon::Wireframe, "Wireframe (W)", app.wireframe, ib))
            app.wireframe = !app.wireframe;
        ImGui::SameLine(0.0f, S(2));
        if (icon_button("##spin", Icon::Orbit, "Turntable", app.auto_rotate, ib))
            app.auto_rotate = !app.auto_rotate;
        ImGui::SameLine(0.0f, S(2));
        if (ghost_button(format("%s##cam", cam_name.c_str()).c_str(), {cam_w, bh}))
            ImGui::OpenPopup("camera_menu");
        {
            const ImVec2 mn = ImGui::GetItemRectMin();
            draw_icon(dl, Icon::Camera, {mn.x + S(16), mn.y + bh * 0.5f}, S(15),
                      col(app.use_profile_camera ? p.accent_hot : p.text_dim));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
        if (ImGui::BeginPopup("camera_menu")) {
            if (ImGui::MenuItem("Free orbit", nullptr, !app.use_profile_camera))
                app.use_profile_camera = false;
            if (!cameras.empty()) {
                ImGui::Separator();
                faint("The director's cameras");
                for (size_t i = 0; i < cameras.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (ImGui::MenuItem(cameras[i].name.c_str(), nullptr,
                                        app.use_profile_camera &&
                                            app.profile_camera_index == int(i))) {
                        app.use_profile_camera   = true;
                        app.profile_camera_index = int(i);
                    }
                    ImGui::PopID();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::MenuItem("The profile's cameras appear after a run");
                ImGui::EndDisabled();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, S(2));
        if (icon_button("##frame", Icon::Frame, "Frame the mesh (F)", false, ib)) {
            app.use_profile_camera = false;
            app.camera.frame(app.scene_bounds);
        }
    }

    // --- bottom left: numbers ------------------------------------------------------
    {
        size_t high_tris = 0, high_verts = 0, low_tris = 0, low_verts = 0;
        float  outline = 0.0f;
        int    kept = 0;
        size_t passes = 0;
        bool   sym = false;
        const char* sym_axis = "";
        app.pipeline.with_results([&](const PipelineResults& r) {
            high_tris  = r.highpoly.triangle_count();
            high_verts = r.highpoly.vertex_count();
            low_tris   = r.lowpoly.triangle_count();
            low_verts  = r.lowpoly.vertex_count();
            outline    = r.silhouette.outline_px;
            kept       = r.kept_iteration;
            passes     = r.iterations.size();
            if (r.analysis.valid() && r.analysis.symmetry.score > 0.8f) {
                sym      = true;
                sym_axis = r.analysis.symmetry.axis_name();
            }
        });

        const Fonts& f = fonts();
        ImGui::PushFont(f.mono, f.size_small);
        const float chip_h = ImGui::GetTextLineHeight() + S(10);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({a.x + m, b.y - m - chip_h});
        const int max_tris = app.settings.profile.max_triangles;
        if (show_low && low_tris) {
            const ImVec4 dot = int(low_tris) <= max_tris ? p.success : p.danger;
            hud_chip(format("%s / %s tris", thousands(low_tris).c_str(),
                            thousands(size_t(max_tris)).c_str()).c_str(), &dot, p.text);
            ImGui::SameLine(0.0f, S(6));
            hud_chip(format("%s verts", thousands(low_verts).c_str()).c_str(), nullptr, p.text_dim);
            if (outline > 0.0f) {
                ImGui::SameLine(0.0f, S(6));
                hud_chip(format("%.2f px outline", outline).c_str(), nullptr, p.text_dim);
            }
            if (passes > 1 && kept > 0) {
                ImGui::SameLine(0.0f, S(6));
                hud_chip(format("pass %d kept", kept).c_str(), nullptr, p.text_dim);
            }
        } else if (app.source == ViewSource::HighPoly && high_tris) {
            hud_chip(format("%s tris", thousands(high_tris).c_str()).c_str(), nullptr, p.text_dim);
            ImGui::SameLine(0.0f, S(6));
            hud_chip(format("%s verts", thousands(high_verts).c_str()).c_str(), nullptr,
                     p.text_dim);
            if (sym) {
                ImGui::SameLine(0.0f, S(6));
                hud_chip(format("symmetry %s", sym_axis).c_str(), nullptr, p.text_dim);
            }
        }
    }

    // --- the run ---------------------------------------------------------------------
    if (app.pipeline.running()) draw_run_card(app, a, size);

    end_surface();
}

// ---------------------------------------------------------------------------
// Renders: the exact images the director was shown.
// ---------------------------------------------------------------------------
void tab_renders(AppState& app)
{
    struct Group {
        std::string title;
        std::vector<std::pair<std::string, fs::path>> images;
    };
    std::vector<Group> groups;

    app.pipeline.with_results([&](const PipelineResults& r) {
        auto collect = [&](std::string title, const ViewSet& set) {
            if (set.entries.empty()) return;
            Group g;
            g.title = std::move(title);
            for (const ViewSet::Entry& e : set.entries) g.images.emplace_back(e.name, e.file);
            groups.push_back(std::move(g));
        };
        collect("Source", r.reference_views);
        collect("Regions", r.region_views);
        collect(r.kept_iteration > 0 ? format("Pass %d - kept", r.kept_iteration)
                                     : std::string("Low poly"),
                r.candidate_views);

        if (r.bake.ok && !r.bake.diffuse.empty()) {
            Group g;
            g.title = "Baked texture";
            g.images.emplace_back("diffuse",
                                  paths::iteration_dir(r.kept_iteration > 0 ? r.kept_iteration
                                                       : (r.iterations.empty()
                                                              ? 1 : r.iterations.back().index)) /
                                      "diffuse.png");
            groups.push_back(std::move(g));
        }
    });

    if (groups.empty()) {
        empty_state("No renders yet",
                    "The reference shots, the region overlay and each pass of the low poly "
                    "collect here once a run starts - the exact images the director is shown.",
                    Icon::Image);
        return;
    }

    const Palette& p = palette();
    hint("The exact images the director was shown. Click one to open it.");
    spacer(4.0f);
    if (ghost_button("Open the folder")) reveal_in_file_manager(paths::renders_dir());
    ImGui::SameLine();
    if (ghost_button("Reload")) app.images.clear();
    spacer(6.0f);

    const float gap  = S(6);
    const int   cols = 3;
    for (const Group& g : groups) {
        section_label(g.title.c_str(), format("%zu", g.images.size()).c_str());
        const float cw = (ImGui::GetContentRegionAvail().x - gap * float(cols - 1)) / float(cols);
        int i = 0;
        for (const auto& [name, path] : g.images) {
            if (i % cols) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(path.string().c_str());
            int w = 0, h = 0;
            const unsigned tex = app.images.get(path, &w, &h);
            const ImVec2 pos   = ImGui::GetCursorScreenPos();
            const float  ch    = cw * 0.8f;
            if (ImGui::InvisibleButton("##thumb", {cw, ch})) app.lightbox = path;
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, {pos.x + cw, pos.y + ch}, col(p.viewport), S(8));
            if (tex && w > 0 && h > 0) {
                const float  s  = std::min(cw / float(w), ch / float(h));
                const ImVec2 is{float(w) * s, float(h) * s};
                const ImVec2 ip{pos.x + (cw - is.x) * 0.5f, pos.y + (ch - is.y) * 0.5f};
                dl->AddImageRounded(ImTextureRef(ImTextureID(tex)), ip,
                                    {ip.x + is.x, ip.y + is.y}, {0, 0}, {1, 1},
                                    IM_COL32_WHITE, S(8));
            }
            if (hov) {
                dl->AddRect(pos, {pos.x + cw, pos.y + ch}, col(p.accent), S(8), 1.5f);
                ImGui::SetTooltip("%s", name.c_str());
            }
            ImGui::PopID();
            ++i;
        }
        spacer(10.0f);
    }
}

} // namespace rd::ui
