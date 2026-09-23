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
    // profile caps the shell count.
    float min_shell_area_share = 0.02f;
    // Upper bound on triangles added by the joint loop rule, as a fraction of
    // the profile budget.
    float joint_split_headroom = 0.08f;
    bool  enforce_symmetry     = true;
    bool  enforce_manifold     = true;
    bool  enforce_joint_loops  = true;
    bool  reproject            = true;
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
size_t limit_shells(Mesh& mesh, int max_shells, float min_area_share);
size_t insert_joint_loops(Mesh& mesh, const MeshAnalysis& analysis, const Bvh& source_bvh,
                          float density, int max_new_triangles);

// Makes the whole surface face one way: orientation is propagated across
// shared edges, then each shell gets a single global sign from its signed
// volume (or, for an open shell, a majority vote against the high poly).
// Mirroring reverses handedness and reprojection can turn a thin triangle
// inside out; an inverted face bakes to black because its sampling
// hemisphere points into the model. Returns how many were flipped.
size_t fix_winding(Mesh& mesh, const Bvh& source_bvh);

} // namespace rd
