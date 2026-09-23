#include "geom/hard_rules.h"

#include "core/log.h"
#include "core/util.h"
#include "mesh/topology.h"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace rd {
namespace {

inline uint64_t edge_key(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

// Splits every triangle that straddles the plane so no triangle crosses it.
// Returns the number of triangles that had to be cut.
size_t split_along_plane(Mesh& mesh, const SymmetryPlane& plane, float epsilon,
                         std::vector<float>& side_out)
{
    const size_t vcount = mesh.vertex_count();
    side_out.assign(vcount, 0.0f);

    // Snap anything within epsilon exactly onto the plane first, which removes
    // most of the crossings before we have to cut anything.
    for (size_t v = 0; v < vcount; ++v) {
        float d = dot(plane.normal, mesh.positions[v]) - plane.offset;
        if (std::fabs(d) <= epsilon) {
            mesh.positions[v] = mesh.positions[v] - plane.normal * d;
            d = 0.0f;
        }
        side_out[v] = d;
    }

    std::unordered_map<uint64_t, uint32_t> cut_vertex;
    std::vector<uint32_t> new_indices;
    std::vector<uint16_t> new_regions;
    const bool keep_regions = mesh.tri_region.size() == mesh.triangle_count();
    new_indices.reserve(mesh.indices.size());

    auto vertex_on_plane = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint64_t key = edge_key(a, b);
        const auto it = cut_vertex.find(key);
        if (it != cut_vertex.end()) return it->second;

        const float da = side_out[a], db = side_out[b];
        const float denom = da - db;
        const float t = std::fabs(denom) > 1e-12f ? clampf(da / denom, 0.0f, 1.0f) : 0.5f;
        Vec3 p = lerp(mesh.positions[a], mesh.positions[b], t);
        p = p - plane.normal * (dot(plane.normal, p) - plane.offset);

        const uint32_t idx = uint32_t(mesh.positions.size());
        mesh.positions.push_back(p);
        side_out.push_back(0.0f);
        cut_vertex.emplace(key, idx);
        return idx;
    };

    size_t cuts = 0;
    const size_t tcount = mesh.triangle_count();

    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t v[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
        const float    d[3] = {side_out[v[0]], side_out[v[1]], side_out[v[2]]};
        const uint16_t r    = keep_regions ? mesh.tri_region[t] : kNoRegion;

        int pos_count = 0, neg_count = 0;
        for (int i = 0; i < 3; ++i) {
            if (d[i] > 0.0f) ++pos_count;
            else if (d[i] < 0.0f) ++neg_count;
        }

        auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a == b || b == c || a == c) return;
            new_indices.push_back(a); new_indices.push_back(b); new_indices.push_back(c);
            if (keep_regions) new_regions.push_back(r);
        };

        if (pos_count == 0 || neg_count == 0) {   // entirely on one side
            emit(v[0], v[1], v[2]);
            continue;
        }
        ++cuts;

        if (pos_count == 1 && neg_count == 1) {
            // One vertex sits on the plane; a single cut splits it in two.
            int on = 0;
            for (int i = 0; i < 3; ++i) if (d[i] == 0.0f) on = i;
            const int a = (on + 1) % 3, b = (on + 2) % 3;
            const uint32_t m = vertex_on_plane(v[a], v[b]);
            emit(v[on], v[a], m);
            emit(v[on], m, v[b]);
            continue;
        }

        // Two vertices on one side, one on the other: cut both crossing edges.
        int lone = 0;
        if (pos_count == 1) { for (int i = 0; i < 3; ++i) if (d[i] > 0.0f) lone = i; }
        else                { for (int i = 0; i < 3; ++i) if (d[i] < 0.0f) lone = i; }

        const int a = (lone + 1) % 3, b = (lone + 2) % 3;
        const uint32_t m0 = vertex_on_plane(v[lone], v[a]);
        const uint32_t m1 = vertex_on_plane(v[b], v[lone]);
        emit(v[lone], m0, m1);      // the lone corner
        emit(m0, v[a], v[b]);       // the other side, as a fan
        emit(m0, v[b], m1);
    }

    mesh.indices.swap(new_indices);
    if (keep_regions) mesh.tri_region.swap(new_regions);
    return cuts;
}

} // namespace

