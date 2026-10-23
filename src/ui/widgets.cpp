#include "ui/widgets.h"

#include "core/util.h"

#include <algorithm>
#include <imgui_internal.h>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rd::ui {
namespace {

float g_form_label_width = 150.0f;
bool  g_in_form = false;

// The slider whose number is being typed into, if any. Only one field can have
// the keyboard at a time, so one id is enough.
ImGuiID g_editing_id    = 0;
bool    g_editing_fresh = false;

float S(float v) { return v * dpi(); }

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
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
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

    // Labels that start with ## are ids only, like ImGui's own.
    const char* shown = label;
    const char* end   = ImGui::FindRenderedTextEnd(label);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
    ImGui::TextUnformatted(shown, end);
    ImGui::PopStyleColor();
    tooltip_for(tooltip);

    ImGui::SetCursorPos({start.x + width, start.y});
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
}

std::string upper(const char* text)
{
    std::string out(text ? text : "");
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Draws a background under whatever the caller lays out between begin and end,
// sized to it after the fact. The draw list channels are what make a block
// whose height is only known once its content has been placed.
struct Backdrop {
    ImDrawList* dl = nullptr;
    ImVec2      pad;

    void begin(const ImVec2& padding)
    {
        dl  = ImGui::GetWindowDrawList();
        pad = padding;
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + pad.x, ImGui::GetCursorPosY() + pad.y});
        ImGui::BeginGroup();
    }
    void end(ImU32 fill, float rounding, float min_width = 0.0f, ImU32 border = 0)
    {
        ImGui::EndGroup();
        ImVec2 inner_max = ImGui::GetItemRectMax();
        ImVec2 inner_min = ImGui::GetItemRectMin();
        inner_max.x = std::max(inner_max.x, inner_min.x + min_width - pad.x * 2.0f);
        ImGui::SetCursorScreenPos({inner_min.x - pad.x, inner_max.y});
        ImGui::Dummy({inner_max.x - inner_min.x + pad.x * 2.0f, pad.y});
        ImGui::EndGroup();

        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(a, b, fill, rounding);
        if (border) dl->AddRect(a, b, border, rounding);
        dl->ChannelsMerge();
    }
};

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

void display(const char* text, const ImVec4& color)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.medium, f.size_display);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void muted(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    text_v(palette().text_dim, fmt, args);
    va_end(args);
}

void faint(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    text_v(palette().text_faint, fmt, args);
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

void hint(const char* text)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.regular, f.size_small);
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void help_marker(const char* text)
{
    ImGui::SameLine(0.0f, 6.0f);
    icon(Icon::Info, ImGui::GetTextLineHeight() * 0.85f, palette().text_faint);
    tooltip_for(text);
}

// ---------------------------------------------------------------------------
bool card_begin(const char* id, const char* title, const char* subtitle, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette().surface_alt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metrics().rounding_card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));

    // BeginChild has to be closed whether or not it returned true - it pushes a
    // window either way, and skipping EndChild leaves the window stack and both
    // style stacks unbalanced for the rest of the frame. The call sites read
    // `if (card_begin(...)) { ... card_end(); }`, so a card that is culled
    // (clipped, zero height, collapsed parent) is closed right here instead.
    const bool open = ImGui::BeginChild(
        id, ImVec2(0, height),
        ImGuiChildFlags_AlwaysUseWindowPadding |
            (height <= 0.0f ? ImGuiChildFlags_AutoResizeY : 0),
        ImGuiWindowFlags_NoScrollbar);
    if (!open) {
        card_end();
        return false;
    }
    if (title) {
        subheading(title);
        if (subtitle && *subtitle) {
            const Fonts& f = fonts();
            ImGui::PushFont(f.regular, f.size_small);
            ImGui::PushStyleColor(ImGuiCol_Text, palette().text_faint);
            ImGui::TextWrapped("%s", subtitle);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        spacer(4.0f);
    }
    return open;
}

