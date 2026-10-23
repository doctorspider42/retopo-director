#pragma once

// Line icons drawn with the draw list.
//
// No icon font ships with the application and none can be counted on being
// installed, so each glyph is a handful of strokes on a 24 unit grid, scaled to
// whatever box it is drawn into. They follow the text colour they are given,
// which keeps them in the palette for free.

#include <imgui.h>

namespace rd::ui {

enum class Icon : int {
    Play,
    Stop,
    Rebuild,
    Gear,
    Folder,
    Open,
    ChevronDown,
    ChevronRight,
    Close,
    Check,
    Cross,
    Warning,
    Info,
    Sparkle,
    Frame,
    Wireframe,
    Camera,
    Orbit,
    Cube,
    Sliders,
    Report,
    Chat,
    Image,
    Terminal,
    More,
    Upload,
    Eye,
    Target,
    Keyboard,
    Clock,
    Export,
};

// Draws `icon` centred in the square at `centre` with side `size`.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 centre, float size, ImU32 color,
               float thickness = 0.0f);

// The same, as an item on the current line, `size` pixels square.
void icon(Icon icon, float size, const ImVec4& color);

// An indeterminate spinner: a three quarter arc that turns.
void draw_spinner(ImDrawList* dl, ImVec2 centre, float radius, ImU32 color,
                  float thickness = 2.0f);

} // namespace rd::ui