// ---------------------------------------------------------------------------
size_t enforce_symmetry(Mesh& mesh, const SymmetryPlane& plane, float epsilon,
                        const Bvh* reproject_onto, size_t* clipped_out)
{
    if (mesh.empty()) return 0;

    std::vector<float> side;
    const size_t cuts = split_along_plane(mesh, plane, epsilon, side);
    if (clipped_out) *clipped_out = cuts;

    // Decide which half to keep: whichever carries more surface area.
    double pos_area = 0.0, neg_area = 0.0;
    const size_t tcount = mesh.triangle_count();
    std::vector<int8_t> tri_side(tcount, 0);

    for (size_t t = 0; t < tcount; ++t) {
        float sum = 0.0f;
        for (int i = 0; i < 3; ++i) sum += side[mesh.indices[t * 3 + i]];
        const float area = mesh.triangle_area(t);
        if (sum > 0.0f)      { tri_side[t] = 1;  pos_area += area; }
        else if (sum < 0.0f) { tri_side[t] = -1; neg_area += area; }
        else                 { tri_side[t] = 0; }   // degenerate sliver on the plane
    }

    const int8_t keep = (pos_area >= neg_area) ? 1 : -1;

    // Mirroring is the one hard rule that can throw geometry away, so it says
    // how much. A half that is nowhere near half the mesh means the plane, not
    // the retopology, is what made the asset small.
    {
        size_t on_plane = 0, kept_tris = 0;
        for (size_t t = 0; t < tcount; ++t) {
            if (tri_side[t] == 0)    ++on_plane;
            else if (tri_side[t] == keep) ++kept_tris;
        }
        RD_DEBUG("symmetry: %zu tri in, keeping %zu on the %s side, %zu straddle the "
                 "plane, area %.4f vs %.4f, eps %.5f",
                 tcount, kept_tris, keep > 0 ? "positive" : "negative", on_plane,
                 pos_area, neg_area, epsilon);
    }

    // Collect the kept half.
    Mesh half;
    half.name             = mesh.name;
    half.armature         = mesh.armature;
    half.import_transform = mesh.import_transform;
    half.import_scale     = mesh.import_scale;

    std::vector<uint32_t> remap(mesh.vertex_count(), kInvalidIndex);
    const bool keep_regions = mesh.tri_region.size() == tcount;

    for (size_t t = 0; t < tcount; ++t) {
        if (tri_side[t] != keep) continue;
        for (int i = 0; i < 3; ++i) {
            const uint32_t v = mesh.indices[t * 3 + i];
            if (remap[v] == kInvalidIndex) {
                remap[v] = uint32_t(half.positions.size());
                half.positions.push_back(mesh.positions[v]);
            }
            half.indices.push_back(remap[v]);
        }
        if (keep_regions) half.tri_region.push_back(mesh.tri_region[t]);
    }

    if (half.empty()) {
        RD_WARN("symmetry: nothing survived the cut, leaving the mesh alone");
        return 0;
    }

    // Mirror it.
    const size_t half_v = half.positions.size();
    const size_t half_t = half.triangle_count();
    std::vector<uint32_t> mirror_index(half_v, kInvalidIndex);

    for (size_t v = 0; v < half_v; ++v) {
        const float d = dot(plane.normal, half.positions[v]) - plane.offset;
        if (std::fabs(d) <= epsilon) {
            // Sits on the plane: shared between both halves.
            half.positions[v] = half.positions[v] - plane.normal * d;
            mirror_index[v]   = uint32_t(v);
        } else {
            mirror_index[v] = uint32_t(half.positions.size());
            half.positions.push_back(plane.mirror(half.positions[v]));
        }
    }

    half.indices.reserve(half.indices.size() * 2);
    if (keep_regions) half.tri_region.reserve(half_t * 2);
    for (size_t t = 0; t < half_t; ++t) {
        // Reversed winding, because mirroring flips handedness.
        const uint32_t a = mirror_index[half.indices[t * 3 + 0]];
        const uint32_t b = mirror_index[half.indices[t * 3 + 1]];
        const uint32_t c = mirror_index[half.indices[t * 3 + 2]];
        half.indices.push_back(a);
        half.indices.push_back(c);
        half.indices.push_back(b);
        // Copy first: push_back on the vector we are indexing may reallocate.
        if (keep_regions) {
            const uint16_t region = half.tri_region[t];
            half.tri_region.push_back(region);
        }
    }

    if (reproject_onto && !reproject_onto->empty()) {
        // Pull the surface back onto the high poly, which mirroring left it
        // slightly off - and do it only for the half that was kept.
        //
        // Snapping every vertex independently, mirrored copies included, is
        // what used to happen, and it quietly undoes the mirror: the source is
        // never perfectly symmetric (this character scores 0.97 on its best
        // plane), so the two copies of a vertex get pulled to two different
        // places. The finer the low poly, the more faithfully it inherits the
        // source's asymmetry, which is a strange way to fail - raising the
        // budget made the asset stop passing a check it used to pass. So the
        // kept half is reprojected and its mirror is derived from the result,
        // which is exact by construction.
        //
        // This has to run before the weld, because that is the last moment at
        // which `mirror_index` still says which vertex is whose reflection.
        const float max_move = std::max(epsilon * 12.0f, 1e-6f);
        for (size_t v = 0; v < half_v; ++v) {
            Vec3&       p = half.positions[v];
            const float d = dot(plane.normal, p) - plane.offset;

            // The search is capped: a vertex allowed to jump across a thin
            // feature turns its triangles inside out, which then bakes to black.
            const ClosestHit hit = reproject_onto->closest_point(p, max_move);
            if (hit.hit()) {
                Vec3 snapped = hit.point;
                if (std::fabs(d) <= epsilon)
                    snapped = snapped - plane.normal * (dot(plane.normal, snapped) - plane.offset);
                p = snapped;
            }

            const uint32_t m = mirror_index[v];
            if (m != uint32_t(v) && m < half.positions.size())
                half.positions[m] = plane.mirror(p);
        }
    }

    half.weld(epsilon * 0.5f);
    half.remove_degenerate();
    half.compact();

    const size_t mirrored = half_t;
    mesh = std::move(half);
    return mirrored;
}

