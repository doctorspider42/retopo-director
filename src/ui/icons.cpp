#include "ui/icons.h"

#include "ui/theme.h"

#include <algorithm>
#include <cmath>

namespace rd::ui {
namespace {

constexpr float kPi = 3.14159265f;

// Everything is authored on a 24 unit grid with the origin top left, the same
// grid most line icon sets use, so the shapes can be read off a sketch.
struct Pen {
    ImDrawList* dl;
    ImVec2      c;
    float       s;
    ImU32       col;
    float       t;

    ImVec2 p(float x, float y) const { return {c.x + (x - 12.0f) * s, c.y + (y - 12.0f) * s}; }
    float  r(float v) const { return v * s; }

    void line(float x0, float y0, float x1, float y1) const
    {
        dl->AddLine(p(x0, y0), p(x1, y1), col, t);
    }
    void poly(std::initializer_list<ImVec2> pts, bool closed) const
    {
        ImVec2 buf[16];
        int    n = 0;
        for (const ImVec2& q : pts) buf[n++] = p(q.x, q.y);
        dl->AddPolyline(buf, n, col, t, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    }
    void fill(std::initializer_list<ImVec2> pts) const
    {
        ImVec2 buf[16];
        int    n = 0;
        for (const ImVec2& q : pts) buf[n++] = p(q.x, q.y);
        dl->AddConvexPolyFilled(buf, n, col);
    }
    void rect(float x0, float y0, float x1, float y1, float rounding) const
    {
        dl->AddRect(p(x0, y0), p(x1, y1), col, r(rounding), t);
    }
    void circle(float x, float y, float radius) const
    {
        dl->AddCircle(p(x, y), r(radius), col, 0, t);
    }
    void disc(float x, float y, float radius) const
    {
        dl->AddCircleFilled(p(x, y), r(radius), col);
    }
    void arc(float x, float y, float radius, float a0, float a1) const
    {
        dl->PathArcTo(p(x, y), r(radius), a0, a1);
        dl->PathStroke(col, t, ImDrawFlags_None);
    }
};

} // namespace

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 centre, float size, ImU32 color, float thickness)
{
    const float s = size / 24.0f;
    const float t = thickness > 0.0f ? thickness : std::max(1.3f, size / 13.0f);
    const Pen   g{dl, centre, s, color, t};

    switch (icon) {
    case Icon::Play:
        g.fill({{7.5f, 5.0f}, {19.0f, 12.0f}, {7.5f, 19.0f}});
        break;
    case Icon::Stop:
        dl->AddRectFilled(g.p(6.5f, 6.5f), g.p(17.5f, 17.5f), color, g.r(2.0f));
        break;
    case Icon::Rebuild:
        g.arc(12.0f, 12.0f, 7.0f, -kPi * 0.15f, kPi * 1.35f);
        g.poly({{19.6f, 5.6f}, {19.4f, 10.4f}, {14.8f, 9.6f}}, false);
        break;
    case Icon::Gear: {
        // Eight square teeth on a ring, walked as one outline.
        const int teeth = 8;
        ImVec2    pts[teeth * 4];
        for (int i = 0; i < teeth; ++i) {
            const float a  = float(i) / float(teeth) * 2.0f * kPi;
            const float hw = kPi / float(teeth) * 0.42f;
            const float rr[4] = {7.0f, 9.6f, 9.6f, 7.0f};
            const float aa[4] = {a - hw * 1.5f, a - hw, a + hw, a + hw * 1.5f};
            for (int k = 0; k < 4; ++k)
                pts[i * 4 + k] = g.p(12.0f + std::cos(aa[k]) * rr[k], 12.0f + std::sin(aa[k]) * rr[k]);
        }
        dl->AddPolyline(pts, teeth * 4, color, t, ImDrawFlags_Closed);
        g.circle(12.0f, 12.0f, 3.0f);
        break;
    }
    case Icon::Folder:
        g.poly({{3.5f, 7.0f}, {3.5f, 18.5f}, {20.5f, 18.5f}, {20.5f, 8.5f}, {11.5f, 8.5f},
                {9.5f, 5.5f}, {3.5f, 5.5f}}, true);
        break;
    case Icon::Open:
        g.poly({{3.5f, 18.5f}, {3.5f, 5.5f}, {9.5f, 5.5f}, {11.5f, 8.0f}, {18.0f, 8.0f},
                {18.0f, 10.5f}}, false);
        g.poly({{3.5f, 18.5f}, {6.5f, 10.5f}, {21.0f, 10.5f}, {18.0f, 18.5f}}, true);
        break;
    case Icon::ChevronDown:
        g.poly({{6.5f, 9.5f}, {12.0f, 15.0f}, {17.5f, 9.5f}}, false);
        break;
    case Icon::ChevronRight:
        g.poly({{9.5f, 6.5f}, {15.0f, 12.0f}, {9.5f, 17.5f}}, false);
        break;
    case Icon::Close:
    case Icon::Cross:
        g.line(6.5f, 6.5f, 17.5f, 17.5f);
        g.line(17.5f, 6.5f, 6.5f, 17.5f);
        break;
    case Icon::Check:
        g.poly({{5.0f, 12.5f}, {9.8f, 17.0f}, {19.0f, 7.0f}}, false);
        break;
    case Icon::Warning:
        g.poly({{12.0f, 3.5f}, {21.5f, 20.0f}, {2.5f, 20.0f}}, true);
        g.line(12.0f, 9.5f, 12.0f, 14.0f);
        g.disc(12.0f, 17.0f, 1.1f);
        break;
    case Icon::Info:
        g.circle(12.0f, 12.0f, 9.0f);
        g.line(12.0f, 11.0f, 12.0f, 16.5f);
        g.disc(12.0f, 7.8f, 1.1f);
        break;
    case Icon::Sparkle:
        g.fill({{12.0f, 2.5f}, {14.2f, 9.8f}, {21.5f, 12.0f}, {14.2f, 14.2f},
                {12.0f, 21.5f}, {9.8f, 14.2f}, {2.5f, 12.0f}, {9.8f, 9.8f}});
        break;
    case Icon::Frame:
        g.poly({{4.0f, 9.0f}, {4.0f, 4.0f}, {9.0f, 4.0f}}, false);
        g.poly({{15.0f, 4.0f}, {20.0f, 4.0f}, {20.0f, 9.0f}}, false);
        g.poly({{20.0f, 15.0f}, {20.0f, 20.0f}, {15.0f, 20.0f}}, false);
        g.poly({{9.0f, 20.0f}, {4.0f, 20.0f}, {4.0f, 15.0f}}, false);
        g.circle(12.0f, 12.0f, 2.6f);
        break;
    case Icon::Wireframe:
        g.poly({{12.0f, 3.5f}, {20.5f, 19.5f}, {3.5f, 19.5f}}, true);
        g.line(7.75f, 11.5f, 16.25f, 11.5f);
        g.line(7.75f, 11.5f, 12.0f, 19.5f);
        g.line(16.25f, 11.5f, 12.0f, 19.5f);
        break;
    case Icon::Camera:
        g.rect(3.5f, 7.0f, 20.5f, 19.0f, 2.5f);
        g.poly({{8.5f, 7.0f}, {10.0f, 4.5f}, {14.0f, 4.5f}, {15.5f, 7.0f}}, false);
        g.circle(12.0f, 13.0f, 3.5f);
        break;
    case Icon::Orbit:
        dl->PathEllipticalArcTo(g.p(12.0f, 12.0f), ImVec2(g.r(9.5f), g.r(4.0f)), 0.0f,
                                kPi * 0.15f, kPi * 1.85f);
        dl->PathStroke(color, t, ImDrawFlags_None);
        g.poly({{19.0f, 9.0f}, {21.6f, 10.6f}, {19.6f, 13.0f}}, false);
        g.disc(12.0f, 12.0f, 2.4f);
        break;
    case Icon::Cube:
        g.poly({{12.0f, 3.0f}, {20.0f, 7.5f}, {20.0f, 16.5f}, {12.0f, 21.0f}, {4.0f, 16.5f},
                {4.0f, 7.5f}}, true);
        g.poly({{4.0f, 7.5f}, {12.0f, 12.0f}, {20.0f, 7.5f}}, false);
        g.line(12.0f, 12.0f, 12.0f, 21.0f);
        break;
    case Icon::Sliders:
        g.line(4.0f, 7.0f, 20.0f, 7.0f);
        g.line(4.0f, 12.0f, 20.0f, 12.0f);
        g.line(4.0f, 17.0f, 20.0f, 17.0f);
        g.disc(15.0f, 7.0f, 2.4f);
        g.disc(8.0f, 12.0f, 2.4f);
        g.disc(13.5f, 17.0f, 2.4f);
        break;
    case Icon::Report:
        g.rect(5.0f, 3.5f, 19.0f, 20.5f, 2.5f);
        g.line(8.5f, 9.0f, 15.5f, 9.0f);
        g.line(8.5f, 12.5f, 15.5f, 12.5f);
        g.line(8.5f, 16.0f, 12.5f, 16.0f);
        break;
    case Icon::Chat:
        g.poly({{4.0f, 5.0f}, {20.0f, 5.0f}, {20.0f, 16.0f}, {11.0f, 16.0f}, {6.5f, 20.0f},
                {6.5f, 16.0f}, {4.0f, 16.0f}}, true);
        break;
    case Icon::Image:
        g.rect(3.5f, 4.5f, 20.5f, 19.5f, 2.5f);
        g.poly({{3.5f, 16.5f}, {9.0f, 11.0f}, {14.0f, 16.0f}, {16.5f, 13.5f}, {20.5f, 17.5f}},
               false);
        g.disc(15.5f, 8.8f, 1.6f);
        break;
    case Icon::Terminal:
        g.rect(3.0f, 4.5f, 21.0f, 19.5f, 2.5f);
        g.poly({{7.0f, 9.5f}, {10.0f, 12.0f}, {7.0f, 14.5f}}, false);
        g.line(12.0f, 15.0f, 16.5f, 15.0f);
        break;
    case Icon::More:
        g.disc(6.0f, 12.0f, 1.7f);
        g.disc(12.0f, 12.0f, 1.7f);
        g.disc(18.0f, 12.0f, 1.7f);
        break;
    case Icon::Upload:
        g.line(12.0f, 15.5f, 12.0f, 4.0f);
        g.poly({{7.0f, 8.5f}, {12.0f, 3.5f}, {17.0f, 8.5f}}, false);
        g.poly({{4.0f, 14.5f}, {4.0f, 20.0f}, {20.0f, 20.0f}, {20.0f, 14.5f}}, false);
        break;
    case Icon::Eye:
        dl->PathArcTo(g.p(12.0f, 20.0f), g.r(11.0f), -kPi * 0.82f, -kPi * 0.18f);
        dl->PathStroke(color, t, ImDrawFlags_None);
        dl->PathArcTo(g.p(12.0f, 4.0f), g.r(11.0f), kPi * 0.18f, kPi * 0.82f);
        dl->PathStroke(color, t, ImDrawFlags_None);
        g.circle(12.0f, 12.0f, 3.0f);
        break;
    case Icon::Target:
        g.circle(12.0f, 12.0f, 8.5f);
        g.circle(12.0f, 12.0f, 4.5f);
        g.disc(12.0f, 12.0f, 1.4f);
        break;
    case Icon::Keyboard:
        g.rect(2.5f, 6.0f, 21.5f, 18.0f, 2.5f);
        for (int i = 0; i < 4; ++i) g.disc(6.5f + float(i) * 3.7f, 10.0f, 0.9f);
        g.line(8.0f, 14.2f, 16.0f, 14.2f);
        break;
    case Icon::Clock:
        g.circle(12.0f, 12.0f, 8.5f);
        g.poly({{12.0f, 7.5f}, {12.0f, 12.0f}, {15.5f, 14.0f}}, false);
        break;
    case Icon::Export:
        g.poly({{9.0f, 4.0f}, {4.0f, 4.0f}, {4.0f, 20.0f}, {20.0f, 20.0f}, {20.0f, 15.0f}},
               false);
        g.line(11.0f, 13.0f, 20.0f, 4.0f);
        g.poly({{14.0f, 4.0f}, {20.0f, 4.0f}, {20.0f, 10.0f}}, false);
        break;
    }
}

void icon(Icon which, float size, const ImVec4& color)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float  h   = std::max(size, ImGui::GetTextLineHeight());
    draw_icon(ImGui::GetWindowDrawList(), which, {pos.x + size * 0.5f, pos.y + h * 0.5f}, size,
              col(color));
    ImGui::Dummy({size, h});
}

void draw_spinner(ImDrawList* dl, ImVec2 centre, float radius, ImU32 color, float thickness)
{
    const float a0 = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(centre, radius, a0, a0 + kPi * 1.4f, 24);
    dl->PathStroke(color, thickness, ImDrawFlags_None);
}

} // namespace rd::ui
