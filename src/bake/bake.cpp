#include "bake/bake.h"

#include "core/log.h"
#include "mesh/topology.h"
#include "core/thread_pool.h"
#include "core/util.h"

#include <xatlas.h>

#include <algorithm>
#include <functional>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace rd {
namespace {

struct TexelJob {
    int      x, y;
    Vec3     position;
    Vec3     normal;
    uint32_t triangle;
    // Where this texel sits in the source layout, when the source layout was
    // carried over. Interpolated at rasterisation time, which costs nothing:
    // the barycentrics are already in hand.
    Vec2     source_uv{0.0f, 0.0f};
    bool     has_source_uv = false;
};

// The albedo at a point on the high poly, given the barycentric weights of the
// triangle that was hit.
//
// The original artwork first, when the asset brought any: interpolate the
// source uv the same way as any other vertex attribute and read the material's
// base colour map through it. That is the whole trick, and it is worth saying
// why it is this cheap. The bake already has to answer "which point of the high
// poly does this texel of the low poly correspond to", because that is what the
// ambient occlusion and the light rig need. Once that question is answered, the
// source uv is just another attribute sitting on the triangle it landed on.
//
// Vertex colours are the fallback, and a flat white is the fallback to that, so
// a mesh with neither still bakes - it just bakes lighting onto nothing.
Vec3 albedo_at(const Mesh& source, uint32_t triangle, float w0, float w1, float w2)
{
    const uint32_t i0 = source.indices[triangle * 3 + 0];
    const uint32_t i1 = source.indices[triangle * 3 + 1];
    const uint32_t i2 = source.indices[triangle * 3 + 2];

    if (source.has_uvs()) {
        if (const SourceMaterial* mat = source.material_for(triangle)) {
            if (!mat->base_color.empty()) {
                const Vec2 uv = source.corner_uv(triangle, 0) * w0 +
                                source.corner_uv(triangle, 1) * w1 +
                                source.corner_uv(triangle, 2) * w2;
                const Vec4 c  = mat->base_color.sample(uv);
                return {saturate(c.x * mat->base_factor.x), saturate(c.y * mat->base_factor.y),
                        saturate(c.z * mat->base_factor.z)};
            }
        }
    }

    if (!source.has_colors()) return Vec3{1.0f, 1.0f, 1.0f};
    const Vec4 c = source.colors[i0] * w0 + source.colors[i1] * w1 + source.colors[i2] * w2;
    return {saturate(c.x), saturate(c.y), saturate(c.z)};
}

Vec3 sample_source_color(const Mesh& source, const RayHit& hit)
{
    if (!hit.hit()) return Vec3{1.0f, 1.0f, 1.0f};
    return albedo_at(source, hit.triangle, 1.0f - hit.u - hit.v, hit.u, hit.v);
}

// Barycentric weights of a point against the triangle it was projected onto.
// Saturated, because a closest point query can land marginally outside.
void barycentric_at(const Mesh& source, const ClosestHit& hit, float& w0, float& w1, float& w2)
{
    const uint32_t i0 = source.indices[hit.triangle * 3 + 0];
    const uint32_t i1 = source.indices[hit.triangle * 3 + 1];
    const uint32_t i2 = source.indices[hit.triangle * 3 + 2];

    const Vec3 a = source.positions[i0], b = source.positions[i1], c = source.positions[i2];
    const Vec3 v0 = b - a, v1 = c - a, v2 = hit.point - a;
    const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    const float d20 = dot(v2, v0), d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    float u = 0.0f, v = 0.0f;
    if (std::fabs(denom) > 1e-16f) {
        u = (d11 * d20 - d01 * d21) / denom;
        v = (d00 * d21 - d01 * d20) / denom;
    }
    w1 = saturate(u);
    w2 = saturate(v);
    w0 = saturate(1.0f - u - v);
}

Vec3 sample_source_color_at(const Mesh& source, const ClosestHit& hit)
{
    if (!hit.hit()) return Vec3{1.0f, 1.0f, 1.0f};
    float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f;
    barycentric_at(source, hit, w0, w1, w2);
    return albedo_at(source, hit.triangle, w0, w1, w2);
}

// Half space triangle rasteriser over the UV domain, with a conservative
// half texel bleed so chart edges are covered.
template <typename Fn>
void rasterise_uv_triangle(const Vec2& a, const Vec2& b, const Vec2& c,
                           int width, int height, const Fn& emit)
{
    const float min_x = std::floor(std::min({a.x, b.x, c.x})) - 1.0f;
    const float max_x = std::ceil(std::max({a.x, b.x, c.x})) + 1.0f;
    const float min_y = std::floor(std::min({a.y, b.y, c.y})) - 1.0f;
    const float max_y = std::ceil(std::max({a.y, b.y, c.y})) + 1.0f;

    const int x0 = std::max(0, int(min_x));
    const int x1 = std::min(width - 1, int(max_x));
    const int y0 = std::max(0, int(min_y));
    const int y1 = std::min(height - 1, int(max_y));
    if (x1 < x0 || y1 < y0) return;

    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-12f) return;
    const float inv_area = 1.0f / area;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const Vec2 p{float(x) + 0.5f, float(y) + 0.5f};
            float w0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) * inv_area;
            float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            // The tolerance is what covers the seam texels.
            const float tol = -0.75f / std::max(1.0f, std::fabs(area));
            if (w0 < tol || w1 < tol || w2 < tol) continue;
            w0 = saturate(w0); w1 = saturate(w1); w2 = saturate(w2);
            const float sum = w0 + w1 + w2;
            if (sum <= kEps) continue;
            emit(x, y, w0 / sum, w1 / sum, w2 / sum);
        }
    }
}

} // namespace

std::vector<BakeLight> default_light_rig()
{
    // Key from the upper front left, cool fill from the opposite side, warm
    // bounce from below. Standard three point setup, baked flat.
    // Intensities are chosen so key + fill + rim + ambient peaks at roughly 1.0
    // on a surface facing the key. Anything hotter clips to white and throws
    // away exactly the shading information the bake exists to capture.
    return {
        {normalize(Vec3{-0.45f, 0.75f, 0.50f}), {1.00f, 0.97f, 0.90f}, 0.62f, true},
        {normalize(Vec3{0.70f, 0.20f, 0.35f}),  {0.62f, 0.70f, 0.85f}, 0.24f, false},
        {normalize(Vec3{0.05f, -0.80f, -0.40f}), {0.85f, 0.75f, 0.62f}, 0.14f, false},
    };
}

