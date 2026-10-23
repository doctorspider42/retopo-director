#pragma once

// Visual language for the whole application: one palette, one set of metrics,
// one place to change them. Panels never hard code a colour.
//
// The look is a dark tool, not a dashboard: surfaces are separated by tone
// rather than by outlines, the accent is spent only on the one thing that can
// be done next, and the viewport is the brightest-feeling thing on screen
// because it is the only thing anybody came to look at.

#include <imgui.h>

#include <string>

namespace rd::ui {

struct Palette {
    // Surfaces, darkest to lightest. Each step is a tone, not a border.
    ImVec4 canvas     {0.043f, 0.047f, 0.059f, 1.00f};
    ImVec4 viewport   {0.055f, 0.063f, 0.078f, 1.00f};
    ImVec4 surface    {0.071f, 0.078f, 0.094f, 1.00f};
    ImVec4 surface_alt{0.098f, 0.106f, 0.129f, 1.00f};
    ImVec4 elevated   {0.129f, 0.141f, 0.169f, 1.00f};
    ImVec4 overlay    {0.169f, 0.184f, 0.220f, 1.00f};
    ImVec4 line       {0.145f, 0.157f, 0.188f, 1.00f};

    // Type.
    ImVec4 text       {0.925f, 0.933f, 0.953f, 1.00f};
    ImVec4 text_dim   {0.604f, 0.631f, 0.682f, 1.00f};
    ImVec4 text_faint {0.400f, 0.424f, 0.478f, 1.00f};

    // Accent and semantics.
    ImVec4 accent     {0.443f, 0.494f, 1.000f, 1.00f};
    ImVec4 accent_hot {0.557f, 0.600f, 1.000f, 1.00f};
    ImVec4 accent_dim {0.443f, 0.494f, 1.000f, 0.18f};
    ImVec4 success    {0.235f, 0.800f, 0.529f, 1.00f};
    ImVec4 warning    {0.961f, 0.718f, 0.290f, 1.00f};
    ImVec4 danger     {0.957f, 0.373f, 0.388f, 1.00f};
    ImVec4 info       {0.333f, 0.737f, 0.961f, 1.00f};
};

struct Metrics {
    float rounding_window = 12.0f;
    float rounding_frame  = 8.0f;
    float rounding_card   = 12.0f;
    float rounding_tab    = 8.0f;
    float rounding_grab   = 8.0f;
    float padding_window_x = 18.0f;
    float padding_window_y = 16.0f;
    float padding_frame_x  = 10.0f;
    float padding_frame_y  = 6.0f;
    float spacing_x        = 8.0f;
    float spacing_y        = 8.0f;
    float scrollbar        = 10.0f;
    float indent           = 16.0f;

    // The fixed frame around the viewport.
    float topbar_height    = 56.0f;
    float statusbar_height = 32.0f;
    float inspector_width  = 400.0f;
    float log_height       = 260.0f;
};

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium  = nullptr;
    ImFont* mono    = nullptr;
    float   size_small   = 13.0f;
    float   size_body    = 15.0f;
    float   size_title   = 18.0f;
    float   size_display = 28.0f;
    float   size_mono    = 13.0f;
};

const Palette& palette();
const Metrics& metrics();
const Fonts&   fonts();
// The content scale the theme was applied at. Hand placed geometry - the
// shell, the icons, the custom widgets - multiplies its pixel sizes by this.
float          dpi();

// Applies colours and metrics to the current ImGui style.
void apply_theme(float dpi_scale = 1.0f);

// Loads the UI fonts. Falls back to the built-in font when no system face is
// found, which still looks acceptable, just tighter.
void load_fonts(float dpi_scale = 1.0f);

// Colour helpers.
ImU32  col(const ImVec4& c, float alpha_scale = 1.0f);
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);
ImVec4 with_alpha(const ImVec4& c, float a);

} // namespace rd::ui
