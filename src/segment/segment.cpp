#include "segment/segment.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace rd {
namespace {

struct QueueEntry {
    float    cost;
    uint32_t triangle;
    bool operator>(const QueueEntry& o) const { return cost > o.cost; }
};

using MinQueue = std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>>;

// Evenly spread hues, with alternating lightness so neighbouring ids stay
// distinguishable in the overlay render.
Vec3 palette_color(uint32_t index)
{
    const float golden = 0.61803398875f;
    const float h = std::fmod(index * golden, 1.0f);
    const float s = 0.55f + 0.25f * ((index % 3) / 2.0f);
    const float v = 0.70f + 0.25f * ((index % 2) ? 1.0f : 0.0f);

    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
    case 0:  return {v, t, p};
    case 1:  return {q, v, p};
    case 2:  return {p, v, t};
    case 3:  return {p, q, v};
    case 4:  return {t, p, v};
    default: return {v, p, q};
    }
}

int triangle_dominant_joint(const Mesh& mesh, const MeshAnalysis& analysis, size_t tri)
{
    if (mesh.armature.empty()) return -1;
    if (mesh.has_skin()) {
        float best_w = 0.0f;
        int   best_j = -1;
        for (int c = 0; c < 3; ++c) {
            const SkinVertex& sv = mesh.skin[mesh.indices[tri * 3 + c]];
            for (int k = 0; k < 4; ++k)
                if (sv.weights[k] > best_w) { best_w = sv.weights[k]; best_j = sv.joints[k]; }
        }
        return best_j;
    }
    // No skin weights: fall back to the nearest pivot.
    std::unordered_map<int, int> votes;
    for (int c = 0; c < 3; ++c) {
        const uint32_t v = mesh.indices[tri * 3 + c];
        if (v < analysis.nearest_joint.size()) ++votes[analysis.nearest_joint[v]];
    }
    int best = -1, best_count = 0;
    for (const auto& [j, n] : votes)
        if (n > best_count) { best_count = n; best = j; }
    return best;
}

// Edge weight of the dual graph, shared by the geometric split and by the fill
// that an alternative segmenter runs over the triangles it could not label.
// Holds the per triangle attributes the weight needs, so building it once and
// passing it around costs one pass over the mesh instead of one per query.
struct DualCost {
    DualCost(const Mesh& mesh, const MeshAnalysis& analysis, const SegmentationOptions& o)
        : topo(analysis.topology), analysis_(analysis), opts(o)
    {
        const size_t tcount = mesh.triangle_count();
        centroid.resize(tcount);
        joint.assign(tcount, -1);
        for (size_t t = 0; t < tcount; ++t) {
            centroid[t] = mesh.triangle_centroid(t);
            if (opts.use_armature) joint[t] = triangle_dominant_joint(mesh, analysis, t);
        }
        scale = std::max(analysis.bbox_diagonal, kEps);
    }

    float operator()(uint32_t a, uint32_t b, uint32_t half_edge) const
    {
        float w = length(centroid[b] - centroid[a]) / scale;
        w = std::max(w, 1e-5f);

        const Vec3  na   = analysis_.tri_normal[a];
        const Vec3  nb   = analysis_.tri_normal[b];
        const float bend = 1.0f - saturate(dot(na, nb));
        w *= 1.0f + opts.normal_weight * bend;

        const uint32_t edge = topo.half_edge_to_edge[half_edge];
        if (edge < analysis_.edge_sharp.size() && analysis_.edge_sharp[edge])
            w *= 1.0f + opts.sharp_penalty;

        if (opts.use_armature && joint[a] != joint[b] && joint[a] >= 0 && joint[b] >= 0)
            w *= 1.0f + opts.joint_penalty;

        return w;
    }

    const MeshTopology&        topo;
    const MeshAnalysis&        analysis_;
    const SegmentationOptions& opts;
    std::vector<Vec3>          centroid;
    std::vector<int>           joint;
    float                      scale = 1.0f;
};

} // namespace

