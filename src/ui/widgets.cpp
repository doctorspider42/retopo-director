#include "ui/widgets.h"

#include "core/util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace rd::ui {
namespace {

float g_form_label_width = 150.0f;
bool  g_in_form = false;

void text_v(const ImVec4& color, const char* fmt, va_list args)
{
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void tooltip_for(const char* text)
{
    if (!text || !*text) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
}

// Lays out "label ....... [control]" and leaves the cursor on the control.
void form_label(const char* label, const char* tooltip)
{
    const float width = g_in_form ? g_form_label_width : ImGui::GetContentRegionAvail().x * 0.45f;
    const ImVec2 start = ImGui::GetCursorPos();

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    tooltip_for(tooltip);

    ImGui::SetCursorPos({start.x + width, start.y});
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
}

} // namespace

// ---------------------------------------------------------------------------
void heading(const char* text)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.medium, f.size_title);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void subheading(const char* text)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.medium, f.size_body);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void muted(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    text_v(palette().text_dim, fmt, args);
    va_end(args);
}

void mono(const char* fmt, ...)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.mono, f.size_mono);
    va_list args;
    va_start(args, fmt);
    text_v(palette().text, fmt, args);
    va_end(args);
    ImGui::PopFont();
}

void help_marker(const char* text)
{
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
    ImGui::TextUnformatted("(?)");
    ImGui::PopStyleColor();
    tooltip_for(text);
}

// ---------------------------------------------------------------------------
bool card_begin(const char* id, const char* title, const char* subtitle, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette().surface_alt);
    ImGui::PushStyleColor(ImGuiCol_Border, palette().line);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metrics().rounding_frame + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));

    // BeginChild has to be closed whether or not it returned true - it pushes a
    // window either way, and skipping EndChild leaves the window stack and both
    // style stacks unbalanced for the rest of the frame. The call sites read
    // `if (card_begin(...)) { ... card_end(); }`, so a card that is culled
    // (clipped, zero height, collapsed parent) is closed right here instead.
    const bool open = ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders |
                                        (height <= 0.0f ? ImGuiChildFlags_AutoResizeY : 0));
    if (!open) {
        card_end();
        return false;
    }
    if (title) {
        subheading(title);
        if (subtitle && *subtitle) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopStyleColor();
        }
        spacer(4.0f);
    }
    return open;
}

void card_end()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void divider(const char* label)
{
    spacer(4.0f);
    if (label && *label) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
        ImGui::SeparatorText(label);
        ImGui::PopStyleColor();
    } else {
        ImGui::Separator();
    }
    spacer(4.0f);
}

void spacer(float y) { ImGui::Dummy(ImVec2(0.0f, y)); }

void empty_state(const char* title, const char* body)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, std::max(20.0f, avail.y * 0.32f)));

    const float width = std::min(avail.x, 420.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - width) * 0.5f));
    ImGui::BeginGroup();
    {
        const Fonts& f = fonts();
        ImGui::PushFont(f.medium, f.size_title);
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
        const float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - tw) * 0.5f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        spacer(6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextUnformatted(body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}

// ---------------------------------------------------------------------------
void badge(const char* text, const ImVec4& color)
{
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad{8.0f, 3.0f};
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 box{size.x + pad.x * 2.0f, size.y + pad.y * 2.0f};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + box.x, pos.y + box.y}, col(color, 0.18f), 6.0f);
    dl->AddRect(pos, {pos.x + box.x, pos.y + box.y}, col(color, 0.45f), 6.0f);
    dl->AddText({pos.x + pad.x, pos.y + pad.y}, col(color), text);

    ImGui::Dummy(box);
}

void key_value(const char* key, const char* value_fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, value_fmt);
    std::vsnprintf(buf, sizeof(buf), value_fmt, args);
    va_end(args);

    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();

    const float value_width = ImGui::CalcTextSize(buf).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(4.0f, ImGui::GetContentRegionAvail().x - value_width));
    ImGui::TextUnformatted(buf);
}

