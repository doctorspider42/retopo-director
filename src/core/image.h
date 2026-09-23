#pragma once

// A CPU image and nothing else. No GPU, no palette work, no atlas: those live
// in bake/texture.h and are the bake's business.
//
// This sits in core/ rather than in bake/ because a Mesh now carries the source
// textures it was authored with, and mesh/ must not depend on bake/. Everything
// that used to include bake/texture.h for Texture alone still compiles: that
// header includes this one.

#include "core/math.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace rd {

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

    // Bilinear fetch in normalised uv, wrapping outside 0..1. Wrapping rather
    // than clamping because that is what every DCC does with a tiling texture,
    // and a transferred uv lands outside the unit square often enough to matter.
    Vec4 sample(Vec2 uv) const;

    bool save_png(const std::filesystem::path& path) const;
    // Despite the name, stb decodes png, jpg, tga and bmp, which is what an
    // asset's material actually points at.
    bool load_png(const std::filesystem::path& path);
    // Decodes an image already in memory, which is how a glb or an fbx carries
    // one. Returns false and leaves the texture empty on anything unreadable.
    bool load_memory(const uint8_t* bytes, size_t size);
};

} // namespace rd
