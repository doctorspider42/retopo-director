#include "geom/quadric.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace rd {
namespace {

// ---------------------------------------------------------------------------
// Symmetric 4x4 quadric, upper triangle in double precision. Single precision
// falls apart on meshes with a wide dynamic range of triangle sizes, which is
// exactly what a decimated sculpt is.
// ---------------------------------------------------------------------------
struct Quadric {
    double m[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // indices: 0:xx 1:xy 2:xz 3:xw 4:yy 5:yz 6:yw 7:zz 8:zw 9:ww

    void add_plane(double a, double b, double c, double d, double weight)
    {
        m[0] += weight * a * a; m[1] += weight * a * b; m[2] += weight * a * c; m[3] += weight * a * d;
        m[4] += weight * b * b; m[5] += weight * b * c; m[6] += weight * b * d;
        m[7] += weight * c * c; m[8] += weight * c * d;
        m[9] += weight * d * d;
    }

    Quadric& operator+=(const Quadric& o)
    {
        for (int i = 0; i < 10; ++i) m[i] += o.m[i];
        return *this;
    }

    double error(double x, double y, double z) const
    {
        return m[0] * x * x + 2 * m[1] * x * y + 2 * m[2] * x * z + 2 * m[3] * x +
               m[4] * y * y + 2 * m[5] * y * z + 2 * m[6] * y +
               m[7] * z * z + 2 * m[8] * z +
               m[9];
    }

    // Solves the 3x3 system for the error minimising point. Returns false when
    // the system is too ill conditioned to trust.
    bool optimum(Vec3& out) const
    {
        const double a11 = m[0], a12 = m[1], a13 = m[2];
        const double a22 = m[4], a23 = m[5], a33 = m[7];
        const double b1 = -m[3], b2 = -m[6], b3 = -m[8];

        const double det = a11 * (a22 * a33 - a23 * a23) -
                           a12 * (a12 * a33 - a23 * a13) +
                           a13 * (a12 * a23 - a22 * a13);
        if (std::fabs(det) < 1e-14) return false;

        const double inv = 1.0 / det;
        const double x = inv * (b1 * (a22 * a33 - a23 * a23) -
                                a12 * (b2 * a33 - a23 * b3) +
                                a13 * (b2 * a23 - a22 * b3));
        const double y = inv * (a11 * (b2 * a33 - a23 * b3) -
                                b1 * (a12 * a33 - a23 * a13) +
                                a13 * (a12 * b3 - b2 * a13));
        const double z = inv * (a11 * (a22 * b3 - b2 * a23) -
                                a12 * (a12 * b3 - b2 * a13) +
                                b1 * (a12 * a23 - a22 * a13));
        out = {float(x), float(y), float(z)};
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }
};

struct PendingEdge {
    double   cost;
    uint32_t v0, v1;
    uint32_t stamp0, stamp1;
    Vec3     position;
    bool operator>(const PendingEdge& o) const { return cost > o.cost; }
};

using EdgeQueue = std::priority_queue<PendingEdge, std::vector<PendingEdge>, std::greater<PendingEdge>>;

struct Simplifier {
    // --- state -----------------------------------------------------------
    std::vector<Vec3>     pos;
    std::vector<Quadric>  quad;
    std::vector<uint8_t>  alive;
    std::vector<uint8_t>  locked;
    std::vector<uint32_t> stamp;         // bumped on every change, for lazy queue entries
    std::vector<float>    target_edge;
    std::vector<float>    importance;
    std::vector<std::vector<uint32_t>> vtri;

    std::vector<uint32_t> tri;           // 3 per triangle
    std::vector<uint8_t>  tri_alive;
    std::vector<uint16_t> tri_region;

    std::vector<int>      region_count;
    std::vector<int>      region_budget;

    EdgeQueue queue;

    float flip_cos = 0.0f;
    const QuadricOptions* opts = nullptr;
    SymmetryPlane symmetry;
    bool  use_symmetry = false;
    float symmetry_eps = 0.0f;

    QuadricResult result;

    // --- helpers ---------------------------------------------------------
    bool triangle_has(uint32_t t, uint32_t v) const
    {
        return tri[t * 3] == v || tri[t * 3 + 1] == v || tri[t * 3 + 2] == v;
    }