void card_end()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void section_label(const char* text, const char* right_text)
{
    const Palette& p = palette();
    const Fonts&   f = fonts();
    ImGui::PushFont(f.medium, f.size_small);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
    ImGui::TextUnformatted(upper(text).c_str());
    if (right_text && *right_text) {
        ImGui::SameLine();
        const float tw = ImGui::CalcTextSize(right_text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(4.0f, ImGui::GetContentRegionAvail().x - tw));
        ImGui::TextUnformatted(right_text);
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    spacer(2.0f);
}

bool section(const char* text, bool default_open, const char* right_text)
{
    const Palette& p = palette();
    const Fonts&   f = fonts();

    ImGui::PushID(text);
    const ImGuiID id   = ImGui::GetID("##section");
    bool*         open = ImGui::GetStateStorage()->GetBoolRef(id, default_open);

    ImGui::PushFont(f.medium, f.size_small);
    const float  h     = ImGui::GetTextLineHeight() + S(10);
    const float  w     = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos   = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##hdr", {w, h})) *open = !*open;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList*  dl   = ImGui::GetWindowDrawList();
    const ImVec4 tint = hovered ? p.text_dim : p.text_faint;
    const float  is   = ImGui::GetTextLineHeight() * 0.8f;
    draw_icon(dl, *open ? Icon::ChevronDown : Icon::ChevronRight,
              {pos.x + is * 0.5f, pos.y + h * 0.5f}, is, col(tint));
    const std::string label = upper(text);
    dl->AddText({pos.x + is + S(6), pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f},
                col(tint), label.c_str());
    if (right_text && *right_text) {
        const float tw = ImGui::CalcTextSize(right_text).x;
        dl->AddText({pos.x + w - tw, pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f},
                    col(p.text_faint), right_text);
    }
    ImGui::PopFont();
    ImGui::PopID();
    if (*open) spacer(2.0f);
    return *open;
}

void divider(const char* label)
{
    spacer(4.0f);
    if (label && *label) {
        section_label(label);
    } else {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float  w   = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(pos, {pos.x + w, pos.y}, col(palette().line), 1.0f);
        ImGui::Dummy({w, 1.0f});
    }
    spacer(4.0f);
}

void spacer(float y) { ImGui::Dummy(ImVec2(0.0f, y)); }

void empty_state(const char* title, const char* body, Icon glyph)
{
    const Palette& p = palette();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, std::max(20.0f, avail.y * 0.24f)));

    const float width = std::min(avail.x, S(340));
    const float x0    = ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - width) * 0.5f);

    // The glyph in a soft disc.
    {
        const float  r   = S(26);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 c{pos.x + avail.x * 0.5f, pos.y + r};
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(c, r, col(p.elevated));
        draw_icon(dl, glyph, c, r * 0.95f, col(p.text_dim));
        ImGui::Dummy({avail.x, r * 2.0f + S(14)});
    }

    const Fonts& f = fonts();
    ImGui::PushFont(f.medium, f.size_title);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    const float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - tw) * 0.5f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    spacer(4.0f);
    // Centre each wrapped line by hand; ImGui only wraps left aligned.
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    const char* text = body;
    const char* end  = body + std::strlen(body);
    ImFont*     font = ImGui::GetFont();
    const float fs   = ImGui::GetFontSize();
    while (text < end) {
        const char* line_end = font->CalcWordWrapPosition(fs, text, end, width);
        if (line_end == text) line_end = text + 1;
        const float lw = ImGui::CalcTextSize(text, line_end).x;
        ImGui::SetCursorPosX(x0 + std::max(0.0f, (width - lw) * 0.5f));
        ImGui::TextUnformatted(text, line_end);
        text = line_end;
        while (text < end && (*text == ' ' || *text == '\n')) ++text;
    }
    ImGui::PopStyleColor();
}

void callout(Icon glyph, const ImVec4& color, const char* title, const char* body)
{
    const Palette& p = palette();
    const float    full = ImGui::GetContentRegionAvail().x;

    Backdrop bd;
    bd.begin({S(14), S(12)});
    const float is = ImGui::GetTextLineHeight();
    icon(glyph, is, color);
    ImGui::SameLine(0.0f, S(10));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full - is - S(10) - S(28));
    if (title && *title) {
        const Fonts& f = fonts();
        ImGui::PushFont(f.medium, f.size_body);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text);
        ImGui::TextWrapped("%s", title);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    if (body && *body) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
        ImGui::TextWrapped("%s", body);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    bd.end(col(color, 0.10f), metrics().rounding_card, full, col(color, 0.22f));
}

// ---------------------------------------------------------------------------
void badge(const char* text, const ImVec4& color)
{
    const Fonts& f = fonts();
    ImGui::PushFont(f.medium, f.size_small);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad{S(8), S(3)};
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 box{size.x + pad.x * 2.0f, size.y + pad.y * 2.0f};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + box.x, pos.y + box.y}, col(color, 0.16f), box.y * 0.5f);
    dl->AddText({pos.x + pad.x, pos.y + pad.y}, col(color), text);
    ImGui::PopFont();

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

