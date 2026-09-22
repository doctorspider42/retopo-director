#pragma once

// Small, dependency free linear algebra. Column vectors, right handed, with
// matrices stored column major so they can be handed to OpenGL untouched.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rd {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kTwoPi   = 6.28318530717958647692f;
constexpr float kHalfPi  = 1.57079632679489661923f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kEps     = 1e-6f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v)                   { return clampf(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t)    { return a + (b - a) * t; }
inline float sqr(float v)                        { return v * v; }
inline float smoothstepf(float e0, float e1, float x)
{
    const float t = saturate((x - e0) / std::max(e1 - e0, kEps));
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f, y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float xx, float yy) : x(xx), y(yy) {}
    explicit constexpr Vec2(float s) : x(s), y(s) {}

    float&       operator[](int i)       { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }
};

inline Vec2 operator+(Vec2 a, Vec2 b)  { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b)  { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, Vec2 a) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(Vec2 a, Vec2 b)  { return {a.x * b.x, a.y * b.y}; }
inline Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
inline Vec2 operator-(Vec2 a)          { return {-a.x, -a.y}; }
inline Vec2& operator+=(Vec2& a, Vec2 b) { a = a + b; return a; }
inline Vec2& operator-=(Vec2& a, Vec2 b) { a = a - b; return a; }
inline Vec2& operator*=(Vec2& a, float s) { a = a * s; return a; }
inline bool  operator==(Vec2 a, Vec2 b)  { return a.x == b.x && a.y == b.y; }

