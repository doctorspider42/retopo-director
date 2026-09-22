#include "geom/density.h"

#include "core/log.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "render/camera.h"

#include <algorithm>
#include <unordered_map>

namespace rd {
namespace {

// Edge length of an equilateral triangle with the given area.
inline float edge_for_area(float area)
{
    // area = sqrt(3)/4 * L^2
    return std::sqrt(std::max(area, 1e-20f) * 4.0f / 1.7320508075688772f);
}

inline float area_for_edge(float edge)
{
    return 0.4330127018922193f * edge * edge;
}

} // namespace

void DensityField::clear()
{
    target_edge.clear();
    importance.clear();
    silhouette.clear();
    vertex_target_edge.clear();
    vertex_importance.clear();
    plans.clear();
    min_edge = max_edge = 0.0f;
}

float DensityField::predicted_triangles() const
{
    float total = 0.0f;
    for (const RegionPlan& p : plans) total += p.predicted;
    return total;
}

void compute_silhouette_importance(const Mesh& mesh, const MeshAnalysis& analysis,
                                   const TargetProfile& profile, std::vector<float>& out)
{
    const size_t tcount = mesh.triangle_count();
    out.assign(tcount, 0.0f);
    if (tcount == 0) return;

    const std::vector<ViewCamera> cameras = build_camera_rig(mesh, profile);
    if (cameras.empty()) return;

    float weight_total = 0.0f;
    for (const ViewCamera& c : cameras) weight_total += c.weight;
    if (weight_total <= kEps) return;

    const float eps = std::max(analysis.bbox_diagonal * 1e-4f, 1e-6f);

    ThreadPool::shared().parallel_ranges(tcount, 512, [&](size_t begin, size_t end, unsigned) {
        for (size_t t = begin; t < end; ++t) {
            const Vec3 centroid = mesh.triangle_centroid(t);
            const Vec3 n        = analysis.tri_normal[t];
            float score = 0.0f;

            for (const ViewCamera& cam : cameras) {
                const Vec3  to_eye = cam.eye - centroid;
                const float dist   = length(to_eye);
                if (dist < kEps) continue;
                const Vec3 dir = to_eye / dist;

                // Facing away entirely: cannot form this view's outline.
                const float facing = dot(n, dir);
                if (facing < -0.15f) continue;

                // Occluded triangles do not draw the outline either.
                if (analysis.bvh.occluded(centroid + n * eps, dir, eps, dist - eps)) continue;

                // Grazing angles are exactly where the silhouette lives.
                const float grazing = 1.0f - saturate(std::fabs(facing));
                score += cam.weight * grazing * grazing;
            }
            out[t] = saturate(score / weight_total * 2.0f);
        }
    });
}

void build_density_field(const Mesh& mesh, const MeshAnalysis& analysis,
                         const Segmentation& seg, const KnobPanel& panel,
                         const TargetProfile& profile, DensityField& out,
                         const DensityOptions& opts)
{
    Stopwatch watch;
    out.clear();

    const size_t tcount = mesh.triangle_count();
    const size_t vcount = mesh.vertex_count();
    if (tcount == 0) return;

    out.target_edge.assign(tcount, 0.0f);
    out.importance.assign(tcount, 1.0f);

    // --- silhouette --------------------------------------------------------
    if (opts.compute_silhouette) {
        compute_silhouette_importance(mesh, analysis, profile, out.silhouette);
    } else {
        out.silhouette.assign(tcount, 0.0f);
    }

    // --- region plans ------------------------------------------------------
    std::unordered_map<uint16_t, size_t> plan_of;
    out.plans.reserve(seg.regions.size());
    for (const Region& r : seg.regions) {
        DensityField::RegionPlan p;
        p.id   = r.id;
        p.area = r.area;

        if (const RegionKnobs* k = panel.find(r.id)) {
            p.budget          = std::max(4, k->triangle_budget);
            p.fidelity        = k->fidelity;
            p.lock_silhouette = k->preserve_silhouette;
        } else {
            // A region the panel never heard of still needs a budget.
            p.budget = std::max(4, int(r.area_share * profile.max_triangles));
        }

        p.base_edge = edge_for_area(p.area / float(std::max(1, p.budget)));
        plan_of[r.id] = out.plans.size();
        out.plans.push_back(p);
    }

    if (out.plans.empty()) {
        // No segmentation: treat the whole mesh as one region.
        DensityField::RegionPlan p;
        p.id     = kNoRegion;
        p.area   = analysis.stats.surface_area;
        p.budget = std::max(4, profile.max_triangles);
        p.base_edge = edge_for_area(p.area / float(p.budget));
        plan_of[kNoRegion] = 0;
        out.plans.push_back(p);
    }

    // --- per triangle modulation ------------------------------------------
    const GlobalKnobs& g = panel.global;
    const float bbox     = std::max(analysis.bbox_diagonal, kEps);
    // Joint proximity is measured relative to the model, not in absolute units.
    const float joint_radius = bbox * 0.08f;

    std::vector<float> scale(tcount, 1.0f);

    ThreadPool::shared().parallel_ranges(tcount, 1024, [&](size_t begin, size_t end, unsigned) {
        for (size_t t = begin; t < end; ++t) {
            const uint16_t region = t < seg.tri_region.size() ? seg.tri_region[t] : kNoRegion;
            const RegionKnobs* k = panel.find(region);

            const float curvature_bias = k ? k->curvature_bias : 0.5f;
            const float curv = analysis.tri_curvature[t];

            // Curvature: high curvature wants smaller triangles.
            const float curv_term =
                1.0f / (1.0f + opts.curvature_strength * g.curvature_influence *
                                   curvature_bias * curv);

            // Joint proximity: deformation needs rings, so tighten near pivots.
            float joint_term = 1.0f;
            if (!mesh.armature.empty() && g.joint_loop_density > 0.0f) {
                float d = std::numeric_limits<float>::max();
                for (int c = 0; c < 3; ++c) {
                    const uint32_t v = mesh.indices[t * 3 + c];
                    if (v < analysis.joint_distance.size())
                        d = std::min(d, analysis.joint_distance[v]);
                }
                if (d < std::numeric_limits<float>::max()) {
                    const float closeness = 1.0f - saturate(d / joint_radius);
                    joint_term = 1.0f / (1.0f + opts.joint_strength * g.joint_loop_density *
                                                    closeness * closeness);
                }
            }

            // Silhouette: outline forming triangles are worth keeping dense.
            float sil_term = 1.0f;
            if (!out.silhouette.empty()) {
                const float protect = (k && !k->preserve_silhouette) ? 0.25f : 1.0f;
                sil_term = 1.0f / (1.0f + opts.silhouette_strength * g.silhouette_weight *
                                              protect * out.silhouette[t]);
            }

            // Cavities nobody sees may be coarser.
            float cavity_term = 1.0f;
            {
                float amb = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    const uint32_t v = mesh.indices[t * 3 + c];
                    amb += v < analysis.ambient.size() ? analysis.ambient[v] : 1.0f;
                }
                amb /= 3.0f;
                cavity_term = lerpf(opts.cavity_relax, 1.0f, saturate(amb));
            }

            // Fidelity decides whether detail is modelled or painted on.
            float fidelity_term = 1.0f;
            if (k) {
                if (k->fidelity == Fidelity::Geometry) fidelity_term = opts.geometry_fidelity_scale;
                else if (k->fidelity == Fidelity::Texture) fidelity_term = opts.texture_fidelity_scale;
            }

            // Merge aggressiveness globally pushes toward coarser results.
            const float merge_term = lerpf(0.9f, 1.25f, g.merge_aggressiveness);

            const float s = curv_term * joint_term * sil_term * cavity_term *
                            fidelity_term * merge_term;
            scale[t] = clampf(s, opts.min_scale, opts.max_scale);

            // Importance for the quadric engine: the inverse intuition.
            const float priority = k ? k->detail_priority : 0.5f;
            out.importance[t] = 1.0f +
                                2.0f * curv * g.curvature_influence +
                                2.5f * (out.silhouette.empty() ? 0.0f : out.silhouette[t]) *
                                    g.silhouette_weight +
                                1.5f * priority;
        }
    });

