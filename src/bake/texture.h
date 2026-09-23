#pragma once

// The palette work a console target needs, on top of the plain CPU image in
// core/image.h. No GPU is involved: the bake has to run identically in
// headless mode. Texture is re-exported through this header so the renderer,
// the exporter and the bake keep including one thing.

#include "core/image.h"
#include "core/math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rd {


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

} // namespace rd