size_t fix_winding(Mesh& mesh, const Bvh& source_bvh)
{
    if (mesh.empty()) return 0;

    const size_t tcount = mesh.triangle_count();

    // --- 1. propagate a consistent orientation across shared edges ----------
    // Two triangles sharing an edge agree when they traverse that edge in
    // opposite directions. Flipping triangle by triangle against a geometric
    // reference does not work: near a crease the nearest source face can point
    // the other way, and the resulting patchwork orientation makes the unwrap
    // shatter into tiny charts.
    std::unordered_map<uint64_t, std::vector<uint32_t>> edge_faces;
    edge_faces.reserve(tcount * 3);
    for (size_t t = 0; t < tcount; ++t)
        for (int i = 0; i < 3; ++i)
            edge_faces[edge_key(mesh.indices[t * 3 + i], mesh.indices[t * 3 + (i + 1) % 3])]
                .push_back(uint32_t(t));

    std::vector<uint8_t>  visited(tcount, 0);
    std::vector<uint32_t> component(tcount, kInvalidIndex);
    std::vector<std::vector<uint32_t>> components;
    std::vector<uint32_t> stack;
    size_t flipped = 0;

    auto traverses = [&](uint32_t t, uint32_t a, uint32_t b) {
        for (int i = 0; i < 3; ++i)
            if (mesh.indices[t * 3 + i] == a && mesh.indices[t * 3 + (i + 1) % 3] == b)
                return true;
        return false;
    };

    for (size_t seed = 0; seed < tcount; ++seed) {
        if (visited[seed]) continue;

        components.emplace_back();
        const uint32_t comp = uint32_t(components.size() - 1);

        stack.clear();
        stack.push_back(uint32_t(seed));
        visited[seed]   = 1;
        component[seed] = comp;
        components.back().push_back(uint32_t(seed));

        while (!stack.empty()) {
            const uint32_t t = stack.back();
            stack.pop_back();

            for (int i = 0; i < 3; ++i) {
                const uint32_t a = mesh.indices[t * 3 + i];
                const uint32_t b = mesh.indices[t * 3 + (i + 1) % 3];
                const auto it = edge_faces.find(edge_key(a, b));
                if (it == edge_faces.end()) continue;

                for (uint32_t n : it->second) {
                    if (n == t || visited[n]) continue;
                    // n agrees when it walks this edge the other way round.
                    if (traverses(n, a, b)) {
                        std::swap(mesh.indices[n * 3 + 1], mesh.indices[n * 3 + 2]);
                        ++flipped;
                    }
                    visited[n]   = 1;
                    component[n] = comp;
                    components[comp].push_back(n);
                    stack.push_back(n);
                }
            }
        }
    }

    // --- 2. decide the global sign, once per component ----------------------
    for (const std::vector<uint32_t>& faces : components) {
        if (faces.empty()) continue;

        // Signed volume: positive when the normals of a closed surface face out.
        double volume = 0.0;
        for (uint32_t t : faces) {
            const Vec3 a = mesh.positions[mesh.indices[t * 3 + 0]];
            const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
            const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
            volume += double(dot(a, cross(b, c)));
        }

        bool flip = volume < 0.0;

        // An open shell has no meaningful volume, so ask the high poly instead
        // and take the majority verdict rather than any single answer.
        if (std::fabs(volume) < 1e-9 && !source_bvh.empty()) {
            int agree = 0, disagree = 0;
            const size_t step = std::max<size_t>(1, faces.size() / 256);
            for (size_t i = 0; i < faces.size(); i += step) {
                const uint32_t t = faces[i];
                const Vec3 n = mesh.triangle_normal(t);
                if (length2(n) < 0.5f) continue;
                const ClosestHit hit = source_bvh.closest_point(mesh.triangle_centroid(t));
                if (!hit.hit()) continue;
                if (dot(n, source_bvh.geometric_normal(hit.triangle)) >= 0.0f) ++agree;
                else                                                           ++disagree;
            }
            flip = disagree > agree;
        }

        if (!flip) continue;
        for (uint32_t t : faces) {
            std::swap(mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]);
            ++flipped;
        }
    }

    return flipped;
}

