#include "mesh/analysis.h"

#include "core/log.h"
#include "core/thread_pool.h"
#include "core/util.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>

namespace rd {
namespace {

// Distance from p to the segment ab.
float point_segment_distance(Vec3 p, Vec3 a, Vec3 b)
{
    const Vec3  ab = b - a;
    const float denom = length2(ab);
    if (denom < 1e-12f) return length(p - a);
    const float t = clampf(dot(p - a, ab) / denom, 0.0f, 1.0f);
    return length(p - (a + ab * t));
}

float percentile_of(std::vector<float> values, float p)
{
    if (values.empty()) return 1.0f;
    const size_t k = static_cast<size_t>(clampf(p, 0.0f, 1.0f) * (values.size() - 1));
    std::nth_element(values.begin(), values.begin() + k, values.end());
    return values[k];
}

} // namespace

const char* SymmetryPlane::axis_name() const
{
    const Vec3 a = abs(normal);
    if (a.x > 0.9f) return "X";
    if (a.y > 0.9f) return "Y";
    if (a.z > 0.9f) return "Z";
    return "oblique";
}

void MeshAnalysis::clear()
{
    topology.clear();
    bvh.clear();
    curvature.clear();
    curvature_signed.clear();
    sharpness.clear();
    bone_distance.clear();
    joint_distance.clear();
    nearest_joint.clear();
    ambient.clear();
    thickness.clear();
    vertex_area.clear();
    tri_curvature.clear();
    tri_area.clear();
    tri_normal.clear();
    edge_dihedral.clear();
    edge_sharp.clear();
    symmetry = SymmetryPlane{};
    stats    = Mesh::Stats{};
}

SymmetryPlane detect_symmetry(const Mesh& mesh, const Bvh& bvh, const AnalysisOptions& opts)
{
    SymmetryPlane best;
    if (mesh.empty() || bvh.empty()) return best;

    const Aabb  box = mesh.bounds();
    const float diag = std::max(box.diagonal(), kEps);
    const Vec3  centre = mesh.centroid_area_weighted();
    const float tolerance = diag * opts.symmetry_tolerance_rel;

    // Sample a deterministic subset of vertices.
    const size_t vcount = mesh.vertex_count();
    const size_t sample_count = std::min<size_t>(vcount, std::max(64, opts.symmetry_samples));
    std::vector<uint32_t> samples(sample_count);
    if (sample_count == vcount) {
        std::iota(samples.begin(), samples.end(), 0u);
    } else {
        // Regular stride keeps the sample spread out over the whole surface.
        const double step = double(vcount) / double(sample_count);
        for (size_t i = 0; i < sample_count; ++i)
            samples[i] = static_cast<uint32_t>(std::min<double>(vcount - 1, i * step));
    }

    const Vec3 candidates[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    for (const Vec3& n : candidates) {
        SymmetryPlane plane;
        plane.normal = n;
        plane.offset = dot(n, centre);

        std::atomic<uint32_t> matched{0};
        std::atomic<uint64_t> error_bits{0};
        double                error_sum = 0.0;
        std::mutex            error_mutex;

        ThreadPool::shared().parallel_ranges(
            samples.size(), 256, [&](size_t begin, size_t end, unsigned) {
                uint32_t local_match = 0;
                double   local_error = 0.0;
                for (size_t i = begin; i < end; ++i) {
                    const Vec3 p  = mesh.positions[samples[i]];
                    const Vec3 mp = plane.mirror(p);
                    const ClosestHit hit = bvh.closest_point(mp, tolerance * 8.0f);
                    const float d = hit.hit() ? std::sqrt(hit.distance2) : tolerance * 8.0f;
                    local_error += d;
                    if (d <= tolerance) ++local_match;
                }
                matched.fetch_add(local_match, std::memory_order_relaxed);
                std::lock_guard lock(error_mutex);
                error_sum += local_error;
            });
        (void)error_bits;

        plane.score      = float(matched.load()) / float(samples.size());
        plane.mean_error = float(error_sum / double(samples.size())) / diag;

        if (plane.score > best.score) best = plane;
    }

    best.accepted = best.score >= opts.symmetry_min_score;
    return best;
}

void analyse_mesh(const Mesh& mesh, MeshAnalysis& out, const AnalysisOptions& opts,
                  const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    out.clear();
    if (mesh.empty()) return;

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

    const size_t vcount = mesh.vertex_count();
    const size_t tcount = mesh.triangle_count();

    report(0.02f, "topology");
    out.topology.build(mesh);

    report(0.12f, "bounding volume hierarchy");
    out.bvh.build(mesh);

    out.stats         = mesh.compute_stats();
    out.bbox_diagonal = std::max(out.stats.bounds.diagonal(), kEps);
    out.mean_edge     = out.stats.mean_edge;

    // --- per triangle ------------------------------------------------------
    report(0.20f, "triangle frames");
    out.tri_area.resize(tcount);
    out.tri_normal.resize(tcount);
    out.tri_curvature.assign(tcount, 0.0f);

    ThreadPool::shared().parallel_ranges(tcount, 1024, [&](size_t b, size_t e, unsigned) {
        for (size_t t = b; t < e; ++t) {
            Vec3 a, bb, c;
            mesh.tri_positions(t, a, bb, c);
            out.tri_area[t]   = rd::triangle_area(a, bb, c);
            out.tri_normal[t] = rd::triangle_normal(a, bb, c);
        }
    });

    // --- per vertex area ---------------------------------------------------
    out.vertex_area.assign(vcount, 0.0f);
    for (size_t t = 0; t < tcount; ++t) {
        const float third = out.tri_area[t] / 3.0f;
        for (int c = 0; c < 3; ++c) out.vertex_area[mesh.indices[t * 3 + c]] += third;
    }

    // --- edges -------------------------------------------------------------
    report(0.30f, "dihedral angles");
    const size_t ecount = out.topology.edges.size();
    out.edge_dihedral.assign(ecount, 0.0f);
    out.edge_sharp.assign(ecount, false);
    const float sharp_limit = opts.sharp_angle_degrees * kDeg2Rad;

    ThreadPool::shared().parallel_ranges(ecount, 2048, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float angle = out.topology.dihedral_angle(mesh, i);
            out.edge_dihedral[i] = angle;
            out.edge_sharp[i]    = std::fabs(angle) >= sharp_limit ||
                                   out.topology.is_boundary_edge(i);
        }
    });

