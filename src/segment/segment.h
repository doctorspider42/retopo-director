#pragma once

// Region decomposition of the high poly.
//
// The split itself is deterministic: a multi source Dijkstra over the dual
// graph whose edge weights punish crossing hard creases and bone boundaries,
// seeded by farthest point sampling (biased toward joints when there is an
// armature). The model never draws these boundaries; it only names the patches
// and tells us which ones are really one part.

#include "core/math.h"
#include "mesh/analysis.h"
#include "mesh/mesh.h"

#include <functional>
#include <string>
#include <vector>

namespace rd {

struct Region {
    uint16_t    id = 0;
    std::string name;        // set by the model, or auto generated
    std::string auto_label;  // what the segmenter could work out on its own

    uint32_t triangle_count = 0;
    float    area           = 0.0f;
    float    area_share     = 0.0f;   // of total surface area
    Vec3     centroid;
    Aabb     bounds;

    float mean_curvature = 0.0f;
    float max_curvature  = 0.0f;
    float mean_ambient   = 1.0f;

    int   dominant_joint     = -1;
    float joint_weight_share = 0.0f;  // how cleanly it maps to that joint
    // Length over diameter of the thin tube this region is the tip of - a
    // tail, an antenna, a horn - or 0. Measured from the tip up, across
    // whatever regions the tube runs through (sweep_thin_tubes, tubes.h).
    float tube_aspect = 0.0f;

    // Fraction of rendered pixels this region covers across all cameras,
    // weighted by the primary flag. Filled in by the render stage.
    float visibility = 0.0f;

    std::vector<uint16_t> neighbours;

    Vec3 debug_color;
};

struct Segmentation {
    std::vector<uint16_t> tri_region;   // one entry per triangle
    std::vector<Region>   regions;
    double                seconds = 0.0;

    bool   valid() const { return !regions.empty() && !tri_region.empty(); }
    size_t size()  const { return regions.size(); }
    void   clear();

    Region*       find(uint16_t id);
    const Region* find(uint16_t id) const;

    // Index into `regions` for a region id, or SIZE_MAX.
    size_t index_of(uint16_t id) const;

    // Recomputes area, curvature, bounds, neighbours and colours from scratch.
    void refresh_statistics(const Mesh& mesh, const MeshAnalysis& analysis);

    std::vector<uint16_t>    ids() const;
    std::vector<std::string> names() const;
    std::vector<float>       area_shares() const;
};

struct SegmentationOptions {
    // Upper bound on patches handed to the model. More than about 60 and the
    // naming step stops being useful.
    int   target_regions = 28;
    // Weight of the normal deviation term when growing across an edge.
    float normal_weight  = 2.5f;
    // Extra cost for crossing an edge flagged sharp by the analysis.
    float sharp_penalty  = 6.0f;
    // Extra cost for crossing between triangles skinned to different joints.
    float joint_penalty  = 4.0f;
    bool  use_armature   = true;
    // Regions below this share of the surface get merged into a neighbour.
    float min_area_share = 0.004f;
    // Bias the first seeds toward joint positions.
    bool  seed_from_joints = true;
    uint32_t random_seed    = 0x5EED1337u;
};

void segment_mesh(const Mesh& mesh, const MeshAnalysis& analysis, Segmentation& out,
                  const SegmentationOptions& opts = {},
                  const std::function<void(float, const char*)>& progress = nullptr);

// --- pieces of the split, reusable by an alternative segmenter --------------
//
// A segmenter that labels triangles some other way (SAM, a painted mask, an
// imported vertex group) only has to produce a `tri_region` array and can then
// borrow the two steps below, which is what keeps the two paths comparable:
// whatever labels the triangles, the regions that come out have been through
// the same fill, the same sliver absorption and the same statistics.

// Grows the labelled triangles over the ones still marked `kNoRegion`, using
// the same dual graph cost as the geometric split. Triangles in a shell that
// carries no label at all stay unlabelled.
void grow_unassigned_regions(const Mesh& mesh, const MeshAnalysis& analysis,
                             const SegmentationOptions& opts,
                             std::vector<uint16_t>& tri_region);

// Turns a raw `tri_region` array into a finished Segmentation: dense ids,
// sliver absorption, statistics. `out.tri_region` must already be filled.
void finalize_segmentation(const Mesh& mesh, const MeshAnalysis& analysis,
                           const SegmentationOptions& opts, Segmentation& out,
                           const char* name_prefix = "region");

// Applies a grouping decided by the model: every inner vector is a set of
// existing region ids that should become one region with the given name.
// Ids not mentioned are left alone. Returns the number of merges performed.
struct MergeGroup {
    std::string           name;
    std::string           role;
    std::vector<uint16_t> members;
};

int merge_regions(Segmentation& seg, const Mesh& mesh, const MeshAnalysis& analysis,
                  const std::vector<MergeGroup>& groups);

// Per triangle colours for the region overlay render.
void region_colors(const Segmentation& seg, std::vector<Vec4>& tri_colors);

// A compact table of the regions, formatted for the prompt.
std::string region_table_text(const Segmentation& seg, const Mesh& mesh);

} // namespace rd