size_t repair_nonmanifold(Mesh& mesh)
{
    if (mesh.empty()) return 0;

    const size_t tcount = mesh.triangle_count();
    std::unordered_map<uint64_t, std::vector<uint32_t>> edge_faces;
    edge_faces.reserve(tcount * 3);

    for (size_t t = 0; t < tcount; ++t)
        for (int i = 0; i < 3; ++i)
            edge_faces[edge_key(mesh.indices[t * 3 + i], mesh.indices[t * 3 + (i + 1) % 3])]
                .push_back(uint32_t(t));

    std::vector<uint8_t> drop(tcount, 0);
    size_t removed = 0;

    // Deterministic order: walk the map through a sorted key list.
    std::vector<uint64_t> keys;
    keys.reserve(edge_faces.size());
    for (const auto& kv : edge_faces)
        if (kv.second.size() > 2) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (uint64_t k : keys) {
        auto& faces = edge_faces[k];
        // Keep the two faces whose normals disagree the least; the rest are the
        // stray fins that sculpt decimation leaves behind.
        std::vector<std::pair<float, uint32_t>> scored;
        for (uint32_t t : faces) {
            if (drop[t]) continue;
            float agreement = 0.0f;
            const Vec3 n = mesh.triangle_normal(t);
            for (uint32_t o : faces)
                if (o != t && !drop[o]) agreement += std::fabs(dot(n, mesh.triangle_normal(o)));
            scored.push_back({agreement, t});
        }
        if (scored.size() <= 2) continue;
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        for (size_t i = 2; i < scored.size(); ++i) {
            drop[scored[i].second] = 1;
            ++removed;
        }
    }

    if (removed == 0) return 0;

    std::vector<uint32_t> kept;
    std::vector<uint16_t> kept_regions;
    const bool keep_regions = mesh.tri_region.size() == tcount;
    for (size_t t = 0; t < tcount; ++t) {
        if (drop[t]) continue;
        for (int i = 0; i < 3; ++i) kept.push_back(mesh.indices[t * 3 + i]);
        if (keep_regions) kept_regions.push_back(mesh.tri_region[t]);
    }
    mesh.indices.swap(kept);
    if (keep_regions) mesh.tri_region.swap(kept_regions);
    mesh.compact();
    return removed;
}

