#include "mesh/bvh.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <iterator>

namespace rd {
namespace {

constexpr uint32_t kBinCount    = 16;
constexpr uint32_t kMaxLeafSize = 4;
constexpr uint32_t kMaxDepth    = 64;

inline bool slab_test(const Aabb& box, Vec3 origin, Vec3 inv_dir, float tmin, float tmax)
{
    for (int a = 0; a < 3; ++a) {
        float t0 = (box.lo[a] - origin[a]) * inv_dir[a];
        float t1 = (box.hi[a] - origin[a]) * inv_dir[a];
        if (t0 > t1) std::swap(t0, t1);
        tmin = t0 > tmin ? t0 : tmin;
        tmax = t1 < tmax ? t1 : tmax;
        if (tmax < tmin) return false;
    }
    return true;
}

inline float box_distance2(const Aabb& box, Vec3 p)
{
    float d = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float v  = p[a];
        const float lo = box.lo[a], hi = box.hi[a];
        if (v < lo) d += sqr(lo - v);
        else if (v > hi) d += sqr(v - hi);
    }
    return d;
}

} // namespace

void Bvh::clear()
{
    mesh_ = nullptr;
    nodes_.clear();
    prims_.clear();
    prim_boxes_.clear();
    prim_centroids_.clear();
    max_depth_ = 0;
}

void Bvh::build(const Mesh& mesh)
{
    clear();
    mesh_ = &mesh;

    const size_t tri_count = mesh.triangle_count();
    if (tri_count == 0) return;

    prims_.resize(tri_count);
    prim_boxes_.resize(tri_count);
    prim_centroids_.resize(tri_count);

    for (size_t t = 0; t < tri_count; ++t) {
        prims_[t] = static_cast<uint32_t>(t);
        Vec3 a, b, c;
        mesh.tri_positions(t, a, b, c);
        Aabb box;
        box.grow(a); box.grow(b); box.grow(c);
        // Fatten degenerate slabs so axis aligned geometry still gets hit.
        const Vec3 pad{std::max(1e-7f, (box.hi.x - box.lo.x) * 1e-4f),
                       std::max(1e-7f, (box.hi.y - box.lo.y) * 1e-4f),
                       std::max(1e-7f, (box.hi.z - box.lo.z) * 1e-4f)};
        box.lo -= pad;
        box.hi += pad;
        prim_boxes_[t]     = box;
        prim_centroids_[t] = (a + b + c) * (1.0f / 3.0f);
    }

    nodes_.reserve(tri_count * 2);
    build_range(0, static_cast<uint32_t>(tri_count), 0);

    RD_DEBUG("bvh: %zu triangles, %zu nodes, depth %zu", tri_count, nodes_.size(), max_depth_);
}