void stat(const char* caption, const char* value, const ImVec4& accent, float width)
{
    const Fonts& f = fonts();
    ImGui::BeginGroup();
    if (width > 0.0f) ImGui::PushItemWidth(width);

    ImGui::PushFont(f.medium, f.size_title + 4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::PushFont(f.regular, f.size_small);
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
    ImGui::TextUnformatted(caption);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (width > 0.0f) ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void meter(const char* label, float value, float limit, const char* value_text)
{
    const Palette& p = palette();
    const float ratio = limit > 0.0f ? value / limit : 0.0f;

    ImVec4 color = p.success;
    if (ratio > 1.0f)       color = p.danger;
    else if (ratio > 0.88f) color = p.warning;

    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    char buf[96];
    if (value_text) std::snprintf(buf, sizeof(buf), "%s", value_text);
    else            std::snprintf(buf, sizeof(buf), "%.0f / %.0f", value, limit);

    ImGui::SameLine();
    const float tw = ImGui::CalcTextSize(buf).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(4.0f, ImGui::GetContentRegionAvail().x - tw));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();

    const float height = 6.0f;
    const ImVec2 pos   = ImGui::GetCursorScreenPos();
    const float  full  = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + full, pos.y + height}, col(p.canvas), height * 0.5f);

    const float filled = full * std::clamp(ratio, 0.0f, 1.0f);
    if (filled > 1.0f)
        dl->AddRectFilled(pos, {pos.x + filled, pos.y + height}, col(color), height * 0.5f);

    // Overshoot gets a hard red cap at the end of the track.
    if (ratio > 1.0f)
        dl->AddRectFilled({pos.x + full - 3.0f, pos.y}, {pos.x + full, pos.y + height},
                          col(p.danger), height * 0.5f);

    ImGui::Dummy({full, height});
}

void progress_bar(float fraction, const char* overlay, float height)
{
    const Palette& p = palette();
    if (height <= 0.0f) height = ImGui::GetFrameHeight() * 0.62f;

    const ImVec2 pos  = ImGui::GetCursorScreenPos();
    const float  full = ImGui::GetContentRegionAvail().x;
    const float  r    = height * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + full, pos.y + height}, col(p.canvas), r);

    const float filled = full * std::clamp(fraction, 0.0f, 1.0f);
    if (filled > 1.0f) {
        dl->AddRectFilledMultiColor({pos.x, pos.y}, {pos.x + filled, pos.y + height},
                                    col(p.accent), col(p.accent_hot),
                                    col(p.accent_hot), col(p.accent));
        // The rounded caps have to be drawn on top of the gradient.
        dl->AddRectFilled(pos, {pos.x + std::min(filled, r * 2.0f), pos.y + height},
                          col(p.accent), r);
    }

    if (overlay && *overlay) {
        const ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText({pos.x + (full - ts.x) * 0.5f, pos.y + (height - ts.y) * 0.5f},
                    col(p.text), overlay);
    }
    ImGui::Dummy({full, height});
}

void dot(const ImVec4& color, float radius)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float  h   = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        {pos.x + radius, pos.y + h * 0.5f}, radius, col(color), 16);
    ImGui::Dummy({radius * 2.0f + 4.0f, h});
    ImGui::SameLine(0.0f, 4.0f);
}

// ---------------------------------------------------------------------------
bool slider_float(const char* label, float* value, float min, float max,
                  float default_value, const char* tooltip, const char* format)
{
    ImGui::PushID(label);
    form_label(label, tooltip);
    const bool changed = ImGui::SliderFloat("##v", value, min, max, format);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = default_value;
        ImGui::PopID();
        return true;
    }
    tooltip_for(tooltip);
    ImGui::PopID();
    return changed;
}

bool slider_int(const char* label, int* value, int min, int max, int default_value,
                const char* tooltip)
{
    ImGui::PushID(label);
    form_label(label, tooltip);
    const bool changed = ImGui::SliderInt("##v", value, min, max);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = default_value;
        ImGui::PopID();
        return true;
    }
    tooltip_for(tooltip);
    ImGui::PopID();
    return changed;
}

