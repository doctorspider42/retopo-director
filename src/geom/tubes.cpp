#include "geom/tubes.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <limits>
#include <unordered_set>

namespace rd {
namespace {

inline uint64_t edge_key(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}


// A tube as measured on the source, by geodesic distance from its tip.
struct TubeShape {
    float              cut = 0.0f;         // distance from the tip where the tube meets the body
    float              bin_width = 0.0f;
    float              mean_radius = 0.0f;
    std::vector<Vec3>  centre;             // per band of distance from the tip
    std::vector<float> radius;
    Vec3               tip;
    std::vector<float> vertex_distance;    // per source vertex, -1 where not reached
};

struct SourceGraph {
    std::vector<uint32_t> begin;           // CSR over vertices
    std::vector<std::pair<uint32_t, float>> next;
    std::unordered_map<uint64_t, int> edge_uses;

    explicit SourceGraph(const Mesh& m)
    {
        edge_uses.reserve(m.indices.size());
        for (size_t t = 0; t < m.triangle_count(); ++t)
            for (int i = 0; i < 3; ++i)
                ++edge_uses[edge_key(m.indices[t * 3 + i], m.indices[t * 3 + (i + 1) % 3])];
        begin.assign(m.vertex_count() + 1, 0);
        for (const auto& [k, n] : edge_uses) {
            ++begin[uint32_t(k >> 32) + 1];
            ++begin[uint32_t(k & 0xffffffffu) + 1];
        }
        for (size_t v = 1; v < begin.size(); ++v) begin[v] += begin[v - 1];
        next.resize(begin.back());
        std::vector<uint32_t> fill(begin.begin(), begin.end() - 1);
        for (const auto& [k, n] : edge_uses) {
            const uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xffffffffu);
            const float len = length(m.positions[a] - m.positions[b]);
            next[fill[a]++] = {b, len};
            next[fill[b]++] = {a, len};
        }
    }
};

// Finds a thin tube ending in region `id`. The region must be a cap - one
// border loop, no open edges of its own - so that the point furthest from its
// border is a tip. From there the geodesic distance runs up the tube whatever
// regions it crosses, its level sets are rings for as long as the tube lasts,
// and the first band that is not a thin ring is where it meets the body.
bool measure_tube(const Mesh& source, const Segmentation& seg, const SourceGraph& graph,
                  uint16_t id, float low_edge, float max_reach, const TubeOptions& opts,
                  TubeShape& out)
{
    const size_t tcount = source.triangle_count();
    std::vector<uint32_t> tris;
    for (size_t t = 0; t < tcount; ++t)
        if (seg.tri_region[t] == id) tris.push_back(uint32_t(t));
    if (tris.size() < 16) return false;

    std::unordered_map<uint64_t, int> uses;
    for (uint32_t t : tris)
        for (int i = 0; i < 3; ++i)
            ++uses[edge_key(source.indices[t * 3 + i], source.indices[t * 3 + (i + 1) % 3])];
    std::unordered_map<uint32_t, std::vector<uint32_t>> border_adj;
    std::unordered_set<uint32_t> inside;
    for (const auto& [k, n] : uses) {
        const uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xffffffffu);
        inside.insert(a);
        inside.insert(b);
        if (n != 1) continue;
        const auto it = graph.edge_uses.find(k);
        if (it != graph.edge_uses.end() && it->second == 1) return false;   // an open edge
        border_adj[a].push_back(b);
        border_adj[b].push_back(a);
    }
    if (border_adj.empty()) return false;
    {
        std::unordered_set<uint32_t> seen{border_adj.begin()->first};
        std::vector<uint32_t> stack{border_adj.begin()->first};
        while (!stack.empty()) {
            const uint32_t v = stack.back();
            stack.pop_back();
            for (uint32_t w : border_adj[v])
                if (seen.insert(w).second) stack.push_back(w);
        }
        if (seen.size() != border_adj.size()) return false;   // more than one loop
    }

