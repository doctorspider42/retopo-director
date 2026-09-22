#pragma once

// Garland-Heckbert edge collapse, driven by the density field rather than by a
// single global target. Every candidate edge is scored as quadric error divided
// by the square of the locally desired edge length, so a region the director
// gave a fat budget resists collapse while a region it wrote off melts away.
//
// meshoptimizer covers the global, region blind case and all of the cache/strip
// work at export time; this exists because per region budgets need per region
// stopping conditions, which a single target_index_count cannot express.

#include "geom/density.h"
#include "knobs/knobs.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <functional>
#include <vector>

namespace rd {

struct QuadricOptions {
    // Reject a collapse that rotates any incident face by more than this.
    float max_normal_flip_degrees = 75.0f;
    // Weight of the virtual planes placed along boundaries, creases and region
    // borders. Higher means those lines survive longer.
    float boundary_weight      = 1000.0f;
    float sharp_weight         = 250.0f;
    float region_border_weight = 400.0f;
    // Creases steeper than this get a constraint plane.
    float sharp_angle_degrees  = 40.0f;
    // Penalty for a collapse that would stretch an edge past the local target.
    float overshoot_penalty    = 4.0f;
    // Stop once every region is at or under budget.
    bool  enforce_region_budgets = true;
    // Absolute floor so a pathological mesh cannot spin forever.
    int   max_collapses = 0;   // 0 = unlimited
    // Keep vertices sitting on the symmetry plane exactly on it.
    bool  lock_symmetry_plane = true;
    float symmetry_epsilon_rel = 2e-3f;
};

struct QuadricResult {
    Mesh   mesh;
    size_t collapses      = 0;
    size_t rejected_flip  = 0;
    size_t rejected_link  = 0;
    size_t blocked_budget = 0;
    size_t sweeps         = 0;
    double seconds        = 0.0;
    // Triangles per region in the result, indexed by region id.
    std::vector<int> region_triangles;
    float  max_error = 0.0f;
};

// `budgets` is indexed by region id and may be shorter than the region list;
// missing entries mean "no limit".
QuadricResult quadric_simplify(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const std::vector<int>& budgets,
                               const SymmetryPlane& symmetry,
                               const QuadricOptions& opts = {},
                               const std::function<void(float, const char*)>& progress = nullptr);

// Convenience wrapper that pulls the budgets straight out of the knob panel.
QuadricResult quadric_simplify(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const KnobPanel& panel, const SymmetryPlane& symmetry,
                               const QuadricOptions& opts = {},
                               const std::function<void(float, const char*)>& progress = nullptr);

} // namespace rd
