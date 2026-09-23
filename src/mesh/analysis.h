#pragma once

// Everything the director needs to know about the high poly before any budget
// decision is made. Computed once per loaded mesh, then read by the segmenter,
// the density field and the validator.

#include "core/math.h"
#include "mesh/bvh.h"
#include "mesh/mesh.h"
#include "mesh/topology.h"

#include <functional>
#include <string>
#include <vector>

namespace rd {

struct SymmetryPlane {
    Vec3  normal{1.0f, 0.0f, 0.0f};
    float offset = 0.0f;        // plane is dot(normal, p) == offset
    float score  = 0.0f;        // 0..1, fraction of samples that mirror onto the surface
    float mean_error = 0.0f;    // average mirror distance, relative to bbox diagonal
    bool  accepted = false;

    Vec3 mirror(Vec3 p) const { return p - normal * (2.0f * (dot(normal, p) - offset)); }
    const char* axis_name() const;
};

struct MeshAnalysis {
    // --- inputs kept for downstream stages ---------------------------------
    MeshTopology topology;
    Bvh          bvh;

    // --- per vertex --------------------------------------------------------
    std::vector<float> curvature;        // |mean curvature|, normalised 0..1
    std::vector<float> curvature_signed; // signed mean curvature, -1..1
    std::vector<float> sharpness;        // max |dihedral| of incident edges, 0..1
    std::vector<float> bone_distance;    // to nearest bone segment, model units
    std::vector<float> joint_distance;   // to nearest joint pivot, model units
    std::vector<int>   nearest_joint;    // -1 when there is no armature
    std::vector<float> ambient;          // cheap AO proxy, 0 = crevice, 1 = exposed
    // Distance through the model to the opposite wall, model units: the
    // diameter of a finger, the depth of an ear. bbox_diagonal where the inward
    // ray leaves the model without hitting anything (an open sheet).
    std::vector<float> thickness;
    std::vector<float> vertex_area;      // one third of incident triangle area

    // --- per triangle ------------------------------------------------------
    std::vector<float> tri_curvature;
    std::vector<float> tri_area;
    std::vector<Vec3>  tri_normal;

    // --- per edge ----------------------------------------------------------
    std::vector<float> edge_dihedral;    // radians, signed
    std::vector<bool>  edge_sharp;

    // --- per joint ---------------------------------------------------------
    // How far the skin reaches from each pivot. The hard rules use it to size
    // the deformation loops, so it lives here rather than mutating the mesh.
    std::vector<float> joint_influence_radius;

    // --- global ------------------------------------------------------------
    SymmetryPlane symmetry;
    Mesh::Stats   stats;
    float         bbox_diagonal = 1.0f;
    float         mean_edge     = 0.0f;
    double        seconds       = 0.0;

    bool valid() const { return !curvature.empty(); }
    void clear();
};

struct AnalysisOptions {
    // Dihedral angle above which an edge counts as a hard crease.
    float sharp_angle_degrees = 35.0f;
    // Symmetry acceptance: minimum fraction of samples that must mirror onto
    // the surface within `symmetry_tolerance_rel` of the bbox diagonal.
    float symmetry_min_score      = 0.90f;
    float symmetry_tolerance_rel  = 0.01f;
    int   symmetry_samples        = 4096;
    // Cheap ambient term: rays per vertex. 0 disables it.
    int   ambient_rays            = 24;
    float ambient_ray_length_rel  = 0.25f;
    // Rays per vertex for the thickness estimate, in a narrow cone around the
    // inward normal; the median is kept so one ray down a crevice does not
    // decide it. 0 disables it.
    int   thickness_rays          = 5;
    // Curvature is normalised against this percentile so a handful of spikes
    // does not flatten the whole field.
    float curvature_percentile    = 0.97f;
};

// Runs the full analysis. `progress` is called with 0..1 and may be null.
void analyse_mesh(const Mesh& mesh, MeshAnalysis& out,
                  const AnalysisOptions& opts = {},
                  const std::function<void(float, const char*)>& progress = nullptr);

// Detects the dominant mirror plane on its own (used by the UI when the user
// wants to re-test after editing the mesh).
SymmetryPlane detect_symmetry(const Mesh& mesh, const Bvh& bvh,
                              const AnalysisOptions& opts);

} // namespace rd