    using Entry = std::pair<float, uint32_t>;
    auto dijkstra = [&](const std::vector<uint32_t>& from, float reach, bool region_only,
                        std::vector<float>& d) {
        d.assign(source.vertex_count(), -1.0f);
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
        for (uint32_t v : from) { d[v] = 0.0f; queue.push({0.0f, v}); }
        while (!queue.empty()) {
            const auto [dv, v] = queue.top();
            queue.pop();
            if (dv > d[v] || dv > reach) continue;
            for (uint32_t e = graph.begin[v]; e < graph.begin[v + 1]; ++e) {
                const auto [w, len] = graph.next[e];
                if (region_only && !inside.count(w)) continue;
                if (d[w] < 0.0f || dv + len < d[w]) { d[w] = dv + len; queue.push({d[w], w}); }
            }
        }
    };

    // The tip: the point of the region furthest from its border.
    std::vector<uint32_t> border;
    for (const auto& [v, _] : border_adj) border.push_back(v);
    std::vector<float> from_border;
    dijkstra(border, std::numeric_limits<float>::max(), true, from_border);
    uint32_t tip = border.front();
    float cap_length = 0.0f;
    for (uint32_t v : inside)
        if (from_border[v] > cap_length) { cap_length = from_border[v]; tip = v; }
    if (cap_length <= 0.0f) return false;

    // Bands of distance from the tip, over the whole mesh.
    const float w = cap_length / 16.0f;
    const float reach = std::min(max_reach, 256.0f * w);
    dijkstra({tip}, reach, false, out.vertex_distance);
    const std::vector<float>& d = out.vertex_distance;
    const int bands = std::max(2, int(std::ceil(reach / w)));

    // Points sampled over the triangles, not the vertices: a scan's tail is
    // long thin triangles, and most bands along it hold no vertex at all.
    struct Sample { Vec3 p; float d; };
    std::vector<std::vector<Sample>> band(bands);
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t i0 = source.indices[t * 3], i1 = source.indices[t * 3 + 1],
                       i2 = source.indices[t * 3 + 2];
        if (d[i0] < 0.0f || d[i1] < 0.0f || d[i2] < 0.0f) continue;
        if (std::min({d[i0], d[i1], d[i2]}) >= reach) continue;
        const Vec3 a = source.positions[i0], b = source.positions[i1], c = source.positions[i2];
        const float longest = std::max({length(b - a), length(c - b), length(a - c)});
        const int n = std::clamp(int(std::ceil(longest / w * 3.0f)), 1, 24);
        for (int u = 0; u <= n; ++u)
            for (int v = 0; u + v <= n; ++v) {
                const float fu = float(u) / n, fv = float(v) / n, fw = 1.0f - fu - fv;
                const float sd = d[i0] * fw + d[i1] * fu + d[i2] * fv;
                const int k = int(sd / w);
                if (k < bands) band[k].push_back({a * fw + b * fu + c * fv, sd});
            }
    }
    out.bin_width = w;
    out.centre.assign(bands, Vec3{});
    out.radius.assign(bands, 0.0f);
    for (int k = 0; k < bands; ++k) {
        for (const Sample& s : band[k]) out.centre[k] += s.p;
        if (!band[k].empty()) out.centre[k] = out.centre[k] / float(band[k].size());
        else if (k > 0)       out.centre[k] = out.centre[k - 1];
    }

    // A band is a tube's ring when its points go all the way round its centre
    // at a steady distance, and the tube is thin when the low poly cannot fit
    // `max_edges_around` of its edges round that ring. Walk out from the tip
    // until a band is not one.
    int end = 1;
    double radius_sum = 0.0;
    for (; end < bands; ++end) {
        const int k = end;
        if (band[k].size() < 12) break;
        const Vec3 t = normalize(out.centre[std::min(bands - 1, k + 1)] - out.centre[k - 1]);
        if (length2(t) < 0.5f) break;
        Vec3 u = std::fabs(t.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        u = normalize(u - t * dot(u, t));
        const Vec3 v = cross(t, u);
        std::vector<float> angle;
        double sum = 0.0, sum2 = 0.0;
        for (const Sample& s : band[k]) {
            Vec3 r = s.p - out.centre[k];
            r = r - t * dot(r, t);
            const float rl = length(r);
            sum += rl;
            sum2 += double(rl) * rl;
            angle.push_back(std::atan2(dot(r, v), dot(r, u)));
        }
        const double n = double(band[k].size());
        const double mean = sum / n;
        const double sd = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
        std::sort(angle.begin(), angle.end());
        float gap = angle.front() + 2.0f * kPi - angle.back();
        for (size_t i = 1; i < angle.size(); ++i) gap = std::max(gap, angle[i] - angle[i - 1]);
        // A flat flap - an ear - is a line through its centre: the distance
        // swings from nothing to its half width, and fails the steadiness.
        const bool round = gap < 100.0f * kDeg2Rad && sd < 0.4 * mean;
        const bool thin  = 2.0f * kPi * float(mean) < opts.max_edges_around * low_edge;
        if (!round || !thin) break;
        out.radius[k] = float(mean);
        radius_sum += mean;
    }
    const int good = end - 1;
    if (good < 4) return false;
    out.radius[0]   = out.radius[1] * 0.5f;
    out.mean_radius = float(radius_sum / good);
    out.cut         = (float(end) - 0.5f) * w;
    out.centre.resize(end);
    out.radius.resize(end);
    if (out.cut < opts.min_length_diameters * 2.0f * out.mean_radius) return false;
    out.tip = source.positions[tip];
    return true;
}