// ---------------------------------------------------------------------------
// Carrying the source uv layout across
// ---------------------------------------------------------------------------
namespace {

// Barycentric weights of `p` against a source triangle, unclamped so a point
// outside the triangle extrapolates the same linear mapping. That is the whole
// point: one low poly triangle covers many source triangles, and extrapolating
// one of them is the linear approximation of the uv function over that patch.
void barycentric_unclamped(const Mesh& source, uint32_t tri, const Vec3& p,
                           float& w0, float& w1, float& w2)
{
    const uint32_t i0 = source.indices[tri * 3 + 0];
    const uint32_t i1 = source.indices[tri * 3 + 1];
    const uint32_t i2 = source.indices[tri * 3 + 2];
    const Vec3 a = source.positions[i0], b = source.positions[i1], c = source.positions[i2];

    const Vec3  v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    const float d20 = dot(v2, v0), d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) <= 1e-16f) { w0 = 1.0f; w1 = w2 = 0.0f; return; }
    w1 = (d11 * d20 - d01 * d21) / denom;
    w2 = (d00 * d21 - d01 * d20) / denom;
    w0 = 1.0f - w1 - w2;
}

Vec2 uv_on_triangle(const Mesh& source, uint32_t tri, float w0, float w1, float w2)
{
    return source.corner_uv(tri, 0) * w0 + source.corner_uv(tri, 1) * w1 +
           source.corner_uv(tri, 2) * w2;
}

// Which uv island each source triangle belongs to. Two triangles sharing an
// edge are in the same island when they agree on the uv at both ends of it; a
// uv seam is precisely where they do not.
std::vector<uint32_t> source_uv_islands(const Mesh& source, const MeshTopology& topo)
{
    const size_t tcount = source.triangle_count();
    std::vector<uint32_t> parent(tcount);
    for (uint32_t i = 0; i < tcount; ++i) parent[i] = i;

    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[b] = a;
    };

    constexpr float kUvSame = 1e-5f;
    for (size_t t = 0; t < tcount; ++t) {
        for (int c = 0; c < 3; ++c) {
            const uint32_t n = topo.neighbour(uint32_t(t), c);
            if (n == kInvalidIndex || n < t) continue;

            // The shared edge runs from corner c to corner c+1 on this side.
            const uint32_t a = source.indices[t * 3 + c];
            const uint32_t b = source.indices[t * 3 + (c + 1) % 3];

            bool agree = true;
            for (int d = 0; d < 3 && agree; ++d) {
                const uint32_t nv = source.indices[n * 3 + d];
                if (nv != a && nv != b) continue;
                const Vec2 mine = source.corner_uv(t, (source.indices[t * 3 + 0] == nv) ? 0 :
                                                      (source.indices[t * 3 + 1] == nv) ? 1 : 2);
                const Vec2 theirs = source.corner_uv(n, d);
                if (std::fabs(mine.x - theirs.x) > kUvSame ||
                    std::fabs(mine.y - theirs.y) > kUvSame)
                    agree = false;
            }
            if (agree) unite(uint32_t(t), n);
        }
    }

    std::vector<uint32_t> island(tcount);
    std::unordered_map<uint32_t, uint32_t> sizes;
    for (uint32_t i = 0; i < tcount; ++i) { island[i] = find(i); ++sizes[island[i]]; }
    RD_DEBUG("source uv layout: %zu islands over %zu triangles", sizes.size(), tcount);
    return island;
}

// How far apart two uvs may be and still plausibly belong to the same island.
// A chart that spans more than this much of the uv square is rare, and a jump
// larger than it means the three corners landed on different islands - across
// a mirror seam, or on two bits of costume that sit next to each other in space
// and nowhere near each other in the layout.
constexpr float kSameIslandSpan = 0.25f;

} // namespace