    // --- calibrate per region so the field lands on budget -----------------
    std::vector<float> region_scale(out.plans.size(), 1.0f);

    for (int pass = 0; pass < std::max(1, opts.calibration_passes); ++pass) {
        std::vector<double> predicted(out.plans.size(), 0.0);

        for (size_t t = 0; t < tcount; ++t) {
            const uint16_t region = t < seg.tri_region.size() ? seg.tri_region[t] : kNoRegion;
            const auto it = plan_of.find(region);
            if (it == plan_of.end()) continue;
            const size_t pi = it->second;
            const float  edge = out.plans[pi].base_edge * scale[t] * region_scale[pi];
            predicted[pi] += double(analysis.tri_area[t]) / double(area_for_edge(edge));
        }

        bool converged = true;
        for (size_t pi = 0; pi < out.plans.size(); ++pi) {
            out.plans[pi].predicted = static_cast<float>(predicted[pi]);
            const double budget = std::max(1, out.plans[pi].budget);
            if (predicted[pi] <= 0.0) continue;
            // Triangle count scales with 1/edge^2, so correct with a sqrt.
            const double correction = std::sqrt(predicted[pi] / budget);
            if (std::fabs(correction - 1.0) > 0.01) converged = false;
            region_scale[pi] *= static_cast<float>(clampf(float(correction), 0.5f, 2.0f));
        }
        if (converged) break;
    }

