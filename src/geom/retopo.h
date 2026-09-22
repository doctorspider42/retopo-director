#pragma once

// Front door to the geometry engine. Picks a backend, runs it, applies the hard
// rules, transfers skinning, and reports what actually happened so the director
// can be told the truth rather than what it asked for.

#include "geom/density.h"
#include "geom/hard_rules.h"
#include "geom/quadric.h"
#include "geom/remesh.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <functional>
#include <string>
#include <vector>

namespace rd {

struct RetopoResult {
    Mesh           mesh;
    RetopoBackend  backend_used = RetopoBackend::Auto;
    HardRuleReport hard_rules;

    size_t collapses = 0;
    size_t splits    = 0;
    size_t flips     = 0;
    size_t quads     = 0;
    float  quad_ratio = 0.0f;

    // Triangles per region id in the finished mesh, next to what was asked for.
    std::vector<int> region_triangles;
    std::vector<int> region_budgets;

    double seconds = 0.0;
    bool   ok      = false;
    std::string error;

    // Regions that missed their budget by more than `tolerance` (fractional).
    struct BudgetMiss {
        uint16_t    id;
        std::string name;
        int         budget;
        int         actual;
    };
    std::vector<BudgetMiss> budget_misses(const Segmentation& seg, float tolerance = 0.25f) const;
};

struct RetopoOptions {
    QuadricOptions  quadric;
    RemeshOptions   remesh;
    HardRuleOptions hard_rules;
    // Auto backend rule: anything skinned, or with fewer creases than this share
    // of its edges, is treated as organic and goes through the quad field. The
    // knob panel and the command line can both override it.
    bool  prefer_quad_for_skinned   = true;
    float hard_surface_crease_ratio = 0.18f;
    // Overrides the panel entirely when set, so the two paths can be compared
    // on one mesh without editing knobs.
    bool          forced_backend_valid = false;
    RetopoBackend forced_backend       = RetopoBackend::Auto;
};

RetopoResult run_retopo(const Mesh& source, const MeshAnalysis& analysis,
                        const Segmentation& seg, const DensityField& density,
                        const KnobPanel& panel, const TargetProfile& profile,
                        const RetopoOptions& opts = {},
                        const std::function<void(float, const char*)>& progress = nullptr);

} // namespace rd