    Vec3 tri_normal(uint32_t t) const
    {
        const Vec3 a = pos[tri[t * 3]], b = pos[tri[t * 3 + 1]], c = pos[tri[t * 3 + 2]];
        return normalize(cross(b - a, c - a));
    }

    Vec3 tri_normal_with(uint32_t t, uint32_t replaced, Vec3 np) const
    {
        Vec3 p[3];
        for (int i = 0; i < 3; ++i) {
            const uint32_t v = tri[t * 3 + i];
            p[i] = (v == replaced) ? np : pos[v];
        }
        return normalize(cross(p[1] - p[0], p[2] - p[0]));
    }

    // Triangles that contain both u and v.
    void shared_triangles(uint32_t u, uint32_t v, std::vector<uint32_t>& out) const
    {
        out.clear();
        for (uint32_t t : vtri[u])
            if (tri_alive[t] && triangle_has(t, v)) out.push_back(t);
    }

    void neighbours_of(uint32_t u, std::unordered_set<uint32_t>& out) const
    {
        out.clear();
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t w = tri[t * 3 + i];
                if (w != u) out.insert(w);
            }
        }
    }

    // The link condition: collapsing u into v keeps the mesh manifold only if
    // u and v share exactly as many neighbours as they share triangles.
    bool link_condition(uint32_t u, uint32_t v) const
    {
        std::vector<uint32_t> shared;
        shared_triangles(u, v, shared);
        if (shared.empty() || shared.size() > 2) return false;

        std::unordered_set<uint32_t> nu, nv;
        neighbours_of(u, nu);
        neighbours_of(v, nv);

        size_t common = 0;
        for (uint32_t w : nu)
            if (nv.count(w)) ++common;

        return common == shared.size();
    }

    // Area vectors (twice the area, along the normal), unnormalised on purpose.
    // normalize() has an absolute epsilon, and the loader hands over metres: a
    // 15 cm model's millimetre triangles have cross products below it, so every
    // normal came back zero, every collapse "flipped", and the simplifier stalled
    // at 12k triangles against a budget of 400. Comparing the raw vectors against
    // their own lengths is scale free.
    Vec3 tri_area_vector(uint32_t t) const
    {
        const Vec3 a = pos[tri[t * 3]], b = pos[tri[t * 3 + 1]], c = pos[tri[t * 3 + 2]];
        return cross(b - a, c - a);
    }

    Vec3 tri_area_vector_with(uint32_t t, uint32_t replaced, Vec3 np) const
    {
        Vec3 p[3];
        for (int i = 0; i < 3; ++i) {
            const uint32_t v = tri[t * 3 + i];
            p[i] = (v == replaced) ? np : pos[v];
        }
        return cross(p[1] - p[0], p[2] - p[0]);
    }

    bool flips(uint32_t t, uint32_t moved, Vec3 np) const
    {
        const Vec3  before = tri_area_vector(t);
        const Vec3  after  = tri_area_vector_with(t, moved, np);
        const float lb = length2(before), la = length2(after);
        // A triangle that was already degenerate has no orientation to lose,
        // and refusing to touch it is how slivers get stuck forever.
        if (lb <= 0.0f) return false;
        // Collapsing to (almost) nothing is a fold in all but name.
        if (la <= lb * 1e-10f) return true;
        return dot(before, after) < flip_cos * std::sqrt(lb * la);
    }

