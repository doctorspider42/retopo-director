#include "bake/texture.h"

#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <numeric>

namespace rd {
namespace {

inline uint8_t encode(float linear) { return uint8_t(clampf(linear_to_srgb(linear) * 255.0f + 0.5f, 0.0f, 255.0f)); }
inline float   decode(uint8_t v)    { return srgb_to_linear(float(v) / 255.0f); }

struct ColorBox {
    std::vector<uint32_t> members;   // indices into the sample list
    Vec3 lo{1, 1, 1}, hi{0, 0, 0};
    int  longest_axis = 0;
    float extent = 0.0f;
};

} // namespace

float CoverageMask::utilisation() const
{
    if (covered.empty()) return 0.0f;
    const size_t used = std::count(covered.begin(), covered.end(), uint8_t(1));
    return float(double(used) / double(covered.size()));
}

void dilate(Texture& tex, CoverageMask& mask, int rings)
{
    if (tex.empty() || rings <= 0) return;

    std::vector<uint8_t> current = mask.covered;
    for (int pass = 0; pass < rings; ++pass) {
        std::vector<uint8_t> next = current;
        bool changed = false;

        for (int y = 0; y < tex.height; ++y) {
            for (int x = 0; x < tex.width; ++x) {
                if (current[size_t(y) * tex.width + x]) continue;

                int   count = 0;
                float acc[4] = {0, 0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= tex.height) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        if (nx < 0 || nx >= tex.width) continue;
                        if (!current[size_t(ny) * tex.width + nx]) continue;
                        const uint8_t* p = tex.at(nx, ny);
                        for (int c = 0; c < tex.channels; ++c) acc[c] += float(p[c]);
                        ++count;
                    }
                }
                if (count == 0) continue;

                uint8_t* dst = tex.at(x, y);
                for (int c = 0; c < tex.channels; ++c)
                    dst[c] = uint8_t(clampf(acc[c] / float(count) + 0.5f, 0.0f, 255.0f));
                if (tex.channels > 3) dst[3] = 255;
                next[size_t(y) * tex.width + x] = 1;
                changed = true;
            }
        }
        current.swap(next);
        if (!changed) break;
    }
    // The mask keeps its original meaning: what the bake actually covered.
}

Palette build_palette(const Texture& tex, const CoverageMask& mask, int max_colors)
{
    Palette palette;
    if (tex.empty() || max_colors < 2) return palette;

    std::vector<Vec3> samples;
    samples.reserve(size_t(tex.width) * tex.height / 4 + 16);
    for (int y = 0; y < tex.height; ++y)
        for (int x = 0; x < tex.width; ++x) {
            if (!mask.covered.empty() && !mask.at(x, y)) continue;
            const uint8_t* p = tex.at(x, y);
            samples.push_back({float(p[0]) / 255.0f,
                               float(tex.channels > 1 ? p[1] : p[0]) / 255.0f,
                               float(tex.channels > 2 ? p[2] : p[0]) / 255.0f});
        }
    if (samples.empty()) return palette;

    auto recompute = [&](ColorBox& box) {
        box.lo = Vec3{1, 1, 1};
        box.hi = Vec3{0, 0, 0};
        for (uint32_t i : box.members) {
            box.lo = min(box.lo, samples[i]);
            box.hi = max(box.hi, samples[i]);
        }
        const Vec3 e = box.hi - box.lo;
        box.longest_axis = major_axis(e);
        box.extent = e[box.longest_axis];
    };

    std::vector<ColorBox> boxes(1);
    boxes[0].members.resize(samples.size());
    std::iota(boxes[0].members.begin(), boxes[0].members.end(), 0u);
    recompute(boxes[0]);

    while (int(boxes.size()) < max_colors) {
        // Split whichever box spans the most colour space.
        size_t target = 0;
        float  best   = 0.0f;
        for (size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].members.size() > 1 && boxes[i].extent > best) {
                best = boxes[i].extent;
                target = i;
            }
        if (best <= 1e-4f) break;

        ColorBox& box = boxes[target];
        const int axis = box.longest_axis;
        std::sort(box.members.begin(), box.members.end(), [&](uint32_t a, uint32_t b) {
            if (samples[a][axis] != samples[b][axis]) return samples[a][axis] < samples[b][axis];
            return a < b;
        });
        const size_t half = box.members.size() / 2;

        ColorBox upper;
        upper.members.assign(box.members.begin() + long(half), box.members.end());
        box.members.resize(half);
        recompute(box);
        recompute(upper);
        boxes.push_back(std::move(upper));
    }

    palette.colors.reserve(boxes.size());
    for (const ColorBox& box : boxes) {
        if (box.members.empty()) continue;
        Vec3 sum{};
        for (uint32_t i : box.members) sum += samples[i];
        sum = sum / float(box.members.size());
        palette.colors.push_back(Vec4{sum, 1.0f});
    }
    // Stable ordering makes the exported CLUT reproducible between runs.
    std::sort(palette.colors.begin(), palette.colors.end(), [](const Vec4& a, const Vec4& b) {
        const float la = 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z;
        const float lb = 0.2126f * b.x + 0.7152f * b.y + 0.0722f * b.z;
        if (la != lb) return la < lb;
        return a.x < b.x;
    });
    return palette;
}

