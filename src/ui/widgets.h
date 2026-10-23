#pragma once

// Small composed widgets so the panels read as intent rather than as a pile of
// ImGui calls, and so spacing and colour stay consistent everywhere.

#include "ui/icons.h"
#include "ui/theme.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace rd::ui {

// --- typography -------------------------------------------------------------
void heading(const char* text);
void subheading(const char* text);
void display(const char* text, const ImVec4& color);
void muted(const char* fmt, ...);
void faint(const char* fmt, ...);
void mono(const char* fmt, ...);
// Wrapped secondary text: the sentence under a control that says what it does.
void hint(const char* text);
void help_marker(const char* text);

// --- structure --------------------------------------------------------------
// A filled, rounded block: `if (card_begin(...)) { ... card_end(); }`.
// A false return means the card was culled and has already closed itself, so
// card_end() is called exactly when the body runs, never otherwise.
bool card_begin(const char* id, const char* title = nullptr, const char* subtitle = nullptr,
                float height = 0.0f);
void card_end();

// A small caps label that opens a group of controls. The collapsible form
// remembers its state per id and returns whether the body should be drawn.
void section_label(const char* text, const char* right_text = nullptr);
bool section(const char* text, bool default_open = true, const char* right_text = nullptr);

void divider(const char* label = nullptr);
void spacer(float y = 6.0f);

// A full width, centred message for a panel that has nothing to show yet.
void empty_state(const char* title, const char* body, Icon glyph = Icon::Info);

// A tinted block with an icon, for the one thing on a page worth reading first.
void callout(Icon glyph, const ImVec4& color, const char* title, const char* body = nullptr);

// --- readouts ---------------------------------------------------------------
void badge(const char* text, const ImVec4& color);
void key_value(const char* key, const char* value_fmt, ...);

// Large number with a caption underneath. `width` 0 uses the remaining space.
void stat(const char* caption, const char* value, const ImVec4& accent, float width = 0.0f);

// A tile for a grid of headline numbers: caption, value, a sub line, and an
// optional thin meter when `fraction` is not negative.
void stat_tile(const char* id, const char* caption, const char* value, const char* sub,
               const ImVec4& accent, float width, float fraction = -1.0f,
               const ImVec4* value_color = nullptr);

// Horizontal bar with a limit marker. Goes amber past `warn_at` and red past 1.
void meter(const char* label, float value, float limit, const char* value_text = nullptr);

// Flat rounded progress bar with centred text. `width` 0 takes the rest of the
// line, which is what a panel wants.
void progress_bar(float fraction, const char* overlay = nullptr, float height = 0.0f,
                  float width = 0.0f);

// A coloured dot, for region swatches and status lights.
void dot(const ImVec4& color, float radius = 5.0f);

// --- inputs -----------------------------------------------------------------
// Label on the left, control on the right, tooltip on hover, right click resets
// to `default_value`, double click types a number. Returns true when the value
// changed.
bool slider_float(const char* label, float* value, float min, float max,
                  float default_value, const char* tooltip = nullptr,
                  const char* format = "%.2f");
bool slider_int(const char* label, int* value, int min, int max, int default_value,
                const char* tooltip = nullptr);
bool toggle(const char* label, bool* value, const char* tooltip = nullptr);
bool combo(const char* label, int* current, const char* const items[], int count,
           const char* tooltip = nullptr);
bool text_input(const char* label, std::string& value, const char* hint = nullptr,
                const char* tooltip = nullptr);
bool text_area(const char* label, std::string& value, float height, const char* tooltip = nullptr);

// Primary / secondary / danger / ghost buttons at a consistent size. The icon
// overloads put a glyph before the label.
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool secondary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool danger_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool ghost_button(const char* label, const ImVec2& size = ImVec2(0, 0));

enum class ButtonKind { Primary, Secondary, Danger, Ghost };
bool icon_label_button(const char* label, Icon glyph, ButtonKind kind,
                       const ImVec2& size = ImVec2(0, 0));

// A square button holding one glyph. `active` draws it pressed in.
bool icon_button(const char* id, Icon glyph, const char* tooltip = nullptr, bool active = false,
                 float size = 0.0f);

// A segmented control: one row of mutually exclusive options in a shared
// track. `width` 0 takes the rest of the line; negative sizes each option to
// its label.
bool segmented(const char* id, int* current, const char* const items[], int count,
               float width = 0.0f);

// --- layout helpers ---------------------------------------------------------
// Begins a two column layout where the left column is a fixed label width.
void begin_form(const char* id, float label_width = 0.0f);
void end_form();

} // namespace rd::ui