void stat_tile(const char* id, const char* caption, const char* value, const char* sub,
               const ImVec4& accent, float width, float fraction, const ImVec4* value_color)
{
    const Palette& p = palette();
    const Fonts&   f = fonts();
    ImGui::PushID(id);

    const ImVec2 pad{S(14), S(12)};
    const float  inner = width - pad.x * 2.0f;

    Backdrop bd;
    bd.begin(pad);
    ImGui::Dummy({inner, 0.0f});

    ImGui::PushFont(f.regular, f.size_small);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text_dim);
    ImGui::TextUnformatted(caption);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::PushFont(f.medium, f.size_display * 0.86f);
    ImGui::PushStyleColor(ImGuiCol_Text, value_color ? *value_color : accent);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (sub && *sub) {
        ImGui::PushFont(f.regular, f.size_small);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text_faint);
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    if (fraction >= 0.0f) {
        spacer(2.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float  h   = S(4);
        ImDrawList*  dl  = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, {pos.x + inner, pos.y + h}, col(p.overlay), h);
        const float fill = inner * std::clamp(fraction, 0.0f, 1.0f);
        if (fill > 1.0f)
            dl->AddRectFilled(pos, {pos.x + fill, pos.y + h}, col(accent), h);
        ImGui::Dummy({inner, h});
    }
    bd.end(col(p.surface_alt), metrics().rounding_card, width);
    ImGui::PopID();
}

void meter(const char* label, float value, float limit, const char* value_text)
{
    const Palette& p = palette();
    const float ratio = limit > 0.0f ? value / limit : 0.0f;

    ImVec4 color = p.success;
    if (ratio > 1.0f)       color = p.danger;
    else if (ratio > 0.92f) color = p.warning;

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
    ImGui::PushStyleColor(ImGuiCol_Text, ratio > 1.0f ? p.danger : p.text);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();

    const float height = S(5);
    const ImVec2 pos   = ImGui::GetCursorScreenPos();
    const float  full  = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + full, pos.y + height}, col(p.overlay), height);

    const float filled = full * std::clamp(ratio, 0.0f, 1.0f);
    if (filled > 1.0f)
        dl->AddRectFilled(pos, {pos.x + filled, pos.y + height}, col(color), height);

    ImGui::Dummy({full, height});
}

void progress_bar(float fraction, const char* overlay, float height, float width)
{
    const Palette& p = palette();
    if (height <= 0.0f) height = S(6);

    const ImVec2 pos  = ImGui::GetCursorScreenPos();
    const float  full = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float  r    = height * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + full, pos.y + height}, col(p.overlay), r);

    const float filled = full * std::clamp(fraction, 0.0f, 1.0f);
    if (filled > 1.0f)
        dl->AddRectFilled(pos, {pos.x + std::max(filled, r * 2.0f), pos.y + height},
                          col(p.accent), r);

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
    ImGui::GetWindowDrawList()->AddCircleFilled({pos.x + radius, pos.y + h * 0.5f}, radius,
                                                col(color), 16);
    ImGui::Dummy({radius * 2.0f + 2.0f, h});
    ImGui::SameLine(0.0f, S(6));
}

// ---------------------------------------------------------------------------
namespace {

// One drawing for both slider types: a slim track with a round knob, and the
// number in a little field on the right that can be typed into.
template <typename T>
bool slider_impl(const char* label, T* value, T min, T max, T default_value,
                 const char* tooltip, const char* format, bool is_int)
{
    const Palette& p = palette();
    ImGui::PushID(label);
    form_label(label, tooltip);

    const ImGuiID id     = ImGui::GetID("##slider");
    const float   full   = std::max(S(90), ImGui::CalcItemWidth());
    const float   h      = ImGui::GetFrameHeight();
    const float   box_w  = std::min(S(76), full * 0.42f);
    const float   gap    = S(10);
    const float   track_w = std::max(S(30), full - box_w - gap);
    const float   knob_r = S(6.5f);
    const ImVec2  pos    = ImGui::GetCursorScreenPos();

    bool changed = false;

    // --- track ---------------------------------------------------------------
    ImGui::InvisibleButton("##track", {track_w, h});
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    tooltip_for(tooltip);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        changed = *value != default_value;
        *value  = default_value;
    } else if (active && max > min) {
        const float span = track_w - knob_r * 2.0f;
        float t = (ImGui::GetIO().MousePos.x - pos.x - knob_r) / std::max(1.0f, span);
        t = std::clamp(t, 0.0f, 1.0f);
        double v = double(min) + double(t) * (double(max) - double(min));
        if (is_int) v = std::round(v);
        const T nv = T(v);
        if (nv != *value) {
            *value  = nv;
            changed = true;
        }
    }

