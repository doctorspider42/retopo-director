#include "bake/charts.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace rd {
namespace {

inline uint64_t edge_key(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

struct Topology {
    // Undirected edge -> the triangles on it.
    std::unordered_map<uint64_t, std::vector<uint32_t>> edge_tris;
    explicit Topology(const Mesh& m)
    {
        edge_tris.reserve(m.indices.size());
        for (uint32_t t = 0; t < m.triangle_count(); ++t)
            for (int i = 0; i < 3; ++i)
                edge_tris[edge_key(m.indices[t * 3 + i], m.indices[t * 3 + (i + 1) % 3])].push_back(t);
    }
};

// The one other triangle across edge (a, b) inside `in_chart`, if exactly one.
uint32_t across(const Topology& topo, uint32_t t, uint32_t a, uint32_t b,
                const std::vector<int>& chart_of, int chart)
{
    const auto it = topo.edge_tris.find(edge_key(a, b));
    if (it == topo.edge_tris.end() || it->second.size() != 2) return kInvalidIndex;
    const uint32_t o = it->second[0] == t ? it->second[1] : it->second[0];
    return chart_of[o] == chart ? o : kInvalidIndex;
}

// ---------------------------------------------------------------------------
// One chart being made into a disk and flattened.
struct Chart {
    std::vector<uint32_t> tris;
    // Filled by flatten(): per corner (3 per chart triangle) the uv vertex it
    // uses, and the uvs.
    std::vector<uint32_t> corner_vertex;
    std::vector<uint32_t> vertex_source;   // uv vertex -> mesh vertex
    std::vector<Vec2>     uv;
};

// Flattening a disk chart without folds: a Tutte embedding with mean value
// weights (Floater 2003) - boundary on a circle, every interior vertex a
// positive combination of its neighbours, which cannot fold - then a few
// as-rigid-as-possible passes (Liu et al. 2008) to give the triangles their
// shapes and areas back, keeping the last pass that has none folded.
//
// LSCM came first and folded on anything round: a paw or a head cap pinned at
// two far points collapses triangles to nothing near the pins.
bool flatten_chart(const Mesh& mesh, const Chart& c, std::vector<Vec2>& uv)
{
    const size_t n = c.vertex_source.size();
    const size_t m = c.tris.size();
    if (n < 3 || m == 0) return false;
    auto P = [&](uint32_t v) { return mesh.positions[c.vertex_source[v]]; };
    auto tri = [&](size_t k, int i) { return c.corner_vertex[k * 3 + i]; };

    // --- boundary: one loop, in order ----------------------------------------
    std::unordered_map<uint64_t, int> uses;
    for (size_t k = 0; k < m; ++k)
        for (int i = 0; i < 3; ++i) ++uses[edge_key(tri(k, i), tri(k, (i + 1) % 3))];
    std::unordered_map<uint32_t, uint32_t> next;
    for (size_t k = 0; k < m; ++k)
        for (int i = 0; i < 3; ++i) {
            const uint32_t a = tri(k, i), b = tri(k, (i + 1) % 3);
            if (uses[edge_key(a, b)] != 1) continue;
            if (!next.emplace(a, b).second) return false;   // pinched boundary
        }
    if (next.size() < 3) return false;
    std::vector<uint32_t> loop{next.begin()->first};
    while (true) {
        const auto it = next.find(loop.back());
        if (it == next.end()) return false;
        if (it->second == loop.front()) break;
        loop.push_back(it->second);
        if (loop.size() > next.size()) return false;
    }
    if (loop.size() != next.size()) return false;   // more than one boundary loop

    // --- Tutte embedding with mean value weights -----------------------------
    uv.assign(n, Vec2{});
    std::vector<uint8_t> on_boundary(n, 0);
    {
        std::vector<float> acc(loop.size() + 1, 0.0f);
        for (size_t i = 0; i < loop.size(); ++i)
            acc[i + 1] = acc[i] + length(P(loop[(i + 1) % loop.size()]) - P(loop[i]));
        const float total = std::max(acc.back(), 1e-12f);
        // The loop runs the way the triangles wind, so the circle is walked
        // counter clockwise and the chart comes out facing up.
        for (size_t i = 0; i < loop.size(); ++i) {
            const float a = 2.0f * kPi * acc[i] / total;
            uv[loop[i]] = {std::cos(a), std::sin(a)};
            on_boundary[loop[i]] = 1;
        }
    }
    // Mean value weight of neighbour j at vertex i: (tan(a1/2) + tan(a2/2)) / |ij|,
    // a1 and a2 the angles at i in the two triangles on edge ij.
    std::vector<std::unordered_map<uint32_t, double>> w(n);
    for (size_t k = 0; k < m; ++k)
        for (int i = 0; i < 3; ++i) {
            const uint32_t a = tri(k, i), b = tri(k, (i + 1) % 3), d = tri(k, (i + 2) % 3);
            const Vec3 eb = P(b) - P(a), ed = P(d) - P(a);
            const float lb = length(eb), ld = length(ed);
            if (lb <= 0.0f || ld <= 0.0f) continue;
            const float cosang = clampf(dot(eb, ed) / (lb * ld), -1.0f, 1.0f);
            const double t = std::tan(0.5 * std::acos(double(cosang)));
            w[a][b] += t / lb;
            w[a][d] += t / ld;
        }
    for (int sweep = 0; sweep < 4000; ++sweep) {
        double moved = 0.0;
        for (uint32_t v = 0; v < n; ++v) {
            if (on_boundary[v] || w[v].empty()) continue;
            double sw = 0.0, su = 0.0, sv = 0.0;
            for (const auto& [j, wj] : w[v]) { sw += wj; su += wj * uv[j].x; sv += wj * uv[j].y; }
            if (sw <= 0.0) continue;
            const Vec2 nu{float(su / sw), float(sv / sw)};
            moved = std::max(moved, double(length(nu - uv[v])));
            uv[v] = nu;
        }
        if (moved < 1e-7) break;
    }

    auto signed_area = [&](const std::vector<Vec2>& q, size_t k) {
        const Vec2 a = q[tri(k, 0)], b = q[tri(k, 1)], d = q[tri(k, 2)];
        return 0.5f * ((b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x));
    };
    // The loop's direction decides which way up the circle maps; turn the
    // whole chart over if it came out mirrored.
    {
        double total = 0.0;
        for (size_t k = 0; k < m; ++k) total += signed_area(uv, k);
        if (total < 0.0) for (Vec2& p : uv) p.y = -p.y;
    }
    auto folds = [&](const std::vector<Vec2>& q) {
        for (size_t k = 0; k < m; ++k)
            if (signed_area(q, k) <= 0.0f && mesh.triangle_area(c.tris[k]) > 0.0f) return true;
        return false;
    };
    if (folds(uv)) return false;

    // --- ARAP: local rotations, global cotangent solve -------------------------
    // Each triangle in its own plane, and cotangent weights per edge (clamped:
    // an obtuse sliver's negative weight would make the system indefinite).
    struct Local { Vec2 x[3]; float cot[3]; };   // cot[i]: angle opposite edge (i, i+1)
    std::vector<Local> loc(m);
    for (size_t k = 0; k < m; ++k) {
        const Vec3 a = P(tri(k, 0)), b = P(tri(k, 1)), d = P(tri(k, 2));
        const Vec3 e1 = b - a;
        const Vec3 cr = cross(e1, d - a);
        const float l1 = length(e1), lc = length(cr);
        Local L{};
        if (l1 > 0.0f && lc > 0.0f) {
            const Vec3 ex = e1 / l1, ey = cross(cr / lc, ex);
            L.x[0] = {0.0f, 0.0f};
            L.x[1] = {l1, 0.0f};
            L.x[2] = {dot(d - a, ex), dot(d - a, ey)};
            for (int i = 0; i < 3; ++i) {
                const Vec2 o = L.x[(i + 2) % 3];
                const Vec2 u = L.x[i] - o, v = L.x[(i + 1) % 3] - o;
                const float cr2 = u.x * v.y - u.y * v.x;
                L.cot[i] = std::max(0.05f, (u.x * v.x + u.y * v.y) / std::max(std::fabs(cr2), 1e-12f));
            }
        }
        loc[k] = L;
    }
    std::vector<std::unordered_map<uint32_t, double>> lap(n);
    for (size_t k = 0; k < m; ++k)
        for (int i = 0; i < 3; ++i) {
            const uint32_t a = tri(k, i), b = tri(k, (i + 1) % 3);
            lap[a][b] += loc[k].cot[i];
            lap[b][a] += loc[k].cot[i];
        }
    std::vector<double> diag(n, 0.0);
    for (uint32_t v = 0; v < n; ++v)
        for (const auto& [j, wj] : lap[v]) diag[v] += wj;

    // Scale the Tutte layout to the surface's area first: ARAP fits lengths.
    {
        double a2 = 0.0, a3 = 0.0;
        for (size_t k = 0; k < m; ++k) { a2 += signed_area(uv, k); a3 += mesh.triangle_area(c.tris[k]); }
        if (a2 > 0.0) { const float s = float(std::sqrt(a3 / a2)); for (Vec2& p : uv) p = p * s; }
    }

    std::vector<Vec2> best = uv;
    const uint32_t anchor = loop.front();
    std::vector<double> bu(n), bv(n);
    for (int pass = 0; pass < 40; ++pass) {
        // Local: the rotation closest to each triangle's current map.
        std::vector<std::array<float, 4>> rot(m);
        for (size_t k = 0; k < m; ++k) {
            double s00 = 0, s01 = 0, s10 = 0, s11 = 0;
            for (int i = 0; i < 3; ++i) {
                const Vec2 dx = loc[k].x[i] - loc[k].x[(i + 1) % 3];
                const Vec2 du = uv[tri(k, i)] - uv[tri(k, (i + 1) % 3)];
                const double wgt = loc[k].cot[i];
                s00 += wgt * du.x * dx.x; s01 += wgt * du.x * dx.y;
                s10 += wgt * du.y * dx.x; s11 += wgt * du.y * dx.y;
            }
            // Closest rotation to [[s00 s01][s10 s11]]: angle of (s00 + s11, s10 - s01).
            const double ang = std::atan2(s10 - s01, s00 + s11);
            rot[k] = {float(std::cos(ang)), float(-std::sin(ang)), float(std::sin(ang)), float(std::cos(ang))};
        }
        // Global: L u = b, by Gauss-Seidel from the current layout (it is close).
        std::fill(bu.begin(), bu.end(), 0.0);
        std::fill(bv.begin(), bv.end(), 0.0);
        for (size_t k = 0; k < m; ++k)
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = tri(k, i), b = tri(k, (i + 1) % 3);
                const Vec2 dx = loc[k].x[i] - loc[k].x[(i + 1) % 3];
                const auto& R = rot[k];
                const double rx = R[0] * dx.x + R[1] * dx.y, ry = R[2] * dx.x + R[3] * dx.y;
                const double wgt = loc[k].cot[i];
                bu[a] += wgt * rx; bv[a] += wgt * ry;
                bu[b] -= wgt * rx; bv[b] -= wgt * ry;
            }
        std::vector<Vec2> next_uv = uv;
        for (int sweep = 0; sweep < 200; ++sweep) {
            double moved = 0.0;
            for (uint32_t v = 0; v < n; ++v) {
                if (v == anchor || diag[v] <= 0.0) continue;
                double su = bu[v], sv = bv[v];
                for (const auto& [j, wj] : lap[v]) { su += wj * next_uv[j].x; sv += wj * next_uv[j].y; }
                const Vec2 nu{float(su / diag[v]), float(sv / diag[v])};
                moved = std::max(moved, double(length(nu - next_uv[v])));
                next_uv[v] = nu;
            }
            if (moved < 1e-7) break;
        }
        // Starting from the circle, the first passes often fold a triangle or
        // two on the way to unfolding the chart; carry on, and keep the last
        // layout that had none.
        uv = next_uv;
        if (!folds(uv)) best = uv;
    }
    uv = best;
    for (const Vec2& p : uv)
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    return true;
}

bool segments_cross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    auto orient = [](Vec2 p, Vec2 q, Vec2 r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const float o1 = orient(a, b, c), o2 = orient(a, b, d);
    const float o3 = orient(c, d, a), o4 = orient(c, d, b);
    return ((o1 > 0.0f) != (o2 > 0.0f)) && ((o3 > 0.0f) != (o4 > 0.0f)) &&
           o1 != 0.0f && o2 != 0.0f && o3 != 0.0f && o4 != 0.0f;
}

} // namespace