// ---------------------------------------------------------------------------
void Segmentation::clear()
{
    tri_region.clear();
    regions.clear();
    seconds = 0.0;
}

Region* Segmentation::find(uint16_t id)
{
    for (Region& r : regions)
        if (r.id == id) return &r;
    return nullptr;
}

const Region* Segmentation::find(uint16_t id) const
{
    return const_cast<Segmentation*>(this)->find(id);
}

size_t Segmentation::index_of(uint16_t id) const
{
    for (size_t i = 0; i < regions.size(); ++i)
        if (regions[i].id == id) return i;
    return SIZE_MAX;
}

std::vector<uint16_t> Segmentation::ids() const
{
    std::vector<uint16_t> out;
    out.reserve(regions.size());
    for (const Region& r : regions) out.push_back(r.id);
    return out;
}

std::vector<std::string> Segmentation::names() const
{
    std::vector<std::string> out;
    out.reserve(regions.size());
    for (const Region& r : regions) out.push_back(r.name);
    return out;
}

std::vector<float> Segmentation::area_shares() const
{
    std::vector<float> out;
    out.reserve(regions.size());
    for (const Region& r : regions) out.push_back(r.area_share);
    return out;
}

void Segmentation::refresh_statistics(const Mesh& mesh, const MeshAnalysis& analysis)
{
    std::unordered_map<uint16_t, size_t> slot;
    for (size_t i = 0; i < regions.size(); ++i) {
        Region& r = regions[i];
        slot[r.id] = i;
        r.triangle_count = 0;
        r.area           = 0.0f;
        r.area_share     = 0.0f;
        r.centroid       = Vec3{};
        r.bounds         = Aabb{};
        r.mean_curvature = 0.0f;
        r.max_curvature  = 0.0f;
        r.mean_ambient   = 0.0f;
        r.neighbours.clear();
        r.debug_color = palette_color(static_cast<uint32_t>(i));
    }

    const size_t tcount = mesh.triangle_count();
    double total_area = 0.0;
    std::vector<double> curv_accum(regions.size(), 0.0);
    std::vector<double> amb_accum(regions.size(), 0.0);
    std::vector<std::unordered_map<int, float>> joint_votes(regions.size());

    for (size_t t = 0; t < tcount && t < tri_region.size(); ++t) {
        const auto it = slot.find(tri_region[t]);
        if (it == slot.end()) continue;
        Region& r = regions[it->second];

        const float area = t < analysis.tri_area.size() ? analysis.tri_area[t] : mesh.triangle_area(t);
        r.triangle_count++;
        r.area += area;
        total_area += area;

        const Vec3 c = mesh.triangle_centroid(t);
        r.centroid += c * area;
        for (int k = 0; k < 3; ++k) r.bounds.grow(mesh.positions[mesh.indices[t * 3 + k]]);

        const float curv = t < analysis.tri_curvature.size() ? analysis.tri_curvature[t] : 0.0f;
        curv_accum[it->second] += double(curv) * area;
        r.max_curvature = std::max(r.max_curvature, curv);

        float amb = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = mesh.indices[t * 3 + k];
            amb += v < analysis.ambient.size() ? analysis.ambient[v] : 1.0f;
        }
        amb_accum[it->second] += double(amb / 3.0f) * area;

        const int joint = triangle_dominant_joint(mesh, analysis, t);
        if (joint >= 0) joint_votes[it->second][joint] += area;
    }

    for (size_t i = 0; i < regions.size(); ++i) {
        Region& r = regions[i];
        if (r.area > kEps) {
            r.centroid       = r.centroid / r.area;
            r.mean_curvature = static_cast<float>(curv_accum[i] / r.area);
            r.mean_ambient   = static_cast<float>(amb_accum[i] / r.area);
        }
        r.area_share = total_area > 0.0 ? static_cast<float>(r.area / total_area) : 0.0f;

        float best = 0.0f;
        for (const auto& [j, w] : joint_votes[i])
            if (w > best) { best = w; r.dominant_joint = j; }
        r.joint_weight_share = r.area > kEps ? best / r.area : 0.0f;

        if (r.name.empty()) r.name = format("region_%u", unsigned(r.id));
        if (r.auto_label.empty()) {
            if (r.dominant_joint >= 0 &&
                static_cast<size_t>(r.dominant_joint) < mesh.armature.size())
                r.auto_label = "near joint " + mesh.armature.joints[r.dominant_joint].name;
            else
                r.auto_label = r.mean_curvature > 0.35f ? "high detail patch" : "smooth patch";
        }
    }

    // Neighbour graph from shared edges.
    std::vector<std::unordered_set<uint16_t>> adjacency(regions.size());
    for (size_t t = 0; t < tcount && t < tri_region.size(); ++t) {
        const auto it = slot.find(tri_region[t]);
        if (it == slot.end()) continue;
        for (int c = 0; c < 3; ++c) {
            const uint32_t n = analysis.topology.neighbour(static_cast<uint32_t>(t), c);
            if (n == kInvalidIndex || n >= tri_region.size()) continue;
            if (tri_region[n] == tri_region[t]) continue;
            adjacency[it->second].insert(tri_region[n]);
        }
    }
    for (size_t i = 0; i < regions.size(); ++i) {
        regions[i].neighbours.assign(adjacency[i].begin(), adjacency[i].end());
        std::sort(regions[i].neighbours.begin(), regions[i].neighbours.end());
    }
}

