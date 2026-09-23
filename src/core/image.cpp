#include "core/image.h"

#include "core/log.h"
#include "core/paths.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cmath>

namespace rd {
namespace {

inline uint8_t encode(float linear)
{
    return uint8_t(clampf(linear_to_srgb(linear) * 255.0f + 0.5f, 0.0f, 255.0f));
}
inline float decode(uint8_t v) { return srgb_to_linear(float(v) / 255.0f); }

} // namespace

void Texture::set(int x, int y, Vec4 c)
{
    uint8_t* p = at(x, y);
    p[0] = encode(c.x);
    if (channels > 1) p[1] = encode(c.y);
    if (channels > 2) p[2] = encode(c.z);
    if (channels > 3) p[3] = uint8_t(clampf(c.w * 255.0f + 0.5f, 0.0f, 255.0f));
}

Vec4 Texture::get(int x, int y) const
{
    const uint8_t* p = at(x, y);
    Vec4 c;
    c.x = decode(p[0]);
    c.y = channels > 1 ? decode(p[1]) : c.x;
    c.z = channels > 2 ? decode(p[2]) : c.x;
    c.w = channels > 3 ? float(p[3]) / 255.0f : 1.0f;
    return c;
}

Vec4 Texture::sample(Vec2 uv) const
{
    if (empty()) return Vec4{1.0f, 1.0f, 1.0f, 1.0f};

    // v is flipped because image row 0 is the top, while every uv convention
    // this pipeline sees puts v = 0 at the bottom.
    float u = uv.x - std::floor(uv.x);
    float v = 1.0f - (uv.y - std::floor(uv.y));
    if (v >= 1.0f) v = 0.0f;

    const float fx = u * float(width) - 0.5f;
    const float fy = v * float(height) - 0.5f;
    const int   x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);

    auto wrap = [](int i, int n) { const int m = i % n; return m < 0 ? m + n : m; };
    const int xa = wrap(x0, width),  xb = wrap(x0 + 1, width);
    const int ya = wrap(y0, height), yb = wrap(y0 + 1, height);

    const Vec4 c00 = get(xa, ya), c10 = get(xb, ya);
    const Vec4 c01 = get(xa, yb), c11 = get(xb, yb);
    const Vec4 top = c00 * (1.0f - tx) + c10 * tx;
    const Vec4 bot = c01 * (1.0f - tx) + c11 * tx;
    return top * (1.0f - ty) + bot * ty;
}

bool Texture::save_png(const std::filesystem::path& path) const
{
    if (empty()) return false;
    paths::ensure_dir(path.parent_path());
    const std::string utf8 = path.string();
    const int ok = stbi_write_png(utf8.c_str(), width, height, channels,
                                  pixels.data(), width * channels);
    if (!ok) RD_ERROR("failed to write %s", utf8.c_str());
    return ok != 0;
}

bool Texture::load_png(const std::filesystem::path& path)
{
    int w = 0, h = 0, c = 0;
    stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &c, 4);
    if (!data) return false;
    width = w; height = h; channels = 4;
    pixels.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

bool Texture::load_memory(const uint8_t* bytes, size_t size)
{
    if (!bytes || size == 0) return false;
    int w = 0, h = 0, c = 0;
    stbi_uc* data = stbi_load_from_memory(bytes, int(size), &w, &h, &c, 4);
    if (!data) return false;
    width = w; height = h; channels = 4;
    pixels.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

} // namespace rd