uint32_t Bvh::build_range(uint32_t begin, uint32_t end, uint32_t depth)
{
    const uint32_t node_index = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
    max_depth_ = std::max<size_t>(max_depth_, depth);

    Aabb box, centroid_box;
    for (uint32_t i = begin; i < end; ++i) {
        box.grow(prim_boxes_[prims_[i]]);
        centroid_box.grow(prim_centroids_[prims_[i]]);
    }

    const uint32_t count = end - begin;
    auto make_leaf = [&]() {
        Node& n = nodes_[node_index];
        n.box   = box;
        n.first = begin;
        n.count = count;
        return node_index;
    };

    if (count <= kMaxLeafSize || depth >= kMaxDepth) return make_leaf();

    const Vec3 extent = centroid_box.extent();
    const int  axis   = major_axis(extent);
    if (extent[axis] < 1e-12f) return make_leaf();

    // Binned SAH.
    struct Bin { Aabb box; uint32_t count = 0; };
    Bin bins[kBinCount];

    const float scale = kBinCount / extent[axis];
    const float lo    = centroid_box.lo[axis];

    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t p  = prims_[i];
        uint32_t bin = static_cast<uint32_t>((prim_centroids_[p][axis] - lo) * scale);
        bin = std::min(bin, kBinCount - 1);
        bins[bin].count++;
        bins[bin].box.grow(prim_boxes_[p]);
    }

    float    right_area[kBinCount];
    uint32_t right_count[kBinCount];
    {
        Aabb     acc;
        uint32_t n = 0;
        for (int i = kBinCount - 1; i >= 0; --i) {
            acc.grow(bins[i].box);
            n += bins[i].count;
            right_area[i]  = acc.surface_area();
            right_count[i] = n;
        }
    }

    float    best_cost = std::numeric_limits<float>::max();
    uint32_t best_split = kBinCount;
    {
        Aabb     acc;
        uint32_t n = 0;
        for (uint32_t i = 0; i + 1 < kBinCount; ++i) {
            acc.grow(bins[i].box);
            n += bins[i].count;
            if (n == 0 || right_count[i + 1] == 0) continue;
            const float cost = acc.surface_area() * n + right_area[i + 1] * right_count[i + 1];
            if (cost < best_cost) { best_cost = cost; best_split = i; }
        }
    }

    const float leaf_cost = box.surface_area() * count;
    if (best_split == kBinCount || best_cost >= leaf_cost) return make_leaf();

    const auto mid = std::partition(
        prims_.begin() + begin, prims_.begin() + end, [&](uint32_t p) {
            uint32_t bin = static_cast<uint32_t>((prim_centroids_[p][axis] - lo) * scale);
            bin = std::min(bin, kBinCount - 1);
            return bin <= best_split;
        });

    const uint32_t split = static_cast<uint32_t>(mid - prims_.begin());
    if (split == begin || split == end) return make_leaf();

    build_range(begin, split, depth + 1);            // always node_index + 1
    const uint32_t right = build_range(split, end, depth + 1);

    Node& n = nodes_[node_index];
    n.box   = box;
    n.first = right;
    n.count = 0;
    return node_index;
}

RayHit Bvh::intersect(Vec3 origin, Vec3 dir, float tmin, float tmax) const
{
    RayHit best;
    if (nodes_.empty() || !mesh_) return best;

    const Vec3 inv_dir{1.0f / (std::fabs(dir.x) > 1e-20f ? dir.x : (dir.x >= 0 ? 1e-20f : -1e-20f)),
                       1.0f / (std::fabs(dir.y) > 1e-20f ? dir.y : (dir.y >= 0 ? 1e-20f : -1e-20f)),
                       1.0f / (std::fabs(dir.z) > 1e-20f ? dir.z : (dir.z >= 0 ? 1e-20f : -1e-20f))};

    uint32_t stack[kMaxDepth * 2 + 4];
    int      sp = 0;
    stack[sp++] = 0;
    float    closest = tmax;

    while (sp > 0) {
        const uint32_t ni   = stack[--sp];
        const Node&    node = nodes_[ni];
        if (!slab_test(node.box, origin, inv_dir, tmin, closest)) continue;

        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t tri = prims_[node.first + i];
                Vec3 a, b, c;
                mesh_->tri_positions(tri, a, b, c);
                float t, u, v;
                if (ray_triangle(origin, dir, a, b, c, tmin, closest, t, u, v)) {
                    closest       = t;
                    best.t        = t;
                    best.u        = u;
                    best.v        = v;
                    best.triangle = tri;
                }
            }
        } else if (sp + 2 <= static_cast<int>(std::size(stack))) {
            stack[sp++] = ni + 1;
            stack[sp++] = node.first;
        }
    }
    return best;
}

bool Bvh::occluded(Vec3 origin, Vec3 dir, float tmin, float tmax) const
{
    if (nodes_.empty() || !mesh_) return false;

    const Vec3 inv_dir{1.0f / (std::fabs(dir.x) > 1e-20f ? dir.x : (dir.x >= 0 ? 1e-20f : -1e-20f)),
                       1.0f / (std::fabs(dir.y) > 1e-20f ? dir.y : (dir.y >= 0 ? 1e-20f : -1e-20f)),
                       1.0f / (std::fabs(dir.z) > 1e-20f ? dir.z : (dir.z >= 0 ? 1e-20f : -1e-20f))};

    uint32_t stack[kMaxDepth * 2 + 4];
    int      sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const uint32_t ni   = stack[--sp];
        const Node&    node = nodes_[ni];
        if (!slab_test(node.box, origin, inv_dir, tmin, tmax)) continue;

        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t tri = prims_[node.first + i];
                Vec3 a, b, c;
                mesh_->tri_positions(tri, a, b, c);
                float t, u, v;
                if (ray_triangle(origin, dir, a, b, c, tmin, tmax, t, u, v)) return true;
            }
        } else if (sp + 2 <= static_cast<int>(std::size(stack))) {
            stack[sp++] = ni + 1;
            stack[sp++] = node.first;
        }
    }
    return false;
}

