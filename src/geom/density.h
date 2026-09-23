#pragma once

// The density field is the translation layer between the knob panel and the
// geometry engines. Knobs go in, a per triangle target edge length and a per
// triangle importance weight come out. Neither retopo backend knows anything
// about regions, budgets or art direction - they only read this field.

#include "core/math.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <vector>

namespace rd {

struct DensityField {
    // Desired edge length at each triangle of the source mesh, in model units.
    std::vector<float> target_edge;
    // How badly we want to keep this triangle, 1 = ordinary. Feeds the quadric
    // error scaling and the remesher's projection weights.
    std::vector<float> importance;
    // 0..1, how much this triangle contributes to an outline in the profile
    // cameras. Computed once per mesh and reused across iterations.
    std::vector<float> silhouette;

    // Per vertex versions, averaged from the triangles, for the remesher.
    std::vector<float> vertex_target_edge;
    std::vector<float> vertex_importance;

    // Per region bookkeeping, mirrored from the knob panel for convenience.
    struct RegionPlan {
        uint16_t id           = 0;
        int      budget       = 0;
        float    area         = 0.0f;
        float    base_edge    = 0.0f;   // before local modulation
        float    predicted    = 0.0f;   // triangles the field is expected to yield
        Fidelity fidelity     = Fidelity::Balanced;
        bool     lock_silhouette = true;
    };
    std::vector<RegionPlan> plans;

    float min_edge = 0.0f;
    float max_edge = 0.0f;

    bool  valid() const { return !target_edge.empty(); }
    void  clear();

    // Triangle count the field predicts overall. Useful as a sanity check
    // before the engine spends thirty seconds proving it wrong.
    float predicted_triangles() const;
};

struct DensityOptions {
    // How strongly curvature shrinks the target edge at its maximum.
    float curvature_strength = 2.2f;
    // How strongly joint proximity shrinks it.
    float joint_strength     = 1.8f;
    // How strongly the silhouette term shrinks it.
    float silhouette_strength = 1.4f;
    // Crevices nobody will see are allowed to get coarser by this factor.
    float cavity_relax        = 1.25f;
    // Fidelity multipliers.
    float geometry_fidelity_scale = 0.82f;
    float texture_fidelity_scale  = 1.35f;
    // Clamp the local modulation so one term cannot dominate everything.
    float min_scale = 0.35f;
    float max_scale = 2.60f;
    // Calibration passes that rescale each region so it actually lands on
    // budget rather than near it.
    int   calibration_passes = 4;
    // Thin features: where the edge length would outgrow the local thickness a
    // limb collapses to a blade and a finger disappears, so thin parts take a
    // smaller edge than broad parts of the same region. An edge of half the
    // thickness puts about six segments around a round cross section, the
    // fewest that still reads as round from a game camera.
    float thin_edge_ratio = 0.5f;
    float thin_strength   = 1.0f;
    // Below this share of the bbox diagonal a "thickness" is two layers of a
    // double sided sheet (a cloak, a hair card), not a volume, and is ignored:
    // honouring it would pour the whole budget into a flat plane.
    float thin_sheet_cutoff_rel = 0.0025f;
    // And no feature is protected below this fraction of the edge the whole
    // budget would give an evenly spread mesh. A whisker or the last inch of a
    // tail is thinner than anything the budget can resolve; honouring it made
    // the rat's whiskers a few thousand triangles too small for the weld to
    // keep, and the rest of the rat paid for them.
    float thin_floor_of_mean_edge = 0.15f;
    // Rays used for the silhouette visibility test.
    bool  compute_silhouette = true;
};

// Builds the field. `seg` must be in sync with `mesh`; `panel` must already
// have resolved triangle budgets (call KnobPanel::resolve_budgets first).
void build_density_field(const Mesh& mesh, const MeshAnalysis& analysis,
                         const Segmentation& seg, const KnobPanel& panel,
                         const TargetProfile& profile, DensityField& out,
                         const DensityOptions& opts = {});

// Standalone silhouette importance, also used by the metrics stage.
void compute_silhouette_importance(const Mesh& mesh, const MeshAnalysis& analysis,
                                   const TargetProfile& profile,
                                   std::vector<float>& out);

} // namespace rd