PartsUnwrapResult unwrap_by_parts(Mesh& mesh, const std::vector<float>& visibility,
                                  const PartsUnwrapOptions& opts)
{
    PartsUnwrapResult res;
    const uint32_t tcount = uint32_t(mesh.triangle_count());
    if (tcount == 0) { res.error = "empty mesh"; return res; }
    const bool have_regions = mesh.tri_region.size() == tcount;
    const Topology topo(mesh);

    auto vis = [&](uint32_t v) {
        return v < visibility.size() ? clampf(visibility[v], 0.0f, 1.0f) : 1.0f;
    };

    // --- 1. regions as charts, split into connected pieces ---------------------
    std::vector<int> chart_of(tcount, -1);
    int chart_count = 0;
    for (uint32_t seed = 0; seed < tcount; ++seed) {
        if (chart_of[seed] >= 0) continue;
        const int id = chart_count++;
        std::vector<uint32_t> stack{seed};
        chart_of[seed] = id;
        while (!stack.empty()) {
            const uint32_t t = stack.back();
            stack.pop_back();
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                const auto it = topo.edge_tris.find(edge_key(a, b));
                if (it == topo.edge_tris.end() || it->second.size() != 2) continue;
                const uint32_t o = it->second[0] == t ? it->second[1] : it->second[0];
                if (chart_of[o] >= 0) continue;
                if (have_regions && mesh.tri_region[o] != mesh.tri_region[t]) continue;
                chart_of[o] = id;
                stack.push_back(o);
            }
        }
    }

    // --- 2. small pieces join the neighbour they share most border with ------
    for (bool merged = true; merged;) {
        merged = false;
        std::vector<int> size(chart_count, 0);
        for (int c : chart_of) ++size[c];
        for (int c = 0; c < chart_count; ++c) {
            if (size[c] == 0 || size[c] >= opts.min_chart_triangles) continue;
            std::unordered_map<int, float> border;
            for (uint32_t t = 0; t < tcount; ++t) {
                if (chart_of[t] != c) continue;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                    const auto it = topo.edge_tris.find(edge_key(a, b));
                    if (it == topo.edge_tris.end() || it->second.size() != 2) continue;
                    const uint32_t o = it->second[0] == t ? it->second[1] : it->second[0];
                    if (chart_of[o] != c)
                        border[chart_of[o]] += length(mesh.positions[a] - mesh.positions[b]);
                }
            }
            int best = -1;
            float best_len = 0.0f;
            for (const auto& [o, len] : border)
                if (len > best_len) { best_len = len; best = o; }
            if (best < 0) continue;
            for (int& x : chart_of) if (x == c) x = best;
            size[best] += size[c];
            size[c] = 0;
            merged = true;
        }
    }

    std::deque<Chart> pending;
    {
        std::vector<Chart> by_id(chart_count);
        for (uint32_t t = 0; t < tcount; ++t) by_id[chart_of[t]].tris.push_back(t);
        for (Chart& c : by_id) if (!c.tris.empty()) pending.push_back(std::move(c));
    }

    // --- 3. each chart: cut to a disk, flatten, check, split if it must ------
    // build() does one chart: marks its triangles with a fresh id (so stale ids
    // from earlier attempts never match), cuts, flattens and judges it.
    auto build = [&](Chart& c, int cid) -> bool {
        for (uint32_t t : c.tris) chart_of[t] = cid;

        // Edges of the chart: interior (two chart triangles) or border.
        std::unordered_map<uint64_t, int> uses;
        for (uint32_t t : c.tris)
            for (int i = 0; i < 3; ++i)
                if (across(topo, t, mesh.indices[t * 3 + i], mesh.indices[t * 3 + (i + 1) % 3], chart_of, cid) !=
                    kInvalidIndex)
                    ++uses[edge_key(mesh.indices[t * 3 + i], mesh.indices[t * 3 + (i + 1) % 3])];
        // uses == 2 means interior (counted from both sides).

        // Vertices of the chart, and which lie on a border.
        std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, float>>> adj;
        std::unordered_map<uint32_t, uint8_t> on_border;
        for (uint32_t t : c.tris)
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                const bool interior = uses.count(edge_key(a, b)) && uses[edge_key(a, b)] == 2;
                if (!interior) { on_border[a] = 1; on_border[b] = 1; }
                const float cost = length(mesh.positions[a] - mesh.positions[b]) *
                                   (1.0f + opts.seam_visibility_weight * 0.5f * (vis(a) + vis(b)));
                adj[a].push_back({b, cost});
            }

        // Border loops as connected components of border vertices over border edges.
        std::unordered_map<uint32_t, int> loop_of;
        int loops = 0;
        {
            std::unordered_map<uint32_t, std::vector<uint32_t>> badj;
            for (uint32_t t : c.tris)
                for (int i = 0; i < 3; ++i) {
                    const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                    const uint64_t k = edge_key(a, b);
                    if (uses.count(k) && uses[k] == 2) continue;
                    badj[a].push_back(b);
                    badj[b].push_back(a);
                }
            for (const auto& [v, _] : badj) {
                if (loop_of.count(v)) continue;
                std::vector<uint32_t> stack{v};
                loop_of[v] = loops;
                while (!stack.empty()) {
                    const uint32_t x = stack.back();
                    stack.pop_back();
                    for (uint32_t w : badj[x])
                        if (!loop_of.count(w)) { loop_of[w] = loops; stack.push_back(w); }
                }
                ++loops;
            }
        }

        // Cut edges: paths joining every loop into one, or a slit through a
        // closed chart, each the cheapest by length and visibility.
        std::unordered_map<uint64_t, uint8_t> cut;
        auto cheapest_path = [&](const std::vector<uint32_t>& from, auto&& is_goal) {
            std::unordered_map<uint32_t, float> dist;
            std::unordered_map<uint32_t, uint32_t> prev;
            using E = std::pair<float, uint32_t>;
            std::priority_queue<E, std::vector<E>, std::greater<E>> q;
            for (uint32_t v : from) { dist[v] = 0.0f; q.push({0.0f, v}); }
            uint32_t goal = kInvalidIndex;
            while (!q.empty()) {
                const auto [d, v] = q.top();
                q.pop();
                if (d > dist[v]) continue;
                if (is_goal(v)) { goal = v; break; }
                for (const auto& [w, len] : adj[v]) {
                    const float nd = d + len;
                    const auto it = dist.find(w);
                    if (it == dist.end() || nd < it->second) { dist[w] = nd; prev[w] = v; q.push({nd, w}); }
                }
            }
            std::vector<uint32_t> path;
            for (uint32_t v = goal; v != kInvalidIndex;) {
                path.push_back(v);
                const auto it = prev.find(v);
                v = it == prev.end() ? kInvalidIndex : it->second;
            }
            return path;
        };
        bool cut_failed = false;
        if (loops == 0) {
            // Closed: slit it between two far apart points, through the hidden side.
            uint32_t a = c.tris.empty() ? 0 : mesh.indices[c.tris[0] * 3];
            for (int pass = 0; pass < 2; ++pass) {
                float best = -1.0f;
                uint32_t far = a;
                for (const auto& [v, _] : adj) {
                    const float d = length2(mesh.positions[v] - mesh.positions[a]);
                    if (d > best) { best = d; far = v; }
                }
                a = pass == 0 ? far : a;
                if (pass == 1) {
                    const std::vector<uint32_t> path =
                        cheapest_path({a}, [&](uint32_t v) { return v == far; });
                    if (path.size() < 2) cut_failed = true;
                    for (size_t i = 1; i < path.size(); ++i) cut[edge_key(path[i - 1], path[i])] = 1;
                }
            }
        } else if (loops > 1) {
            // Grow one boundary by joining the nearest other loop to it, until one remains.
            std::vector<uint8_t> joined(loops, 0);
            joined[0] = 1;
            for (int step = 1; step < loops && !cut_failed; ++step) {
                std::vector<uint32_t> from;
                for (const auto& [v, l] : loop_of) if (joined[l]) from.push_back(v);
                // Vertices already on a cut belong to the joined boundary too.
                for (const auto& [k, _] : cut) {
                    from.push_back(uint32_t(k >> 32));
                    from.push_back(uint32_t(k & 0xffffffffu));
                }
                const std::vector<uint32_t> path = cheapest_path(from, [&](uint32_t v) {
                    const auto it = loop_of.find(v);
                    return it != loop_of.end() && !joined[it->second];
                });
                if (path.empty()) { cut_failed = true; break; }
                joined[loop_of[path.front()]] = 1;
                for (size_t i = 1; i < path.size(); ++i) cut[edge_key(path[i - 1], path[i])] = 1;
            }
        }

        // Corners to uv vertices: corners of one mesh vertex share a uv vertex
        // when their triangles meet across an interior edge that is not cut.
        const size_t corners = c.tris.size() * 3;
        std::vector<uint32_t> parent(corners);
        std::iota(parent.begin(), parent.end(), 0u);
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        std::unordered_map<uint32_t, uint32_t> local_tri;
        for (uint32_t k = 0; k < c.tris.size(); ++k) local_tri[c.tris[k]] = k;
        for (uint32_t k = 0; k < c.tris.size(); ++k) {
            const uint32_t t = c.tris[k];
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                if (cut.count(edge_key(a, b))) continue;
                const uint32_t o = across(topo, t, a, b, chart_of, cid);
                if (o == kInvalidIndex) continue;
                const uint32_t ko = local_tri[o];
                for (int j = 0; j < 3; ++j) {
                    const uint32_t w = mesh.indices[o * 3 + j];
                    if (w == a) parent[find(k * 3 + i)] = find(ko * 3 + j);
                    if (w == b) parent[find(k * 3 + (i + 1) % 3)] = find(ko * 3 + j);
                }
            }
        }
        std::unordered_map<uint32_t, uint32_t> group_vertex;
        c.corner_vertex.assign(corners, 0);
        c.vertex_source.clear();
        for (uint32_t q = 0; q < corners; ++q) {
            const uint32_t g = find(q);
            auto it = group_vertex.find(g);
            if (it == group_vertex.end()) {
                it = group_vertex.emplace(g, uint32_t(c.vertex_source.size())).first;
                c.vertex_source.push_back(mesh.indices[c.tris[q / 3] * 3 + q % 3]);
            }
            c.corner_vertex[q] = it->second;
        }

        // Flatten and judge.
        bool valid = !cut_failed && flatten_chart(mesh, c, c.uv);
        if (valid) {
            double area3 = 0.0, area2 = 0.0;
            std::vector<float> ratio(c.tris.size());
            for (size_t k = 0; k < c.tris.size(); ++k) {
                const uint32_t t = c.tris[k];
                const float a3 = mesh.triangle_area(t);
                const Vec2 p = c.uv[c.corner_vertex[k * 3]], q = c.uv[c.corner_vertex[k * 3 + 1]],
                           r = c.uv[c.corner_vertex[k * 3 + 2]];
                const float a2 = 0.5f * ((q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x));
                if (a2 <= 0.0f && a3 > 0.0f) { valid = false; break; }
                ratio[k] = a3 > 0.0f ? a2 / a3 : 1.0f;
                area3 += a3;
                area2 += a2;
            }
            if (valid && c.tris.size() > 1) {
                // Density spread over the surface, by area, over the same 96% of
                // it the validator judges (texture.stretch). A sliver's ratio
                // says little, and on a chart of six triangles one sliver was
                // the whole minimum.
                std::vector<std::pair<float, float>> by_ratio;   // (ratio, area)
                double total = 0.0;
                for (size_t k = 0; k < c.tris.size(); ++k) {
                    const float a3 = mesh.triangle_area(c.tris[k]);
                    by_ratio.push_back({ratio[k], a3});
                    total += a3;
                }
                std::sort(by_ratio.begin(), by_ratio.end());
                float lo = by_ratio.front().first, hi = by_ratio.back().first;
                double acc = 0.0;
                bool have_lo = false;
                for (const auto& [r, a] : by_ratio) {
                    acc += a;
                    if (!have_lo && acc >= 0.02 * total) { lo = r; have_lo = true; }
                    if (acc >= 0.98 * total) { hi = r; break; }
                }
                if (lo <= 0.0f || hi / lo > opts.max_density_ratio) valid = false;
                // And no single triangle far off the chart's own density: the
                // percentiles above let 2% of the surface through, and a merged
                // chart hid there one triangle spanning half the atlas.
                const float mean = float(area2 / std::max(area3, 1e-30));
                for (size_t k = 0; k < c.tris.size() && valid; ++k) {
                    if (mesh.triangle_area(c.tris[k]) < 1e-4 * area3) continue;   // degenerate
                    const float rel = ratio[k] / std::max(mean, 1e-30f);
                    if (rel > opts.max_triangle_density || rel < 1.0f / opts.max_triangle_density) valid = false;
                }
            }
            if (valid) {
                // The boundary must not cross itself.
                std::vector<std::pair<uint32_t, uint32_t>> border;
                std::unordered_map<uint64_t, int> uv_uses;
                for (size_t k = 0; k < c.tris.size(); ++k)
                    for (int i = 0; i < 3; ++i)
                        ++uv_uses[edge_key(c.corner_vertex[k * 3 + i], c.corner_vertex[k * 3 + (i + 1) % 3])];
                for (const auto& [e, u] : uv_uses)
                    if (u == 1) border.push_back({uint32_t(e >> 32), uint32_t(e & 0xffffffffu)});
                if (border.size() <= 4000)
                    for (size_t i = 0; i < border.size() && valid; ++i)
                        for (size_t j = i + 1; j < border.size(); ++j) {
                            const auto [a, b] = border[i];
                            const auto [d, e] = border[j];
                            if (a == d || a == e || b == d || b == e) continue;
                            if (segments_cross(c.uv[a], c.uv[b], c.uv[d], c.uv[e])) { valid = false; break; }
                        }
            }
            if (valid && area2 > 0.0) {
                // One density everywhere, times the weight of the regions the
                // chart covers (by area): the packer keeps relative sizes.
                double weight = 1.0, wsum = 0.0, asum = 0.0;
                if (!opts.region_texel_weight.empty() && have_regions) {
                    for (uint32_t t : c.tris) {
                        const uint16_t r = mesh.tri_region[t];
                        const float w = r < opts.region_texel_weight.size() ? opts.region_texel_weight[r] : 1.0f;
                        const float a = mesh.triangle_area(t);
                        wsum += w * a;
                        asum += a;
                    }
                    if (asum > 0.0) weight = wsum / asum;
                }
                const float s = float(std::sqrt(area3 / area2 * weight));
                for (Vec2& p : c.uv) p = p * s;
            }
        }
        return valid;
    };

    std::vector<Chart> done;
    int guard = 0;
    int next_marker = chart_count;   // a fresh id per chart handled, never reused
    while (!pending.empty()) {
        if (++guard > int(tcount) * 4) { res.error = "chart splitting did not converge"; return res; }
        Chart c = std::move(pending.front());
        pending.pop_front();
        const int cid = next_marker++;
        if (build(c, cid)) { done.push_back(std::move(c)); continue; }
        if (c.tris.size() <= 1) { res.error = "a single triangle would not flatten"; return res; }

        // Split in two: grow from the two triangles furthest apart.
        ++res.splits;
        uint32_t sa = c.tris[0], sb = c.tris[0];
        {
            float best = -1.0f;
            for (uint32_t t : c.tris) {
                const float d = length2(mesh.triangle_centroid(t) - mesh.triangle_centroid(c.tris[0]));
                if (d > best) { best = d; sa = t; }
            }
            best = -1.0f;
            for (uint32_t t : c.tris) {
                const float d = length2(mesh.triangle_centroid(t) - mesh.triangle_centroid(sa));
                if (d > best) { best = d; sb = t; }
            }
        }
        // Where the cut goes: a minimum cut on the chart's dual graph, an edge
        // costing its length times how visible it is, between the 30% of the
        // chart nearest each seed. Growing the halves from the two seeds put
        // the new seam wherever the fronts met - across the middle of a chest
        // as often as not; this puts it in the least visible band between them.
        std::unordered_map<uint32_t, int> side;
        {
            const size_t nt = c.tris.size();
            std::unordered_map<uint32_t, uint32_t> local;
            for (uint32_t k = 0; k < nt; ++k) local[c.tris[k]] = k;
            struct Arc { uint32_t to; float cap; uint32_t rev; };
            std::vector<std::vector<Arc>> g(nt + 2);
            const uint32_t S = uint32_t(nt), T = uint32_t(nt + 1);
            auto add = [&](uint32_t u, uint32_t v, float cu, float cv) {
                g[u].push_back({v, cu, uint32_t(g[v].size())});
                g[v].push_back({u, cv, uint32_t(g[u].size() - 1)});
            };
            // Geodesic-ish distance from each seed over triangle centroids.
            auto grow = [&](uint32_t seed) {
                std::vector<float> d(nt, std::numeric_limits<float>::max());
                using E = std::pair<float, uint32_t>;
                std::priority_queue<E, std::vector<E>, std::greater<E>> q;
                d[local[seed]] = 0.0f;
                q.push({0.0f, local[seed]});
                while (!q.empty()) {
                    const auto [dv, k] = q.top();
                    q.pop();
                    if (dv > d[k]) continue;
                    const uint32_t t = c.tris[k];
                    for (int i = 0; i < 3; ++i) {
                        const uint32_t o = across(topo, t, mesh.indices[t * 3 + i],
                                                  mesh.indices[t * 3 + (i + 1) % 3], chart_of, cid);
                        if (o == kInvalidIndex) continue;
                        const uint32_t ko = local[o];
                        const float nd = dv + length(mesh.triangle_centroid(o) - mesh.triangle_centroid(t));
                        if (nd < d[ko]) { d[ko] = nd; q.push({nd, ko}); }
                    }
                }
                return d;
            };
            const std::vector<float> da = grow(sa), db = grow(sb);
            const float inf = std::numeric_limits<float>::max() / 4.0f;
            bool connected = true;
            for (uint32_t k = 0; k < nt; ++k) {
                if (da[k] >= inf || db[k] >= inf) { connected = false; break; }
                const float r = da[k] / std::max(da[k] + db[k], 1e-12f);
                if (r < 0.3f) add(S, k, inf, 0.0f);
                else if (r > 0.7f) add(k, T, inf, 0.0f);
                const uint32_t t = c.tris[k];
                for (int i = 0; i < 3; ++i) {
                    const uint32_t va = mesh.indices[t * 3 + i], vb = mesh.indices[t * 3 + (i + 1) % 3];
                    const uint32_t o = across(topo, t, va, vb, chart_of, cid);
                    if (o == kInvalidIndex || local[o] < k) continue;   // each edge once
                    const float cap = length(mesh.positions[va] - mesh.positions[vb]) *
                                      (0.05f + 0.5f * (vis(va) + vis(vb)));
                    add(k, local[o], cap, cap);
                }
            }
            if (connected) {
                // Edmonds-Karp: charts are a few hundred triangles.
                for (int iter = 0; iter < 10000; ++iter) {
                    std::vector<std::pair<uint32_t, uint32_t>> parent(nt + 2, {kInvalidIndex, 0});
                    std::deque<uint32_t> bfs{S};
                    parent[S] = {S, 0};
                    while (!bfs.empty() && parent[T].first == kInvalidIndex) {
                        const uint32_t u = bfs.front();
                        bfs.pop_front();
                        for (uint32_t ai = 0; ai < g[u].size(); ++ai) {
                            const Arc& arc = g[u][ai];
                            if (arc.cap <= 1e-12f || parent[arc.to].first != kInvalidIndex) continue;
                            parent[arc.to] = {u, ai};
                            bfs.push_back(arc.to);
                        }
                    }
                    if (parent[T].first == kInvalidIndex) break;
                    float push = inf;
                    for (uint32_t v = T; v != S; v = parent[v].first)
                        push = std::min(push, g[parent[v].first][parent[v].second].cap);
                    for (uint32_t v = T; v != S; v = parent[v].first) {
                        Arc& arc = g[parent[v].first][parent[v].second];
                        arc.cap -= push;
                        g[arc.to][arc.rev].cap += push;
                    }
                }
                // The source side of the cut: everything still reachable.
                std::vector<uint8_t> reach(nt + 2, 0);
                std::deque<uint32_t> bfs{S};
                reach[S] = 1;
                while (!bfs.empty()) {
                    const uint32_t u = bfs.front();
                    bfs.pop_front();
                    for (const Arc& arc : g[u])
                        if (arc.cap > 1e-12f && !reach[arc.to]) { reach[arc.to] = 1; bfs.push_back(arc.to); }
                }
                for (uint32_t k = 0; k < nt; ++k) side[c.tris[k]] = reach[k] ? 0 : 1;
            }
        }
        if (side.empty()) {
            std::deque<uint32_t> front{sa, sb};
            side[sa] = 0;
            side[sb] = 1;
            while (!front.empty()) {
                const uint32_t t = front.front();
                front.pop_front();
                for (int i = 0; i < 3; ++i) {
                    const uint32_t o = across(topo, t, mesh.indices[t * 3 + i], mesh.indices[t * 3 + (i + 1) % 3],
                                              chart_of, cid);
                    if (o == kInvalidIndex || side.count(o)) continue;
                    side[o] = side[t];
                    front.push_back(o);
                }
            }
        }
        Chart halves[2];
        for (uint32_t t : c.tris) {
            const auto it = side.find(t);
            halves[it == side.end() ? 0 : it->second].tris.push_back(t);
        }
        if (halves[0].tris.empty() || halves[1].tris.empty()) {
            // Could not grow apart (disconnected pieces): split by triangle order.
            halves[0].tris.assign(c.tris.begin(), c.tris.begin() + c.tris.size() / 2);
            halves[1].tris.assign(c.tris.begin() + c.tris.size() / 2, c.tris.end());
        }
        pending.push_back(std::move(halves[0]));
        pending.push_back(std::move(halves[1]));
    }

    // --- 3b. neighbours join while the union still flattens -----------------
    // Regions are where the cutting starts, not a rule: on a sphere or a torus
    // twenty eight regions made twenty eight charts where a handful would do,
    // and every chart border is vertices. Largest shared border first.
    for (int round = 0; round < 4; ++round) {
        std::vector<int> owner(tcount, -1);
        for (size_t ci = 0; ci < done.size(); ++ci)
            for (uint32_t t : done[ci].tris) owner[t] = int(ci);
        std::unordered_map<uint64_t, float> shared;
        for (uint32_t t = 0; t < tcount; ++t)
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = mesh.indices[t * 3 + i], b = mesh.indices[t * 3 + (i + 1) % 3];
                const auto it = topo.edge_tris.find(edge_key(a, b));
                if (it == topo.edge_tris.end() || it->second.size() != 2) continue;
                const uint32_t o = it->second[0] == t ? it->second[1] : it->second[0];
                const int ca = owner[t], cb = owner[o];
                if (ca < 0 || cb < 0 || ca >= cb) continue;
                // Weighted by how much of the seam shows: a border across the
                // back is worth joining before one along the belly.
                const float len = length(mesh.positions[a] - mesh.positions[b]);
                shared[(uint64_t(ca) << 32) | uint32_t(cb)] += len * (0.25f + 0.5f * (vis(a) + vis(b)));
            }
        std::vector<std::pair<float, uint64_t>> pairs;
        for (const auto& [k, len] : shared) pairs.push_back({len, k});
        std::sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) {
            return x.first != y.first ? x.first > y.first : x.second < y.second;
        });
        std::vector<uint8_t> dead(done.size(), 0), touched(done.size(), 0);
        int merged = 0;
        for (const auto& [len, k] : pairs) {
            const int ca = int(k >> 32), cb = int(k & 0xffffffffu);
            if (dead[ca] || dead[cb] || touched[ca] || touched[cb]) continue;
            if (done[ca].tris.size() + done[cb].tris.size() > 800) continue;
            Chart u;
            u.tris = done[ca].tris;
            u.tris.insert(u.tris.end(), done[cb].tris.begin(), done[cb].tris.end());
            if (!build(u, next_marker++)) continue;
            done[ca] = std::move(u);
            dead[cb] = 1;
            touched[ca] = 1;
            ++merged;
        }
        if (merged == 0) break;
        res.merges += merged;
        std::vector<Chart> kept;
        for (size_t ci = 0; ci < done.size(); ++ci)
            if (!dead[ci]) kept.push_back(std::move(done[ci]));
        done = std::move(kept);
    }

    // --- 4. rebuild the mesh with a vertex per chart corner -------------------
    Mesh out;
    out.name             = mesh.name;
    out.armature         = mesh.armature;
    out.import_transform = mesh.import_transform;
    out.import_scale     = mesh.import_scale;
    out.materials        = mesh.materials;
    out.colors_prelit    = mesh.colors_prelit;
    out.tri_region       = mesh.tri_region;
    out.tri_page         = mesh.tri_page;
    out.tri_material     = mesh.tri_material;
    out.indices.assign(mesh.indices.size(), 0);
    res.tri_chart.assign(tcount, 0);
    // Charts are laid side by side so no two overlap before packing; the
    // packer moves them anyway.
    float offset = 0.0f;
    for (size_t ci = 0; ci < done.size(); ++ci) {
        const Chart& c = done[ci];
        float minx = std::numeric_limits<float>::max(), maxx = -minx, miny = minx;
        for (const Vec2& p : c.uv) { minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); miny = std::min(miny, p.y); }
        const uint32_t base = uint32_t(out.positions.size());
        for (size_t v = 0; v < c.vertex_source.size(); ++v) {
            const uint32_t s = c.vertex_source[v];
            out.positions.push_back(mesh.positions[s]);
            out.uvs.push_back({c.uv[v].x - minx + offset, c.uv[v].y - miny});
            if (mesh.has_normals()) out.normals.push_back(mesh.normals[s]);
            if (mesh.has_colors())  out.colors.push_back(mesh.colors[s]);
            if (mesh.has_skin())    out.skin.push_back(mesh.skin[s]);
        }
        offset += (maxx - minx) * 1.05f + 1e-6f;
        for (size_t k = 0; k < c.tris.size(); ++k) {
            const uint32_t t = c.tris[k];
            for (int i = 0; i < 3; ++i) out.indices[t * 3 + i] = base + c.corner_vertex[k * 3 + i];
            res.tri_chart[t] = uint32_t(ci);
        }
    }
    res.charts = int(done.size());
    // Seam length that shows: every chart border that is not an open edge of
    // the mesh, by length and visibility.
    {
        std::vector<int> owner(tcount, -1);
        for (size_t ci = 0; ci < done.size(); ++ci)
            for (uint32_t t : done[ci].tris) owner[t] = int(ci);
        for (const auto& [k, tris] : topo.edge_tris) {
            if (tris.size() != 2 || owner[tris[0]] == owner[tris[1]]) continue;
            const uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xffffffffu);
            res.visible_seam += length(mesh.positions[a] - mesh.positions[b]) * 0.5f * (vis(a) + vis(b));
            res.seam += length(mesh.positions[a] - mesh.positions[b]);
        }
    }
    res.ok     = true;
    mesh       = std::move(out);
    return res;
}

} // namespace rd
