#include "geom/remesh.h"

#include "core/log.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "geom/hard_rules.h"
#include "geom/quadric.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace rd {
namespace {

inline uint64_t edge_key(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

// ---------------------------------------------------------------------------
// Density lookup in space rather than on a particular mesh. The working mesh
// changes constantly, the field does not.
// ---------------------------------------------------------------------------
struct FieldSampler {
    const Bvh*          bvh   = nullptr;
    const DensityField* field = nullptr;
    float               fallback_edge = 0.01f;

    float edge_at(Vec3 p) const
    {
        if (!bvh || !field || field->target_edge.empty()) return fallback_edge;
        const ClosestHit hit = bvh->closest_point(p);
        if (!hit.hit() || hit.triangle >= field->target_edge.size()) return fallback_edge;
        return field->target_edge[hit.triangle];
    }

    Vec3 project(Vec3 p) const
    {
        if (!bvh) return p;
        const ClosestHit hit = bvh->closest_point(p);
        return hit.hit() ? hit.point : p;
    }
};

// ---------------------------------------------------------------------------
// Editable triangle soup with vertex to triangle adjacency.
// ---------------------------------------------------------------------------
struct Dyn {
    std::vector<Vec3>     pos;
    std::vector<uint8_t>  alive;
    std::vector<uint8_t>  pinned;     // region borders and mesh boundaries
    std::vector<uint8_t>  on_plane;   // sits on the symmetry plane
    std::vector<std::vector<uint32_t>> vtri;

    std::vector<uint32_t> tri;
    std::vector<uint8_t>  tri_alive;
    std::vector<uint16_t> tri_region;

    size_t live_triangles = 0;
    float  flip_cos       = 0.0f;

    // --- construction ------------------------------------------------------
    void from_mesh(const Mesh& m)
    {
        pos = m.positions;
        alive.assign(pos.size(), 1);
        pinned.assign(pos.size(), 0);
        on_plane.assign(pos.size(), 0);
        vtri.assign(pos.size(), {});

        tri.assign(m.indices.begin(), m.indices.end());
        tri_alive.assign(m.triangle_count(), 1);
        tri_region.assign(m.triangle_count(), kNoRegion);
        if (m.tri_region.size() == m.triangle_count()) tri_region = m.tri_region;

        for (size_t t = 0; t < tri_alive.size(); ++t)
            for (int i = 0; i < 3; ++i) vtri[tri[t * 3 + i]].push_back(uint32_t(t));

        live_triangles = tri_alive.size();
    }

    Mesh to_mesh() const
    {
        Mesh out;
        std::vector<uint32_t> remap(pos.size(), kInvalidIndex);
        for (size_t t = 0; t < tri_alive.size(); ++t) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t v = tri[t * 3 + i];
                if (remap[v] == kInvalidIndex) {
                    remap[v] = uint32_t(out.positions.size());
                    out.positions.push_back(pos[v]);
                }
                out.indices.push_back(remap[v]);
            }
            out.tri_region.push_back(tri_region[t]);
        }
        return out;
    }

    // --- queries -----------------------------------------------------------
    bool has(uint32_t t, uint32_t v) const
    {
        return tri[t * 3] == v || tri[t * 3 + 1] == v || tri[t * 3 + 2] == v;
    }

    // Area vectors, unnormalised. normalize() has an absolute epsilon, and the
    // loader works in metres: on a small asset every millimetre triangle came
    // back with a zero normal and every collapse and flip was refused as a fold.
    // The tests below compare these against their own lengths instead.
    Vec3 area_of(uint32_t t) const
    {
        const Vec3 a = pos[tri[t * 3]], b = pos[tri[t * 3 + 1]], c = pos[tri[t * 3 + 2]];
        return cross(b - a, c - a);
    }

    Vec3 area_with(uint32_t t, uint32_t replaced, Vec3 np) const
    {
        Vec3 p[3];
        for (int i = 0; i < 3; ++i) {
            const uint32_t v = tri[t * 3 + i];
            p[i] = (v == replaced) ? np : pos[v];
        }
        return cross(p[1] - p[0], p[2] - p[0]);
    }

    static Vec3 unit(Vec3 v)
    {
        const float l = length(v);
        return l > 1e-30f ? v / l : Vec3{};
    }