    {
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        const float cy   = pos.y + h * 0.5f;
        const float th   = S(4);
        const float t    = max > min ? std::clamp(float(double(*value - min) /
                                                        double(max - min)), 0.0f, 1.0f)
                                     : 0.0f;
        const float x0   = pos.x + knob_r;
        const float x1   = pos.x + track_w - knob_r;
        const float kx   = x0 + (x1 - x0) * t;
        dl->AddRectFilled({x0 - th * 0.5f, cy - th * 0.5f}, {x1 + th * 0.5f, cy + th * 0.5f},
                          col(p.overlay), th);
        dl->AddRectFilled({x0 - th * 0.5f, cy - th * 0.5f}, {kx, cy + th * 0.5f},
                          col(active || hovered ? p.accent_hot : p.accent), th);
        if (active || hovered)
            dl->AddCircleFilled({kx, cy}, knob_r + S(4), col(p.accent, 0.22f), 24);
        dl->AddCircleFilled({kx, cy}, knob_r, col(p.text), 24);
    }

    // --- number --------------------------------------------------------------
    ImGui::SameLine(0.0f, gap);
    const ImVec2 box_pos = ImGui::GetCursorScreenPos();
    if (g_editing_id == id) {
        ImGui::SetNextItemWidth(box_w);
        if (g_editing_fresh) {
            ImGui::SetKeyboardFocusHere();
            g_editing_fresh = false;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, p.overlay);
        const bool edited =
            is_int ? ImGui::InputScalar("##edit", ImGuiDataType_S32, value, nullptr, nullptr, "%d")
                   : ImGui::InputScalar("##edit", ImGuiDataType_Float, value, nullptr, nullptr,
                                        "%.3f");
        ImGui::PopStyleColor();
        if (edited) {
            *value  = std::clamp(*value, min, max);
            changed = true;
        }
        if (ImGui::IsItemDeactivated() ||
            (!ImGui::IsItemActive() && !ImGui::IsItemActivated() &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()))
            g_editing_id = 0;
    } else {
        if (ImGui::InvisibleButton("##num", {box_w, h})) {
            g_editing_id    = id;
            g_editing_fresh = true;
        }
        const bool box_hovered = ImGui::IsItemHovered();
        if (box_hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            changed = *value != default_value;
            *value  = default_value;
        }

        char buf[64];
        if (is_int) std::snprintf(buf, sizeof(buf), "%d", int(*value));
        else        std::snprintf(buf, sizeof(buf), format, double(*value));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(box_pos, {box_pos.x + box_w, box_pos.y + h},
                          col(box_hovered ? p.elevated : p.surface_alt),
                          metrics().rounding_frame);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->PushClipRect(box_pos, {box_pos.x + box_w, box_pos.y + h}, true);
        dl->AddText({box_pos.x + box_w - ts.x - S(8), box_pos.y + (h - ts.y) * 0.5f},
                    col(*value == default_value ? p.text_dim : p.text), buf);
        dl->PopClipRect();
    }

    ImGui::PopID();
    return changed;
}

} // namespace

bool slider_float(const char* label, float* value, float min, float max,
                  float default_value, const char* tooltip, const char* format)
{
    return slider_impl<float>(label, value, min, max, default_value, tooltip, format, false);
}

bool slider_int(const char* label, int* value, int min, int max, int default_value,
                const char* tooltip)
{
    return slider_impl<int>(label, value, min, max, default_value, tooltip, "%d", true);
}