size_t limit_shells(Mesh& mesh, int max_shells, float min_area_share,
                    const SymmetryPlane* mirror, int secondary_triangle_cap)
{
    if (max_shells <= 0 || mesh.empty()) return 0;

    const size_t tcount = mesh.triangle_count();
    const size_t vcount = mesh.vertex_count();

    std::vector<uint32_t> parent(vcount);
    for (uint32_t i = 0; i < vcount; ++i) parent[i] = i;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[b] = a;
    };
    for (size_t t = 0; t < tcount; ++t) {
        unite(mesh.indices[t * 3], mesh.indices[t * 3 + 1]);
        unite(mesh.indices[t * 3], mesh.indices[t * 3 + 2]);
    }

    std::map<uint32_t, double> shell_area;
    std::map<uint32_t, int>    shell_tris;
    std::vector<uint32_t>      tri_shell(tcount);
    double                     total_area = 0.0;
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t root = find(mesh.indices[t * 3]);
        tri_shell[t] = root;
        const double a = mesh.triangle_area(t);
        shell_area[root] += a;
        ++shell_tris[root];
        total_area += a;
    }
    if (shell_area.size() <= size_t(max_shells)) return 0;

    std::vector<std::pair<double, uint32_t>> ranked;
    ranked.reserve(shell_area.size());
    for (const auto& [root, area] : shell_area) ranked.push_back({area, root});
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });

    // Mirrored shells have identical area, so the sort above separates them by
    // nothing but their arbitrary root index, and a cut that lands between the
    // two keeps one arm of a pair and deletes the other. The asset then fails
    // the symmetry check it was mirrored to pass, and the reason is invisible:
    // the shell count is right, the geometry is symmetric everywhere it still
    // exists. Pair them up first and admit a pair or neither.
    std::unordered_map<uint32_t, uint32_t> partner;
    if (mirror) {
        std::unordered_map<uint32_t, Vec3>  centroid;
        std::unordered_map<uint32_t, float> weight;
        for (size_t t = 0; t < tcount; ++t) {
            const float a = mesh.triangle_area(t);
            const Vec3  c = (mesh.positions[mesh.indices[t * 3 + 0]] +
                             mesh.positions[mesh.indices[t * 3 + 1]] +
                             mesh.positions[mesh.indices[t * 3 + 2]]) * (1.0f / 3.0f);
            centroid[tri_shell[t]] = centroid[tri_shell[t]] + c * a;
            weight[tri_shell[t]]  += a;
        }
        for (auto& [root, c] : centroid)
            if (weight[root] > kEps) c = c * (1.0f / weight[root]);

        const float tol = std::max(mesh.bounds().diagonal() * 0.01f, 1e-6f);
        for (const auto& [area_a, root_a] : ranked) {
            if (partner.count(root_a)) continue;
            const Vec3 want = mirror->mirror(centroid[root_a]);
            // A shell straddling the plane is its own mirror; leave it single.
            if (length2(want - centroid[root_a]) <= tol * tol) continue;

            uint32_t best = kInvalidIndex;
            float    best_d2 = tol * tol;
            for (const auto& [area_b, root_b] : ranked) {
                if (root_b == root_a || partner.count(root_b)) continue;
                if (std::fabs(area_b - area_a) > area_a * 0.05 + 1e-9) continue;
                const float d2 = length2(want - centroid[root_b]);
                if (d2 < best_d2) { best_d2 = d2; best = root_b; }
            }
            if (best != kInvalidIndex) {
                partner[root_a] = best;
                partner[best]   = root_a;
            }
        }
    }

    std::unordered_set<uint32_t> keep;
    int secondary_tris = 0;
    for (size_t i = 0; i < ranked.size(); ++i) {
        const uint32_t root = ranked[i].second;
        if (keep.count(root)) continue;

        const double share = total_area > 0.0 ? ranked[i].first / total_area : 0.0;
        if (!keep.empty() && share < double(min_area_share)) break;

        const auto it = partner.find(root);
        const int  needed = (it != partner.end() && !keep.count(it->second)) ? 2 : 1;
        if (int(keep.size()) + needed > max_shells) {
            // A pair that does not fit ends the admission rather than being
            // halved; a later, smaller pair would fit but taking it out of
            // order would drop a bigger piece of the asset for a smaller one.
            break;
        }
        const int cost = shell_tris[root] + (needed == 2 ? shell_tris[it->second] : 0);
        if (!keep.empty() && secondary_triangle_cap > 0 &&
            secondary_tris + cost > secondary_triangle_cap)
            break;
        if (!keep.empty()) secondary_tris += cost;
        keep.insert(root);
        if (needed == 2) keep.insert(it->second);
    }
    if (keep.empty()) keep.insert(ranked.front().second);

    std::vector<uint32_t> kept;
    std::vector<uint16_t> kept_regions;
    const bool keep_regions = mesh.tri_region.size() == tcount;
    size_t removed = 0;
    for (size_t t = 0; t < tcount; ++t) {
        if (!keep.count(tri_shell[t])) { ++removed; continue; }
        for (int i = 0; i < 3; ++i) kept.push_back(mesh.indices[t * 3 + i]);
        if (keep_regions) kept_regions.push_back(mesh.tri_region[t]);
    }
    mesh.indices.swap(kept);
    if (keep_regions) mesh.tri_region.swap(kept_regions);
    mesh.compact();
    return removed;
}

