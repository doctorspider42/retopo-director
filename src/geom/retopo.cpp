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
        //
        // Creases alone do not make hard surface. A scanned animal trips the
        // angle test all over its fur - the street rat is 18.7% creases, more
        // than a car's body - and sent to the quadric it came back as fans the
        // unwrap cut into 162 charts, 12 px off its outline; the quad field
        // gets it to 2.6 px. What sets a machined object apart is that its
        // creases run between flat panels: 44% of the car's and the trilobite's
        // edges join coplanar faces, 16% of the rat's. The smooth synthetic
        // meshes are flatter still, but have no creases to speak of.
        size_t flat = 0, interior = 0;
        const float flat_cos = std::cos(1.0f * kDeg2Rad);
        for (const MeshTopology::Edge& e : analysis.topology.edges) {
            if (e.tri1 == kInvalidIndex || e.tri1 >= analysis.tri_normal.size()) continue;
            ++interior;
            if (dot(analysis.tri_normal[e.tri0], analysis.tri_normal[e.tri1]) > flat_cos) ++flat;
        }
        const float flat_share = float(double(flat) / double(std::max<size_t>(1, interior)));
        const bool  hard_surface = crease_ratio >= opts.hard_surface_crease_ratio &&
                                   flat_share >= opts.hard_surface_flat_share;
        const bool  organic = skinned || !hard_surface;

        // None of which matters if there is no surface to walk over. The quad
        // field is an isotropic remesh: it needs one closed manifold shell. Give
        // it an asset assembled from separate pieces with open borders - which
        // is every character wearing anything - and it starves, returning a
        // fraction of the budget and leaving whole regions at zero. Measured on
        // four sources against quadric, by mean silhouette error:
        //
        //   one closed shell (subdivided sphere)   0.067 vs 0.120   quad field
        //   costumed character, 65 pieces          0.390 vs 0.102   quadric
        //   creature, assembled                    0.381 vs 0.0003  quadric
        //   building, assembled                    0.546 vs 0.116   quadric
        //
        // The two losses on assembled organic sources were bad enough to fail
        // validation, so the shell test comes first and the crease ratio only
        // decides between them once the surface is actually walkable.
        //
        // What counts is the surface the low poly will actually have. Pieces
        // the hard rules are going to drop - eyeballs, brows, straps lying on
        // the skin - do not make a character "assembled", and a handful of
        // open edges round an eye socket do not make its body unwalkable. On
        // the superhero those two together sent a clean body to the quadric,
        // which is what fanned its hands into slivers.
        size_t kept_shells = 0;
        for (uint32_t s = 0; s < analysis.shell_area_share.size(); ++s)
            if (!source_shell_is_droppable(analysis, s, opts.hard_rules)) ++kept_shells;
        if (analysis.shell_area_share.empty()) kept_shells = analysis.stats.shells;

        size_t main_edges = 0, main_boundary = 0;
        for (const MeshTopology::Edge& e : analysis.topology.edges) {
            if (e.tri0 >= analysis.tri_shell.size() ||
                analysis.tri_shell[e.tri0] != analysis.largest_shell)
                continue;
            ++main_edges;
            if (e.tri1 == kInvalidIndex) ++main_boundary;
        }
        // Half a percent of open edges is a few small holes, not a costume.
        const bool main_closed = analysis.stats.closed ||
                                 (main_edges > 0 && main_boundary * 200 <= main_edges);
        const bool walkable = kept_shells == 1 && main_closed;
        backend = (walkable && organic) ? RetopoBackend::QuadField : RetopoBackend::Quadric;

        RD_INFO("auto backend: %zu shell%s (%zu kept), %s, %.1f%% of edges are creases, "
                "%.0f%% flat: %s",
                analysis.stats.shells, analysis.stats.shells == 1 ? "" : "s", kept_shells,
                analysis.stats.closed ? "watertight"
                                      : (main_closed ? "main piece nearly closed" : "not watertight"),
                crease_ratio * 100.0f, flat_share * 100.0f,
                backend == RetopoBackend::QuadField
                    ? "one walkable shell and organic, using the quad field"
                    : (!walkable ? "assembled from pieces, so the quad field would starve"
                                 : "hard surface, keeping the creases"));
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

    // --- thin tubes -----------------------------------------------------------
    // Before the hard rules, so the mirror and the manifold and winding
    // repairs see the swept tubes like any other surface.
    {
        const bool mirrored = panel.global.enforce_symmetry && profile.require_symmetry;
        const TubeReport tubes = sweep_thin_tubes(result.mesh, source, analysis, seg, mirrored,
                                                  opts.tubes);
        for (const std::string& n : tubes.notes) RD_DEBUG("tubes: %s", n.c_str());
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
