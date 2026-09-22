#pragma once

// Visual language for the whole application: one palette, one set of metrics,
// one place to change them. Panels never hard code a colour.

#include <imgui.h>

#include <string>

namespace rd::ui {

struct Palette {
    // Surfaces, darkest to lightest.
    ImVec4 canvas     {0.055f, 0.063f, 0.078f, 1.00f};
    ImVec4 surface    {0.086f, 0.098f, 0.118f, 1.00f};
    ImVec4 surface_alt{0.114f, 0.129f, 0.153f, 1.00f};
    ImVec4 elevated   {0.149f, 0.169f, 0.200f, 1.00f};
    ImVec4 overlay    {0.192f, 0.216f, 0.255f, 1.00f};
    ImVec4 line       {0.192f, 0.216f, 0.259f, 1.00f};

    // Type.
    ImVec4 text       {0.898f, 0.914f, 0.941f, 1.00f};
    ImVec4 text_dim   {0.592f, 0.631f, 0.694f, 1.00f};
    ImVec4 text_faint {0.412f, 0.447f, 0.510f, 1.00f};

    // Accent and semantics.
    ImVec4 accent     {0.357f, 0.553f, 0.937f, 1.00f};
    ImVec4 accent_hot {0.478f, 0.639f, 0.961f, 1.00f};
    ImVec4 accent_dim {0.357f, 0.553f, 0.937f, 0.22f};
    ImVec4 success    {0.247f, 0.698f, 0.498f, 1.00f};
    ImVec4 warning    {0.878f, 0.643f, 0.231f, 1.00f};
    ImVec4 danger     {0.898f, 0.325f, 0.294f, 1.00f};
    ImVec4 info       {0.400f, 0.702f, 0.851f, 1.00f};
};

struct Metrics {
    float rounding_window = 10.0f;
    float rounding_frame  = 7.0f;
    float rounding_tab    = 7.0f;
    float rounding_grab   = 7.0f;
    float padding_window_x = 16.0f;
    float padding_window_y = 14.0f;
    float padding_frame_x  = 11.0f;
    float padding_frame_y  = 7.0f;
    float spacing_x        = 10.0f;
    float spacing_y        = 9.0f;
    float scrollbar        = 12.0f;
    float indent           = 18.0f;
};

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* medium  = nullptr;
    ImFont* mono    = nullptr;
    float   size_small  = 13.0f;
    float   size_body   = 15.0f;
    float   size_title  = 18.0f;
    float   size_mono   = 13.0f;
};

const Palette& palette();
const Metrics& metrics();
const Fonts&   fonts();

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
