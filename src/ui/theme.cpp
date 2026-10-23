#include "ui/theme.h"

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace rd::ui {
namespace {

Palette g_palette;
Metrics g_metrics;
Fonts   g_fonts;
float   g_dpi = 1.0f;

// Faces worth trying, best first. Nothing is downloaded; if none of these are
// installed the built-in bitmap font takes over.
const char* const kRegularCandidates[] = {
#if defined(RD_PLATFORM_WINDOWS)
    "C:/Windows/Fonts/SegoeUIVF.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    "C:/Windows/Fonts/arial.ttf",
#elif defined(RD_PLATFORM_MACOS)
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
};

const char* const kMediumCandidates[] = {
#if defined(RD_PLATFORM_WINDOWS)
    "C:/Windows/Fonts/seguisb.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "C:/Windows/Fonts/tahomabd.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
#elif defined(RD_PLATFORM_MACOS)
    "/System/Library/Fonts/SFNS.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
#endif
};

const char* const kMonoCandidates[] = {
#if defined(RD_PLATFORM_WINDOWS)
    "C:/Windows/Fonts/CascadiaMono.ttf",
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/cour.ttf",
#elif defined(RD_PLATFORM_MACOS)
    "/System/Library/Fonts/Menlo.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
#endif
};

template <size_t N>
ImFont* try_load(const char* const (&candidates)[N], float size, ImFontConfig& cfg)
{
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    for (const char* path : candidates) {
        if (!std::filesystem::exists(path, ec)) continue;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, &cfg)) {
            RD_DEBUG("font: %s at %.0fpx", path, size);
            return font;
        }
    }
    return nullptr;
}

} // namespace

const Palette& palette() { return g_palette; }
const Metrics& metrics() { return g_metrics; }
const Fonts&   fonts()   { return g_fonts; }
float          dpi()     { return g_dpi; }