ClosestHit Bvh::closest_point(Vec3 p, float max_distance) const
{
    ClosestHit best;
    if (nodes_.empty() || !mesh_) return best;

    float best_d2 = max_distance == std::numeric_limits<float>::max()
                        ? std::numeric_limits<float>::max()
                        : max_distance * max_distance;

    // Depth first with a distance ordered two-entry choice: good enough and
    // avoids a priority queue allocation in the inner loop of the remesher.
    uint32_t stack[kMaxDepth * 2 + 4];
    int      sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const uint32_t ni = stack[--sp];
        const Node&    node = nodes_[ni];
        if (box_distance2(node.box, p) >= best_d2) continue;

        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t tri = prims_[node.first + i];
                Vec3 a, b, c;
                mesh_->tri_positions(tri, a, b, c);
                const Vec3  q  = closest_point_on_triangle(p, a, b, c);
                const float d2 = length2(q - p);
                if (d2 < best_d2) {
                    best_d2        = d2;
                    best.point     = q;
                    best.distance2 = d2;
                    best.triangle  = tri;
                }
            }
        } else if (sp + 2 <= static_cast<int>(std::size(stack))) {
            const uint32_t l = ni + 1;
            const uint32_t r = node.first;
            const float dl = box_distance2(nodes_[l].box, p);
            const float dr = box_distance2(nodes_[r].box, p);
            // Push the far child first so the near one is popped next.
            if (dl < dr) { stack[sp++] = r; stack[sp++] = l; }
            else         { stack[sp++] = l; stack[sp++] = r; }
        }
    }
    return best;
}

Vec3 Bvh::geometric_normal(uint32_t triangle) const
{
    if (!mesh_ || triangle >= mesh_->triangle_count()) return {0, 1, 0};
    return mesh_->triangle_normal(triangle);
}

Vec3 Bvh::shading_normal(const RayHit& hit) const
{
    if (!mesh_ || !hit.hit()) return {0, 1, 0};
    if (!mesh_->has_normals()) return geometric_normal(hit.triangle);

    const uint32_t i0 = mesh_->indices[hit.triangle * 3 + 0];
    const uint32_t i1 = mesh_->indices[hit.triangle * 3 + 1];
    const uint32_t i2 = mesh_->indices[hit.triangle * 3 + 2];
    const float w = 1.0f - hit.u - hit.v;
    const Vec3 n = mesh_->normals[i0] * w + mesh_->normals[i1] * hit.u +
                   mesh_->normals[i2] * hit.v;
    return normalize(n);
}

bool Bvh::inside(Vec3 p) const
{
    if (nodes_.empty() || !mesh_) return false;

    // Three axis-ish rays, majority vote. Cheap and robust enough for the
    // sanity checks we use it for.
    static const Vec3 dirs[3] = {{0.7071f, 0.5f, 0.5f},
                                 {-0.5f, 0.7071f, 0.5f},
                                 {0.5f, -0.5f, 0.7071f}};
    int inside_votes = 0;
    for (const Vec3& d : dirs) {
        int   crossings = 0;
        float t         = 1e-4f;
        for (int guard = 0; guard < 64; ++guard) {
            const RayHit h = intersect(p, d, t, std::numeric_limits<float>::max());
            if (!h.hit()) break;
            ++crossings;
            t = h.t + 1e-4f;
        }
        if (crossings % 2 == 1) ++inside_votes;
    }
    return inside_votes >= 2;
}

} // namespace rd