// ---------------------------------------------------------------------------
void segment_mesh(const Mesh& mesh, const MeshAnalysis& analysis, Segmentation& out,
                  const SegmentationOptions& opts,
                  const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    out.clear();

    const size_t tcount = mesh.triangle_count();
    if (tcount == 0) return;

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };
    report(0.02f, "preparing dual graph");

    const MeshTopology& topo = analysis.topology;
    if (topo.triangle_count() != tcount) {
        RD_ERROR("segmentation: topology does not match the mesh");
        return;
    }

    // --- per triangle attributes used by the cost function ------------------
    const DualCost           edge_cost(mesh, analysis, opts);
    const std::vector<Vec3>& centroid = edge_cost.centroid;
    const std::vector<int>&  joint    = edge_cost.joint;

    // --- seeds --------------------------------------------------------------
    const int target = std::clamp(opts.target_regions, 2, 512);
    std::vector<uint32_t> seeds;
    seeds.reserve(target);

    if (opts.seed_from_joints && opts.use_armature && !mesh.armature.empty()) {
        // One seed per joint that actually owns surface, nearest triangle to
        // the pivot. Gives limbs their own patches from the start.
        std::unordered_map<int, uint32_t> best_tri;
        std::unordered_map<int, float>    best_dist;
        std::unordered_map<int, double>   owned_area;
        for (size_t t = 0; t < tcount; ++t) {
            const int j = joint[t];
            if (j < 0 || static_cast<size_t>(j) >= mesh.armature.size()) continue;
            owned_area[j] += mesh.triangle_area(t);
            const float d = length(centroid[t] - mesh.armature.joints[j].bind_position);
            const auto it = best_dist.find(j);
            if (it == best_dist.end() || d < it->second) {
                best_dist[j] = d;
                best_tri[j]  = static_cast<uint32_t>(t);
            }
        }
        // When there are more joints than regions, the joints that own the
        // most surface get the seeds. Taking them in index order kept the
        // first 28 of a 65 joint skeleton - spine, arms and every finger bone
        // - and left the legs, which a UE style rig numbers last, without a
        // seed of their own: both legs and both feet grew out of the pelvis
        // as one region of 45% of the body, and no brief could ask for feet.
        std::vector<std::pair<int, uint32_t>> ordered(best_tri.begin(), best_tri.end());
        std::sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
            const double wa = owned_area[a.first], wb = owned_area[b.first];
            if (wa != wb) return wa > wb;
            return a.first < b.first;
        });
        if (static_cast<int>(ordered.size()) > target) ordered.resize(size_t(target));
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [j, t] : ordered) seeds.push_back(t);
    }

    if (seeds.empty()) {
        // Start from the triangle furthest from the area weighted centroid so
        // the first patch lands on an extremity, not in the middle of the torso.
        const Vec3 c = mesh.centroid_area_weighted();
        uint32_t   best = 0;
        float      best_d = -1.0f;
        for (size_t t = 0; t < tcount; ++t) {
            const float d = length2(centroid[t] - c);
            if (d > best_d) { best_d = d; best = static_cast<uint32_t>(t); }
        }
        seeds.push_back(best);
    }

    // --- incremental multi source Dijkstra with farthest point sampling ----
    std::vector<float>    dist(tcount, std::numeric_limits<float>::max());
    std::vector<uint16_t> owner(tcount, kNoRegion);
    MinQueue queue;

    auto add_seed = [&](uint32_t tri, uint16_t region) {
        if (dist[tri] == 0.0f && owner[tri] == region) return;
        dist[tri]  = 0.0f;
        owner[tri] = region;
        queue.push({0.0f, tri});
    };

    auto drain = [&]() {
        while (!queue.empty()) {
            const QueueEntry e = queue.top();
            queue.pop();
            if (e.cost > dist[e.triangle]) continue;
            for (int c = 0; c < 3; ++c) {
                const uint32_t h = e.triangle * 3 + c;
                const uint32_t n = topo.neighbour(e.triangle, c);
                if (n == kInvalidIndex) continue;
                const float nd = e.cost + edge_cost(e.triangle, n, h);
                if (nd < dist[n]) {
                    dist[n]  = nd;
                    owner[n] = owner[e.triangle];
                    queue.push({nd, n});
                }
            }
        }
    };

    for (size_t i = 0; i < seeds.size(); ++i)
        add_seed(seeds[i], static_cast<uint16_t>(i));
    drain();

    while (static_cast<int>(seeds.size()) < target) {
        report(0.05f + 0.55f * float(seeds.size()) / float(target), "growing regions");

        uint32_t far_tri = kInvalidIndex;
        float    far_d   = 0.0f;
        for (size_t t = 0; t < tcount; ++t) {
            if (dist[t] == std::numeric_limits<float>::max()) { far_tri = uint32_t(t); far_d = 1e30f; break; }
            if (dist[t] > far_d) { far_d = dist[t]; far_tri = static_cast<uint32_t>(t); }
        }
        if (far_tri == kInvalidIndex || far_d <= 0.0f) break;

        const uint16_t region = static_cast<uint16_t>(seeds.size());
        seeds.push_back(far_tri);
        add_seed(far_tri, region);
        drain();
    }

    // Triangles unreachable from any seed (separate shells with no seed of
    // their own) get flood filled into fresh regions.
    for (size_t t = 0; t < tcount; ++t) {
        if (owner[t] != kNoRegion) continue;
        const uint16_t region = static_cast<uint16_t>(seeds.size());
        seeds.push_back(static_cast<uint32_t>(t));
        add_seed(static_cast<uint32_t>(t), region);
        drain();
    }

    // --- build the region list ---------------------------------------------
    report(0.65f, "collecting regions");
    out.tri_region.assign(owner.begin(), owner.end());
    report(0.85f, "merging slivers");
    finalize_segmentation(mesh, analysis, opts, out);

    out.seconds = watch.seconds();
    report(1.0f, "done");
    RD_INFO("segmentation: %zu regions over %zu triangles in %s",
            out.regions.size(), tcount, format_duration(out.seconds).c_str());
}