    bool would_flip(uint32_t u, uint32_t v, Vec3 np) const
    {
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t] || triangle_has(t, v)) continue;
            if (flips(t, u, np)) return true;
        }
        for (uint32_t t : vtri[v]) {
            if (!tri_alive[t] || triangle_has(t, u)) continue;
            if (flips(t, v, np)) return true;
        }
        return false;
    }

    Vec3 snap_to_symmetry(uint32_t u, uint32_t v, Vec3 p) const
    {
        if (!use_symmetry) return p;
        const bool on_u = std::fabs(dot(symmetry.normal, pos[u]) - symmetry.offset) <= symmetry_eps;
        const bool on_v = std::fabs(dot(symmetry.normal, pos[v]) - symmetry.offset) <= symmetry_eps;
        if (!on_u || !on_v) return p;
        return p - symmetry.normal * (dot(symmetry.normal, p) - symmetry.offset);
    }

    void evaluate(uint32_t u, uint32_t v, PendingEdge& out) const
    {
        Quadric q = quad[u];
        q += quad[v];

        Vec3 candidate;
        const bool locked_u = locked[u] != 0;
        const bool locked_v = locked[v] != 0;

        if (locked_u && locked_v)      candidate = (pos[u] + pos[v]) * 0.5f;
        else if (locked_u)             candidate = pos[u];
        else if (locked_v)             candidate = pos[v];
        else if (!q.optimum(candidate)) {
            // Ill conditioned: pick whichever of the three obvious points is best.
            const Vec3 mid = (pos[u] + pos[v]) * 0.5f;
            const double eu = q.error(pos[u].x, pos[u].y, pos[u].z);
            const double ev = q.error(pos[v].x, pos[v].y, pos[v].z);
            const double em = q.error(mid.x, mid.y, mid.z);
            candidate = (eu <= ev && eu <= em) ? pos[u] : (ev <= em ? pos[v] : mid);
        }
        candidate = snap_to_symmetry(u, v, candidate);

        double error = q.error(candidate.x, candidate.y, candidate.z);
        if (!(error >= 0.0)) error = 0.0;   // catches NaN too

        const float local = std::max(std::min(target_edge[u], target_edge[v]), 1e-8f);
        double cost = error / (double(local) * double(local));

        // Collapses that would leave a stretched edge behind are discouraged:
        // this is what keeps dense regions from being eaten by a neighbour.
        float longest = 0.0f;
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t w = tri[t * 3 + i];
                if (w == u || w == v) continue;
                longest = std::max(longest, length(pos[w] - candidate));
            }
        }
        const float overshoot = std::max(0.0f, longest / local - 1.0f);
        cost *= 1.0 + double(opts->overshoot_penalty) * double(overshoot * overshoot);

        // Importance nudges, it does not dominate: the density field already
        // carries most of the director's intent.
        const float imp = 0.5f * (importance[u] + importance[v]);
        cost *= 1.0 + 0.4 * double(std::max(0.0f, imp - 1.0f));

        out.cost     = cost;
        out.v0       = u;
        out.v1       = v;
        out.stamp0   = stamp[u];
        out.stamp1   = stamp[v];
        out.position = candidate;
    }

    // Every edge that still exists, deduplicated. Used to re-seed the queue.
    void collect_live_edges(std::vector<std::pair<uint32_t, uint32_t>>& out) const
    {
        out.clear();
        std::unordered_set<uint64_t> seen;
        seen.reserve(tri_alive.size() * 3);
        for (size_t t = 0; t < tri_alive.size(); ++t) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri[t * 3 + i];
                const uint32_t b = tri[t * 3 + (i + 1) % 3];
                const uint64_t key = a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
                if (seen.insert(key).second) out.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        std::sort(out.begin(), out.end());
    }

    // Are any regions still above their allocation?
    bool any_region_over_budget() const
    {
        for (size_t r = 0; r < region_count.size(); ++r)
            if (region_count[r] > region_budget[r]) return true;
        return false;
    }

    void push_edges_around(uint32_t u)
    {
        std::unordered_set<uint32_t> seen;
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t v = tri[t * 3 + i];
                if (v == u || !seen.insert(v).second) continue;
                PendingEdge e;
                evaluate(u, v, e);
                queue.push(e);
            }
        }
    }

    // Does collapsing this edge remove triangles only from regions that are
    // already at budget?
    bool budget_blocks(uint32_t u, uint32_t v)
    {
        if (!opts->enforce_region_budgets) return false;
        std::vector<uint32_t> shared;
        shared_triangles(u, v, shared);
        if (shared.empty()) return false;

        for (uint32_t t : shared) {
            const uint16_t r = tri_region[t];
            if (r >= region_budget.size()) return false;           // unbudgeted, always allowed
            if (region_count[r] > region_budget[r]) return false;  // still over, allow
        }
        return true;
    }

    bool collapse(uint32_t u, uint32_t v, Vec3 np)
    {
        std::vector<uint32_t> shared;
        shared_triangles(u, v, shared);
        if (shared.empty()) return false;

        // Retire the triangles on the edge.
        for (uint32_t t : shared) {
            tri_alive[t] = 0;
            const uint16_t r = tri_region[t];
            if (r < region_count.size() && region_count[r] > 0) --region_count[r];
        }

        // Rewire u's remaining triangles onto v.
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i)
                if (tri[t * 3 + i] == u) tri[t * 3 + i] = v;
            // A rewired triangle can become degenerate if it already held v.
            if (tri[t * 3] == tri[t * 3 + 1] || tri[t * 3 + 1] == tri[t * 3 + 2] ||
                tri[t * 3] == tri[t * 3 + 2]) {
                tri_alive[t] = 0;
                const uint16_t r = tri_region[t];
                if (r < region_count.size() && region_count[r] > 0) --region_count[r];
                continue;
            }
            vtri[v].push_back(t);
        }

        pos[v]         = np;
        quad[v]       += quad[u];
        target_edge[v] = std::min(target_edge[u], target_edge[v]);
        importance[v]  = std::max(importance[u], importance[v]);
        locked[v]      = locked[u] || locked[v];

        alive[u] = 0;
        vtri[u].clear();
        ++stamp[u];
        ++stamp[v];

        // Drop dead entries so the adjacency list does not grow without bound.
        auto& list = vtri[v];
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](uint32_t t) { return !tri_alive[t]; }),
                   list.end());
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());

        // Only u and v changed. Bumping the whole one-ring would invalidate every
        // queued edge among the neighbours as well, and since only edges incident
        // to v get re-pushed, the queue would drain long before the budget is met.
        return true;
    }
};

} // namespace