// Linear interpolation over the band centres, band k sitting at (k + 0.5) * width.
template <typename T>
T along(const std::vector<T>& v, float x, float width)
{
    const int   n = int(v.size());
    const float f = clampf(x / width - 0.5f, 0.0f, float(n - 1));
    const int   k = std::min(n - 2, int(f));
    const float t = f - float(k);
    return v[k] * (1.0f - t) + v[k + 1] * t;
}

} // namespace

void measure_region_tubes(const Mesh& source, const MeshAnalysis& analysis, Segmentation& seg,
                          const TubeOptions& opts)
{
    if (seg.tri_region.size() != source.triangle_count()) return;
    const SourceGraph graph(source);
    for (Region& r : seg.regions) {
        r.tube_aspect = 0.0f;
        TubeShape tube;
        // No low poly yet, so nothing is too thin for it: the question here is
        // only whether the shape is a tube.
        if (!measure_tube(source, seg, graph, r.id, 1e30f,
                          analysis.bbox_diagonal, opts, tube))
            continue;
        const float aspect = tube.cut / std::max(2.0f * tube.mean_radius, 1e-9f);
        // Six diameters: a finger is about four, a rat's tail thirty.
        if (aspect >= 6.0f) r.tube_aspect = aspect;
    }
}

TubeReport sweep_thin_tubes(Mesh& low, const Mesh& source, const MeshAnalysis& analysis,
                            const Segmentation& seg, bool symmetric, const TubeOptions& opts)
{
    TubeReport rep;
    if (!opts.enabled || low.empty() || seg.tri_region.size() != source.triangle_count() ||
        low.tri_region.size() != low.triangle_count() || analysis.bvh.empty())
        return rep;

    const SourceGraph graph(source);
    const float max_reach = analysis.bbox_diagonal;

    const SymmetryPlane& plane = analysis.symmetry;
    const bool use_plane = symmetric && plane.accepted;

    for (const Region& region : seg.regions) {
        const uint16_t id = region.id;

        // The edge the low poly spends, which is what decides whether a tube
        // is too thin for it. Over the whole mesh: the triangles a starved
        // tail was left with are no measure of anything.
        double edge_sum = 0.0;
        size_t edge_n = 0;
        for (size_t t = 0; t < low.triangle_count(); ++t) {
            for (int i = 0; i < 3; ++i) {
                edge_sum += length(low.positions[low.indices[t * 3 + i]] -
                                   low.positions[low.indices[t * 3 + (i + 1) % 3]]);
                ++edge_n;
            }
        }
        if (edge_n == 0) continue;
        const float low_edge = float(edge_sum / double(edge_n));

        TubeShape tube;
        if (!measure_tube(source, seg, graph, id, low_edge, max_reach, opts, tube)) continue;

        // --- cut the thin end off the low poly ------------------------------
        const size_t tcount = low.triangle_count();
        std::vector<uint8_t> removed(tcount, 0);
        size_t removed_n = 0;
        for (size_t t = 0; t < tcount; ++t) {
            const ClosestHit hit = analysis.bvh.closest_point(low.triangle_centroid(t));
            if (!hit.hit()) continue;
            float dsum = 0.0f;
            bool  reached = true;
            for (int i = 0; i < 3; ++i) {
                const float dv = tube.vertex_distance[source.indices[hit.triangle * 3 + i]];
                reached = reached && dv >= 0.0f;
                dsum += dv;
            }
            if (reached && dsum / 3.0f < tube.cut) { removed[t] = 1; ++removed_n; }
        }
        if (removed_n < 6) continue;

        // The border the cut leaves must be one simple loop, walked the way
        // the remaining surface runs along it.
        std::unordered_map<uint64_t, int> before, after;
        for (size_t t = 0; t < tcount; ++t)
            for (int i = 0; i < 3; ++i) {
                const uint64_t k = edge_key(low.indices[t * 3 + i], low.indices[t * 3 + (i + 1) % 3]);
                ++before[k];
                if (!removed[t]) ++after[k];
            }
        std::unordered_map<uint32_t, uint32_t> next;
        bool simple = true;
        size_t open = 0;
        for (size_t t = 0; t < tcount && simple; ++t) {
            if (removed[t]) continue;
            for (int i = 0; i < 3; ++i) {
                const uint32_t a = low.indices[t * 3 + i], b = low.indices[t * 3 + (i + 1) % 3];
                const uint64_t k = edge_key(a, b);
                if (after[k] != 1 || before[k] != 2) continue;
                if (!next.emplace(a, b).second) { simple = false; break; }
                ++open;
            }
        }
        if (!simple || open < 3) continue;
        std::vector<uint32_t> loop{next.begin()->first};
        while (loop.size() <= open) {
            const auto it = next.find(loop.back());
            if (it == next.end()) { simple = false; break; }
            if (it->second == loop.front()) break;
            loop.push_back(it->second);
        }
        if (!simple || loop.size() != open) continue;
        const int k_loop = int(loop.size());
        // A tube the remesher left fewer triangles than the smallest prism
        // costs was not kept badly but given up on, on purpose or by budget:
        // a finger inside a mitten hand is one, and sweeping it out alone
        // leaves a mitten with one finger. What this repairs is a tube that
        // was paid for and came out as a ribbon.
        if (int(removed_n) < k_loop + 6 * opts.min_rings) continue;

        // --- plan the prism: sides, rings -----------------------------------
        // Angles round the loop are taken about the tube's own axis at the cut.
        // The loop is three to six of the remesher's vertices, some up on the
        // flare, and their centroid can sit off the axis far enough that the
        // loop no longer goes round it once - which failed the bridge on two
        // budget attempts in five, and the attempt that won had no tube.
        const Vec3 c0 = along(tube.centre, tube.cut, tube.bin_width);
        const float span = tube.cut;
        const int   budget = int(removed_n);
        // At least `min_rings`: fewer is not a taper but a cone, and a tail the
        // remesher had nearly given up on (ten triangles, once) is worth the
        // few it costs; the budget re-fit takes them back from everywhere else.
        int sides = 3, rings = opts.min_rings;
        for (int s = 6; s >= 3; --s) {
            const int spare = budget - k_loop - 2 * s;
            if (spare < 0) continue;
            const int m = spare / (2 * s) + 1;
            if (m < opts.min_rings) continue;
            // Rings no closer together than about a side's length, or the
            // triangles go to slivers and the sides were better spent as rings.
            const float side = 2.0f * tube.mean_radius * std::sin(kPi / float(s));
            if (span / float(m + 1) >= 0.75f * side || s == 3) { sides = s; rings = m; break; }
        }

        // Rings sit along the centreline; on the mirror plane when the tube
        // does, so the cut the mirror makes through it is clean.
        bool on_plane = use_plane;
        if (on_plane)
            for (const Vec3& c : tube.centre)
                if (std::fabs(dot(plane.normal, c) - plane.offset) >
                    std::max(0.5f * tube.mean_radius, 1e-6f)) {
                    on_plane = false;
                    break;
                }
        auto onto_plane = [&](Vec3 p) {
            return on_plane ? p - plane.normal * (dot(plane.normal, p) - plane.offset) : p;
        };

        std::vector<Vec3> ring_centre(rings), ring_t(rings), ring_u(rings);
        std::vector<float> ring_r(rings);
        const float around = kPi / (float(sides) * std::sin(kPi / float(sides)));   // same perimeter as the circle
        for (int i = 0; i < rings; ++i) {
            // Distance from the tip, ring 0 nearest the body.
            const float x = span * (1.0f - float(i + 1) / float(rings + 1));
            ring_centre[i] = onto_plane(along(tube.centre, x, tube.bin_width));
            ring_r[i]      = along(tube.radius, x, tube.bin_width) * around;
        }
        const Vec3 tip = onto_plane(tube.tip);
        for (int i = 0; i < rings; ++i) {
            const Vec3 prev = i == 0 ? c0 : ring_centre[i - 1];
            const Vec3 nxt  = i + 1 < rings ? ring_centre[i + 1] : tip;
            ring_t[i] = normalize(nxt - prev);
        }
        const Vec3 t0 = normalize(ring_centre[0] - c0);
        if (length2(t0) < 0.5f) continue;

        // Which way the loop turns round the axis; the rings turn the same way.
        float turn = 0.0f;
        for (int i = 0; i < k_loop; ++i)
            turn += dot(cross(low.positions[loop[i]] - c0, low.positions[loop[(i + 1) % k_loop]] - c0), t0);
        if (turn == 0.0f) continue;
        const float sense = turn > 0.0f ? 1.0f : -1.0f;

        for (int i = 0; i < rings; ++i) {
            const Vec3 t = ring_t[i];
            Vec3 u = on_plane ? cross(t, plane.normal) : Vec3{};
            if (length2(u) < 1e-8f) {
                // Parallel transport from the ring before, or any perpendicular.
                u = i > 0 ? ring_u[i - 1] : (std::fabs(t.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
            }
            ring_u[i] = normalize(u - t * dot(u, t));
        }

        // Where each loop vertex sits round the axis, from the first ring's
        // first vertex, for the bridge below.
        Vec3 u0 = ring_u[0] - t0 * dot(ring_u[0], t0);
        u0 = normalize(u0);
        const Vec3 w0 = cross(t0, u0);
        auto angle_of = [&](Vec3 p) {
            const Vec3 r = p - c0;
            float a = sense * std::atan2(dot(r, w0), dot(r, u0));
            if (a < 0.0f) a += 2.0f * kPi;
            return a;
        };
        int start = 0;
        float best = 1e9f;
        for (int i = 0; i < k_loop; ++i) {
            const float a = angle_of(low.positions[loop[i]]);
            const float dist = std::min(a, 2.0f * kPi - a);
            if (dist < best) { best = dist; start = i; }
        }
        std::vector<float> A(k_loop + 1);
        float a0 = angle_of(low.positions[loop[start]]);
        if (a0 > kPi) a0 -= 2.0f * kPi;
        A[0] = a0;
        for (int m = 1; m <= k_loop; ++m) {
            const float prev = angle_of(low.positions[loop[(start + m - 1) % k_loop]]);
            const float cur  = angle_of(low.positions[loop[(start + m) % k_loop]]);
            float step = cur - prev;
            if (step < 0.0f) step += 2.0f * kPi;
            A[m] = A[m - 1] + step;
        }
        // A loop that does not go once round the axis is not a ring we
        // can bridge; the tube stays as the remesher left it.
        if (std::fabs(A[k_loop] - A[0] - 2.0f * kPi) > 0.5f) continue;

        // --- emit --------------------------------------------------------------
        std::vector<uint32_t> indices;
        std::vector<uint16_t> regions;
        indices.reserve(low.indices.size());
        for (size_t t = 0; t < tcount; ++t) {
            if (removed[t]) continue;
            for (int i = 0; i < 3; ++i) indices.push_back(low.indices[t * 3 + i]);
            regions.push_back(low.tri_region[t]);
        }
        auto add_vertex = [&](Vec3 p) {
            low.positions.push_back(p);
            return uint32_t(low.positions.size() - 1);
        };
        auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            regions.push_back(id);
        };

        std::vector<std::vector<uint32_t>> ring(rings, std::vector<uint32_t>(sides));
        for (int i = 0; i < rings; ++i) {
            const Vec3 w = cross(ring_t[i], ring_u[i]);
            for (int j = 0; j < sides; ++j) {
                const float a = sense * 2.0f * kPi * float(j) / float(sides);
                ring[i][j] = add_vertex(ring_centre[i] +
                                        (ring_u[i] * std::cos(a) + w * std::sin(a)) * ring_r[i]);
            }
        }
        const uint32_t tip_v = add_vertex(tip);

        // Bridge the cut to the first ring, walking both by angle round the
        // axis so each loop vertex meets the ring vertex facing it.
        {
            int i = 0, j = 0;
            while (i < k_loop || j < sides) {
                const float next_ring = 2.0f * kPi * float(j + 1) / float(sides);
                const bool advance_loop = j == sides || (i < k_loop && A[i + 1] <= next_ring);
                if (advance_loop) {
                    emit(loop[(start + i + 1) % k_loop], loop[(start + i) % k_loop], ring[0][j % sides]);
                    ++i;
                } else {
                    emit(loop[(start + i) % k_loop], ring[0][j % sides], ring[0][(j + 1) % sides]);
                    ++j;
                }
            }
        }
        for (int r = 0; r + 1 < rings; ++r)
            for (int j = 0; j < sides; ++j) {
                const int jn = (j + 1) % sides;
                emit(ring[r][jn], ring[r][j], ring[r + 1][j]);
                emit(ring[r][jn], ring[r + 1][j], ring[r + 1][jn]);
            }
        for (int j = 0; j < sides; ++j)
            emit(ring[rings - 1][(j + 1) % sides], ring[rings - 1][j], tip_v);

        const size_t added = regions.size() - (tcount - removed_n);
        low.indices.swap(indices);
        low.tri_region.swap(regions);
        if (!low.normals.empty())  low.normals.resize(low.positions.size(), Vec3{0, 1, 0});
        if (!low.uvs.empty())      low.uvs.resize(low.positions.size(), Vec2{});
        if (!low.colors.empty())   low.colors.resize(low.positions.size(), Vec4{1, 1, 1, 1});
        if (!low.skin.empty())     low.skin.resize(low.positions.size());
        if (!low.tri_page.empty())     low.tri_page.clear();
        if (!low.tri_material.empty()) low.tri_material.resize(low.triangle_count(), 0);

        ++rep.swept;
        const std::string name = region.name.empty() ? format("region %u", unsigned(id)) : region.name;
        rep.notes.push_back(format("swept the thin end of '%s' as a %d sided tube with %d rings: "
                                   "%zu triangles where the remesher had %zu",
                                   name.c_str(), sides, rings, added, removed_n));
        RD_INFO("tubes: '%s' %.4f long, radius %.4f against a %.4f edge, "
                "%d sides x %d rings, %zu tri (was %zu), loop of %d",
                name.c_str(), tube.cut, tube.mean_radius, low_edge, sides, rings, added,
                removed_n, k_loop);
    }

    if (rep.swept) {
        low.compact();
        if (!low.normals.empty()) low.compute_normals(60.0f);
    }
    return rep;
}

} // namespace rd
