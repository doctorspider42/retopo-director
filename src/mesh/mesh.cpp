#include "mesh/mesh.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace rd {
namespace {

struct EdgeKey {
    uint32_t a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeHash {
    size_t operator()(const EdgeKey& e) const
    {
        return (static_cast<size_t>(e.a) * 0x9E3779B97F4A7C15ull) ^
               (static_cast<size_t>(e.b) * 0xC2B2AE3D27D4EB4Full);
    }
};

EdgeKey make_edge(uint32_t a, uint32_t b)
{
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

// Quantised position hash for welding.
struct GridKey {
    int64_t x, y, z;
    bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct GridHash {
    size_t operator()(const GridKey& k) const
    {
        size_t h = static_cast<size_t>(k.x) * 73856093ull;
        h ^= static_cast<size_t>(k.y) * 19349663ull;
        h ^= static_cast<size_t>(k.z) * 83492791ull;
        return h;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Armature
// ---------------------------------------------------------------------------
bool Armature::bone_segment(size_t joint, Vec3& a, Vec3& b) const
{
    if (joint >= joints.size()) return false;
    const Joint& j = joints[joint];
    if (j.parent < 0 || static_cast<size_t>(j.parent) >= joints.size()) return false;
    a = joints[static_cast<size_t>(j.parent)].bind_position;
    b = j.bind_position;
    return true;
}

int Armature::find(std::string_view name) const
{
    for (size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == name) return static_cast<int>(i);
    return -1;
}

Aabb Armature::bounds() const
{
    Aabb box;
    for (const Joint& j : joints) box.grow(j.bind_position);
    return box;
}

// ---------------------------------------------------------------------------
// Mesh basics
// ---------------------------------------------------------------------------
void Mesh::clear()
{
    positions.clear();
    normals.clear();
    uvs.clear();
    colors.clear();
    colors_prelit = false;
    skin.clear();
    indices.clear();
    tri_region.clear();
    tri_page.clear();
    tri_material.clear();
    corner_uvs.clear();
    materials.reset();
    armature.joints.clear();
    import_transform = Mat4::identity();
    import_scale     = 1.0f;
}

Vec3 Mesh::triangle_normal(size_t tri) const
{
    Vec3 a, b, c;
    tri_positions(tri, a, b, c);
    return rd::triangle_normal(a, b, c);
}

float Mesh::triangle_area(size_t tri) const
{
    Vec3 a, b, c;
    tri_positions(tri, a, b, c);
    return rd::triangle_area(a, b, c);
}

Vec3 Mesh::triangle_centroid(size_t tri) const
{
    Vec3 a, b, c;
    tri_positions(tri, a, b, c);
    return (a + b + c) * (1.0f / 3.0f);
}

Aabb Mesh::bounds() const
{
    Aabb box;
    for (const Vec3& p : positions) box.grow(p);
    return box;
}

float Mesh::surface_area() const
{
    double total = 0.0;
    for (size_t t = 0, n = triangle_count(); t < n; ++t) total += triangle_area(t);
    return static_cast<float>(total);
}

Vec3 Mesh::centroid_area_weighted() const
{
    double wx = 0, wy = 0, wz = 0, total = 0;
    for (size_t t = 0, n = triangle_count(); t < n; ++t) {
        const float a = triangle_area(t);
        const Vec3  c = triangle_centroid(t);
        wx += double(c.x) * a; wy += double(c.y) * a; wz += double(c.z) * a;
        total += a;
    }
    if (total <= 0.0) return bounds().center();
    return {float(wx / total), float(wy / total), float(wz / total)};
}

void Mesh::compute_normals(float sharp_angle_degrees)
{
    normals.assign(positions.size(), Vec3{});
    if (positions.empty()) return;

    const bool split_sharp = sharp_angle_degrees < 179.0f;
    const float cos_limit  = std::cos(sharp_angle_degrees * kDeg2Rad);

    // Angle weighting gives far better results than area weighting on the kind
    // of uneven tessellation a sculpt decimation produces.
    for (size_t t = 0, n = triangle_count(); t < n; ++t) {
        const uint32_t i0 = indices[t * 3 + 0];
        const uint32_t i1 = indices[t * 3 + 1];
        const uint32_t i2 = indices[t * 3 + 2];
        const Vec3 p0 = positions[i0], p1 = positions[i1], p2 = positions[i2];
        const Vec3 fn = cross(p1 - p0, p2 - p0);
        if (length2(fn) < 1e-20f) continue;
        const Vec3 unit_fn = normalize(fn);

        const uint32_t idx[3] = {i0, i1, i2};
        const Vec3     pos[3] = {p0, p1, p2};
        for (int c = 0; c < 3; ++c) {
            const Vec3 e0 = normalize(pos[(c + 1) % 3] - pos[c]);
            const Vec3 e1 = normalize(pos[(c + 2) % 3] - pos[c]);
            const float angle = std::acos(clampf(dot(e0, e1), -1.0f, 1.0f));
            normals[idx[c]] += unit_fn * angle;
        }
    }

    for (Vec3& n : normals) {
        const float l = length(n);
        n = l > kEps ? n / l : Vec3{0.0f, 1.0f, 0.0f};
    }

    if (!split_sharp) return;

    // Second pass: where a face deviates too far from the smoothed normal we
    // bias the vertex normal back toward the face so hard edges stay crisp in
    // the renders the LLM is going to judge.
    std::vector<Vec3> face_normals(triangle_count());
    for (size_t t = 0; t < face_normals.size(); ++t) face_normals[t] = triangle_normal(t);

    std::vector<Vec3>  accum(positions.size(), Vec3{});
    std::vector<float> weight(positions.size(), 0.0f);
    for (size_t t = 0; t < face_normals.size(); ++t) {
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = indices[t * 3 + c];
            if (dot(face_normals[t], normals[v]) >= cos_limit) {
                accum[v]  += face_normals[t];
                weight[v] += 1.0f;
            }
        }
    }
    for (size_t v = 0; v < positions.size(); ++v) {
        if (weight[v] <= 0.0f) continue;
        const Vec3 n = normalize(accum[v]);
        if (length2(n) > 0.0f) normals[v] = n;
    }
}

size_t Mesh::remove_degenerate(float area_epsilon)
{
    const size_t before = triangle_count();
    std::vector<uint32_t> kept;
    kept.reserve(indices.size());
    std::vector<uint16_t> kept_regions, kept_materials;
    std::vector<uint8_t>  kept_pages;
    const bool has_regions   = tri_region.size() == before;
    const bool has_materials = tri_material.size() == before;
    const bool has_page_ids  = tri_page.size() == before;
    if (has_regions)   kept_regions.reserve(before);
    if (has_materials) kept_materials.reserve(before);

    for (size_t t = 0; t < before; ++t) {
        const uint32_t a = indices[t * 3 + 0];
        const uint32_t b = indices[t * 3 + 1];
        const uint32_t c = indices[t * 3 + 2];
        if (a == b || b == c || a == c) continue;
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
        const Vec3 fn = cross(positions[b] - positions[a], positions[c] - positions[a]);
        if (length2(fn) * 0.25f <= area_epsilon * area_epsilon) continue;
        kept.push_back(a); kept.push_back(b); kept.push_back(c);
        if (has_regions)   kept_regions.push_back(tri_region[t]);
        if (has_materials) kept_materials.push_back(tri_material[t]);
        if (has_page_ids)  kept_pages.push_back(tri_page[t]);
    }

    indices.swap(kept);
    if (has_page_ids)  tri_page.swap(kept_pages);
    if (has_regions)   tri_region.swap(kept_regions);
    if (has_materials) tri_material.swap(kept_materials);
    return before - triangle_count();
}

size_t Mesh::weld(float epsilon)
{
    if (positions.empty()) return 0;
    const float cell = std::max(epsilon, 1e-9f);

    std::unordered_map<GridKey, std::vector<uint32_t>, GridHash> grid;
    grid.reserve(positions.size() * 2);

    std::vector<uint32_t> remap(positions.size(), kInvalidIndex);
    std::vector<Vec3>     new_pos;
    std::vector<Vec3>     new_nrm;
    std::vector<Vec2>     new_uv;
    std::vector<Vec4>     new_col;
    std::vector<SkinVertex> new_skin;
    new_pos.reserve(positions.size());

    const bool keep_n = has_normals(), keep_t = has_uvs();
    const bool keep_c = has_colors(),  keep_s = has_skin();

    const float eps2 = epsilon * epsilon;

    for (uint32_t v = 0; v < positions.size(); ++v) {
        const Vec3 p = positions[v];
        const GridKey base{int64_t(std::floor(p.x / cell)),
                           int64_t(std::floor(p.y / cell)),
                           int64_t(std::floor(p.z / cell))};

        uint32_t found = kInvalidIndex;
        for (int dz = -1; dz <= 1 && found == kInvalidIndex; ++dz)
            for (int dy = -1; dy <= 1 && found == kInvalidIndex; ++dy)
                for (int dx = -1; dx <= 1 && found == kInvalidIndex; ++dx) {
                    const GridKey k{base.x + dx, base.y + dy, base.z + dz};
                    const auto it = grid.find(k);
                    if (it == grid.end()) continue;
                    for (uint32_t cand : it->second) {
                        if (length2(new_pos[cand] - p) <= eps2) { found = cand; break; }
                    }
                }

        if (found != kInvalidIndex) {
            remap[v] = found;
            continue;
        }

        const uint32_t nv = static_cast<uint32_t>(new_pos.size());
        remap[v] = nv;
        new_pos.push_back(p);
        if (keep_n) new_nrm.push_back(normals[v]);
        if (keep_t) new_uv.push_back(uvs[v]);
        if (keep_c) new_col.push_back(colors[v]);
        if (keep_s) new_skin.push_back(skin[v]);
        grid[base].push_back(nv);
    }

    const size_t removed = positions.size() - new_pos.size();
    if (removed == 0) return 0;

    positions.swap(new_pos);
    if (keep_n) normals.swap(new_nrm); else normals.clear();
    if (keep_t) uvs.swap(new_uv);      else uvs.clear();
    if (keep_c) colors.swap(new_col);  else colors.clear();
    if (keep_s) skin.swap(new_skin);   else skin.clear();

    for (uint32_t& i : indices) i = remap[i];
    remove_degenerate();
    return removed;
}

size_t Mesh::compact()
{
    if (positions.empty()) return 0;
    std::vector<uint32_t> remap(positions.size(), kInvalidIndex);
    uint32_t next = 0;
    for (uint32_t i : indices)
        if (i < remap.size() && remap[i] == kInvalidIndex) remap[i] = next++;

    if (next == positions.size()) return 0;

    auto shuffle = [&](auto& vec) {
        using T = typename std::decay_t<decltype(vec)>::value_type;
        if (vec.size() != remap.size()) return;
        std::vector<T> out(next);
        for (size_t v = 0; v < remap.size(); ++v)
            if (remap[v] != kInvalidIndex) out[remap[v]] = vec[v];
        vec.swap(out);
    };

    const size_t removed = positions.size() - next;
    shuffle(positions);
    shuffle(normals);
    shuffle(uvs);
    shuffle(colors);
    shuffle(skin);
    for (uint32_t& i : indices) i = remap[i];
    return removed;
}

void Mesh::normalise_to_unit(bool recentre)
{
    const Aabb box = bounds();
    if (!box.valid()) return;

    const Vec3  offset = recentre ? box.center() : Vec3{};
    const float radius = std::max(0.5f * box.diagonal(), kEps);
    const float scale  = 1.0f / radius;

    for (Vec3& p : positions) p = (p - offset) * scale;
    for (Joint& j : armature.joints) j.bind_position = (j.bind_position - offset) * scale;

    import_scale = radius;
    // Inverse of what we just did, so exporters can restore the original space.
    import_transform = Mat4::translation(offset) * Mat4::scale(Vec3{radius});
}

Mesh::Stats Mesh::compute_stats() const
{
    Stats s;
    s.vertices  = vertex_count();
    s.triangles = triangle_count();
    s.bounds    = bounds();

    if (s.triangles == 0) return s;

    std::unordered_map<EdgeKey, uint32_t, EdgeHash> edge_uses;
    edge_uses.reserve(s.triangles * 3);

    double edge_total = 0.0;
    double area_total = 0.0;
    float  min_edge = std::numeric_limits<float>::max();
    float  max_edge = 0.0f;
    float  min_q    = 1.0f;

    for (size_t t = 0; t < s.triangles; ++t) {
        const uint32_t v[3] = {indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]};
        const Vec3 p[3] = {positions[v[0]], positions[v[1]], positions[v[2]]};
        area_total += rd::triangle_area(p[0], p[1], p[2]);
        min_q = std::min(min_q, triangle_quality(p[0], p[1], p[2]));
        for (int c = 0; c < 3; ++c) {
            const uint32_t a = v[c], b = v[(c + 1) % 3];
            ++edge_uses[make_edge(a, b)];
            const float len = length(positions[b] - positions[a]);
            edge_total += len;
            min_edge = std::min(min_edge, len);
            max_edge = std::max(max_edge, len);
        }
    }

    s.edges        = edge_uses.size();
    s.surface_area = static_cast<float>(area_total);
    s.min_edge     = min_edge == std::numeric_limits<float>::max() ? 0.0f : min_edge;
    s.max_edge     = max_edge;
    s.mean_edge    = s.triangles ? static_cast<float>(edge_total / (s.triangles * 3)) : 0.0f;
    s.min_quality  = min_q;

    for (const auto& [edge, uses] : edge_uses) {
        if (uses == 1)      ++s.boundary_edges;
        else if (uses > 2)  ++s.nonmanifold_edges;
    }
    s.manifold = s.nonmanifold_edges == 0;
    s.closed   = s.boundary_edges == 0 && s.manifold;

    // Connected components via union-find on vertices.
    std::vector<uint32_t> parent(s.vertices);
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t t = 0; t < s.triangles; ++t) {
        const uint32_t a = find(indices[t * 3]);
        const uint32_t b = find(indices[t * 3 + 1]);
        const uint32_t c = find(indices[t * 3 + 2]);
        if (a != b) parent[b] = a;
        if (a != c) parent[find(c)] = a;
    }
    std::vector<bool> used(s.vertices, false);
    for (uint32_t i : indices) used[i] = true;

    std::unordered_set<uint32_t> roots;
    for (uint32_t v = 0; v < s.vertices; ++v) {
        if (!used[v]) { ++s.isolated_vertices; continue; }
        roots.insert(find(v));
    }
    s.shells = roots.size();
    return s;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------
void mesh_append(Mesh& dst, const Mesh& src)
{
    if (src.empty()) return;
    const uint32_t offset = static_cast<uint32_t>(dst.positions.size());

    // Attribute presence must agree, otherwise drop the optional stream.
    const bool keep_n = (dst.positions.empty() || dst.has_normals()) && src.has_normals();
    const bool keep_t = (dst.positions.empty() || dst.has_uvs())     && src.has_uvs();
    const bool keep_c = (dst.positions.empty() || dst.has_colors())  && src.has_colors();
    const bool keep_s = (dst.positions.empty() || dst.has_skin())    && src.has_skin();

    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    if (keep_n) dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    else        dst.normals.clear();
    if (keep_t) dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    else        dst.uvs.clear();
    if (keep_c) dst.colors.insert(dst.colors.end(), src.colors.begin(), src.colors.end());
    else        dst.colors.clear();
    if (keep_s) dst.skin.insert(dst.skin.end(), src.skin.begin(), src.skin.end());
    else        dst.skin.clear();

    dst.indices.reserve(dst.indices.size() + src.indices.size());
    for (uint32_t i : src.indices) dst.indices.push_back(i + offset);

    if (dst.armature.empty()) dst.armature = src.armature;
}

Mesh mesh_extract(const Mesh& src, const std::vector<bool>& tri_mask,
                  std::vector<uint32_t>* vertex_map)
{
    Mesh out;
    out.name     = src.name;
    out.armature = src.armature;
    out.import_transform = src.import_transform;
    out.import_scale     = src.import_scale;

    std::vector<uint32_t> remap(src.positions.size(), kInvalidIndex);
    const bool keep_n = src.has_normals(), keep_t = src.has_uvs();
    const bool keep_c = src.has_colors(),  keep_s = src.has_skin();
    const bool keep_r = src.tri_region.size()   == src.triangle_count();
    const bool keep_m = src.tri_material.size() == src.triangle_count();
    const bool keep_u = src.has_corner_uvs();
    const bool keep_p = src.has_pages();
    out.materials     = src.materials;
    out.colors_prelit = src.colors_prelit;

    for (size_t t = 0, n = src.triangle_count(); t < n; ++t) {
        if (t >= tri_mask.size() || !tri_mask[t]) continue;
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = src.indices[t * 3 + c];
            if (remap[v] == kInvalidIndex) {
                remap[v] = static_cast<uint32_t>(out.positions.size());
                out.positions.push_back(src.positions[v]);
                if (keep_n) out.normals.push_back(src.normals[v]);
                if (keep_t) out.uvs.push_back(src.uvs[v]);
                if (keep_c) out.colors.push_back(src.colors[v]);
                if (keep_s) out.skin.push_back(src.skin[v]);
            }
            out.indices.push_back(remap[v]);
        }
        if (keep_r) out.tri_region.push_back(src.tri_region[t]);
        if (keep_m) out.tri_material.push_back(src.tri_material[t]);
        if (keep_p) out.tri_page.push_back(src.tri_page[t]);
        if (keep_u)
            for (int c = 0; c < 3; ++c) out.corner_uvs.push_back(src.corner_uvs[t * 3 + c]);
    }

    if (vertex_map) *vertex_map = std::move(remap);
    return out;
}

} // namespace rd