namespace {

size_t nearest_entry(const Palette& palette, Vec3 c)
{
    size_t best = 0;
    float  best_d = std::numeric_limits<float>::max();
    for (size_t i = 0; i < palette.colors.size(); ++i) {
        const Vec3 p = palette.colors[i].xyz();
        const float d = length2(p - c);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

} // namespace

void apply_palette(Texture& tex, const CoverageMask& mask, const Palette& palette, bool dither)
{
    if (tex.empty() || palette.colors.empty()) return;

    // Work in 0..1 gamma space: that is what the eye and the CLUT both see.
    std::vector<Vec3> buffer(size_t(tex.width) * tex.height);
    for (int y = 0; y < tex.height; ++y)
        for (int x = 0; x < tex.width; ++x) {
            const uint8_t* p = tex.at(x, y);
            buffer[size_t(y) * tex.width + x] = {
                float(p[0]) / 255.0f,
                float(tex.channels > 1 ? p[1] : p[0]) / 255.0f,
                float(tex.channels > 2 ? p[2] : p[0]) / 255.0f};
        }

    for (int y = 0; y < tex.height; ++y) {
        for (int x = 0; x < tex.width; ++x) {
            const size_t i = size_t(y) * tex.width + x;
            const Vec3 old = buffer[i];
            const size_t entry = nearest_entry(palette, old);
            const Vec3 chosen = palette.colors[entry].xyz();

            uint8_t* p = tex.at(x, y);
            p[0] = uint8_t(clampf(chosen.x * 255.0f + 0.5f, 0.0f, 255.0f));
            if (tex.channels > 1) p[1] = uint8_t(clampf(chosen.y * 255.0f + 0.5f, 0.0f, 255.0f));
            if (tex.channels > 2) p[2] = uint8_t(clampf(chosen.z * 255.0f + 0.5f, 0.0f, 255.0f));

            if (!dither) continue;
            const Vec3 err = old - chosen;
            auto spread = [&](int nx, int ny, float w) {
                if (nx < 0 || nx >= tex.width || ny < 0 || ny >= tex.height) return;
                if (!mask.covered.empty() && !mask.at(nx, ny)) return;
                Vec3& target = buffer[size_t(ny) * tex.width + nx];
                target += err * w;
                target = {saturate(target.x), saturate(target.y), saturate(target.z)};
            };
            spread(x + 1, y,     7.0f / 16.0f);
            spread(x - 1, y + 1, 3.0f / 16.0f);
            spread(x,     y + 1, 5.0f / 16.0f);
            spread(x + 1, y + 1, 1.0f / 16.0f);
        }
    }
}

bool save_indexed(const std::filesystem::path& base_path, const Texture& tex,
                  const Palette& palette, std::string* error)
{
    if (tex.empty() || palette.colors.empty()) {
        if (error) *error = "nothing to write";
        return false;
    }

    std::vector<uint8_t> indices(size_t(tex.width) * tex.height, 0);
    for (int y = 0; y < tex.height; ++y)
        for (int x = 0; x < tex.width; ++x) {
            const uint8_t* p = tex.at(x, y);
            const Vec3 c{float(p[0]) / 255.0f,
                         float(tex.channels > 1 ? p[1] : p[0]) / 255.0f,
                         float(tex.channels > 2 ? p[2] : p[0]) / 255.0f};
            indices[size_t(y) * tex.width + x] = uint8_t(nearest_entry(palette, c) & 0xFFu);
        }

    const auto index_path = base_path.parent_path() / (base_path.stem().string() + "_index.png");
    paths::ensure_dir(index_path.parent_path());
    if (!stbi_write_png(index_path.string().c_str(), tex.width, tex.height, 1,
                        indices.data(), tex.width)) {
        if (error) *error = "cannot write the index image";
        return false;
    }

    Json j;
    j["width"]  = tex.width;
    j["height"] = tex.height;
    j["bits"]   = palette.colors.size() <= 16 ? 4 : 8;
    Json entries = Json::array();
    for (const Vec4& c : palette.colors) {
        Json e = Json::array();
        e.push_back(int(clampf(c.x * 255.0f + 0.5f, 0.0f, 255.0f)));
        e.push_back(int(clampf(c.y * 255.0f + 0.5f, 0.0f, 255.0f)));
        e.push_back(int(clampf(c.z * 255.0f + 0.5f, 0.0f, 255.0f)));
        entries.push_back(e);
    }
    j["palette"] = entries;

    std::string err;
    const auto clut_path = base_path.parent_path() / (base_path.stem().string() + "_clut.json");
    if (!json_save_file(clut_path.string(), j, err)) {
        if (error) *error = err;
        return false;
    }
    return true;
}

} // namespace rd