// ---------------------------------------------------------------------------
void grow_unassigned_regions(const Mesh& mesh, const MeshAnalysis& analysis,
                             const SegmentationOptions& opts,
                             std::vector<uint16_t>& tri_region)
{
    const size_t tcount = mesh.triangle_count();
    if (tri_region.size() != tcount || tcount == 0) return;

    const MeshTopology& topo = analysis.topology;
    if (topo.triangle_count() != tcount) {
        RD_ERROR("region fill: topology does not match the mesh");
        return;
    }

    const DualCost edge_cost(mesh, analysis, opts);

    // Same Dijkstra as the geometric split, except that the seeds are every
    // triangle that already carries a label rather than a sampled few.
    std::vector<float> dist(tcount, std::numeric_limits<float>::max());
    MinQueue queue;
    for (size_t t = 0; t < tcount; ++t) {
        if (tri_region[t] == kNoRegion) continue;
        dist[t] = 0.0f;
        queue.push({0.0f, static_cast<uint32_t>(t)});
    }
    if (queue.empty()) return;

    while (!queue.empty()) {
        const QueueEntry e = queue.top();
        queue.pop();
        if (e.cost > dist[e.triangle]) continue;
        for (int c = 0; c < 3; ++c) {
            const uint32_t h = e.triangle * 3 + c;
            const uint32_t n = topo.neighbour(e.triangle, c);
            if (n == kInvalidIndex) continue;
            const float nd = e.cost + edge_cost(e.triangle, n, h);
            if (nd < dist[n]) {
                dist[n]        = nd;
                tri_region[n]  = tri_region[e.triangle];
                queue.push({nd, n});
            }
        }
    }
}