inline float dot(Vec2 a, Vec2 b)     { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b)   { return a.x * b.y - a.y * b.x; }
inline float length2(Vec2 v)         { return dot(v, v); }
inline float length(Vec2 v)          { return std::sqrt(dot(v, v)); }
inline Vec2  normalize(Vec2 v)       { const float l = length(v); return l > kEps ? v / l : Vec2{}; }
inline Vec2  min(Vec2 a, Vec2 b)     { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline Vec2  max(Vec2 a, Vec2 b)     { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3(Vec2 v, float zz) : x(v.x), y(v.y), z(zz) {}

    float&       operator[](int i)       { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }
    Vec2         xy() const              { return {x, y}; }
};

inline Vec3 operator+(Vec3 a, Vec3 b)  { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b)  { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(Vec3 a, Vec3 b)  { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3 operator/(Vec3 a, Vec3 b)  { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline Vec3 operator-(Vec3 a)          { return {-a.x, -a.y, -a.z}; }
inline Vec3& operator+=(Vec3& a, Vec3 b)  { a = a + b; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b)  { a = a - b; return a; }
inline Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }
inline Vec3& operator/=(Vec3& a, float s) { a = a / s; return a; }
inline bool  operator==(Vec3 a, Vec3 b)   { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline bool  operator!=(Vec3 a, Vec3 b)   { return !(a == b); }

inline float dot(Vec3 a, Vec3 b)   { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length2(Vec3 v)     { return dot(v, v); }
inline float length(Vec3 v)      { return std::sqrt(dot(v, v)); }
inline float distance(Vec3 a, Vec3 b) { return length(a - b); }
inline Vec3  normalize(Vec3 v)   { const float l = length(v); return l > kEps ? v / l : Vec3{}; }
inline Vec3  min(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3  max(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3  abs(Vec3 v)         { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
inline Vec3  lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float max_component(Vec3 v) { return std::max(v.x, std::max(v.y, v.z)); }
inline float min_component(Vec3 v) { return std::min(v.x, std::min(v.y, v.z)); }
inline int   major_axis(Vec3 v)
{
    const Vec3 a = abs(v);
    return (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
}

// Any unit vector perpendicular to n. Frisvad style, branch on sign.
inline void basis_from_normal(Vec3 n, Vec3& t, Vec3& b)
{
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    t = {1.0f + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}

// ---------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------
struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float xx, float yy, float zz, float ww) : x(xx), y(yy), z(zz), w(ww) {}
    constexpr Vec4(Vec3 v, float ww) : x(v.x), y(v.y), z(v.z), w(ww) {}
    explicit constexpr Vec4(float s) : x(s), y(s), z(s), w(s) {}

    float&       operator[](int i)       { return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }
    Vec3         xyz() const             { return {x, y, z}; }
};

inline Vec4 operator+(Vec4 a, Vec4 b)  { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 operator-(Vec4 a, Vec4 b)  { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float dot(Vec4 a, Vec4 b)       { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// ---------------------------------------------------------------------------
// Mat4, column major: m[col][row] lives at data[col * 4 + row]
// ---------------------------------------------------------------------------
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float&       at(int col, int row)       { return m[col * 4 + row]; }
    const float& at(int col, int row) const { return m[col * 4 + row]; }

    static Mat4 identity() { return {}; }

    static Mat4 zero()
    {
        Mat4 r;
        for (float& v : r.m) v = 0.0f;
        return r;
    }

    static Mat4 translation(Vec3 t)
    {
        Mat4 r;
        r.at(3, 0) = t.x; r.at(3, 1) = t.y; r.at(3, 2) = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s)
    {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }

    static Mat4 rotation_axis(Vec3 axis, float radians)
    {
        const Vec3  a = normalize(axis);
        const float c = std::cos(radians), s = std::sin(radians), t = 1.0f - c;
        Mat4 r;
        r.at(0, 0) = t * a.x * a.x + c;
        r.at(0, 1) = t * a.x * a.y + s * a.z;
        r.at(0, 2) = t * a.x * a.z - s * a.y;
        r.at(1, 0) = t * a.x * a.y - s * a.z;
        r.at(1, 1) = t * a.y * a.y + c;
        r.at(1, 2) = t * a.y * a.z + s * a.x;
        r.at(2, 0) = t * a.x * a.z + s * a.y;
        r.at(2, 1) = t * a.y * a.z - s * a.x;
        r.at(2, 2) = t * a.z * a.z + c;
        return r;
    }

    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up)
    {
        const Vec3 f = normalize(target - eye);
        Vec3 s = cross(f, up);
        if (length2(s) < 1e-12f) s = cross(f, Vec3{0.0f, 0.0f, 1.0f});
        s = normalize(s);
        const Vec3 u = cross(s, f);

        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 0) = s.y; r.at(2, 0) = s.z;
        r.at(0, 1) = u.x; r.at(1, 1) = u.y; r.at(2, 1) = u.z;
        r.at(0, 2) = -f.x; r.at(1, 2) = -f.y; r.at(2, 2) = -f.z;
        r.at(3, 0) = -dot(s, eye);
        r.at(3, 1) = -dot(u, eye);
        r.at(3, 2) =  dot(f, eye);
        return r;
    }

    static Mat4 perspective(float fov_y_radians, float aspect, float znear, float zfar)
    {
        const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
        Mat4 r = zero();
        r.at(0, 0) = f / std::max(aspect, kEps);
        r.at(1, 1) = f;
        r.at(2, 2) = (zfar + znear) / (znear - zfar);
        r.at(2, 3) = -1.0f;
        r.at(3, 2) = (2.0f * zfar * znear) / (znear - zfar);
        return r;
    }

    static Mat4 orthographic(float l, float r_, float b, float t, float n, float f)
    {
        Mat4 r;
        r.at(0, 0) = 2.0f / (r_ - l);
        r.at(1, 1) = 2.0f / (t - b);
        r.at(2, 2) = -2.0f / (f - n);
        r.at(3, 0) = -(r_ + l) / (r_ - l);
        r.at(3, 1) = -(t + b) / (t - b);
        r.at(3, 2) = -(f + n) / (f - n);
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r = Mat4::zero();
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.at(k, row) * b.at(c, k);
            r.at(c, row) = sum;
        }
    return r;
}

inline Vec4 operator*(const Mat4& a, Vec4 v)
{
    Vec4 r;
    for (int row = 0; row < 4; ++row)
        r[row] = a.at(0, row) * v.x + a.at(1, row) * v.y +
                 a.at(2, row) * v.z + a.at(3, row) * v.w;
    return r;
}

inline Vec3 transform_point(const Mat4& a, Vec3 p)
{
    const Vec4 r = a * Vec4{p, 1.0f};
    const float w = std::fabs(r.w) > kEps ? r.w : 1.0f;
    return {r.x / w, r.y / w, r.z / w};
}

inline Vec3 transform_dir(const Mat4& a, Vec3 v)
{
    return {a.at(0, 0) * v.x + a.at(1, 0) * v.y + a.at(2, 0) * v.z,
            a.at(0, 1) * v.x + a.at(1, 1) * v.y + a.at(2, 1) * v.z,
            a.at(0, 2) * v.x + a.at(1, 2) * v.y + a.at(2, 2) * v.z};
}

inline Mat4 transpose(const Mat4& a)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) r.at(c, row) = a.at(row, c);
    return r;
}

Mat4 inverse(const Mat4& a);

// ---------------------------------------------------------------------------
// Quaternion, xyzw
// ---------------------------------------------------------------------------
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static Quat from_axis_angle(Vec3 axis, float radians)
    {
        const Vec3  a = normalize(axis);
        const float h = radians * 0.5f, s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }
};

inline Quat operator*(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat normalize(Quat q)
{
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < kEps) return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

inline Mat4 to_mat4(Quat q)
{
    q = normalize(q);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Mat4 r;
    r.at(0, 0) = 1 - 2 * (yy + zz); r.at(1, 0) = 2 * (xy - wz);     r.at(2, 0) = 2 * (xz + wy);
    r.at(0, 1) = 2 * (xy + wz);     r.at(1, 1) = 1 - 2 * (xx + zz); r.at(2, 1) = 2 * (yz - wx);
    r.at(0, 2) = 2 * (xz - wy);     r.at(1, 2) = 2 * (yz + wx);     r.at(2, 2) = 1 - 2 * (xx + yy);
    return r;
}

// ---------------------------------------------------------------------------
// Axis aligned bounding box
// ---------------------------------------------------------------------------
struct Aabb {
    Vec3 lo{ std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max()};
    Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

    bool valid()  const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
    Vec3 center() const { return (lo + hi) * 0.5f; }
    Vec3 extent() const { return valid() ? hi - lo : Vec3{}; }
    float diagonal() const { return valid() ? length(hi - lo) : 0.0f; }

    float surface_area() const
    {
        if (!valid()) return 0.0f;
        const Vec3 d = hi - lo;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    void grow(Vec3 p)        { lo = rd::min(lo, p); hi = rd::max(hi, p); }
    void grow(const Aabb& b) { if (b.valid()) { lo = rd::min(lo, b.lo); hi = rd::max(hi, b.hi); } }

    bool contains(Vec3 p) const
    {
        return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y &&
               p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
    }
};

// ---------------------------------------------------------------------------
// Triangle helpers
// ---------------------------------------------------------------------------
inline Vec3 triangle_normal(Vec3 a, Vec3 b, Vec3 c)
{
    return normalize(cross(b - a, c - a));
}

inline float triangle_area(Vec3 a, Vec3 b, Vec3 c)
{
    return 0.5f * length(cross(b - a, c - a));
}

// Normalised shape quality: 1 for an equilateral triangle, 0 for a degenerate
// sliver. 4*sqrt(3)*area / (sum of squared edge lengths).
inline float triangle_quality(Vec3 a, Vec3 b, Vec3 c)
{
    const float l2 = length2(b - a) + length2(c - b) + length2(a - c);
    if (l2 < 1e-20f) return 0.0f;
    const float area = 0.5f * length(cross(b - a, c - a));
    return saturate(6.9282032302755088f * area / l2);
}

Vec3 closest_point_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

// Moeller-Trumbore. Returns true and fills t/u/v on hit.
bool ray_triangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c,
                  float tmin, float tmax, float& t, float& u, float& v);

// ---------------------------------------------------------------------------
// Deterministic RNG (PCG32) so every run reproduces bit for bit.
// ---------------------------------------------------------------------------
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc   = 0xda3e39cb94b95bdbULL;

    explicit Rng(uint64_t seed = 0x9e3779b97f4a7c15ULL)
    {
        state = 0;
        inc   = (seed << 1u) | 1u;
        next_u32();
        state += 0x853c49e6748fea9bULL;
        next_u32();
    }

    uint32_t next_u32()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot        = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    float next_float() { return static_cast<float>(next_u32()) * 2.3283064365386963e-10f; }

    uint32_t next_below(uint32_t bound)
    {
        if (bound == 0) return 0;
        const uint32_t threshold = (~bound + 1u) % bound;
        for (;;) {
            const uint32_t r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }
};

// Cosine weighted hemisphere sample around +Z.
inline Vec3 sample_cosine_hemisphere(float u1, float u2)
{
    const float r     = std::sqrt(u1);
    const float theta = kTwoPi * u2;
    return {r * std::cos(theta), r * std::sin(theta), std::sqrt(std::max(0.0f, 1.0f - u1))};
}

} // namespace rd