    Vec3 normal_of(uint32_t t) const { return unit(area_of(t)); }

    // True when moving a corner of `t` turns it over, or squashes it to
    // nothing relative to what it was. A triangle that was already degenerate
    // has no orientation to lose.
    bool folds(Vec3 before, Vec3 after) const
    {
        const float lb = length2(before), la = length2(after);
        if (lb <= 0.0f) return false;
        if (la <= lb * 1e-10f) return true;
        return dot(before, after) < flip_cos * std::sqrt(lb * la);
    }

    void compact_adjacency(uint32_t v)
    {
        auto& list = vtri[v];
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](uint32_t t) { return !tri_alive[t] || !has(t, v); }),
                   list.end());
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    void shared(uint32_t u, uint32_t v, std::vector<uint32_t>& out) const
    {
        out.clear();
        for (uint32_t t : vtri[u])
            if (tri_alive[t] && has(t, v)) out.push_back(t);
    }

    void neighbours(uint32_t u, std::vector<uint32_t>& out) const
    {
        out.clear();
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t w = tri[t * 3 + i];
                if (w != u && std::find(out.begin(), out.end(), w) == out.end())
                    out.push_back(w);
            }
        }
    }

    bool is_boundary_vertex(uint32_t v) const
    {
        std::unordered_map<uint64_t, int> uses;
        for (uint32_t t : vtri[v]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri[t * 3 + i], b = tri[t * 3 + (i + 1) % 3];
                if (a == v || b == v) ++uses[edge_key(a, b)];
            }
        }
        for (const auto& [k, n] : uses)
            if (n == 1) return true;
        return false;
    }

    // --- mutation ----------------------------------------------------------
    uint32_t add_vertex(Vec3 p)
    {
        pos.push_back(p);
        alive.push_back(1);
        pinned.push_back(0);
        on_plane.push_back(0);
        vtri.push_back({});
        return uint32_t(pos.size() - 1);
    }

    uint32_t add_triangle(uint32_t a, uint32_t b, uint32_t c, uint16_t region)
    {
        const uint32_t t = uint32_t(tri_alive.size());
        tri.push_back(a); tri.push_back(b); tri.push_back(c);
        tri_alive.push_back(1);
        tri_region.push_back(region);
        vtri[a].push_back(t);
        vtri[b].push_back(t);
        vtri[c].push_back(t);
        ++live_triangles;
        return t;
    }

    void kill_triangle(uint32_t t)
    {
        if (!tri_alive[t]) return;
        tri_alive[t] = 0;
        --live_triangles;
    }

    bool split_edge(uint32_t u, uint32_t v, Vec3 midpoint, std::vector<uint32_t>& scratch)
    {
        shared(u, v, scratch);
        if (scratch.empty() || scratch.size() > 2) return false;

        const uint32_t m = add_vertex(midpoint);
        on_plane[m] = on_plane[u] && on_plane[v];
        pinned[m]   = pinned[u] && pinned[v];

        // Copy: add_triangle can reallocate, and we iterate the shared list.
        const std::vector<uint32_t> faces(scratch.begin(), scratch.end());
        for (uint32_t t : faces) {
            int corner = -1;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri[t * 3 + i], b = tri[t * 3 + (i + 1) % 3];
                if ((a == u && b == v) || (a == v && b == u)) { corner = i; break; }
            }
            if (corner < 0) continue;
            const uint32_t p = tri[t * 3 + corner];
            const uint32_t q = tri[t * 3 + (corner + 1) % 3];
            const uint32_t w = tri[t * 3 + (corner + 2) % 3];
            const uint16_t r = tri_region[t];
            kill_triangle(t);
            add_triangle(p, m, w, r);
            add_triangle(m, q, w, r);
        }
        compact_adjacency(u);
        compact_adjacency(v);
        return true;
    }

    bool link_condition(uint32_t u, uint32_t v, std::vector<uint32_t>& scratch) const
    {
        const_cast<Dyn*>(this)->shared(u, v, scratch);
        if (scratch.empty() || scratch.size() > 2) return false;
        const size_t face_count = scratch.size();

        std::vector<uint32_t> nu, nv;
        const_cast<Dyn*>(this)->neighbours(u, nu);
        const_cast<Dyn*>(this)->neighbours(v, nv);
        size_t common = 0;
        for (uint32_t w : nu)
            if (std::find(nv.begin(), nv.end(), w) != nv.end()) ++common;
        return common == face_count;
    }

    bool would_flip_normals(uint32_t u, uint32_t v, Vec3 np) const
    {
        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t] || has(t, v)) continue;
            if (folds(area_of(t), area_with(t, u, np))) return true;
        }
        for (uint32_t t : vtri[v]) {
            if (!tri_alive[t] || has(t, u)) continue;
            if (folds(area_of(t), area_with(t, v, np))) return true;
        }
        return false;
    }

    // Collapses u into v. Returns the number of triangles removed, 0 on refusal.
    size_t collapse_edge(uint32_t u, uint32_t v, Vec3 np, std::vector<uint32_t>& scratch)
    {
        if (!link_condition(u, v, scratch)) return 0;
        if (would_flip_normals(u, v, np)) return 0;

        const std::vector<uint32_t> faces(scratch.begin(), scratch.end());
        size_t removed = 0;
        for (uint32_t t : faces) { kill_triangle(t); ++removed; }

        for (uint32_t t : vtri[u]) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i)
                if (tri[t * 3 + i] == u) tri[t * 3 + i] = v;
            if (tri[t * 3] == tri[t * 3 + 1] || tri[t * 3 + 1] == tri[t * 3 + 2] ||
                tri[t * 3] == tri[t * 3 + 2]) {
                kill_triangle(t);
                ++removed;
                continue;
            }
            vtri[v].push_back(t);
        }

        pos[v]      = np;
        pinned[v]   = pinned[u] || pinned[v];
        on_plane[v] = on_plane[u] && on_plane[v];
        alive[u]    = 0;
        vtri[u].clear();
        compact_adjacency(v);
        return removed;
    }

    // Replaces the diagonal of the quad formed by the two faces on edge (u,v).
    bool flip_edge(uint32_t u, uint32_t v, std::vector<uint32_t>& scratch)
    {
        shared(u, v, scratch);
        if (scratch.size() != 2) return false;

        const uint32_t t0 = scratch[0], t1 = scratch[1];
        auto third = [&](uint32_t t) {
            for (int i = 0; i < 3; ++i) {
                const uint32_t w = tri[t * 3 + i];
                if (w != u && w != v) return w;
            }
            return kInvalidIndex;
        };
        const uint32_t w0 = third(t0);
        const uint32_t w1 = third(t1);
        if (w0 == kInvalidIndex || w1 == kInvalidIndex || w0 == w1) return false;

        // The flipped edge must not already exist somewhere else in the fan.
        for (uint32_t t : vtri[w0])
            if (tri_alive[t] && has(t, w1)) return false;

        // Which face carries the directed edge u->v decides the winding.
        bool t0_forward = false;
        for (int i = 0; i < 3; ++i)
            if (tri[t0 * 3 + i] == u && tri[t0 * 3 + (i + 1) % 3] == v) { t0_forward = true; break; }
        const uint32_t fwd = t0_forward ? t0 : t1;   // holds u -> v
        const uint32_t bwd = t0_forward ? t1 : t0;   // holds v -> u
        const uint32_t wf  = third(fwd);             // opposite the u -> v edge
        const uint32_t wb  = third(bwd);

        // Quad boundary, counter clockwise: u, wb, v, wf.
        const Vec3 pa = pos[u], pb = pos[wb], pc = pos[v], pd = pos[wf];
        const Vec3 before = area_of(t0) + area_of(t1);
        const Vec3 after0 = cross(pb - pa, pd - pa);   // (u, wb, wf)
        const Vec3 after1 = cross(pc - pb, pd - pb);   // (wb, v, wf)
        // Each new face is judged against the pair it replaces, scaled to its
        // own share, so a flip that leaves one sliver is refused like a fold.
        if (folds(before * 0.5f, after0) || folds(before * 0.5f, after1)) return false;

        const uint16_t region = tri_region[fwd];
        kill_triangle(t0);
        kill_triangle(t1);
        add_triangle(u, wb, wf, region);
        add_triangle(wb, v, wf, region);

        compact_adjacency(u);
        compact_adjacency(v);
        compact_adjacency(wf);
        compact_adjacency(wb);
        return true;
    }

    int valence(uint32_t v) const
    {
        std::vector<uint32_t> n;
        const_cast<Dyn*>(this)->neighbours(v, n);
        return int(n.size());
    }

    void collect_edges(std::vector<std::pair<uint32_t, uint32_t>>& out) const
    {
        out.clear();
        std::unordered_set<uint64_t> seen;
        seen.reserve(live_triangles * 3);
        for (size_t t = 0; t < tri_alive.size(); ++t) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri[t * 3 + i], b = tri[t * 3 + (i + 1) % 3];
                if (seen.insert(edge_key(a, b)).second)
                    out.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        std::sort(out.begin(), out.end());
    }

    void mark_pinned(bool region_borders)
    {
        std::fill(pinned.begin(), pinned.end(), 0);
        for (size_t t = 0; t < tri_alive.size(); ++t) {
            if (!tri_alive[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri[t * 3 + i], b = tri[t * 3 + (i + 1) % 3];
                // Count how many live faces carry this edge.
                int uses = 0;
                uint16_t other_region = tri_region[t];
                for (uint32_t n : vtri[a]) {
                    if (!tri_alive[n] || !has(n, b)) continue;
                    ++uses;
                    if (n != t) other_region = tri_region[n];
                }
                const bool boundary = uses < 2;
                const bool border   = region_borders && other_region != tri_region[t];
                if (boundary || border) { pinned[a] = 1; pinned[b] = 1; }
            }
        }
    }
};