ImU32 col(const ImVec4& c, float alpha_scale)
{
    ImVec4 v = c;
    v.w *= alpha_scale;
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

ImVec4 with_alpha(const ImVec4& c, float a) { return {c.x, c.y, c.z, a}; }

void load_fonts(float dpi_scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    const float scale = std::clamp(dpi_scale, 0.5f, 4.0f);
    g_fonts.size_body  = std::round(15.0f * scale);
    g_fonts.size_small = std::round(13.0f * scale);
    g_fonts.size_title = std::round(18.0f * scale);
    g_fonts.size_display = std::round(28.0f * scale);
    g_fonts.size_mono  = std::round(13.0f * scale);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = false;

    g_fonts.regular = try_load(kRegularCandidates, g_fonts.size_body, cfg);
    if (!g_fonts.regular) {
        ImFontConfig fallback;
        fallback.SizePixels = g_fonts.size_body;
        g_fonts.regular = io.Fonts->AddFontDefault(&fallback);
        RD_WARN("no system UI font found; falling back to the built-in bitmap font");
    }

    g_fonts.medium = try_load(kMediumCandidates, g_fonts.size_body, cfg);
    if (!g_fonts.medium) g_fonts.medium = g_fonts.regular;

    g_fonts.mono = try_load(kMonoCandidates, g_fonts.size_mono, cfg);
    if (!g_fonts.mono) g_fonts.mono = g_fonts.regular;

    io.FontDefault = g_fonts.regular;
}

void apply_theme(float dpi_scale)
{
    const Palette& p = g_palette;
    ImGuiStyle&    s = ImGui::GetStyle();
    s = ImGuiStyle();
    g_dpi = std::clamp(dpi_scale, 0.5f, 4.0f);

    const Metrics& m = g_metrics;
    s.WindowRounding    = m.rounding_window;
    s.ChildRounding     = m.rounding_card;
    s.FrameRounding     = m.rounding_frame;
    s.PopupRounding     = m.rounding_frame + 2.0f;
    s.ScrollbarRounding = m.rounding_grab;
    s.GrabRounding      = m.rounding_grab;
    s.TabRounding       = m.rounding_tab;

    s.WindowPadding     = {m.padding_window_x, m.padding_window_y};
    s.FramePadding      = {m.padding_frame_x, m.padding_frame_y};
    s.CellPadding       = {8.0f, 7.0f};
    s.ItemSpacing       = {m.spacing_x, m.spacing_y};
    s.ItemInnerSpacing  = {8.0f, 6.0f};
    s.IndentSpacing     = m.indent;
    s.ScrollbarSize     = m.scrollbar;
    s.GrabMinSize       = 10.0f;

    // Tone separates surfaces; outlines are kept for the few places where two
    // surfaces of the same tone meet - popups over a panel, mainly.
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding    = {14.0f, 4.0f};

    s.WindowTitleAlign  = {0.0f, 0.5f};
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ColorButtonPosition      = ImGuiDir_Right;
    s.SelectableTextAlign      = {0.0f, 0.5f};
    s.DockingSeparatorSize     = 1.0f;

    s.AntiAliasedLines       = true;
    s.AntiAliasedLinesUseTex = true;
    s.AntiAliasedFill        = true;
    s.CurveTessellationTol   = 0.8f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = p.text;
    c[ImGuiCol_TextDisabled]          = p.text_faint;
    c[ImGuiCol_WindowBg]              = p.surface;
    c[ImGuiCol_ChildBg]               = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg]               = p.elevated;
    c[ImGuiCol_Border]                = p.line;
    c[ImGuiCol_BorderShadow]          = {0, 0, 0, 0};

    c[ImGuiCol_FrameBg]               = p.surface_alt;
    c[ImGuiCol_FrameBgHovered]        = p.elevated;
    c[ImGuiCol_FrameBgActive]         = p.overlay;

    c[ImGuiCol_TitleBg]               = p.surface;
    c[ImGuiCol_TitleBgActive]         = p.surface;
    c[ImGuiCol_TitleBgCollapsed]      = p.surface;
    c[ImGuiCol_MenuBarBg]             = p.surface;

    c[ImGuiCol_ScrollbarBg]           = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab]         = with_alpha(p.overlay, 0.8f);
    c[ImGuiCol_ScrollbarGrabHovered]  = mix(p.overlay, p.text_faint, 0.4f);
    c[ImGuiCol_ScrollbarGrabActive]   = p.text_faint;

    c[ImGuiCol_CheckMark]             = p.accent;
    c[ImGuiCol_SliderGrab]            = p.accent;
    c[ImGuiCol_SliderGrabActive]      = p.accent_hot;

    c[ImGuiCol_Button]                = p.elevated;
    c[ImGuiCol_ButtonHovered]         = p.overlay;
    c[ImGuiCol_ButtonActive]          = mix(p.overlay, p.accent, 0.35f);

    c[ImGuiCol_Header]                = with_alpha(p.accent, 0.16f);
    c[ImGuiCol_HeaderHovered]         = with_alpha(p.text, 0.06f);
    c[ImGuiCol_HeaderActive]          = with_alpha(p.accent, 0.24f);

    c[ImGuiCol_Separator]             = p.line;
    c[ImGuiCol_SeparatorHovered]      = p.accent;
    c[ImGuiCol_SeparatorActive]       = p.accent_hot;

    c[ImGuiCol_ResizeGrip]            = {0, 0, 0, 0};
    c[ImGuiCol_ResizeGripHovered]     = with_alpha(p.accent, 0.35f);
    c[ImGuiCol_ResizeGripActive]      = with_alpha(p.accent, 0.60f);

    c[ImGuiCol_Tab]                   = {0, 0, 0, 0};
    c[ImGuiCol_TabHovered]            = p.elevated;
    c[ImGuiCol_TabSelected]           = p.elevated;
    c[ImGuiCol_TabSelectedOverline]   = {0, 0, 0, 0};
    c[ImGuiCol_TabDimmed]             = {0, 0, 0, 0};
    c[ImGuiCol_TabDimmedSelected]     = p.surface_alt;
    c[ImGuiCol_TabDimmedSelectedOverline] = {0, 0, 0, 0};

    c[ImGuiCol_DockingPreview]        = with_alpha(p.accent, 0.35f);
    c[ImGuiCol_DockingEmptyBg]        = p.canvas;

    c[ImGuiCol_PlotLines]             = p.accent;
    c[ImGuiCol_PlotLinesHovered]      = p.accent_hot;
    c[ImGuiCol_PlotHistogram]         = p.accent;
    c[ImGuiCol_PlotHistogramHovered]  = p.accent_hot;

    c[ImGuiCol_TableHeaderBg]         = {0, 0, 0, 0};
    c[ImGuiCol_TableBorderStrong]     = p.line;
    c[ImGuiCol_TableBorderLight]      = with_alpha(p.line, 0.6f);
    c[ImGuiCol_TableRowBg]            = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt]         = with_alpha(p.text, 0.02f);

    c[ImGuiCol_TextSelectedBg]        = with_alpha(p.accent, 0.35f);
    c[ImGuiCol_NavCursor]             = with_alpha(p.accent, 0.8f);
    c[ImGuiCol_DragDropTarget]        = p.accent_hot;
    c[ImGuiCol_ModalWindowDimBg]      = {0.01f, 0.01f, 0.02f, 0.66f};

    if (g_dpi != 1.0f) s.ScaleAllSizes(g_dpi);
}

} // namespace rd::ui