// ---------------------------------------------------------------------------
void finalize_segmentation(const Mesh& mesh, const MeshAnalysis& analysis,
                           const SegmentationOptions& opts, Segmentation& out,
                           const char* name_prefix)
{
    if (out.tri_region.empty()) return;
    const char* prefix = name_prefix && *name_prefix ? name_prefix : "region";

    std::unordered_set<uint16_t> live(out.tri_region.begin(), out.tri_region.end());
    live.erase(kNoRegion);
    std::vector<uint16_t> live_sorted(live.begin(), live.end());
    std::sort(live_sorted.begin(), live_sorted.end());

    out.regions.clear();
    out.regions.reserve(live_sorted.size());
    for (size_t i = 0; i < live_sorted.size(); ++i) {
        Region r;
        r.id   = static_cast<uint16_t>(i);
        r.name = format("%s_%02zu", prefix, i);
        out.regions.push_back(std::move(r));
    }
    // Compact the ids so they run 0..n-1.
    std::unordered_map<uint16_t, uint16_t> renumber;
    for (size_t i = 0; i < live_sorted.size(); ++i)
        renumber[live_sorted[i]] = static_cast<uint16_t>(i);
    for (uint16_t& v : out.tri_region) {
        const auto it = renumber.find(v);
        v = it == renumber.end() ? kNoRegion : it->second;
    }

    out.refresh_statistics(mesh, analysis);

    // --- absorb slivers -----------------------------------------------------
    bool merged_any = true;
    int  guard      = 0;
    while (merged_any && guard++ < 32) {
        merged_any = false;
        // Smallest first, so a chain of slivers collapses cleanly.
        std::vector<size_t> order(out.regions.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return out.regions[a].area_share < out.regions[b].area_share;
        });

        for (size_t idx : order) {
            if (idx >= out.regions.size()) continue;
            const Region& r = out.regions[idx];
            if (out.regions.size() <= 2) break;
            if (r.area_share >= opts.min_area_share && r.triangle_count >= 4) continue;
            if (r.neighbours.empty()) continue;

            // Merge into the neighbour whose normal it agrees with most.
            uint16_t best  = r.neighbours.front();
            float    best_score = -2.0f;
            for (uint16_t n : r.neighbours) {
                const Region* nr = out.find(n);
                if (!nr) continue;
                const float score = nr->area_share -
                                    std::fabs(nr->mean_curvature - r.mean_curvature);
                if (score > best_score) { best_score = score; best = n; }
            }

            const uint16_t victim = r.id;
            for (uint16_t& v : out.tri_region)
                if (v == victim) v = best;
            out.regions.erase(out.regions.begin() + static_cast<long>(idx));
            out.refresh_statistics(mesh, analysis);
            merged_any = true;
            break;
        }
    }

    // Final renumber to a dense 0..n-1 range.
    {
        std::unordered_map<uint16_t, uint16_t> dense;
        for (size_t i = 0; i < out.regions.size(); ++i) dense[out.regions[i].id] = static_cast<uint16_t>(i);
        for (uint16_t& v : out.tri_region) {
            const auto it = dense.find(v);
            v = it == dense.end() ? kNoRegion : it->second;
        }
        for (size_t i = 0; i < out.regions.size(); ++i) {
            out.regions[i].id   = static_cast<uint16_t>(i);
            out.regions[i].name = format("%s_%02zu", prefix, i);
        }
        out.refresh_statistics(mesh, analysis);
    }
}

