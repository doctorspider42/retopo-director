#pragma once

// Minimal CPU image plus the palette work a console target needs. No GPU is
// involved: the bake has to run identically in headless mode.

#include "core/math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rd {

struct Texture {
    int                  width    = 0;
    int                  height   = 0;
    int                  channels = 4;
    std::vector<uint8_t> pixels;      // row major, top row first

    bool   empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    size_t byte_size() const { return pixels.size(); }

    void resize(int w, int h, int c = 4)
    {
        width = w; height = h; channels = c;
        pixels.assign(size_t(w) * size_t(h) * size_t(c), 0);
    }

    uint8_t*       at(int x, int y)       { return &pixels[(size_t(y) * width + x) * channels]; }
    const uint8_t* at(int x, int y) const { return &pixels[(size_t(y) * width + x) * channels]; }

    void set(int x, int y, Vec4 linear_rgba);
    Vec4 get(int x, int y) const;

    bool save_png(const std::filesystem::path& path) const;
    bool load_png(const std::filesystem::path& path);
};

// Coverage mask produced by the rasteriser, used for dilation and for the
// utilisation metric in the validation report.
struct CoverageMask {
    int                  width = 0, height = 0;
    std::vector<uint8_t> covered;

    void resize(int w, int h) { width = w; height = h; covered.assign(size_t(w) * h, 0); }
    bool at(int x, int y) const { return covered[size_t(y) * width + x] != 0; }
    void set(int x, int y)      { covered[size_t(y) * width + x] = 1; }
    float utilisation() const;
};

// Spreads colour outward from covered texels so bilinear filtering does not
// pull background into a chart edge. `rings` is the padding in texels.
void dilate(Texture& tex, CoverageMask& mask, int rings);

struct Palette {
    std::vector<Vec4> colors;   // linear
    size_t size() const { return colors.size(); }
};

// Median cut over the covered texels only; ignoring background keeps the
// palette from wasting entries on empty space.
Palette build_palette(const Texture& tex, const CoverageMask& mask, int max_colors);

// Maps the image onto the palette. Floyd-Steinberg when `dither` is set.
void apply_palette(Texture& tex, const CoverageMask& mask, const Palette& palette,
                   bool dither);

// Writes an indexed PNG-adjacent sidecar: the palette as JSON plus an 8 bit
// index image. Engines that want a CLUT read this instead of the RGBA png.
bool save_indexed(const std::filesystem::path& base_path, const Texture& tex,
                  const Palette& palette, std::string* error = nullptr);

// sRGB conversions. The bake works in linear and only encodes on the way out.
inline float linear_to_srgb(float v)
{
    v = saturate(v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline float srgb_to_linear(float v)
{
    v = saturate(v);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

} // namespace rd