UvTransferResult transfer_source_uvs(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                                     float search_distance)
{
    UvTransferResult res;
    res.vertices_before = mesh.vertex_count();

    if (mesh.empty())          { res.error = "mesh is empty"; return res; }
    if (!source.has_uvs())     { res.error = "the source has no uvs to carry over"; return res; }
    if (!source.has_materials()) { res.error = "the source has no materials"; return res; }

    const size_t tcount = mesh.triangle_count();

    // Islands of the source layout, and the adjacency of both meshes. The
    // smoothing pass below is the whole reason this is worth computing: without
    // it neighbouring low poly triangles pick neighbouring but *different*
    // source islands, every disagreement becomes a chart border, and a nine
    // hundred triangle mesh comes back as two hundred charts whose seams cost
    // more vertices than the geometry does.
    MeshTopology source_topo, mesh_topo;
    source_topo.build(source);
    mesh_topo.build(mesh);
    const std::vector<uint32_t> source_island = source_uv_islands(source, source_topo);

    // One uv and one material per corner, resolved before anything is rebuilt.
    std::vector<Vec2>     corner_uv(tcount * 3, Vec2{0.0f, 0.0f});
    std::vector<uint16_t> tri_material(tcount, 0);
    std::vector<uint8_t>  tri_state(tcount, 0);   // 0 unmapped, 1 direct, 2 extrapolated
    std::vector<uint32_t> tri_source(tcount, kInvalidIndex);  // the source triangle to read from

    ThreadPool::shared().parallel_ranges(tcount, 32, [&](size_t begin, size_t end, unsigned) {
        for (size_t t = begin; t < end; ++t) {
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            const Vec3 p[3] = {mesh.positions[i0], mesh.positions[i1], mesh.positions[i2]};

            // Each corner asks for itself first. When the three answers agree -
            // same material, uvs close enough to be one island - that is the
            // most faithful mapping available, because every corner is reading
            // the source right underneath it.
            ClosestHit hit[3];
            Vec2       uv[3];
            uint16_t   mat[3] = {0, 0, 0};
            bool       all_hit = true;
            for (int c = 0; c < 3; ++c) {
                hit[c] = source_bvh.closest_point(p[c], search_distance);
                if (!hit[c].hit()) { all_hit = false; break; }
                float w0, w1, w2;
                barycentric_unclamped(source, hit[c].triangle, p[c], w0, w1, w2);
                uv[c]  = uv_on_triangle(source, hit[c].triangle, w0, w1, w2);
                mat[c] = source.tri_material[hit[c].triangle];
            }

            bool consistent = all_hit && mat[0] == mat[1] && mat[1] == mat[2];
            if (consistent) {
                for (int a = 0; a < 3 && consistent; ++a) {
                    const Vec2 d = uv[a] - uv[(a + 1) % 3];
                    if (std::fabs(d.x) > kSameIslandSpan || std::fabs(d.y) > kSameIslandSpan)
                        consistent = false;
                }
            }

            if (consistent) {
                for (int c = 0; c < 3; ++c) corner_uv[t * 3 + c] = uv[c];
                tri_material[t] = mat[0];
                tri_source[t]   = hit[0].triangle;
                tri_state[t]    = 1;
                continue;
            }

            // They disagreed. Fall back to the source triangle under the
            // centroid and extrapolate all three corners from it: the triangle
            // then lies inside one island by construction, which is what keeps
            // it from stretching across the whole atlas.
            const Vec3 centroid = (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
            const ClosestHit mid = source_bvh.closest_point(centroid, search_distance);
            if (!mid.hit()) {
                tri_state[t] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                float w0, w1, w2;
                barycentric_unclamped(source, mid.triangle, p[c], w0, w1, w2);
                corner_uv[t * 3 + c] = uv_on_triangle(source, mid.triangle, w0, w1, w2);
            }
            tri_material[t] = source.tri_material[mid.triangle];
            tri_source[t]   = mid.triangle;
            tri_state[t]    = 2;
        }
    });

    // Smooth the island choice across the low poly. A triangle whose three
    // edge neighbours mostly sit on one island joins them, provided reading
    // from that island does not stretch it across the layout. Repeated until
    // it settles, which takes a handful of passes on a console budget mesh.
    size_t switched_total = 0;
    for (int pass = 0; pass < 6; ++pass) {
        size_t switched = 0;
        for (size_t t = 0; t < tcount; ++t) {
            if (tri_source[t] == kInvalidIndex) continue;
            const uint32_t mine = source_island[tri_source[t]];

            uint32_t best_tri = kInvalidIndex;
            int      best_votes = 0;
            for (int c = 0; c < 3; ++c) {
                const uint32_t n = mesh_topo.neighbour(uint32_t(t), c);
                if (n == kInvalidIndex || tri_source[n] == kInvalidIndex) continue;
                const uint32_t their_island = source_island[tri_source[n]];
                if (their_island == mine) { best_votes = 0; break; }   // already agreeing

                int votes = 0;
                for (int d = 0; d < 3; ++d) {
                    const uint32_t m = mesh_topo.neighbour(uint32_t(t), d);
                    if (m != kInvalidIndex && tri_source[m] != kInvalidIndex &&
                        source_island[tri_source[m]] == their_island)
                        ++votes;
                }
                if (votes > best_votes) { best_votes = votes; best_tri = tri_source[n]; }
            }
            if (best_votes < 2 || best_tri == kInvalidIndex) continue;

            // Only if reading from there keeps the triangle compact in uv.
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            const Vec3 p[3] = {mesh.positions[i0], mesh.positions[i1], mesh.positions[i2]};
            Vec2 candidate[3];
            for (int c = 0; c < 3; ++c) {
                float w0, w1, w2;
                barycentric_unclamped(source, best_tri, p[c], w0, w1, w2);
                candidate[c] = uv_on_triangle(source, best_tri, w0, w1, w2);
            }
            bool compact = true;
            for (int a = 0; a < 3 && compact; ++a) {
                const Vec2 d = candidate[a] - candidate[(a + 1) % 3];
                if (std::fabs(d.x) > kSameIslandSpan || std::fabs(d.y) > kSameIslandSpan)
                    compact = false;
            }
            if (!compact) continue;

            for (int c = 0; c < 3; ++c) corner_uv[t * 3 + c] = candidate[c];
            tri_material[t] = source.tri_material[best_tri];
            tri_source[t]   = best_tri;
            tri_state[t]    = 2;
            ++switched;
        }
        switched_total += switched;
        if (switched == 0) break;
    }
    if (switched_total > 0)
        RD_DEBUG("uv transfer: %zu triangles joined an island their neighbours were already on",
                 switched_total);

    for (size_t t = 0; t < tcount; ++t) {
        if      (tri_state[t] == 1) ++res.triangles_mapped;
        else if (tri_state[t] == 2) ++res.triangles_fallback;
        else                        ++res.triangles_unmapped;
    }
    if (res.triangles_mapped + res.triangles_fallback == 0) {
        res.error = "no triangle of the low poly found the source";
        return res;
    }

    // Rebuild so a corner can carry its own uv. Corners that agree on both the
    // original vertex and the uv are merged back, which is most of them inside
    // an island and none of them across a seam.
    struct Key {
        uint32_t vertex;
        float    u, v;
        bool operator==(const Key& o) const { return vertex == o.vertex && u == o.u && v == o.v; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const
        {
            uint32_t bu, bv;
            std::memcpy(&bu, &k.u, 4);
            std::memcpy(&bv, &k.v, 4);
            return (size_t(k.vertex) * 0x9E3779B97F4A7C15ull) ^
                   (size_t(bu) * 0xC2B2AE3D27D4EB4Full) ^ (size_t(bv) * 0x165667B19E3779F9ull);
        }
    };

    Mesh out;
    out.name             = mesh.name;
    out.armature         = mesh.armature;
    out.import_transform = mesh.import_transform;
    out.import_scale     = mesh.import_scale;
    out.materials        = source.materials;

    const bool keep_n = mesh.has_normals();
    const bool keep_c = mesh.has_colors();
    const bool keep_s = mesh.has_skin();
    const bool keep_r = mesh.tri_region.size() == tcount;

    std::unordered_map<Key, uint32_t, KeyHash> unique;
    unique.reserve(mesh.vertex_count() * 2);

    for (size_t t = 0; t < tcount; ++t) {
        if (tri_state[t] == 0) continue;          // nothing to map it to, drop it
        for (int c = 0; c < 3; ++c) {
            const uint32_t src = mesh.indices[t * 3 + c];
            const Vec2     uvc = corner_uv[t * 3 + c];
            const Key      key{src, uvc.x, uvc.y};

            const auto it = unique.find(key);
            if (it != unique.end()) { out.indices.push_back(it->second); continue; }

            const uint32_t nv = uint32_t(out.positions.size());
            unique.emplace(key, nv);
            out.positions.push_back(mesh.positions[src]);
            out.uvs.push_back(uvc);
            if (keep_n) out.normals.push_back(mesh.normals[src]);
            if (keep_c) out.colors.push_back(mesh.colors[src]);
            if (keep_s) out.skin.push_back(mesh.skin[src]);
            out.indices.push_back(nv);
        }
        out.tri_material.push_back(tri_material[t]);
        if (keep_r) out.tri_region.push_back(mesh.tri_region[t]);
    }

    res.vertices_after = out.positions.size();
    res.ok             = true;
    mesh               = std::move(out);

    RD_INFO("uv transfer: %zu triangles direct, %zu extrapolated, %zu dropped; "
            "%zu -> %zu vertices",
            res.triangles_mapped, res.triangles_fallback, res.triangles_unmapped,
            res.vertices_before, res.vertices_after);
    return res;
}

UnwrapResult repack_uvs(Mesh& mesh, std::vector<Vec2>* carried, int width, int height,
                        int padding)
{
    UnwrapResult res;
    if (mesh.empty() || !mesh.has_uvs()) {
        res.error = "nothing to repack";
        return res;
    }

    xatlas::Atlas* atlas = xatlas::Create();

    xatlas::UvMeshDecl decl;
    decl.vertexCount   = uint32_t(mesh.vertex_count());
    decl.vertexUvData  = mesh.uvs.data();
    decl.vertexStride  = sizeof(Vec2);
    decl.indexCount    = uint32_t(mesh.indices.size());
    decl.indexData     = mesh.indices.data();
    decl.indexFormat   = xatlas::IndexFormat::UInt32;

    // Without this xatlas is free to decide that the left arm and the right arm
    // are one chart, because on a mirrored character they occupy the same uvs.
    // They have to be packed apart or the ambient occlusion of one gets written
    // over the other.
    std::vector<uint32_t> face_material;
    if (mesh.tri_material.size() == mesh.triangle_count()) {
        face_material.assign(mesh.tri_material.begin(), mesh.tri_material.end());
        decl.faceMaterialData = face_material.data();
    }

    const xatlas::AddMeshError err = xatlas::AddUvMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        res.error = std::string("xatlas rejected the uv mesh: ") + xatlas::StringForEnum(err);
        xatlas::Destroy(atlas);
        return res;
    }

    // Charts still have to be found, even though nothing is being
    // parameterised: for a uv mesh this segments the existing layout into
    // islands by uv connectivity and by material. PackCharts does nothing
    // without it.
    xatlas::ComputeCharts(atlas);

    xatlas::PackOptions pack;
    pack.padding    = uint32_t(std::max(0, padding));
    pack.resolution = uint32_t(std::max(8, width));
    pack.bilinear   = true;
    pack.blockAlign = false;
    pack.bruteForce = true;
    xatlas::PackCharts(atlas, pack);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) {
        res.error = "repacking produced an empty atlas";
        xatlas::Destroy(atlas);
        return res;
    }

    const xatlas::Mesh& out = atlas->meshes[0];

    // The repack can duplicate a vertex that sits on a chart border, so every
    // per vertex array has to follow the xref the same way the unwrap does.
    std::vector<Vec2> old_uv = mesh.uvs;
    Mesh rebuilt;
    rebuilt.name             = mesh.name;
    rebuilt.armature         = mesh.armature;
    rebuilt.import_transform = mesh.import_transform;
    rebuilt.import_scale     = mesh.import_scale;
    rebuilt.materials        = mesh.materials;
    rebuilt.tri_region       = mesh.tri_region;     // face order survives packing
    rebuilt.tri_material     = mesh.tri_material;
    rebuilt.positions.resize(out.vertexCount);
    rebuilt.uvs.resize(out.vertexCount);
    if (mesh.has_normals()) rebuilt.normals.resize(out.vertexCount);
    if (mesh.has_colors())  rebuilt.colors.resize(out.vertexCount);
    if (mesh.has_skin())    rebuilt.skin.resize(out.vertexCount);

    std::vector<Vec2> carried_out;
    if (carried) carried_out.resize(out.vertexCount);

    const float inv_w = 1.0f / float(atlas->width);
    const float inv_h = 1.0f / float(atlas->height);

    for (uint32_t v = 0; v < out.vertexCount; ++v) {
        const xatlas::Vertex& xv = out.vertexArray[v];
        const uint32_t src = xv.xref;
        rebuilt.positions[v] = mesh.positions[src];
        rebuilt.uvs[v]       = {xv.uv[0] * inv_w, xv.uv[1] * inv_h};
        if (!rebuilt.normals.empty()) rebuilt.normals[v] = mesh.normals[src];
        if (!rebuilt.colors.empty())  rebuilt.colors[v]  = mesh.colors[src];
        if (!rebuilt.skin.empty())    rebuilt.skin[v]    = mesh.skin[src];
        if (carried) carried_out[v] = old_uv[src];
    }
    rebuilt.indices.assign(out.indexArray, out.indexArray + out.indexCount);

    res.charts         = int(out.chartCount);
    res.atlas_count    = int(atlas->atlasCount);
    res.utilisation    = atlas->atlasCount > 0 ? atlas->utilization[0] : 0.0f;
    res.added_vertices = out.vertexCount > mesh.vertex_count()
                             ? out.vertexCount - mesh.vertex_count() : 0;
    res.ok = true;

    xatlas::Destroy(atlas);
    mesh = std::move(rebuilt);
    if (carried) *carried = std::move(carried_out);

    RD_INFO("uv repack: %d charts from the source layout, %d atlas pages, %.1f%% utilised, "
            "+%zu seam vertices",
            res.charts, res.atlas_count, res.utilisation * 100.0f, res.added_vertices);
    return res;
}