size_t insert_joint_loops(Mesh& mesh, const MeshAnalysis& analysis, const Bvh& source_bvh,
                          float density, int max_new_triangles)
{
    if (mesh.armature.empty() || density <= 0.0f || max_new_triangles <= 0) return 0;

    const Armature& arm = mesh.armature;
    size_t inserted = 0;

    // A joint that bends needs enough vertices around it to hold a crease.
    // Four is the bare minimum for a hinge; the knob scales from there.
    const int required = std::max(4, int(std::lround(4.0f + 4.0f * density)));

    for (size_t j = 0; j < arm.size(); ++j) {
        if (int(inserted) * 2 >= max_new_triangles) break;

        const float radius = j < analysis.joint_influence_radius.size()
                                 ? analysis.joint_influence_radius[j]
                                 : 0.0f;
        if (radius <= kEps) continue;

        const Vec3  pivot = arm.joints[j].bind_position;
        const float band  = radius * 0.45f;

        int nearby = 0;
        for (const Vec3& p : mesh.positions)
            if (length(p - pivot) <= band) ++nearby;
        if (nearby >= required) continue;

        // Split the longest edges inside the band until the ring is dense
        // enough, or until we run out of headroom.
        for (int attempt = 0; attempt < required && nearby < required; ++attempt) {
            if (int(inserted) * 2 >= max_new_triangles) break;

            // Find the longest edge with both ends in the band.
            uint32_t best_a = kInvalidIndex, best_b = kInvalidIndex;
            float    best_len = 0.0f;
            const size_t tcount = mesh.triangle_count();
            for (size_t t = 0; t < tcount; ++t) {
                for (int i = 0; i < 3; ++i) {
                    const uint32_t a = mesh.indices[t * 3 + i];
                    const uint32_t b = mesh.indices[t * 3 + (i + 1) % 3];
                    if (length(mesh.positions[a] - pivot) > band) continue;
                    if (length(mesh.positions[b] - pivot) > band) continue;
                    const float len = length(mesh.positions[b] - mesh.positions[a]);
                    if (len > best_len) { best_len = len; best_a = a; best_b = b; }
                }
            }
            if (best_a == kInvalidIndex || best_len <= kEps) break;

            // Insert the midpoint, projected back onto the high poly.
            Vec3 mid = (mesh.positions[best_a] + mesh.positions[best_b]) * 0.5f;
            if (!source_bvh.empty()) {
                const ClosestHit hit = source_bvh.closest_point(mid);
                if (hit.hit()) mid = hit.point;
            }
            const uint32_t m = uint32_t(mesh.positions.size());
            mesh.positions.push_back(mid);

            std::vector<uint32_t> new_indices;
            std::vector<uint16_t> new_regions;
            const bool keep_regions = mesh.tri_region.size() == mesh.triangle_count();
            new_indices.reserve(mesh.indices.size() + 6);

            for (size_t t = 0; t < mesh.triangle_count(); ++t) {
                const uint16_t r = keep_regions ? mesh.tri_region[t] : kNoRegion;
                int corner = -1;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t a = mesh.indices[t * 3 + i];
                    const uint32_t b = mesh.indices[t * 3 + (i + 1) % 3];
                    if ((a == best_a && b == best_b) || (a == best_b && b == best_a)) {
                        corner = i;
                        break;
                    }
                }
                if (corner < 0) {
                    for (int i = 0; i < 3; ++i) new_indices.push_back(mesh.indices[t * 3 + i]);
                    if (keep_regions) new_regions.push_back(r);
                    continue;
                }
                const uint32_t p = mesh.indices[t * 3 + corner];
                const uint32_t q = mesh.indices[t * 3 + (corner + 1) % 3];
                const uint32_t w = mesh.indices[t * 3 + (corner + 2) % 3];
                new_indices.insert(new_indices.end(), {p, m, w});
                new_indices.insert(new_indices.end(), {m, q, w});
                if (keep_regions) { new_regions.push_back(r); new_regions.push_back(r); }
                ++inserted;
            }
            mesh.indices.swap(new_indices);
            if (keep_regions) mesh.tri_region.swap(new_regions);
            ++nearby;
        }
    }
    return inserted;
}