    // --- vertex sharpness --------------------------------------------------
    out.sharpness.assign(vcount, 0.0f);
    for (size_t i = 0; i < ecount; ++i) {
        const auto& edge = out.topology.edges[i];
        const float a = std::fabs(out.edge_dihedral[i]) / kPi;
        out.sharpness[edge.v0] = std::max(out.sharpness[edge.v0], a);
        out.sharpness[edge.v1] = std::max(out.sharpness[edge.v1], a);
    }

    // --- curvature, cotangent Laplacian -----------------------------------
    report(0.40f, "curvature");
    std::vector<Vec3>  laplacian(vcount, Vec3{});
    std::vector<float> mixed_area(vcount, 0.0f);

    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t v[3] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
        const Vec3 p[3] = {mesh.positions[v[0]], mesh.positions[v[1]], mesh.positions[v[2]]};

        for (int c = 0; c < 3; ++c) {
            const int i0 = c, i1 = (c + 1) % 3, i2 = (c + 2) % 3;
            const Vec3 e1 = p[i1] - p[i0];
            const Vec3 e2 = p[i2] - p[i0];
            const float cross_len = length(cross(e1, e2));
            if (cross_len < 1e-12f) continue;
            const float cot = dot(e1, e2) / cross_len;
            // The cotangent at corner i0 weights the opposite edge (i1, i2).
            const Vec3 diff = p[i1] - p[i2];
            laplacian[v[i1]] -= diff * (0.5f * cot);
            laplacian[v[i2]] += diff * (0.5f * cot);
        }
        const float third = out.tri_area[t] / 3.0f;
        for (int c = 0; c < 3; ++c) mixed_area[v[c]] += third;
    }

    out.curvature.assign(vcount, 0.0f);
    out.curvature_signed.assign(vcount, 0.0f);

    std::vector<Vec3> vnormals(vcount, Vec3{});
    if (mesh.has_normals()) {
        vnormals = mesh.normals;
    } else {
        for (size_t t = 0; t < tcount; ++t)
            for (int c = 0; c < 3; ++c)
                vnormals[mesh.indices[t * 3 + c]] += out.tri_normal[t] * out.tri_area[t];
        for (Vec3& n : vnormals) n = normalize(n);
    }

    std::vector<float> raw(vcount, 0.0f);
    for (size_t v = 0; v < vcount; ++v) {
        const float area = std::max(mixed_area[v], 1e-12f);
        const Vec3  H    = laplacian[v] / (2.0f * area);
        const float mag  = length(H);
        raw[v] = mag;
        out.curvature_signed[v] = dot(H, vnormals[v]) >= 0.0f ? mag : -mag;
    }

    // Normalise against a high percentile instead of the max: a single pinched
    // vertex on a sculpt would otherwise squash the entire field to zero.
    const float scale = std::max(percentile_of(raw, opts.curvature_percentile), kEps);
    for (size_t v = 0; v < vcount; ++v) {
        out.curvature[v] = saturate(raw[v] / scale);
        out.curvature_signed[v] = clampf(out.curvature_signed[v] / scale, -1.0f, 1.0f);
        // Hard creases read as high curvature even where the Laplacian is calm.
        out.curvature[v] = std::max(out.curvature[v], out.sharpness[v]);
    }

    for (size_t t = 0; t < tcount; ++t) {
        float c = 0.0f;
        for (int i = 0; i < 3; ++i) c += out.curvature[mesh.indices[t * 3 + i]];
        out.tri_curvature[t] = c / 3.0f;
    }

    // --- armature proximity ------------------------------------------------
    report(0.60f, "armature proximity");
    out.bone_distance.assign(vcount, std::numeric_limits<float>::max());
    out.joint_distance.assign(vcount, std::numeric_limits<float>::max());
    out.nearest_joint.assign(vcount, -1);

    if (!mesh.armature.empty()) {
        const Armature& arm = mesh.armature;
        ThreadPool::shared().parallel_ranges(vcount, 512, [&](size_t b, size_t e, unsigned) {
            for (size_t v = b; v < e; ++v) {
                const Vec3 p = mesh.positions[v];
                float best_bone  = std::numeric_limits<float>::max();
                float best_joint = std::numeric_limits<float>::max();
                int   best_index = -1;

                for (size_t j = 0; j < arm.size(); ++j) {
                    const float dj = length(p - arm.joints[j].bind_position);
                    if (dj < best_joint) { best_joint = dj; best_index = int(j); }

                    Vec3 a, bb;
                    if (arm.bone_segment(j, a, bb)) {
                        const float db = point_segment_distance(p, a, bb);
                        if (db < best_bone) best_bone = db;
                    }
                }
                out.bone_distance[v]  = best_bone == std::numeric_limits<float>::max()
                                            ? best_joint : best_bone;
                out.joint_distance[v] = best_joint;
                out.nearest_joint[v]  = best_index;
            }
        });

        // Per joint influence radius: how far the skin actually reaches.
        out.joint_influence_radius.assign(arm.size(), 0.0f);
        if (mesh.has_skin()) {
            for (size_t v = 0; v < vcount; ++v) {
                const SkinVertex& sv = mesh.skin[v];
                for (int c = 0; c < 4; ++c) {
                    if (sv.weights[c] <= 0.05f) continue;
                    const uint16_t j = sv.joints[c];
                    if (j >= out.joint_influence_radius.size()) continue;
                    const float d = length(mesh.positions[v] - arm.joints[j].bind_position);
                    out.joint_influence_radius[j] = std::max(out.joint_influence_radius[j], d);
                }
            }
        } else {
            for (size_t v = 0; v < vcount; ++v) {
                const int j = out.nearest_joint[v];
                if (j >= 0)
                    out.joint_influence_radius[j] =
                        std::max(out.joint_influence_radius[j], out.joint_distance[v]);
            }
        }
    } else {
        std::fill(out.bone_distance.begin(), out.bone_distance.end(), out.bbox_diagonal);
        std::fill(out.joint_distance.begin(), out.joint_distance.end(), out.bbox_diagonal);
    }

    // --- cheap ambient occlusion proxy ------------------------------------
    out.ambient.assign(vcount, 1.0f);
    if (opts.ambient_rays > 0) {
        report(0.75f, "ambient term");
        const float ray_len = out.bbox_diagonal * opts.ambient_ray_length_rel;
        const int   rays    = opts.ambient_rays;

        ThreadPool::shared().parallel_ranges(vcount, 256, [&](size_t b, size_t e, unsigned) {
            Rng rng(0x5EEDu + static_cast<uint32_t>(b));
            for (size_t v = b; v < e; ++v) {
                const Vec3 n = vnormals[v];
                if (length2(n) < 0.5f) continue;
                Vec3 tangent, bitangent;
                basis_from_normal(n, tangent, bitangent);
                const Vec3 origin = mesh.positions[v] + n * (out.bbox_diagonal * 1e-4f);

                int open = 0;
                for (int r = 0; r < rays; ++r) {
                    const Vec3 local = sample_cosine_hemisphere(rng.next_float(), rng.next_float());
                    const Vec3 dir   = tangent * local.x + bitangent * local.y + n * local.z;
                    if (!out.bvh.occluded(origin, dir, 1e-4f, ray_len)) ++open;
                }
                out.ambient[v] = float(open) / float(rays);
            }
        });
    }

    // --- thickness ---------------------------------------------------------
    // A shape diameter estimate: how far an inward ray travels before it meets
    // the other side. It is what tells a finger from a palm with the same
    // curvature, and what the density field needs to keep a limb round rather
    // than collapsing it to a blade when the edge length outgrows it.
    out.thickness.assign(vcount, out.bbox_diagonal);
    if (opts.thickness_rays > 0) {
        report(0.85f, "thickness");
        const int   rays   = opts.thickness_rays;
        const float offset = out.bbox_diagonal * 1e-4f;
        // 20 degrees: wide enough to step past a single bad triangle, narrow
        // enough that a ray from the inside of an elbow does not find the forearm.
        const float spread = 0.36f;

        ThreadPool::shared().parallel_ranges(vcount, 256, [&](size_t b, size_t e, unsigned) {
            std::vector<float> hits;
            for (size_t v = b; v < e; ++v) {
                const Vec3 n = vnormals[v];
                if (length2(n) < 0.5f) continue;
                Vec3 tangent, bitangent;
                basis_from_normal(n, tangent, bitangent);
                const Vec3 origin = mesh.positions[v] - n * offset;

                hits.clear();
                for (int r = 0; r < rays; ++r) {
                    Vec3 dir = -n;
                    if (r > 0) {
                        const float a = 6.2831853f * float(r - 1) / float(rays - 1);
                        dir = normalize(dir + (tangent * std::cos(a) + bitangent * std::sin(a)) * spread);
                    }
                    const RayHit hit = out.bvh.intersect(origin, dir, offset, out.bbox_diagonal);
                    hits.push_back(hit.hit() ? hit.t : out.bbox_diagonal);
                }
                std::nth_element(hits.begin(), hits.begin() + hits.size() / 2, hits.end());
                out.thickness[v] = hits[hits.size() / 2];
            }
        });
    }

    // --- symmetry ----------------------------------------------------------
    report(0.90f, "symmetry");
    out.symmetry = detect_symmetry(mesh, out.bvh, opts);

    out.seconds = watch.seconds();
    report(1.0f, "done");

    RD_INFO("analysis: %zu vtx / %zu tri, symmetry %s score %.2f%s, %zu shells, %s",
            vcount, tcount, out.symmetry.axis_name(), out.symmetry.score,
            out.symmetry.accepted ? " (accepted)" : " (rejected)",
            out.stats.shells, format_duration(out.seconds).c_str());
    if (!out.stats.manifold)
        RD_WARN("mesh has %zu non manifold edges; hard rules will try to repair",
                out.stats.nonmanifold_edges);
}

} // namespace rd