// ---------------------------------------------------------------------------
UnwrapResult unwrap_uvs(Mesh& mesh, int width, int height, int padding,
                        float stretch_tolerance)
{
    UnwrapResult res;
    if (mesh.empty()) {
        res.error = "mesh is empty";
        return res;
    }

    xatlas::Atlas* atlas = xatlas::Create();

    xatlas::MeshDecl decl;
    decl.vertexCount          = uint32_t(mesh.vertex_count());
    decl.vertexPositionData   = mesh.positions.data();
    decl.vertexPositionStride = sizeof(Vec3);
    // Normals are deliberately withheld. Ours are hard-edged for shading, and
    // xatlas would treat every crease as a normal seam and cut a chart there.
    // On a thousand-triangle mesh that is most of the edges.
    decl.indexCount  = uint32_t(mesh.indices.size());
    decl.indexData   = mesh.indices.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;

    const xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        res.error = std::string("xatlas rejected the mesh: ") + xatlas::StringForEnum(err);
        xatlas::Destroy(atlas);
        return res;
    }
    xatlas::AddMeshJoin(atlas);

    xatlas::ChartOptions chart;
    // A console budget mesh has a thousand triangles, not a million. xatlas'
    // defaults are tuned for dense scans and would shatter this into a hundred
    // ten-triangle charts, and every chart border duplicates its vertices - which
    // is what actually blows the vertex budget. Big charts, few seams.
    chart.maxCost               = 8.0f + 160.0f * clampf(stretch_tolerance, 0.01f, 1.0f);
    chart.maxIterations         = 4;
    chart.normalDeviationWeight = 0.5f;
    chart.roundnessWeight       = 0.02f;
    chart.straightnessWeight    = 1.0f;
    chart.normalSeamWeight      = 0.1f;
    chart.textureSeamWeight     = 0.25f;

    xatlas::PackOptions pack;
    pack.padding    = uint32_t(std::max(0, padding));
    pack.resolution = uint32_t(std::max(8, width));
    pack.bilinear   = true;
    pack.blockAlign = false;
    pack.bruteForce = true;

    xatlas::Generate(atlas, chart, pack);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) {
        res.error = "xatlas produced an empty atlas";
        xatlas::Destroy(atlas);
        return res;
    }

    const xatlas::Mesh& out = atlas->meshes[0];

    Mesh rebuilt;
    rebuilt.name             = mesh.name;
    rebuilt.armature         = mesh.armature;
    rebuilt.import_transform = mesh.import_transform;
    rebuilt.import_scale     = mesh.import_scale;
    rebuilt.positions.resize(out.vertexCount);
    rebuilt.uvs.resize(out.vertexCount);
    if (mesh.has_normals()) rebuilt.normals.resize(out.vertexCount);
    if (mesh.has_colors())  rebuilt.colors.resize(out.vertexCount);
    if (mesh.has_skin())    rebuilt.skin.resize(out.vertexCount);

    const float inv_w = 1.0f / float(atlas->width);
    const float inv_h = 1.0f / float(atlas->height);

    for (uint32_t v = 0; v < out.vertexCount; ++v) {
        const xatlas::Vertex& xv = out.vertexArray[v];
        const uint32_t src = xv.xref;
        rebuilt.positions[v] = mesh.positions[src];
        rebuilt.uvs[v]       = {xv.uv[0] * inv_w, xv.uv[1] * inv_h};
        if (!rebuilt.normals.empty()) rebuilt.normals[v] = mesh.normals[src];
        if (!rebuilt.colors.empty())  rebuilt.colors[v]  = mesh.colors[src];
        if (!rebuilt.skin.empty())    rebuilt.skin[v]    = mesh.skin[src];
    }

    rebuilt.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    if (mesh.tri_region.size() == mesh.triangle_count())
        rebuilt.tri_region = mesh.tri_region;   // face order is preserved by xatlas

    // xatlas splits a vertex per chart it belongs to, but a vertex interior to
    // one chart can still come back duplicated. Every duplicate is a vertex the
    // console has to transform, and on this budget that is the binding limit,
    // so merge back anything whose position and uv are identical.
    {
        struct Key {
            uint32_t src;
            float    u, v;
            bool operator==(const Key& o) const { return src == o.src && u == o.u && v == o.v; }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const
            {
                uint32_t bu, bv;
                std::memcpy(&bu, &k.u, 4);
                std::memcpy(&bv, &k.v, 4);
                return (size_t(k.src) * 0x9E3779B97F4A7C15ull) ^
                       (size_t(bu) * 0xC2B2AE3D27D4EB4Full) ^ (size_t(bv) * 0x165667B19E3779F9ull);
            }
        };

        std::unordered_map<Key, uint32_t, KeyHash> unique;
        unique.reserve(rebuilt.positions.size());
        std::vector<uint32_t> remap(rebuilt.positions.size(), kInvalidIndex);

        Mesh merged;
        merged.name             = rebuilt.name;
        merged.armature         = rebuilt.armature;
        merged.import_transform = rebuilt.import_transform;
        merged.import_scale     = rebuilt.import_scale;
        merged.tri_region       = rebuilt.tri_region;

        const bool keep_n = !rebuilt.normals.empty();
        const bool keep_c = !rebuilt.colors.empty();
        const bool keep_s = !rebuilt.skin.empty();

        for (uint32_t v = 0; v < rebuilt.positions.size(); ++v) {
            const Key key{out.vertexArray[v].xref, rebuilt.uvs[v].x, rebuilt.uvs[v].y};
            const auto it = unique.find(key);
            if (it != unique.end()) { remap[v] = it->second; continue; }

            const uint32_t nv = uint32_t(merged.positions.size());
            remap[v] = nv;
            unique.emplace(key, nv);
            merged.positions.push_back(rebuilt.positions[v]);
            merged.uvs.push_back(rebuilt.uvs[v]);
            if (keep_n) merged.normals.push_back(rebuilt.normals[v]);
            if (keep_c) merged.colors.push_back(rebuilt.colors[v]);
            if (keep_s) merged.skin.push_back(rebuilt.skin[v]);
        }

        if (merged.positions.size() < rebuilt.positions.size()) {
            merged.indices.reserve(rebuilt.indices.size());
            for (uint32_t i : rebuilt.indices) merged.indices.push_back(remap[i]);
            RD_DEBUG("uv weld: %zu -> %zu vertices", rebuilt.positions.size(),
                     merged.positions.size());
            rebuilt = std::move(merged);
        }
    }

    res.charts         = int(out.chartCount);
    res.atlas_count    = int(atlas->atlasCount);
    res.utilisation    = atlas->atlasCount > 0 ? atlas->utilization[0] : 0.0f;
    res.added_vertices = out.vertexCount > mesh.vertex_count()
                             ? out.vertexCount - mesh.vertex_count() : 0;
    res.ok = true;

    xatlas::Destroy(atlas);
    mesh = std::move(rebuilt);

    RD_INFO("uv unwrap: %d charts, %d atlas pages, %.1f%% utilised, +%zu seam vertices",
            res.charts, res.atlas_count, res.utilisation * 100.0f, res.added_vertices);
    return res;
}