// ---------------------------------------------------------------------------
QuadricResult quadric_simplify(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const std::vector<int>& budgets,
                               const SymmetryPlane& symmetry,
                               const QuadricOptions& opts,
                               const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    QuadricResult result;

    const size_t vcount = mesh.vertex_count();
    const size_t tcount = mesh.triangle_count();
    if (tcount == 0 || vcount == 0) return result;

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };
    report(0.02f, "building quadrics");

    Simplifier s;
    s.opts         = &opts;
    s.flip_cos     = std::cos(clampf(opts.max_normal_flip_degrees, 1.0f, 179.0f) * kDeg2Rad);
    s.symmetry     = symmetry;
    s.use_symmetry = opts.lock_symmetry_plane && symmetry.accepted;
    s.symmetry_eps = analysis.bbox_diagonal * opts.symmetry_epsilon_rel;

    s.pos   = mesh.positions;
    s.alive.assign(vcount, 1);
    s.locked.assign(vcount, 0);
    s.stamp.assign(vcount, 0);
    s.quad.assign(vcount, Quadric{});
    s.vtri.assign(vcount, {});

    s.target_edge = density.vertex_target_edge;
    s.importance  = density.vertex_importance;
    if (s.target_edge.size() != vcount) s.target_edge.assign(vcount, analysis.mean_edge);
    if (s.importance.size() != vcount)  s.importance.assign(vcount, 1.0f);

    s.tri.assign(mesh.indices.begin(), mesh.indices.end());
    s.tri_alive.assign(tcount, 1);
    s.tri_region.assign(tcount, kNoRegion);
    if (seg.tri_region.size() == tcount) s.tri_region = seg.tri_region;

    // --- region bookkeeping -------------------------------------------------
    uint16_t max_region = 0;
    for (uint16_t r : s.tri_region)
        if (r != kNoRegion) max_region = std::max(max_region, r);
    s.region_count.assign(size_t(max_region) + 1, 0);
    s.region_budget.assign(size_t(max_region) + 1, std::numeric_limits<int>::max());
    for (size_t i = 0; i < budgets.size() && i < s.region_budget.size(); ++i)
        s.region_budget[i] = std::max(2, budgets[i]);
    for (uint16_t r : s.tri_region)
        if (r < s.region_count.size()) ++s.region_count[r];

    // --- face quadrics ------------------------------------------------------
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t i0 = s.tri[t * 3], i1 = s.tri[t * 3 + 1], i2 = s.tri[t * 3 + 2];
        const Vec3 a = s.pos[i0], b = s.pos[i1], c = s.pos[i2];
        const Vec3 n = cross(b - a, c - a);
        const float len = length(n);
        if (len < 1e-16f) continue;
        const Vec3  un   = n / len;
        const float area = 0.5f * len;
        const double d   = -double(dot(un, a));

        Quadric q;
        q.add_plane(un.x, un.y, un.z, d, double(area));
        s.quad[i0] += q;
        s.quad[i1] += q;
        s.quad[i2] += q;

        s.vtri[i0].push_back(uint32_t(t));
        s.vtri[i1].push_back(uint32_t(t));
        s.vtri[i2].push_back(uint32_t(t));
    }

    // --- constraint planes on boundaries, creases and region borders -------
    report(0.10f, "constraint planes");
    const float sharp_limit = clampf(opts.sharp_angle_degrees, 0.0f, 180.0f) * kDeg2Rad;

    for (size_t e = 0; e < analysis.topology.edges.size(); ++e) {
        const MeshTopology::Edge& edge = analysis.topology.edges[e];
        const bool boundary = edge.tri1 == kInvalidIndex;

        bool border = false;
        if (!boundary && edge.tri0 < s.tri_region.size() && edge.tri1 < s.tri_region.size())
            border = s.tri_region[edge.tri0] != s.tri_region[edge.tri1];

        const bool sharp = !boundary && e < analysis.edge_dihedral.size() &&
                           std::fabs(analysis.edge_dihedral[e]) >= sharp_limit;

        if (!boundary && !border && !sharp) continue;

        const float weight = boundary ? opts.boundary_weight
                                      : (border ? opts.region_border_weight : opts.sharp_weight);

        const Vec3 a = s.pos[edge.v0], b = s.pos[edge.v1];
        const Vec3 dir = b - a;
        if (length2(dir) < 1e-18f) continue;

        // Plane containing the edge and perpendicular to the adjacent face.
        const Vec3 face_n = mesh.triangle_normal(edge.tri0);
        Vec3 plane_n = cross(normalize(dir), face_n);
        if (length2(plane_n) < 1e-14f) continue;
        plane_n = normalize(plane_n);
        const double d = -double(dot(plane_n, a));

        Quadric q;
        q.add_plane(plane_n.x, plane_n.y, plane_n.z, d, double(weight));
        s.quad[edge.v0] += q;
        s.quad[edge.v1] += q;
    }

    // --- symmetry plane vertices stay on the plane -------------------------
    if (s.use_symmetry) {
        for (size_t v = 0; v < vcount; ++v) {
            const float d = dot(symmetry.normal, s.pos[v]) - symmetry.offset;
            if (std::fabs(d) > s.symmetry_eps) continue;
            Quadric q;
            q.add_plane(symmetry.normal.x, symmetry.normal.y, symmetry.normal.z,
                        -double(symmetry.offset), double(opts.boundary_weight) * 4.0);
            s.quad[v] += q;
        }
    }

    // --- collapse, in sweeps -----------------------------------------------
    // A single pass is not enough. An edge rejected for flipping a normal or
    // for breaking the link condition is dropped from the queue, but the very
    // collapses that follow can make it legal again. One sweep therefore
    // stalls far above budget. Sweeping until a pass achieves nothing costs a
    // few extra scans of a shrinking mesh and actually reaches the target.
    report(0.20f, "collapsing");
    const size_t max_collapses = opts.max_collapses > 0
                                     ? size_t(opts.max_collapses)
                                     : tcount * 2;
    size_t live_triangles = tcount;
    size_t iterations     = 0;
    double last_report    = 0.0;
    std::vector<uint32_t> scratch;
    std::vector<std::pair<uint32_t, uint32_t>> edges;

    constexpr int kMaxSweeps = 12;
    for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
        if (s.result.collapses >= max_collapses) break;

        // Seed from the topology on the first sweep, from the live mesh after.
        s.queue = EdgeQueue{};
        if (sweep == 0) {
            for (const MeshTopology::Edge& edge : analysis.topology.edges) {
                PendingEdge pe;
                s.evaluate(edge.v0, edge.v1, pe);
                s.queue.push(pe);
            }
        } else {
            s.collect_live_edges(edges);
            for (const auto& [a, b] : edges) {
                PendingEdge pe;
                s.evaluate(a, b, pe);
                s.queue.push(pe);
            }
        }

        const size_t before = s.result.collapses;

        while (!s.queue.empty() && s.result.collapses < max_collapses) {
            const PendingEdge e = s.queue.top();
            s.queue.pop();
            ++iterations;

            if (!s.alive[e.v0] || !s.alive[e.v1]) continue;
            if (s.stamp[e.v0] != e.stamp0 || s.stamp[e.v1] != e.stamp1) continue;

            s.shared_triangles(e.v0, e.v1, scratch);
            if (scratch.empty()) continue;

            if (s.budget_blocks(e.v0, e.v1)) { ++s.result.blocked_budget; continue; }
            if (!s.link_condition(e.v0, e.v1)) { ++s.result.rejected_link; continue; }
            if (s.would_flip(e.v0, e.v1, e.position)) { ++s.result.rejected_flip; continue; }

            const size_t removed = scratch.size();
            if (!s.collapse(e.v0, e.v1, e.position)) continue;

            live_triangles -= removed;
            ++s.result.collapses;
            s.result.max_error = std::max(s.result.max_error, float(e.cost));
            s.push_edges_around(e.v1);

            if (watch.seconds() - last_report > 0.15) {
                last_report = watch.seconds();
                const float done = 1.0f - float(live_triangles) / float(tcount);
                report(0.20f + 0.72f * saturate(done), "collapsing");
            }
        }

        if (s.result.collapses == before) break;          // nothing left to give
        if (!s.any_region_over_budget()) break;           // every region is in range
        ++s.result.sweeps;
    }

    // --- rebuild -----------------------------------------------------------
    report(0.95f, "rebuilding mesh");
    Mesh& out = s.result.mesh;
    out.name             = mesh.name;
    out.armature         = mesh.armature;
    out.import_transform = mesh.import_transform;
    out.import_scale     = mesh.import_scale;

    std::vector<uint32_t> remap(vcount, kInvalidIndex);
    out.positions.reserve(live_triangles / 2 + 8);
    out.indices.reserve(live_triangles * 3);
    out.tri_region.reserve(live_triangles);

    for (size_t t = 0; t < tcount; ++t) {
        if (!s.tri_alive[t]) continue;
        for (int i = 0; i < 3; ++i) {
            const uint32_t v = s.tri[t * 3 + i];
            if (remap[v] == kInvalidIndex) {
                remap[v] = uint32_t(out.positions.size());
                out.positions.push_back(s.pos[v]);
            }
            out.indices.push_back(remap[v]);
        }
        out.tri_region.push_back(s.tri_region[t]);
    }

    out.remove_degenerate();
    out.compute_normals(60.0f);

    s.result.region_triangles.assign(s.region_count.begin(), s.region_count.end());
    s.result.seconds = watch.seconds();
    report(1.0f, "done");

    RD_INFO("quadric: %zu -> %zu tri in %zu collapses over %zu sweeps (%zu link, "
            "%zu flip, %zu budget rejects, %zu queue pops) in %s",
            tcount, out.triangle_count(), s.result.collapses, s.result.sweeps + 1,
            s.result.rejected_link, s.result.rejected_flip, s.result.blocked_budget,
            iterations, format_duration(s.result.seconds).c_str());

    return std::move(s.result);
}

QuadricResult quadric_simplify(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const KnobPanel& panel, const SymmetryPlane& symmetry,
                               const QuadricOptions& opts,
                               const std::function<void(float, const char*)>& progress)
{
    uint16_t max_region = 0;
    for (const Region& r : seg.regions) max_region = std::max(max_region, r.id);

    std::vector<int> budgets(size_t(max_region) + 1, std::numeric_limits<int>::max());
    for (const RegionKnobs& k : panel.regions)
        if (k.id < budgets.size()) budgets[k.id] = std::max(2, k.triangle_budget);

    return quadric_simplify(mesh, analysis, seg, density, budgets, symmetry, opts, progress);
}

} // namespace rd