// Interior angle quality of a quad: 1 for a perfect square, 0 for a sliver.
float quad_quality(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n0, Vec3 n1)
{
    const Vec3 p[4] = {a, b, c, d};
    float worst = 1.0f;
    for (int i = 0; i < 4; ++i) {
        const Vec3 e0 = normalize(p[(i + 3) % 4] - p[i]);
        const Vec3 e1 = normalize(p[(i + 1) % 4] - p[i]);
        if (length2(e0) < 1e-12f || length2(e1) < 1e-12f) return 0.0f;
        const float angle = std::acos(clampf(dot(e0, e1), -1.0f, 1.0f)) * kRad2Deg;
        worst = std::min(worst, 1.0f - saturate(std::fabs(angle - 90.0f) / 75.0f));
    }
    const float planarity = saturate(dot(n0, n1));
    return worst * planarity;
}

} // namespace

// ---------------------------------------------------------------------------
void transfer_regions(const Mesh& source, const Segmentation& seg, const Bvh& source_bvh,
                      Mesh& target)
{
    const size_t tcount = target.triangle_count();
    target.tri_region.assign(tcount, kNoRegion);
    if (seg.tri_region.empty() || source_bvh.empty()) return;

    ThreadPool::shared().parallel_ranges(tcount, 256, [&](size_t b, size_t e, unsigned) {
        for (size_t t = b; t < e; ++t) {
            const Vec3 c = target.triangle_centroid(t);
            const ClosestHit hit = source_bvh.closest_point(c);
            if (hit.hit() && hit.triangle < seg.tri_region.size())
                target.tri_region[t] = seg.tri_region[hit.triangle];
        }
    });
}