    // --- final per triangle edge lengths -----------------------------------
    float min_edge = std::numeric_limits<float>::max();
    float max_edge = 0.0f;

    for (size_t t = 0; t < tcount; ++t) {
        const uint16_t region = t < seg.tri_region.size() ? seg.tri_region[t] : kNoRegion;
        const auto it = plan_of.find(region);
        const size_t pi = it == plan_of.end() ? 0 : it->second;
        const float edge = std::max(out.plans[pi].base_edge * scale[t] * region_scale[pi],
                                    bbox * 1e-4f);
        out.target_edge[t] = edge;
        min_edge = std::min(min_edge, edge);
        max_edge = std::max(max_edge, edge);
    }
    out.min_edge = min_edge == std::numeric_limits<float>::max() ? 0.0f : min_edge;
    out.max_edge = max_edge;

    // --- per vertex averages ----------------------------------------------
    out.vertex_target_edge.assign(vcount, 0.0f);
    out.vertex_importance.assign(vcount, 0.0f);
    std::vector<float> weight(vcount, 0.0f);

    for (size_t t = 0; t < tcount; ++t) {
        const float a = std::max(analysis.tri_area[t], 1e-12f);
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = mesh.indices[t * 3 + c];
            out.vertex_target_edge[v] += out.target_edge[t] * a;
            out.vertex_importance[v]  += out.importance[t] * a;
            weight[v] += a;
        }
    }
    for (size_t v = 0; v < vcount; ++v) {
        if (weight[v] > 0.0f) {
            out.vertex_target_edge[v] /= weight[v];
            out.vertex_importance[v]  /= weight[v];
        } else {
            out.vertex_target_edge[v] = out.max_edge;
            out.vertex_importance[v]  = 1.0f;
        }
    }

    RD_INFO("density field: %zu regions, edge %.5f .. %.5f, predicts %.0f tri "
            "(budget %d) in %s",
            out.plans.size(), out.min_edge, out.max_edge, out.predicted_triangles(),
            panel.total_budget(), format_duration(watch.seconds()).c_str());
}

} // namespace rd
