#pragma once

// Small composed widgets so the panels read as intent rather than as a pile of
// ImGui calls, and so spacing and colour stay consistent everywhere.

#include "ui/theme.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace rd::ui {

// --- typography -------------------------------------------------------------
void heading(const char* text);
void subheading(const char* text);
void muted(const char* fmt, ...);
void mono(const char* fmt, ...);
void help_marker(const char* text);

// --- structure --------------------------------------------------------------
// A bordered, padded block: `if (card_begin(...)) { ... card_end(); }`.
// A false return means the card was culled and has already closed itself, so
// card_end() is called exactly when the body runs, never otherwise.
bool card_begin(const char* id, const char* title = nullptr, const char* subtitle = nullptr,
                float height = 0.0f);
void card_end();

void divider(const char* label = nullptr);
void spacer(float y = 6.0f);

// A full width, centred message for a panel that has nothing to show yet.
void empty_state(const char* title, const char* body);

// --- readouts ---------------------------------------------------------------
void badge(const char* text, const ImVec4& color);
void key_value(const char* key, const char* value_fmt, ...);

// Large number with a caption underneath. `width` 0 uses the remaining space.
void stat(const char* caption, const char* value, const ImVec4& accent, float width = 0.0f);

// Horizontal bar with a limit marker. Goes amber past `warn_at` and red past 1.
void meter(const char* label, float value, float limit, const char* value_text = nullptr);

// Flat rounded progress bar with centred text.
void progress_bar(float fraction, const char* overlay = nullptr, float height = 0.0f);

// A coloured dot, for region swatches and status lights.
void dot(const ImVec4& color, float radius = 5.0f);

// --- inputs -----------------------------------------------------------------
// Label on the left, control on the right, tooltip on hover, right click resets
// to `default_value`. Returns true when the value changed.
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

// Primary / secondary / danger buttons at a consistent size.
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool secondary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool danger_button(const char* label, const ImVec2& size = ImVec2(0, 0));

// A segmented control: one row of mutually exclusive buttons.
bool segmented(const char* id, int* current, const char* const items[], int count);

// --- layout helpers ---------------------------------------------------------
// Begins a two column layout where the left column is a fixed label width.
void begin_form(const char* id, float label_width = 0.0f);
void end_form();

} // namespace rd::ui
