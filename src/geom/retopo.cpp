#include "geom/retopo.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

namespace rd {

std::vector<RetopoResult::BudgetMiss> RetopoResult::budget_misses(const Segmentation& seg,
                                                                  float tolerance) const
{
    std::vector<BudgetMiss> out;
    for (const Region& r : seg.regions) {
        if (r.id >= region_budgets.size()) continue;
        const int budget = region_budgets[r.id];
        if (budget <= 0 || budget == std::numeric_limits<int>::max()) continue;
        const int actual = r.id < region_triangles.size() ? region_triangles[r.id] : 0;
        const float miss = std::fabs(float(actual - budget)) / float(std::max(1, budget));
        if (miss > tolerance) out.push_back({r.id, r.name, budget, actual});
    }
    std::sort(out.begin(), out.end(), [](const BudgetMiss& a, const BudgetMiss& b) {
        return std::abs(a.actual - a.budget) > std::abs(b.actual - b.budget);
    });
    return out;
}

RetopoResult run_retopo(const Mesh& source, const MeshAnalysis& analysis,
                        const Segmentation& seg, const DensityField& density,
                        const KnobPanel& panel, const TargetProfile& profile,
                        const RetopoOptions& opts,
                        const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    RetopoResult result;

    if (source.empty()) {
        result.error = "no source mesh";
        return result;
    }
    if (!density.valid()) {
        result.error = "density field has not been built";
        return result;
    }

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

    // --- backend choice -----------------------------------------------------
    RetopoBackend backend = opts.forced_backend_valid ? opts.forced_backend
                                                      : panel.global.backend;
    if (backend == RetopoBackend::Auto) {
        // How much of this model is hard creases rather than smooth surface.
        size_t sharp = 0;
        for (size_t e = 0; e < analysis.edge_sharp.size(); ++e)
            if (analysis.edge_sharp[e]) ++sharp;
        const float crease_ratio =
            analysis.edge_sharp.empty()
                ? 0.0f
                : float(double(sharp) / double(analysis.edge_sharp.size()));

        const bool skinned = source.has_skin() && !source.armature.empty();

        // The quad field wins on anything organic, and not only because the edge
        // flow is nicer to look at: its regular topology unwraps into a handful
        // of large charts, while the fans a quadric collapse leaves behind
        // shatter the atlas into dozens of small ones. Every extra chart border
        // duplicates its vertices, and on a console budget the vertex count is
        // usually the limit that actually binds.
        //
        // Quadric still wins on hard surface, where those creases are the shape
        // and an isotropic remesh would round them off.
        const bool organic = skinned || crease_ratio < opts.hard_surface_crease_ratio;
        backend = organic ? RetopoBackend::QuadField : RetopoBackend::Quadric;

        RD_INFO("auto backend: %.1f%% of edges are creases, treating this as %s",
                crease_ratio * 100.0f, organic ? "organic" : "hard surface");
    }
    result.backend_used = backend;
    RD_INFO("retopo backend: %s", backend_name(backend));

    // --- budgets ------------------------------------------------------------
    uint16_t max_region = 0;
    for (const Region& r : seg.regions) max_region = std::max(max_region, r.id);
    result.region_budgets.assign(size_t(max_region) + 1, 0);
    for (const RegionKnobs& k : panel.regions)
        if (k.id < result.region_budgets.size())
            result.region_budgets[k.id] = std::max(2, k.triangle_budget);

    // --- run the backend ----------------------------------------------------
    if (backend == RetopoBackend::QuadField) {
        RemeshOptions ropts = opts.remesh;
        ropts.enforce_symmetry = panel.global.enforce_symmetry && profile.require_symmetry;
        ropts.relax_passes     = std::max(1, panel.global.smoothing_iterations / 3);
        ropts.iterations       = std::clamp(4 + panel.global.smoothing_iterations, 4, 24);

        RemeshResult r = quad_field_retopo(
            source, analysis, seg, density, panel, analysis.symmetry, ropts,
            [&](float f, const char* what) { report(0.05f + 0.65f * f, what); });

        result.mesh       = std::move(r.mesh);
        result.splits     = r.splits;
        result.collapses  = r.collapses;
        result.flips      = r.flips;
        result.quads      = r.quads;
        result.quad_ratio = r.quad_ratio;
    } else {
        QuadricOptions qopts = opts.quadric;
        qopts.lock_symmetry_plane = panel.global.enforce_symmetry && profile.require_symmetry;

        QuadricResult r = quadric_simplify(
            source, analysis, seg, density, panel, analysis.symmetry, qopts,
            [&](float f, const char* what) { report(0.05f + 0.65f * f, what); });

        result.mesh      = std::move(r.mesh);
        result.collapses = r.collapses;
    }

    if (result.mesh.empty()) {
        result.error = "the retopo backend produced an empty mesh";
        return result;
    }

    // --- hard rules ---------------------------------------------------------
    report(0.72f, "hard rules");
    HardRuleOptions hopts = opts.hard_rules;
    result.hard_rules = apply_hard_rules(result.mesh, source, analysis.bvh, analysis,
                                         profile, panel.global, analysis.symmetry, hopts);

    // Mirroring rebuilds connectivity, so region ids have to be re-derived.
    report(0.86f, "transferring regions");
    transfer_regions(source, seg, analysis.bvh, result.mesh);

    // --- skinning -----------------------------------------------------------
    if (source.has_skin() && profile.max_bone_influences > 0) {
        report(0.92f, "transferring skin weights");
        transfer_skinning(source, analysis.bvh, result.mesh, profile.max_bone_influences);
    }

    // --- tally --------------------------------------------------------------
    result.region_triangles.assign(size_t(max_region) + 1, 0);
    for (uint16_t r : result.mesh.tri_region)
        if (r < result.region_triangles.size()) ++result.region_triangles[r];

    result.seconds = watch.seconds();
    result.ok      = true;
    report(1.0f, "done");

    RD_INFO("retopo finished: %zu tri / %zu vtx via %s in %s",
            result.mesh.triangle_count(), result.mesh.vertex_count(),
            backend_name(backend), format_duration(result.seconds).c_str());
    return result;
}

} // namespace rd
