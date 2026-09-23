#pragma once

// Constraints the director does not get a vote on.
//
// These run after whichever retopo backend produced the mesh, and they run
// unconditionally. If the model asked for something that breaks one of them,
// the rule wins and the report says so.

#include "core/math.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "mesh/analysis.h"
#include "mesh/bvh.h"
#include "mesh/mesh.h"

#include <string>
#include <vector>

namespace rd {

struct HardRuleReport {
    size_t mirrored_triangles   = 0;
    size_t clipped_triangles    = 0;
    size_t welded_vertices      = 0;
    size_t removed_degenerate   = 0;
    size_t removed_nonmanifold  = 0;
    size_t removed_shells       = 0;
    size_t joint_splits         = 0;
    size_t reversed_triangles   = 0;
    bool   symmetry_applied     = false;
    double seconds              = 0.0;
    std::vector<std::string> messages;

    void note(std::string msg) { messages.push_back(std::move(msg)); }
};

struct HardRuleOptions {
    float symmetry_epsilon_rel = 2.5e-3f;
    // Shells smaller than this share of the total area are dropped when the
    // profile caps the shell count. Debris only: at 2% it stopped admitting at
    // the first small piece, so a creature with 36 shells against a limit of
    // 32 lost every one of its legs - each under 2% - rather than its four
    // smallest scraps.
    float min_shell_area_share = 0.002f;
    // Share of the mesh's triangles the shells other than the largest may hold
    // between them; 0 leaves it unbounded. Off by default, because an
    // asset made mostly of pieces - a creature whose carapace and legs are all
    // separate - is destroyed by it. The budget re-fit turns it on when it
    // stalls: the costumed ranger's 32 kept pieces put a floor of 2150
    // triangles under a 1400 budget that no smaller budget could move.
    float secondary_shell_budget_share = 0.0f;
    // Upper bound on triangles added by the joint loop rule, as a fraction of
    // the profile budget.
    float joint_split_headroom = 0.08f;
    bool  enforce_symmetry     = true;
    bool  enforce_manifold     = true;
    bool  enforce_joint_loops  = true;
    bool  reproject            = true;
    // Drop small pieces of the low poly whose source is mostly out of sight:
    // an eyeball whose back two thirds sit inside the skull. Built down to a
    // handful of triangles it becomes a polyhedron that pokes through the
    // simplified eyelids, which is how the eyes came out as grey shards. The
    // high poly keeps it, so the bake still paints the eye onto the face.
    //
    // Small pieces lying on the main surface go the same way: an eyebrow card,
    // a strap, a patch. A few millimetres of relief the silhouette cannot
    // show, rebuilt as a closed solid of eight triangles floating in front of
    // a simplified brow - the other half of the "grey shards" on the face.
    // Painted by the bake they cost nothing and sit exactly where they were.
    bool  drop_hidden_shells     = true;
    float hidden_visible_share   = 0.5f;    // of the piece's own area
    float hidden_max_area_share  = 0.02f;   // of the whole surface
    float decal_max_offset_rel   = 0.006f;  // of the bbox diagonal: ~1 cm on a character
    // Passes of fit_to_surface. 0 leaves the vertices where the backend put them.
    int   fit_surface_passes   = 4;
};

// `source` and `source_bvh` are the high poly, used to re-project any vertex
// this pass creates or moves.
HardRuleReport apply_hard_rules(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                                const MeshAnalysis& analysis, const TargetProfile& profile,
                                const GlobalKnobs& knobs, const SymmetryPlane& symmetry,
                                const HardRuleOptions& opts = {});

// Individual rules, exposed so the UI can run them one at a time.
size_t enforce_symmetry(Mesh& mesh, const SymmetryPlane& plane, float epsilon,
                        const Bvh* reproject_onto, size_t* clipped_out = nullptr);
size_t repair_nonmanifold(Mesh& mesh);
// `mirror` makes the cull symmetry aware: mirrored shells are admitted or
// dropped as a pair, never one without the other. Pass null when the asset is
// not being kept symmetric.
//
// `secondary_triangle_cap` bounds the triangles every shell but the largest may
// hold between them; 0 means no bound. A closed piece cannot be simplified
// below a handful of triangles, so a costume of dozens of trinkets has a floor
// the simplifier cannot go under, and past the cap the body would starve.
size_t limit_shells(Mesh& mesh, int max_shells, float min_area_share,
                    const SymmetryPlane* mirror = nullptr, int secondary_triangle_cap = 0);
size_t insert_joint_loops(Mesh& mesh, const MeshAnalysis& analysis, const Bvh& source_bvh,
                          float density, int max_new_triangles);

// Makes the whole surface face one way: orientation is propagated across
// shared edges, then each shell gets a single global sign from its signed
// volume (or, for an open shell, a majority vote against the high poly).
// Mirroring reverses handedness and reprojection can turn a thin triangle
// inside out; an inverted face bakes to black because its sampling
// hemisphere points into the model. Returns how many were flipped.
size_t fix_winding(Mesh& mesh, const Bvh& source_bvh);

// Whether a piece of the source is one drop_hidden_shells would take out of
// the low poly: small, and either mostly out of sight or lying on the main
// surface. The backend choice asks the same question, so the two agree on
// which pieces the retopology really has to build.
bool source_shell_is_droppable(const MeshAnalysis& analysis, uint32_t shell,
                               const HardRuleOptions& opts);

// See HardRuleOptions::drop_hidden_shells. Returns the triangles removed.
size_t drop_hidden_shells(Mesh& mesh, const Bvh& source_bvh, const MeshAnalysis& analysis,
                          float max_visible_share, float max_area_share,
                          float decal_max_offset_rel);

// Moves vertices along their normals until the low poly's faces, not just its
// vertices, sit on the high poly on average. A mesh whose vertices all lie on
// a convex surface has every face cutting beneath it, so a retopology that
// only snaps vertices comes out uniformly smaller than its source: thin limbs,
// a narrower silhouette in every view. `symmetry` keeps a mirrored mesh
// exactly mirrored; pass null when it is not. Returns the mean absolute
// offset of the last pass, in model units.
float fit_to_surface(Mesh& mesh, const Bvh& source_bvh, int passes,
                     const SymmetryPlane* symmetry, float symmetry_epsilon);

} // namespace rd