// ---------------------------------------------------------------------------
float fit_to_surface(Mesh& mesh, const Bvh& source_bvh, int passes,
                     const SymmetryPlane* symmetry, float symmetry_epsilon)
{
    if (mesh.empty() || source_bvh.empty() || passes <= 0) return 0.0f;

    const size_t vcount = mesh.vertex_count();
    const size_t tcount = mesh.triangle_count();

    // Mirror partners, found once: the moves below are computed per vertex and
    // would drift a symmetric mesh apart by float noise, so each pass averages
    // a vertex with its partner's reflection and pins plane vertices to it.
    std::vector<uint32_t> partner(vcount, kInvalidIndex);
    std::vector<bool>     on_plane(vcount, false);
    if (symmetry) {
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        const float cell = std::max(symmetry_epsilon * 2.0f, 1e-7f);
        auto key = [&](Vec3 p) {
            const int64_t x = int64_t(std::floor(p.x / cell));
            const int64_t y = int64_t(std::floor(p.y / cell));
            const int64_t z = int64_t(std::floor(p.z / cell));
            return uint64_t(x * 73856093) ^ uint64_t(y * 19349663) ^ uint64_t(z * 83492791);
        };
        for (uint32_t v = 0; v < vcount; ++v) grid[key(mesh.positions[v])].push_back(v);
        for (uint32_t v = 0; v < vcount; ++v) {
            const Vec3 p = mesh.positions[v];
            if (std::fabs(dot(symmetry->normal, p) - symmetry->offset) <= symmetry_epsilon) {
                on_plane[v] = true;
                continue;
            }
            const Vec3 m = symmetry->mirror(p);
            float best = symmetry_epsilon * symmetry_epsilon;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto it =
                            grid.find(key(m + Vec3(float(dx), float(dy), float(dz)) * cell));
                        if (it == grid.end()) continue;
                        for (uint32_t w : it->second) {
                            const float d2 = length2(mesh.positions[w] - m);
                            if (d2 <= best) { best = d2; partner[v] = w; }
                        }
                    }
        }
    }

    // Where each face is sampled: the centroid and a point toward each corner,
    // with the share of each sample that goes to each corner.
    static const float kSample[4][3] = {{1.f / 3, 1.f / 3, 1.f / 3},
                                        {4.f / 6, 1.f / 6, 1.f / 6},
                                        {1.f / 6, 4.f / 6, 1.f / 6},
                                        {1.f / 6, 1.f / 6, 4.f / 6}};

    float last_move = 0.0f;
    std::vector<double> push(vcount), weight(vcount), edge_sum(vcount);
    std::vector<int>    edge_n(vcount);

    for (int pass = 0; pass < passes; ++pass) {
        mesh.compute_normals();
        std::fill(push.begin(), push.end(), 0.0);
        std::fill(weight.begin(), weight.end(), 0.0);
        std::fill(edge_sum.begin(), edge_sum.end(), 0.0);
        std::fill(edge_n.begin(), edge_n.end(), 0);

        for (size_t t = 0; t < tcount; ++t) {
            const uint32_t idx[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1],
                                     mesh.indices[t * 3 + 2]};
            const Vec3 a = mesh.positions[idx[0]], b = mesh.positions[idx[1]],
                       c = mesh.positions[idx[2]];
            const Vec3  cr   = cross(b - a, c - a);
            const float area = 0.5f * length(cr);
            if (area < 1e-14f) continue;
            const Vec3  n    = cr / (2.0f * area);
            const float edge = (length(b - a) + length(c - b) + length(a - c)) / 3.0f;
            for (int k = 0; k < 3; ++k) { edge_sum[idx[k]] += edge; ++edge_n[idx[k]]; }

            // Only look as far as a face could plausibly be off its own surface.
            // Further than that the nearest surface is some other part - the
            // torso beside an arm, the far side of a thin plate - and pulling
            // toward it is how faces end up bridging a gap.
            const float reach = edge * 0.75f;
            for (const auto& w : kSample) {
                const Vec3 s = a * w[0] + b * w[1] + c * w[2];
                const ClosestHit hit = source_bvh.closest_point(s, reach);
                if (!hit.hit()) continue;
                // A surface facing the other way is the back of something thin.
                if (dot(source_bvh.geometric_normal(hit.triangle), n) < 0.3f) continue;
                const float d = dot(hit.point - s, n);
                for (int k = 0; k < 3; ++k) {
                    push[idx[k]]   += double(d) * w[k] * area;
                    weight[idx[k]] += double(w[k]) * area;
                }
            }
        }

        // Move, damped and capped. The cap is a fraction of the local edge
        // length, which is what keeps a vertex from stepping past a neighbour
        // and folding a face over.
        std::vector<Vec3> moved = mesh.positions;
        double pass_move = 0.0;
        for (size_t v = 0; v < vcount; ++v) {
            if (weight[v] <= 0.0 || edge_n[v] == 0 || !mesh.has_normals()) continue;
            const float mean_edge = float(edge_sum[v] / edge_n[v]);
            const float offset = clampf(float(push[v] / weight[v]) * 0.8f,
                                        -0.2f * mean_edge, 0.2f * mean_edge);
            moved[v] = mesh.positions[v] + mesh.normals[v] * offset;
            pass_move += std::fabs(offset);
        }
        if (symmetry) {
            for (size_t v = 0; v < vcount; ++v) {
                if (on_plane[v]) {
                    moved[v] = moved[v] - symmetry->normal *
                                              (dot(symmetry->normal, moved[v]) - symmetry->offset);
                } else if (partner[v] != kInvalidIndex && partner[partner[v]] == uint32_t(v)) {
                    const uint32_t w = partner[v];
                    if (w > v) {
                        const Vec3 avg = (moved[v] + symmetry->mirror(moved[w])) * 0.5f;
                        moved[v] = avg;
                        moved[w] = symmetry->mirror(avg);
                    }
                }
            }
        }
        mesh.positions.swap(moved);
        last_move = float(pass_move / double(std::max<size_t>(1, vcount)));
    }
    mesh.compute_normals(60.0f);
    return last_move;
}

