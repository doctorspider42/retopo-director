#pragma once

// Quad field retopology.
//
// A coarse quadric pass gets the triangle count into a workable range, then the
// mesh is remeshed incrementally (split / collapse / flip / tangential
// relaxation, reprojected onto the high poly each pass) against edge lengths
// sampled from the density field. The result is isotropic with edge flow that
// follows curvature, which is what a character needs.
//
// Finally adjacent triangles are paired into quads wherever the pair is well
// shaped, and each quad is re-triangulated along its better diagonal. The
// console still eats triangles; the quad step is what makes them line up.

#include "geom/density.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <functional>
#include <vector>

namespace rd {

struct RemeshOptions {
    // Passes of the split/collapse/flip/relax cycle.
    int   iterations = 8;
    // Edges longer than this multiple of the local target get split, shorter
    // than the lower one get collapsed. The classic 4/3 and 4/5 are a good
    // starting point and stable across a wide range of inputs.
    float split_ratio    = 1.333f;
    float collapse_ratio = 0.800f;
    // A collapse that would leave an edge longer than this multiple of the
    // target is refused (0 disables). See the collapse step for why.
    float collapse_max_edge_ratio = 1.333f;
    // Tangential relaxation strength per pass, 0..1.
    float relax_strength = 0.6f;
    int   relax_passes   = 1;
    // Reject a flip or collapse that rotates a face more than this.
    float max_normal_flip_degrees = 70.0f;
    // Start the remesh from a quadric pass at this multiple of the budget.
    float prepass_multiplier = 6.0f;
    // Quad pairing threshold; the knob panel scales it.
    float quad_quality_floor = 0.25f;
    bool  pair_into_quads    = true;
    // Mirror the result across the detected plane.
    bool  enforce_symmetry     = true;
    float symmetry_epsilon_rel = 2e-3f;
    // Keep region boundaries from drifting across the surface.
    bool  preserve_region_borders = true;
    // Final quadric trim so the triangle count lands exactly on budget.
    bool  trim_to_budget = true;
};

struct RemeshResult {
    Mesh   mesh;
    size_t splits    = 0;
    size_t collapses = 0;
    size_t flips     = 0;
    size_t quads     = 0;     // pairs found during the quad step
    float  quad_ratio = 0.0f; // fraction of triangles that ended up in a quad
    double seconds   = 0.0;
    std::vector<int> region_triangles;
};

RemeshResult quad_field_retopo(const Mesh& mesh, const MeshAnalysis& analysis,
                               const Segmentation& seg, const DensityField& density,
                               const KnobPanel& panel, const SymmetryPlane& symmetry,
                               const RemeshOptions& opts = {},
                               const std::function<void(float, const char*)>& progress = nullptr);

// Re-assigns region ids on `target` by projecting each triangle centroid onto
// the source mesh. Used after any operation that rebuilds connectivity.
void transfer_regions(const Mesh& source, const Segmentation& seg, const Bvh& source_bvh,
                      Mesh& target);

// Copies skinning from the high poly onto a retopologised mesh, clamping to the
// influence count the profile allows.
void transfer_skinning(const Mesh& source, const Bvh& source_bvh, Mesh& target,
                       int max_influences);

} // namespace rd