// ---------------------------------------------------------------------------
BakeResult bake_all(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                    const MeshAnalysis& source_analysis, const TargetProfile& profile,
                    const GlobalKnobs& knobs, const BakeOptions& opts,
                    const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    BakeResult result;

    if (mesh.empty()) {
        result.error = "no low poly to bake";
        return result;
    }

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

    const int width   = opts.texture_width  > 0 ? opts.texture_width  : profile.texture.width;
    const int height  = opts.texture_height > 0 ? opts.texture_height : profile.texture.height;
    const int padding = std::max(0, opts.padding_texels > 0
                                        ? opts.padding_texels
                                        : int(std::lround(knobs.uv_padding_texels)));

    // --- 1. unwrap ----------------------------------------------------------
    // Two ways to get a uv onto the low poly. Carrying the source layout over
    // keeps the artwork where the artist put it, so a texel of the atlas maps
    // to a texel of the original through one affine step per chart instead of
    // being reconstructed by nearest surface queries. It needs a source that
    // has uvs and materials; failing that, or failing at all, the fresh unwrap
    // is what has always run.
    std::vector<Vec2> source_uv;
    UnwrapResult      uv;
    bool              carried_layout = false;

    const bool want_transfer = knobs.reuse_source_uvs && source.has_uvs() &&
                               source.has_materials();
    if (want_transfer) {
        report(0.02f, "carrying the source uvs over");
        const float transfer_search =
            std::max(source_analysis.bbox_diagonal, kEps) *
            clampf(opts.projection_distance_rel, 0.001f, 1.0f) * 4.0f;

        Mesh candidate = mesh;
        const UvTransferResult transferred =
            transfer_source_uvs(candidate, source, source_bvh, transfer_search);
        if (transferred.ok) {
            report(0.06f, "repacking the source uvs");
            const UnwrapResult packed = repack_uvs(candidate, &source_uv, width, height, padding);

            // The layout is only worth keeping if the asset can afford it.
            //
            // Every chart border duplicates the vertices along it, and a source
            // layout is cut for the resolution it was authored at: this
            // character's is 107 islands over 3202 triangles, which a nine
            // hundred triangle version cannot carry - it comes back as two
            // hundred charts and twice the vertices, and on this hardware the
            // vertex count is the limit that actually binds. So the transfer
            // has to answer to the profile like everything else, and hand back
            // to the unwrap when it cannot.
            //
            // Fitting is not enough either. A layout that fits by tripling the
            // vertex count has spent the vertex limit on seams, and the budget
            // re-fit then stops with two thirds of the triangle budget unused,
            // because the vertex limit is the one that binds. A fresh unwrap
            // costs 30 to 45 per cent; twice the welded count is as much as
            // keeping the artist's layout is worth.
            const float overhead =
                float(candidate.vertex_count()) / float(std::max<size_t>(1, mesh.vertex_count()));
            constexpr float kMaxCarriedOverhead = 2.0f;
            const bool affordable =
                packed.ok && profile.max_vertices > 0 &&
                candidate.vertex_count() <= size_t(profile.max_vertices) &&
                overhead <= kMaxCarriedOverhead;

            if (packed.ok && !affordable) {
                RD_INFO("the source uv layout needs %zu vertices (%.1fx the welded %zu) against "
                        "a budget of %d (%d charts); unwrapping fresh instead",
                        candidate.vertex_count(), overhead, mesh.vertex_count(),
                        profile.max_vertices, packed.charts);
                result.messages.push_back(
                    format("kept the generated uv layout: carrying the source layout over would "
                           "have cost %zu vertices of %d allowed",
                           candidate.vertex_count(), profile.max_vertices));
                source_uv.clear();
            } else if (packed.ok) {
                mesh           = std::move(candidate);
                uv             = packed;
                carried_layout = true;
                if (transferred.triangles_unmapped > 0)
                    result.messages.push_back(
                        format("%zu triangles found nothing of the source within reach and were "
                               "dropped", transferred.triangles_unmapped));
            } else {
                RD_WARN("repacking the source layout failed (%s); unwrapping instead",
                        packed.error.c_str());
                source_uv.clear();
            }
        } else {
            RD_DEBUG("source uvs not carried over (%s); unwrapping instead",
                     transferred.error.c_str());
        }
    }

    if (!carried_layout) {
        report(0.02f, "unwrapping uvs");
        uv = unwrap_uvs(mesh, width, height, padding, knobs.uv_stretch_tolerance);
    }
    if (!uv.ok) {
        result.error = uv.error;
        return result;
    }
    result.charts         = uv.charts;
    result.atlas_count    = uv.atlas_count;
    result.uv_utilisation = uv.utilisation;
    if (uv.atlas_count > profile.texture.count)
        result.messages.push_back(
            format("unwrap needed %d atlas pages but the profile allows %d",
                   uv.atlas_count, profile.texture.count));

    mesh.compute_normals(60.0f);

    // --- 2. collect texel jobs ---------------------------------------------
    report(0.15f, "rasterising the atlas");
    result.diffuse.resize(width, height, 4);
    result.coverage.resize(width, height);

    std::vector<TexelJob> jobs;
    jobs.reserve(size_t(width) * height / 3 + 64);

    const size_t tcount = mesh.triangle_count();
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t i0 = mesh.indices[t * 3 + 0];
        const uint32_t i1 = mesh.indices[t * 3 + 1];
        const uint32_t i2 = mesh.indices[t * 3 + 2];
        const Vec2 a{mesh.uvs[i0].x * width, mesh.uvs[i0].y * height};
        const Vec2 b{mesh.uvs[i1].x * width, mesh.uvs[i1].y * height};
        const Vec2 c{mesh.uvs[i2].x * width, mesh.uvs[i2].y * height};

        rasterise_uv_triangle(a, b, c, width, height,
                              [&](int x, int y, float w0, float w1, float w2) {
                                  if (result.coverage.at(x, y)) return;
                                  result.coverage.set(x, y);
                                  TexelJob job;
                                  job.x = x;
                                  job.y = y;
                                  job.position = mesh.positions[i0] * w0 +
                                                 mesh.positions[i1] * w1 +
                                                 mesh.positions[i2] * w2;
                                  job.normal = normalize(mesh.normals[i0] * w0 +
                                                         mesh.normals[i1] * w1 +
                                                         mesh.normals[i2] * w2);
                                  job.triangle = uint32_t(t);
                                  if (!source_uv.empty()) {
                                      job.source_uv = source_uv[i0] * w0 + source_uv[i1] * w1 +
                                                      source_uv[i2] * w2;
                                      job.has_source_uv = true;
                                  }
                                  jobs.push_back(job);
                              });
    }
    result.texels_baked = jobs.size();

    if (jobs.empty()) {
        result.error = "the unwrap produced no covered texels";
        return result;
    }

    // --- 3. shade -----------------------------------------------------------
    report(0.25f, "casting occlusion rays");

    const float diag        = std::max(source_analysis.bbox_diagonal, kEps);
    const float bias        = diag * opts.ray_bias_rel;
    const float ao_distance = diag * clampf(opts.ao_distance_rel, 0.01f, 2.0f);
    const float search      = diag * clampf(opts.projection_distance_rel, 0.001f, 1.0f);
    const int   ao_rays     = opts.bake_ao && knobs.bake_ambient_occlusion
                                  ? std::max(1, opts.ao_rays) : 0;
    const float ao_strength = clampf(opts.ao_intensity > 0.0f ? opts.ao_intensity
                                                              : knobs.ao_intensity,
                                     0.0f, 1.0f);
    const std::vector<BakeLight> lights = default_light_rig();
    const bool do_lighting = opts.bake_lighting && profile.bake_lighting_to_diffuse;

    std::vector<Vec3> shaded(jobs.size(), Vec3{1.0f, 1.0f, 1.0f});
    std::atomic<uint64_t> rays{0};
    std::atomic<size_t>   done{0};

    ThreadPool::shared().parallel_ranges(
        jobs.size(), 64, [&](size_t begin, size_t end, unsigned lane) {
            Rng rng(0xBA4Eu + uint32_t(begin) * 2654435761u);
            uint64_t local_rays = 0;

            for (size_t i = begin; i < end; ++i) {
                const TexelJob& job = jobs[i];

                // Find where this texel sits on the high poly. Ray along the
                // normal first (that keeps overhangs honest), closest point as
                // a fallback.
                Vec3  base_color = Vec3{1.0f, 1.0f, 1.0f};
                Vec3  surface    = job.position;
                Vec3  normal     = job.normal;

                // When the layout was carried over, the albedo needs no search
                // at all: this texel already knows which source texel it is.
                bool albedo_done = false;
                if (job.has_source_uv) {
                    if (const SourceMaterial* mat = mesh.material_for(job.triangle)) {
                        if (!mat->base_color.empty()) {
                            const Vec4 c = mat->base_color.sample(job.source_uv);
                            base_color = {saturate(c.x * mat->base_factor.x),
                                          saturate(c.y * mat->base_factor.y),
                                          saturate(c.z * mat->base_factor.z)};
                            albedo_done = true;
                        }
                    }
                }

                const RayHit out_hit = source_bvh.intersect(job.position + job.normal * bias,
                                                            job.normal, bias, search);
                const RayHit in_hit  = source_bvh.intersect(job.position - job.normal * bias,
                                                            -job.normal, bias, search);
                local_rays += 2;

                const RayHit* best = nullptr;
                if (out_hit.hit() && in_hit.hit()) best = out_hit.t <= in_hit.t ? &out_hit : &in_hit;
                else if (out_hit.hit())            best = &out_hit;
                else if (in_hit.hit())             best = &in_hit;

                if (best) {
                    const Vec3 dir = (best == &out_hit) ? job.normal : -job.normal;
                    surface    = job.position + dir * best->t;
                    normal     = source_bvh.shading_normal(*best);
                    if (dot(normal, job.normal) < 0.0f) normal = -normal;
                    if (!albedo_done) base_color = sample_source_color(source, *best);
                } else {
                    const ClosestHit hit = source_bvh.closest_point(job.position, search);
                    if (hit.hit()) {
                        surface    = hit.point;
                        normal     = source_bvh.geometric_normal(hit.triangle);
                        if (dot(normal, job.normal) < 0.0f) normal = -normal;
                        base_color = sample_source_color_at(source, hit);
                    }
                }

                const Vec3 origin = surface + normal * bias;

                // Ambient occlusion.
                float ao = 1.0f;
                if (ao_rays > 0) {
                    Vec3 tangent, bitangent;
                    basis_from_normal(normal, tangent, bitangent);
                    int open = 0;
                    for (int r = 0; r < ao_rays; ++r) {
                        const Vec3 local = sample_cosine_hemisphere(rng.next_float(),
                                                                    rng.next_float());
                        const Vec3 dir = tangent * local.x + bitangent * local.y + normal * local.z;
                        if (!source_bvh.occluded(origin, dir, bias, ao_distance)) ++open;
                    }
                    local_rays += uint64_t(ao_rays);
                    ao = float(open) / float(ao_rays);
                    ao = lerpf(1.0f, ao, ao_strength);
                }

                // Direct lighting, flattened into the albedo.
                Vec3 light_sum{0.0f, 0.0f, 0.0f};
                if (do_lighting) {
                    for (const BakeLight& l : lights) {
                        const float ndl = saturate(dot(normal, l.direction));
                        if (ndl <= 0.0f) continue;
                        float shadow = 1.0f;
                        if (l.casts_shadow) {
                            shadow = source_bvh.occluded(origin, l.direction, bias, ao_distance * 2.0f)
                                         ? 0.25f : 1.0f;
                            ++local_rays;
                        }
                        light_sum += l.color * (l.intensity * ndl * shadow);
                    }
                    // Ambient term so nothing goes fully black on a console.
                    // Not scaled by ao here: the whole sum is multiplied by it
                    // below, and occluding the ambient twice drove creases to
                    // near black. That was invisible while the albedo was a
                    // flat white and every texel was lighting; with the source
                    // artwork underneath it crushes anything dark.
                    light_sum += Vec3{0.17f, 0.19f, 0.23f};
                } else {
                    light_sum = Vec3{1.0f, 1.0f, 1.0f};
                }

                Vec3 c = base_color * light_sum * ao;
                shaded[i] = {saturate(c.x), saturate(c.y), saturate(c.z)};
            }

            rays.fetch_add(local_rays, std::memory_order_relaxed);
            const size_t finished = done.fetch_add(end - begin, std::memory_order_relaxed) +
                                    (end - begin);
            if ((finished & 0x3FFF) == 0)
                report(0.25f + 0.50f * float(finished) / float(jobs.size()), "baking");
        });

    result.rays_cast = size_t(rays.load());

    for (size_t i = 0; i < jobs.size(); ++i)
        result.diffuse.set(jobs[i].x, jobs[i].y, Vec4{shaded[i], 1.0f});

    // --- 4. dilate ----------------------------------------------------------
    report(0.80f, "dilating seams");
    dilate(result.diffuse, result.coverage, std::max(1, padding));

    // --- 5. vertex colours --------------------------------------------------
    if (opts.bake_vertex_colors && knobs.bake_vertex_colors) {
        report(0.86f, "vertex colours");
        const size_t vcount = mesh.vertex_count();
        mesh.colors.assign(vcount, Vec4{1, 1, 1, 1});
        mesh.colors_prelit = true;

        ThreadPool::shared().parallel_ranges(vcount, 64, [&](size_t b, size_t e, unsigned) {
            Rng rng(0xC01Fu + uint32_t(b) * 40503u);
            const int rays_per_vertex = ao_rays > 0 ? std::max(8, ao_rays / 3) : 0;

            for (size_t v = b; v < e; ++v) {
                const Vec3 p = mesh.positions[v];
                Vec3 n = mesh.has_normals() ? mesh.normals[v] : Vec3{0, 1, 0};

                Vec3 base{1, 1, 1};
                const ClosestHit hit = source_bvh.closest_point(p, search * 2.0f);
                Vec3 surface = p;
                if (hit.hit()) {
                    surface = hit.point;
                    const Vec3 sn = source_bvh.geometric_normal(hit.triangle);
                    if (dot(sn, n) > 0.0f) n = normalize(n + sn * 0.5f);
                    base = sample_source_color_at(source, hit);
                }
                const Vec3 origin = surface + n * bias;

                float ao = 1.0f;
                if (rays_per_vertex > 0) {
                    Vec3 tangent, bitangent;
                    basis_from_normal(n, tangent, bitangent);
                    int open = 0;
                    for (int r = 0; r < rays_per_vertex; ++r) {
                        const Vec3 local = sample_cosine_hemisphere(rng.next_float(),
                                                                    rng.next_float());
                        const Vec3 dir = tangent * local.x + bitangent * local.y + n * local.z;
                        if (!source_bvh.occluded(origin, dir, bias, ao_distance)) ++open;
                    }
                    ao = lerpf(1.0f, float(open) / float(rays_per_vertex), ao_strength);
                }

                Vec3 light_sum{0, 0, 0};
                if (do_lighting) {
                    for (const BakeLight& l : lights)
                        light_sum += l.color * (l.intensity * saturate(dot(n, l.direction)));
                    light_sum += Vec3{0.17f, 0.19f, 0.23f} * ao;
                } else {
                    light_sum = Vec3{1, 1, 1};
                }

                const Vec3 c = base * light_sum * ao;
                mesh.colors[v] = {saturate(c.x), saturate(c.y), saturate(c.z), 1.0f};
            }
        });
    }

    // --- 6. palette ---------------------------------------------------------
    const int palette_size = opts.palette_colors != 0 ? opts.palette_colors
                                                      : profile.texture.palette_colors;
    if (palette_size >= 2) {
        report(0.94f, "quantising to the palette");
        result.palette = build_palette(result.diffuse, result.coverage, palette_size);
        apply_palette(result.diffuse, result.coverage, result.palette,
                      opts.dither && profile.texture.dithering);
        result.messages.push_back(format("quantised to %zu colours", result.palette.size()));
    }

    // --- 7. uv stretch metric ----------------------------------------------
    // How far texel density strays from the mesh's own average, either way:
    // more texels than average is wasted atlas and shimmer, fewer is blur.
    //
    // A percentile weighted by surface area, not the single worst triangle.
    // The worst triangle is always a sliver a few millimetres across that the
    // packer rounded up to a texel, and it swung this number from 3 to 47 on
    // the same asset between two runs that looked identical. What an artist
    // sees is the surface, so the surface decides: the density ratio that 98%
    // of the model's area is within.
    {
        struct Sample { float deviation, area; };
        std::vector<Sample> samples;
        samples.reserve(tcount);
        double uv_total = 0.0, area_total = 0.0;
        for (size_t t = 0; t < tcount; ++t) {
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            const float area3 = mesh.triangle_area(t);
            const Vec2 a = mesh.uvs[i0], b = mesh.uvs[i1], c = mesh.uvs[i2];
            const float area2 = 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) -
                                                 (b.y - a.y) * (c.x - a.x));
            if (area3 < 1e-12f || area2 < 1e-12f) continue;
            uv_total   += area2;
            area_total += area3;
            samples.push_back({area2 / area3, area3});
        }
        if (!samples.empty() && uv_total > 0.0 && area_total > 0.0) {
            const float mean = float(uv_total / area_total);
            for (Sample& s : samples) s.deviation = std::max(s.deviation / mean, mean / s.deviation);
            std::sort(samples.begin(), samples.end(),
                      [](const Sample& x, const Sample& y) { return x.deviation < y.deviation; });
            const double cutoff = 0.98 * area_total;
            double acc = 0.0;
            for (const Sample& s : samples) {
                acc += s.area;
                result.uv_max_stretch = s.deviation;
                if (acc >= cutoff) break;
            }
        }
    }

    result.ok      = true;
    result.seconds = watch.seconds();
    report(1.0f, "done");

    RD_INFO("bake: %dx%d, %zu texels, %zu rays, %d charts, %.1f%% utilised, %s",
            width, height, result.texels_baked, result.rays_cast, result.charts,
            result.uv_utilisation * 100.0f, format_duration(result.seconds).c_str());
    return result;
}

} // namespace rd