// ---------------------------------------------------------------------------
HardRuleReport apply_hard_rules(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                                const MeshAnalysis& analysis, const TargetProfile& profile,
                                const GlobalKnobs& knobs, const SymmetryPlane& symmetry,
                                const HardRuleOptions& opts)
{
    Stopwatch watch;
    HardRuleReport rep;
    if (mesh.empty()) return rep;

    const float eps = std::max(analysis.bbox_diagonal * opts.symmetry_epsilon_rel, 1e-7f);

    // --- 1. basic hygiene ---------------------------------------------------
    rep.removed_degenerate = mesh.remove_degenerate();
    rep.welded_vertices    = mesh.weld(eps * 0.4f);

    // --- 2. symmetry --------------------------------------------------------
    const bool want_symmetry = opts.enforce_symmetry && profile.require_symmetry &&
                               knobs.enforce_symmetry;
    if (want_symmetry) {
        if (!symmetry.accepted) {
            rep.note(format("symmetry requested but the high poly only scores %.2f on its "
                            "best plane (%s); skipped to avoid destroying the shape",
                            symmetry.score, symmetry.axis_name()));
        } else {
            size_t clipped = 0;
            rep.mirrored_triangles = enforce_symmetry(
                mesh, symmetry, eps, opts.reproject ? &source_bvh : nullptr, &clipped);
            rep.clipped_triangles = clipped;
            rep.symmetry_applied  = rep.mirrored_triangles > 0;
            if (rep.symmetry_applied)
                rep.note(format("mirrored across %s, %zu triangles cut at the seam",
                                symmetry.axis_name(), clipped));
        }
    }

    // --- 3. manifold --------------------------------------------------------
    if (opts.enforce_manifold && profile.require_manifold) {
        rep.removed_nonmanifold = repair_nonmanifold(mesh);
        if (rep.removed_nonmanifold)
            rep.note(format("removed %zu non manifold faces", rep.removed_nonmanifold));
    }

    if (profile.max_shells > 0) {
        const size_t before_shell_cull = mesh.triangle_count();
        rep.removed_shells = limit_shells(mesh, profile.max_shells, opts.min_shell_area_share,
                                          rep.symmetry_applied ? &symmetry : nullptr,
                                          int(profile.max_triangles * opts.secondary_shell_budget_share));
        if (rep.removed_shells) {
            rep.note(format("dropped %zu triangles in loose shells (profile allows %d)",
                            rep.removed_shells, profile.max_shells));

            // A character authored as separate pieces - body, hood, belts, boots -
            // reads `max_shells: 1` as an instruction to delete everything but the
            // largest piece. That is a faithful reading of the profile and a
            // catastrophic one for the asset, and it used to happen in silence:
            // the validator then reports "1 shell, ok" over what is left of a
            // torso, and every knob upstream looks like the thing to blame.
            const float share = before_shell_cull > 0
                                    ? float(rep.removed_shells) / float(before_shell_cull)
                                    : 0.0f;
            if (share > 0.15f)
                RD_WARN("the shell limit discarded %.0f%% of the mesh (%zu of %zu triangles): "
                        "this source is built from %zu separate pieces and the profile allows "
                        "%d. Raise max_shells, or join the pieces before retopology.",
                        share * 100.0f, rep.removed_shells, before_shell_cull,
                        analysis.stats.shells, profile.max_shells);
        }
    }

    // --- 4. deformation loops ----------------------------------------------
    if (opts.enforce_joint_loops && !mesh.armature.empty() && knobs.joint_loop_density > 0.0f) {
        const int headroom = std::max(
            0, int(profile.max_triangles * opts.joint_split_headroom) -
                   std::max(0, int(mesh.triangle_count()) - profile.max_triangles));
        rep.joint_splits = insert_joint_loops(mesh, analysis, source_bvh,
                                              knobs.joint_loop_density, headroom);
        if (rep.joint_splits)
            rep.note(format("inserted %zu triangles to hold deformation loops at joints",
                            rep.joint_splits));
    }

    // --- 5. fit the faces, not just the vertices ------------------------------
    if (opts.fit_surface_passes > 0) {
        const float moved = fit_to_surface(mesh, source_bvh, opts.fit_surface_passes,
                                           rep.symmetry_applied ? &symmetry : nullptr, eps);
        if (moved > 0.0f)
            rep.note(format("fitted the faces to the surface, last pass moved %.3f%% of the "
                            "model's size on average",
                            100.0f * moved / std::max(analysis.bbox_diagonal, kEps)));
    }

    // --- 6. final tidy ------------------------------------------------------
    rep.removed_degenerate += mesh.remove_degenerate();
    mesh.compact();

    // Orientation last, after everything that could have disturbed it.
    rep.reversed_triangles = fix_winding(mesh, source_bvh);
    if (rep.reversed_triangles)
        rep.note(format("re-oriented %zu triangles so the surface faces one way",
                        rep.reversed_triangles));

    mesh.compute_normals(60.0f);

    // n-gons cannot exist here: everything upstream emits triangles only.
    if (!profile.allow_ngons) rep.note("triangles only, as the profile requires");

    rep.seconds = watch.seconds();
    for (const std::string& m : rep.messages) RD_DEBUG("hard rules: %s", m.c_str());
    RD_INFO("hard rules: %zu tri after %s in %s", mesh.triangle_count(),
            rep.symmetry_applied ? "mirroring" : "cleanup",
            format_duration(rep.seconds).c_str());
    return rep;
}

} // namespace rd