bool toggle(const char* label, bool* value, const char* tooltip)
{
    const Palette& p = palette();
    ImGui::PushID(label);
    form_label(label, tooltip);

    const float fh     = ImGui::GetFrameHeight();
    const float height = S(20);
    const float width  = S(36);
    const ImVec2 pos   = ImGui::GetCursorScreenPos();
    const ImVec2 top{pos.x, pos.y + (fh - height) * 0.5f};

    const bool pressed = ImGui::InvisibleButton("##t", {width, fh});
    if (pressed) *value = !*value;
    tooltip_for(tooltip);

    const bool  hovered = ImGui::IsItemHovered();
    const float r       = height * 0.5f;
    const ImVec4 track  = *value ? (hovered ? p.accent_hot : p.accent)
                                 : (hovered ? mix(p.overlay, p.text_faint, 0.3f) : p.overlay);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(top, {top.x + width, top.y + height}, col(track), r);
    const float knob_x = *value ? top.x + width - r : top.x + r;
    dl->AddCircleFilled({knob_x, top.y + r}, r - S(3), col(p.text), 20);

    ImGui::PopID();
    return pressed;
}

bool combo(const char* label, int* current, const char* const items[], int count,
           const char* tooltip)
{
    const Palette& p = palette();
    ImGui::PushID(label);
    form_label(label, tooltip);

    bool changed = false;
    const char* preview = (*current >= 0 && *current < count) ? items[*current] : "";
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(4)));
    const bool open = ImGui::BeginCombo("##c", preview, ImGuiComboFlags_NoArrowButton);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float  is = ImGui::GetTextLineHeight() * 0.8f;
    draw_icon(ImGui::GetWindowDrawList(), Icon::ChevronDown,
              {b.x - is * 0.5f - S(10), (a.y + b.y) * 0.5f}, is, col(p.text_faint));
    tooltip_for(tooltip);
    if (open) {
        for (int i = 0; i < count; ++i) {
            const bool selected = (*current == i);
            if (ImGui::Selectable(items[i], selected)) {
                *current = i;
                changed  = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(2);
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
    const char* end = ImGui::FindRenderedTextEnd(label);
    if (end != label) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette().text_dim);
        ImGui::TextUnformatted(label, end);
        ImGui::PopStyleColor();
        tooltip_for(tooltip);
    }

    std::vector<char> buf(std::max<size_t>(value.size() + 1024, 4096));
    std::snprintf(buf.data(), buf.size(), "%s", value.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(10)));
    const bool changed = ImGui::InputTextMultiline(
        "##a", buf.data(), buf.size(), ImVec2(-FLT_MIN, height),
        ImGuiInputTextFlags_WordWrap);
    ImGui::PopStyleVar();
    if (changed) value = buf.data();
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------
namespace {

struct ButtonColors {
    ImVec4 base, hot, text;
};

ButtonColors colors_for(ButtonKind kind)
{
    const Palette& p = palette();
    switch (kind) {
    case ButtonKind::Primary:
        return {p.accent, p.accent_hot, ImVec4(1, 1, 1, 1)};
    case ButtonKind::Danger:
        return {with_alpha(p.danger, 0.16f), with_alpha(p.danger, 0.28f), p.danger};
    case ButtonKind::Ghost:
        return {ImVec4(0, 0, 0, 0), p.elevated, p.text_dim};
    case ButtonKind::Secondary:
    default:
        return {p.elevated, p.overlay, p.text};
    }
}

// Every button is one rounded fill and a centred row of icon and label, so
// buttons with and without a glyph line up and share one height.
bool fancy_button(const char* label, const Icon* glyph, ButtonKind kind, ImVec2 size)
{
    const ButtonColors c = colors_for(kind);
    const char* end  = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts  = ImGui::CalcTextSize(label, end);
    const float  is  = glyph ? ImGui::GetTextLineHeight() * 0.95f : 0.0f;
    const float  gap = glyph && end != label ? S(8) : 0.0f;
    const ImVec2 pad{S(14), S(8)};

    const float content = is + gap + ts.x;
    if (size.x <= 0.0f) size.x = content + pad.x * 2.0f;
    if (size.y <= 0.0f) size.y = ImGui::GetTextLineHeight() + pad.y * 2.0f;

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   pressed = ImGui::InvisibleButton(label, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    // Style.Alpha is what BeginDisabled lowers; the draw list does not apply
    // it on its own.
    const float  alpha   = ImGui::GetStyle().Alpha;

    ImVec4 fill = hovered ? c.hot : c.base;
    if (held) fill = mix(c.hot, ImVec4(0, 0, 0, 1), 0.15f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (fill.w > 0.0f)
        dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, col(fill, alpha),
                          metrics().rounding_frame);

    const ImVec4 text = kind == ButtonKind::Ghost && hovered ? palette().text : c.text;
    float x = pos.x + (size.x - content) * 0.5f;
    const float cy = pos.y + size.y * 0.5f;
    if (glyph) {
        draw_icon(dl, *glyph, {x + is * 0.5f, cy}, is, col(text, alpha));
        x += is + gap;
    }
    if (end != label) dl->AddText({x, cy - ts.y * 0.5f}, col(text, alpha), label, end);
    return pressed;
}

} // namespace

bool primary_button(const char* label, const ImVec2& size)
{
    return fancy_button(label, nullptr, ButtonKind::Primary, size);
}

bool secondary_button(const char* label, const ImVec2& size)
{
    return fancy_button(label, nullptr, ButtonKind::Secondary, size);
}

bool danger_button(const char* label, const ImVec2& size)
{
    return fancy_button(label, nullptr, ButtonKind::Danger, size);
}

bool ghost_button(const char* label, const ImVec2& size)
{
    return fancy_button(label, nullptr, ButtonKind::Ghost, size);
}

bool icon_label_button(const char* label, Icon glyph, ButtonKind kind, const ImVec2& size)
{
    return fancy_button(label, &glyph, kind, size);
}

bool icon_button(const char* id, Icon glyph, const char* tooltip, bool active, float size)
{
    const Palette& p = palette();
    if (size <= 0.0f) size = ImGui::GetFrameHeight() + S(4);

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   pressed = ImGui::InvisibleButton(id, {size, size});
    const bool   hovered = ImGui::IsItemHovered();
    const float  alpha   = ImGui::GetStyle().Alpha;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(pos, {pos.x + size, pos.y + size}, col(p.accent, 0.20f * alpha),
                          metrics().rounding_frame);
    else if (hovered)
        dl->AddRectFilled(pos, {pos.x + size, pos.y + size}, col(p.overlay, alpha),
                          metrics().rounding_frame);

    const ImVec4 tint = active ? p.accent_hot : (hovered ? p.text : p.text_dim);
    draw_icon(dl, glyph, {pos.x + size * 0.5f, pos.y + size * 0.5f}, size * 0.52f,
              col(tint, alpha));
    if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

bool segmented(const char* id, int* current, const char* const items[], int count,
               float width)
{
    const Palette& p = palette();
    bool changed = false;

    ImGui::PushID(id);
    const float pad   = S(3);
    const float h     = ImGui::GetFrameHeight() + pad * 2.0f;
    const float txt_pad = S(14);

    std::vector<float> widths(size_t(std::max(count, 0)));
    float total = 0.0f;
    if (width < 0.0f) {
        for (int i = 0; i < count; ++i) {
            widths[size_t(i)] = ImGui::CalcTextSize(items[i]).x + txt_pad * 2.0f;
            total += widths[size_t(i)];
        }
        total += pad * 2.0f;
    } else {
        total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
        const float each = count > 0 ? (total - pad * 2.0f) / float(count) : 0.0f;
        for (float& w : widths) w = each;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + total, pos.y + h}, col(p.canvas, 0.85f),
                      metrics().rounding_frame + 2.0f);

    float x = pos.x + pad;
    for (int i = 0; i < count; ++i) {
        const float w = widths[size_t(i)];
        ImGui::SetCursorScreenPos({x, pos.y + pad});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", {w, h - pad * 2.0f})) {
            if (*current != i) changed = true;
            *current = i;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool active = (*current == i);
        const ImVec2 a{x, pos.y + pad}, b{x + w, pos.y + h - pad};
        if (active)
            dl->AddRectFilled(a, b, col(p.overlay), metrics().rounding_frame);
        else if (hovered)
            dl->AddRectFilled(a, b, col(p.elevated, 0.6f), metrics().rounding_frame);

        const ImVec2 ts = ImGui::CalcTextSize(items[i]);
        dl->AddText({x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f},
                    col(active ? p.text : (hovered ? p.text_dim : p.text_faint)), items[i]);
        x += w;
    }

    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy({total, h});
    ImGui::PopID();
    return changed;
}

void begin_form(const char* id, float label_width)
{
    ImGui::PushID(id);
    g_in_form = true;
    g_form_label_width = label_width > 0.0f
                             ? label_width * dpi()
                             : std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.42f);
}

void end_form()
{
    g_in_form = false;
    ImGui::PopID();
}

} // namespace rd::ui