void transfer_skinning(const Mesh& source, const Bvh& source_bvh, Mesh& target,
                       int max_influences)
{
    if (!source.has_skin() || source_bvh.empty()) {
        target.skin.clear();
        return;
    }
    const int limit = std::clamp(max_influences <= 0 ? 4 : max_influences, 1, 4);
    const size_t vcount = target.vertex_count();
    target.skin.assign(vcount, SkinVertex{});

    ThreadPool::shared().parallel_ranges(vcount, 256, [&](size_t b, size_t e, unsigned) {
        for (size_t v = b; v < e; ++v) {
            const ClosestHit hit = source_bvh.closest_point(target.positions[v]);
            SkinVertex out;
            if (!hit.hit()) { out.weights[0] = 1.0f; target.skin[v] = out; continue; }

            // Barycentric blend of the three source vertices.
            const uint32_t i0 = source.indices[hit.triangle * 3 + 0];
            const uint32_t i1 = source.indices[hit.triangle * 3 + 1];
            const uint32_t i2 = source.indices[hit.triangle * 3 + 2];
            const Vec3 a = source.positions[i0], bb = source.positions[i1], c = source.positions[i2];

            const Vec3 v0 = bb - a, v1 = c - a, v2 = hit.point - a;
            const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
            const float d20 = dot(v2, v0), d21 = dot(v2, v1);
            const float denom = d00 * d11 - d01 * d01;
            float w1 = 0.0f, w2 = 0.0f;
            if (std::fabs(denom) > 1e-16f) {
                w1 = (d11 * d20 - d01 * d21) / denom;
                w2 = (d00 * d21 - d01 * d20) / denom;
            }
            const float w0 = 1.0f - w1 - w2;
            const float bary[3] = {saturate(w0), saturate(w1), saturate(w2)};
            const uint32_t idx[3] = {i0, i1, i2};

            std::unordered_map<uint16_t, float> accum;
            for (int k = 0; k < 3; ++k) {
                const SkinVertex& sv = source.skin[idx[k]];
                for (int c2 = 0; c2 < 4; ++c2)
                    if (sv.weights[c2] > 0.0f) accum[sv.joints[c2]] += sv.weights[c2] * bary[k];
            }

            std::vector<std::pair<uint16_t, float>> sorted(accum.begin(), accum.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& x, const auto& y) {
                if (x.second != y.second) return x.second > y.second;
                return x.first < y.first;
            });
            sorted.resize(std::min<size_t>(sorted.size(), size_t(limit)));

            float sum = 0.0f;
            for (const auto& [j, w] : sorted) sum += w;
            if (sum <= kEps) {
                out.weights[0] = 1.0f;
            } else {
                for (size_t k = 0; k < sorted.size(); ++k) {
                    out.joints[k]  = sorted[k].first;
                    out.weights[k] = sorted[k].second / sum;
                }
            }
            target.skin[v] = out;
        }
    });
}