// ---------------------------------------------------------------------------
int merge_regions(Segmentation& seg, const Mesh& mesh, const MeshAnalysis& analysis,
                  const std::vector<MergeGroup>& groups)
{
    if (groups.empty() || !seg.valid()) return 0;

    // Map every id to the id it should become.
    std::unordered_map<uint16_t, uint16_t> remap;
    std::unordered_map<uint16_t, const MergeGroup*> naming;
    int merges = 0;

    for (const MergeGroup& g : groups) {
        if (g.members.empty()) continue;
        uint16_t keeper = kNoRegion;
        for (uint16_t m : g.members) {
            if (!seg.find(m)) continue;
            if (keeper == kNoRegion) keeper = m;
            else { remap[m] = keeper; ++merges; }
        }
        if (keeper != kNoRegion) naming[keeper] = &g;
    }

    if (!remap.empty()) {
        for (uint16_t& v : seg.tri_region) {
            auto it = remap.find(v);
            // Chains cannot occur (each member appears in one group) but be safe.
            int guard = 0;
            while (it != remap.end() && guard++ < 8) {
                v  = it->second;
                it = remap.find(v);
            }
        }
        seg.regions.erase(
            std::remove_if(seg.regions.begin(), seg.regions.end(),
                           [&](const Region& r) { return remap.count(r.id) > 0; }),
            seg.regions.end());
    }

    for (const auto& [id, group] : naming) {
        if (Region* r = seg.find(id)) {
            if (!group->name.empty()) r->name = group->name;
            if (!group->role.empty()) r->auto_label = group->role;
        }
    }

    seg.refresh_statistics(mesh, analysis);
    RD_INFO("merged %d regions, %zu remain", merges, seg.regions.size());
    return merges;
}

void region_colors(const Segmentation& seg, std::vector<Vec4>& tri_colors)
{
    tri_colors.assign(seg.tri_region.size(), Vec4{0.5f, 0.5f, 0.5f, 1.0f});
    std::unordered_map<uint16_t, Vec3> color;
    for (const Region& r : seg.regions) color[r.id] = r.debug_color;
    for (size_t t = 0; t < seg.tri_region.size(); ++t) {
        const auto it = color.find(seg.tri_region[t]);
        if (it != color.end()) tri_colors[t] = Vec4{it->second, 1.0f};
    }
}

std::string region_table_text(const Segmentation& seg, const Mesh& mesh)
{
    std::string out;
    out += "id  | name        | tris  | area%  | curv | ambient | visibility | shape      | joint\n";
    out += "----+-------------+-------+--------+------+---------+------------+------------+------------------\n";
    for (const Region& r : seg.regions) {
        std::string joint = "-";
        if (r.dominant_joint >= 0 && static_cast<size_t>(r.dominant_joint) < mesh.armature.size())
            joint = mesh.armature.joints[r.dominant_joint].name;
        const std::string shape =
            r.tube_aspect > 0.0f ? format("tube %.0f:1", r.tube_aspect) : std::string("-");
        out += format("%-3u | %-11s | %5u | %5.1f%% | %.2f | %.2f    | %.3f      | %-10s | %s\n",
                      unsigned(r.id), r.name.substr(0, 11).c_str(), r.triangle_count,
                      r.area_share * 100.0f, r.mean_curvature, r.mean_ambient,
                      r.visibility, shape.c_str(), joint.c_str());
    }
    return out;
}

} // namespace rd