bool toggle(const char* label, bool* value, const char* tooltip)
{
    const Palette& p = palette();
    ImGui::PushID(label);
    form_label(label, tooltip);

    const float height = ImGui::GetFrameHeight() * 0.82f;
    const float width  = height * 1.85f;
    const ImVec2 pos   = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton("##t", {width, height});
    if (pressed) *value = !*value;
    tooltip_for(tooltip);

    const bool  hovered = ImGui::IsItemHovered();
    const float r       = height * 0.5f;
    const ImVec4 track  = *value ? (hovered ? p.accent_hot : p.accent)
                                 : (hovered ? p.overlay : p.elevated);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + width, pos.y + height}, col(track), r);
    const float knob_x = *value ? pos.x + width - r : pos.x + r;
    dl->AddCircleFilled({knob_x, pos.y + r}, r - 2.5f, col(p.text), 20);

    ImGui::PopID();
    return pressed;
}

bool combo(const char* label, int* current, const char* const items[], int count,
           const char* tooltip)
{
    ImGui::PushID(label);
    form_label(label, tooltip);
    const bool changed = ImGui::Combo("##c", current, items, count);
    tooltip_for(tooltip);
    ImGui::PopID();
    return changed;
}

bool text_input(const char* label, std::string& value, const char* hint, const char* tooltip)
{
    ImGui::PushID(label);
    form_label(label, tooltip);

    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    bool changed = false;
    if (hint && *hint) changed = ImGui::InputTextWithHint("##t", hint, buf, sizeof(buf));
    else               changed = ImGui::InputText("##t", buf, sizeof(buf));
    if (changed) value = buf;
    tooltip_for(tooltip);
    ImGui::PopID();
    return changed;
}

bool text_area(const char* label, std::string& value, float height, const char* tooltip)
{
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    tooltip_for(tooltip);

    std::vector<char> buf(std::max<size_t>(value.size() + 1024, 4096));
    std::snprintf(buf.data(), buf.size(), "%s", value.c_str());
    const bool changed = ImGui::InputTextMultiline(
        "##a", buf.data(), buf.size(), ImVec2(-FLT_MIN, height));
    if (changed) value = buf.data();
    ImGui::PopID();
    return changed;
}

namespace {

bool styled_button(const char* label, const ImVec2& size, const ImVec4& base,
                   const ImVec4& hot, const ImVec4& text_color)
{
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(hot, ImVec4(0, 0, 0, 1), 0.2f));
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 8));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

} // namespace

bool primary_button(const char* label, const ImVec2& size)
{
    const Palette& p = palette();
    return styled_button(label, size, p.accent, p.accent_hot, ImVec4(1, 1, 1, 1));
}

bool secondary_button(const char* label, const ImVec2& size)
{
    const Palette& p = palette();
    return styled_button(label, size, p.elevated, p.overlay, p.text);
}

bool danger_button(const char* label, const ImVec2& size)
{
    const Palette& p = palette();
    return styled_button(label, size, with_alpha(p.danger, 0.22f),
                         with_alpha(p.danger, 0.40f), p.danger);
}

bool segmented(const char* id, int* current, const char* const items[], int count)
{
    const Palette& p = palette();
    bool changed = false;

    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

    const float total = ImGui::GetContentRegionAvail().x;
    const float each  = count > 0 ? (total - 2.0f * float(count - 1)) / float(count) : total;

    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine();
        const bool active = (*current == i);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? p.accent : p.surface_alt);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? p.accent_hot : p.elevated);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? p.accent_hot : p.overlay);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1, 1, 1, 1) : p.text_dim);
        if (ImGui::Button(items[i], ImVec2(each, 0))) {
            *current = i;
            changed  = true;
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

void begin_form(const char* id, float label_width)
{
    ImGui::PushID(id);
    g_in_form = true;
    g_form_label_width = label_width > 0.0f
                             ? label_width
                             : std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.42f);
}

void end_form()
{
    g_in_form = false;
    ImGui::PopID();
}

} // namespace rd::ui