// ---------------------------------------------------------------------------
RemeshResult quad_field_retopo(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const KnobPanel& panel, const SymmetryPlane& symmetry,
                               const RemeshOptions& opts,
                               const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    RemeshResult result;
    if (mesh.empty()) return result;

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

    const int budget_total = std::max(8, panel.total_budget());

    // --- 1. quadric prepass -------------------------------------------------
    report(0.02f, "quadric prepass");
    Mesh working;
    {
        std::vector<int> prepass_budgets;
        uint16_t max_region = 0;
        for (const Region& r : seg.regions) max_region = std::max(max_region, r.id);
        prepass_budgets.assign(size_t(max_region) + 1, std::numeric_limits<int>::max());
        for (const RegionKnobs& k : panel.regions)
            if (k.id < prepass_budgets.size())
                prepass_budgets[k.id] = std::max(
                    4, int(k.triangle_budget * std::max(1.0f, opts.prepass_multiplier)));

        QuadricOptions qopts;
        qopts.enforce_region_budgets = true;
        qopts.lock_symmetry_plane    = opts.enforce_symmetry;
        QuadricResult pre = quadric_simplify(
            mesh, analysis, seg, density, prepass_budgets, symmetry, qopts,
            [&](float f, const char* what) { report(0.02f + 0.28f * f, what); });
        working = std::move(pre.mesh);
    }
    if (working.empty()) return result;

    // --- 2. incremental remesh ---------------------------------------------
    report(0.32f, "remeshing");

    FieldSampler sampler;
    sampler.bvh           = &analysis.bvh;
    sampler.field         = &density;
    sampler.fallback_edge = std::max(analysis.mean_edge, kEps);

    Dyn dyn;
    dyn.from_mesh(working);
    dyn.flip_cos = std::cos(clampf(opts.max_normal_flip_degrees, 1.0f, 179.0f) * kDeg2Rad);
    dyn.mark_pinned(opts.preserve_region_borders);

    const bool  use_symmetry = opts.enforce_symmetry && symmetry.accepted;
    const float plane_eps    = analysis.bbox_diagonal * opts.symmetry_epsilon_rel;
    if (use_symmetry)
        for (size_t v = 0; v < dyn.pos.size(); ++v)
            dyn.on_plane[v] = std::fabs(dot(symmetry.normal, dyn.pos[v]) - symmetry.offset) <= plane_eps;

    auto snap = [&](Vec3 p, bool on_plane) {
        if (use_symmetry && on_plane)
            p = p - symmetry.normal * (dot(symmetry.normal, p) - symmetry.offset);
        return p;
    };

    std::vector<std::pair<uint32_t, uint32_t>> edges;
    std::vector<uint32_t> scratch;
    std::vector<uint32_t> neighbour_scratch;

    const int passes = std::clamp(opts.iterations, 1, 64);
    for (int pass = 0; pass < passes; ++pass) {
        const float pass_progress = float(pass) / float(passes);
        report(0.32f + 0.40f * pass_progress, "remeshing");

        // --- split ---------------------------------------------------------
        dyn.collect_edges(edges);
        for (const auto& [u, v] : edges) {
            if (!dyn.alive[u] || !dyn.alive[v]) continue;
            const Vec3  a = dyn.pos[u], b = dyn.pos[v];
            const Vec3  mid = (a + b) * 0.5f;
            const float target = sampler.edge_at(mid);
            if (length(b - a) <= target * opts.split_ratio) continue;
            const bool plane = dyn.on_plane[u] && dyn.on_plane[v];
            if (dyn.split_edge(u, v, snap(sampler.project(mid), plane), scratch)) ++result.splits;
        }

        // --- collapse ------------------------------------------------------
        dyn.collect_edges(edges);
        for (const auto& [u, v] : edges) {
            if (!dyn.alive[u] || !dyn.alive[v]) continue;
            const Vec3  a = dyn.pos[u], b = dyn.pos[v];
            const float len = length(b - a);
            const Vec3  mid = (a + b) * 0.5f;
            const float target = sampler.edge_at(mid);
            if (len >= target * opts.collapse_ratio) continue;

            // A pinned vertex may only be collapsed onto another pinned one,
            // otherwise region borders and open boundaries crawl.
            uint32_t from = u, into = v;
            if (dyn.pinned[u] && !dyn.pinned[v])      { from = v; into = u; }
            else if (!dyn.pinned[u] && dyn.pinned[v]) { from = u; into = v; }
            else if (dyn.pinned[u] && dyn.pinned[v])  { /* both pinned: allowed */ }

            const bool plane = dyn.on_plane[from] && dyn.on_plane[into];
            Vec3 np = dyn.pinned[into] && !dyn.pinned[from] ? dyn.pos[into]
                                                           : sampler.project(mid);
            np = snap(np, plane);
            const size_t removed = dyn.collapse_edge(from, into, np, scratch);
            if (removed) ++result.collapses;
        }

        // --- flip toward valence 6 ------------------------------------------
        dyn.collect_edges(edges);
        for (const auto& [u, v] : edges) {
            if (!dyn.alive[u] || !dyn.alive[v]) continue;
            if (dyn.pinned[u] && dyn.pinned[v]) continue;  // never flip a border

            dyn.shared(u, v, scratch);
            if (scratch.size() != 2) continue;
            const uint32_t t0 = scratch[0], t1 = scratch[1];
            if (dyn.tri_region[t0] != dyn.tri_region[t1]) continue;

            auto third = [&](uint32_t t) {
                for (int i = 0; i < 3; ++i) {
                    const uint32_t w = dyn.tri[t * 3 + i];
                    if (w != u && w != v) return w;
                }
                return kInvalidIndex;
            };
            const uint32_t w0 = third(t0), w1 = third(t1);
            if (w0 == kInvalidIndex || w1 == kInvalidIndex) continue;

            const int target_u = dyn.is_boundary_vertex(u) ? 4 : 6;
            const int target_v = dyn.is_boundary_vertex(v) ? 4 : 6;
            const int target_0 = dyn.is_boundary_vertex(w0) ? 4 : 6;
            const int target_1 = dyn.is_boundary_vertex(w1) ? 4 : 6;

            const int du = dyn.valence(u), dv = dyn.valence(v);
            const int d0 = dyn.valence(w0), d1 = dyn.valence(w1);

            const int before = std::abs(du - target_u) + std::abs(dv - target_v) +
                               std::abs(d0 - target_0) + std::abs(d1 - target_1);
            const int after  = std::abs(du - 1 - target_u) + std::abs(dv - 1 - target_v) +
                               std::abs(d0 + 1 - target_0) + std::abs(d1 + 1 - target_1);
            if (after >= before) continue;

            if (dyn.flip_edge(u, v, scratch)) ++result.flips;
        }

        // --- tangential relaxation + reprojection ---------------------------
        for (int relax = 0; relax < std::max(0, opts.relax_passes); ++relax) {
            std::vector<Vec3> moved(dyn.pos.size());
            for (size_t v = 0; v < dyn.pos.size(); ++v) moved[v] = dyn.pos[v];

            for (uint32_t v = 0; v < dyn.pos.size(); ++v) {
                if (!dyn.alive[v] || dyn.pinned[v]) continue;
                dyn.neighbours(v, neighbour_scratch);
                if (neighbour_scratch.size() < 3) continue;

                Vec3 centroid{};
                for (uint32_t n : neighbour_scratch) centroid += dyn.pos[n];
                centroid = centroid / float(neighbour_scratch.size());

                // Average the incident face normals to get a tangent plane.
                Vec3 normal{};
                for (uint32_t t : dyn.vtri[v])
                    if (dyn.tri_alive[t]) normal += dyn.normal_of(t);
                normal = normalize(normal);
                if (length2(normal) < 0.5f) continue;

                Vec3 delta = centroid - dyn.pos[v];
                delta = delta - normal * dot(delta, normal);
                Vec3 candidate = dyn.pos[v] + delta * clampf(opts.relax_strength, 0.0f, 1.0f);
                candidate = sampler.project(candidate);
                moved[v]  = snap(candidate, dyn.on_plane[v] != 0);
            }
            dyn.pos.swap(moved);
        }
    }

    // --- 3. trim to budget --------------------------------------------------
    if (opts.trim_to_budget && dyn.live_triangles > size_t(budget_total)) {
        report(0.74f, "trimming to budget");
        float ratio = opts.collapse_ratio;
        int   guard = 0;
        while (dyn.live_triangles > size_t(budget_total) && guard++ < 24) {
            ratio *= 1.12f;
            size_t before = dyn.live_triangles;

            dyn.collect_edges(edges);
            // Shortest relative to the local target first.
            std::vector<std::pair<float, std::pair<uint32_t, uint32_t>>> scored;
            scored.reserve(edges.size());
            for (const auto& e : edges) {
                if (!dyn.alive[e.first] || !dyn.alive[e.second]) continue;
                const Vec3 mid = (dyn.pos[e.first] + dyn.pos[e.second]) * 0.5f;
                const float target = std::max(sampler.edge_at(mid), kEps);
                scored.push_back({length(dyn.pos[e.second] - dyn.pos[e.first]) / target, e});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            for (const auto& [rel, e] : scored) {
                if (dyn.live_triangles <= size_t(budget_total)) break;
                if (rel > ratio) break;
                const uint32_t u = e.first, v = e.second;
                if (!dyn.alive[u] || !dyn.alive[v]) continue;

                uint32_t from = u, into = v;
                if (dyn.pinned[u] && !dyn.pinned[v])      { from = v; into = u; }
                else if (!dyn.pinned[u] && dyn.pinned[v]) { from = u; into = v; }

                const bool plane = dyn.on_plane[from] && dyn.on_plane[into];
                Vec3 np = dyn.pinned[into] && !dyn.pinned[from]
                              ? dyn.pos[into]
                              : sampler.project((dyn.pos[u] + dyn.pos[v]) * 0.5f);
                np = snap(np, plane);
                if (dyn.collapse_edge(from, into, np, scratch)) ++result.collapses;
            }
            if (dyn.live_triangles == before) break;   // no progress, stop trying
        }
    }

    // --- 4. quad pairing, then back to triangles ---------------------------
    Mesh out = dyn.to_mesh();
    out.remove_degenerate();
    out.compact();

    if (opts.pair_into_quads && out.triangle_count() >= 2) {
        report(0.84f, "pairing quads");

        const float floor_q = clampf(opts.quad_quality_floor *
                                         (1.0f - 0.8f * panel.global.quad_dominance),
                                     0.02f, 0.95f);

        MeshTopology topo;
        topo.build(out);

        struct Pair { float quality; uint32_t t0, t1; uint32_t a, b, c, d; };
        std::vector<Pair> candidates;
        candidates.reserve(topo.edges.size());

        for (const MeshTopology::Edge& e : topo.edges) {
            if (e.tri1 == kInvalidIndex || e.uses != 2) continue;
            if (out.tri_region.size() == out.triangle_count() &&
                out.tri_region[e.tri0] != out.tri_region[e.tri1]) continue;

            auto third = [&](uint32_t t) {
                for (int i = 0; i < 3; ++i) {
                    const uint32_t w = out.indices[t * 3 + i];
                    if (w != e.v0 && w != e.v1) return w;
                }
                return kInvalidIndex;
            };
            const uint32_t w0 = third(e.tri0), w1 = third(e.tri1);
            if (w0 == kInvalidIndex || w1 == kInvalidIndex) continue;

            const float q = quad_quality(out.positions[e.v0], out.positions[w0],
                                         out.positions[e.v1], out.positions[w1],
                                         out.triangle_normal(e.tri0),
                                         out.triangle_normal(e.tri1));
            if (q < floor_q) continue;
            candidates.push_back({q, e.tri0, e.tri1, e.v0, w0, e.v1, w1});
        }

        std::sort(candidates.begin(), candidates.end(), [](const Pair& x, const Pair& y) {
            if (x.quality != y.quality) return x.quality > y.quality;
            return x.t0 < y.t0;
        });

        std::vector<uint8_t> taken(out.triangle_count(), 0);
        std::vector<uint32_t> new_indices;
        std::vector<uint16_t> new_regions;
        new_indices.reserve(out.indices.size());
        const bool keep_regions = out.tri_region.size() == out.triangle_count();

        auto emit = [&](uint32_t a, uint32_t b, uint32_t c, uint16_t r) {
            new_indices.push_back(a); new_indices.push_back(b); new_indices.push_back(c);
            if (keep_regions) new_regions.push_back(r);
        };

        for (const Pair& p : candidates) {
            if (taken[p.t0] || taken[p.t1]) continue;
            taken[p.t0] = taken[p.t1] = 1;
            ++result.quads;

            // Quad corners in order: a, b, c, d. Re-triangulate along whichever
            // diagonal keeps the larger minimum angle.
            const Vec3 pa = out.positions[p.a], pb = out.positions[p.b];
            const Vec3 pc = out.positions[p.c], pd = out.positions[p.d];
            const float diag_ac = length(pc - pa);
            const float diag_bd = length(pd - pb);
            const uint16_t r = keep_regions ? out.tri_region[p.t0] : kNoRegion;

            if (diag_ac <= diag_bd) {
                emit(p.a, p.b, p.c, r);
                emit(p.a, p.c, p.d, r);
            } else {
                emit(p.b, p.c, p.d, r);
                emit(p.b, p.d, p.a, r);
            }
        }
        for (size_t t = 0; t < out.triangle_count(); ++t) {
            if (taken[t]) continue;
            emit(out.indices[t * 3], out.indices[t * 3 + 1], out.indices[t * 3 + 2],
                 keep_regions ? out.tri_region[t] : kNoRegion);
        }

        out.indices.swap(new_indices);
        if (keep_regions) out.tri_region.swap(new_regions);
        result.quad_ratio = out.triangle_count()
                                ? float(result.quads * 2) / float(out.triangle_count())
                                : 0.0f;
    }

    report(0.94f, "finalising");
    out.name             = mesh.name;
    out.armature         = mesh.armature;
    out.import_transform = mesh.import_transform;
    out.import_scale     = mesh.import_scale;
    out.remove_degenerate();
    out.compact();
    transfer_regions(mesh, seg, analysis.bvh, out);
    out.compute_normals(60.0f);
    // Quad pairing re-emits triangles from a quad whose corner order came from
    // two different faces; a badly shaped quad can hand back a reversed one.
    fix_winding(out, analysis.bvh);
    out.compute_normals(60.0f);

    uint16_t max_region = 0;
    for (const Region& r : seg.regions) max_region = std::max(max_region, r.id);
    result.region_triangles.assign(size_t(max_region) + 1, 0);
    for (uint16_t r : out.tri_region)
        if (r < result.region_triangles.size()) ++result.region_triangles[r];

    result.mesh    = std::move(out);
    result.seconds = watch.seconds();
    report(1.0f, "done");

    RD_INFO("quad field: %zu tri, %zu splits / %zu collapses / %zu flips, "
            "%zu quads (%.0f%% quad coverage) in %s",
            result.mesh.triangle_count(), result.splits, result.collapses, result.flips,
            result.quads, result.quad_ratio * 100.0f,
            format_duration(result.seconds).c_str());
    return result;
}

} // namespace rd
