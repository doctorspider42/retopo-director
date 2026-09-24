#pragma once

// A uv layout cut the way a person would cut it: one chart per part of the
// model, opened along its least visible side.
//
// xatlas grows charts by surface cost and then splits wherever a chart would
// fold. On a closed low-poly mesh the folding decides everything - its cost
// settings change nothing - and a 350 triangle prop comes back as fifty charts
// whose seams are more than half of its vertex count, laid out in no order a
// texture artist could follow. Here the segmentation's regions are the charts:
// a tube is cut once along its underside, a part with holes is cut from hole
// to hole, and a chart is only split further when it would fold or stretch.

#include "mesh/mesh.h"

#include <string>
#include <vector>

namespace rd {

struct PartsUnwrapOptions {
    // A chart is split when its texel density (uv area over surface area)
    // varies by more than this factor across 96% of its surface - the measure
    // and the limit the validator's texture.stretch uses - or when any fold.
    float max_density_ratio = 4.0f;
    // No triangle's density may be further than this from its chart's mean.
    float max_triangle_density = 6.0f;
    // Charts smaller than this join the neighbour they share most border with.
    int   min_chart_triangles = 8;
    // How much a seam avoids visible surface: an edge costs its length times
    // (1 + weight * visibility), visibility 0 for a crevice or an underside.
    float seam_visibility_weight = 4.0f;
};

struct PartsUnwrapResult {
    bool        ok = false;
    int         charts = 0;
    int         splits = 0;       // charts split again because they folded or stretched
    int         merges = 0;       // neighbouring charts joined afterwards
    std::vector<uint32_t> tri_chart;   // per triangle of the rebuilt mesh
    std::string error;
};

// Rebuilds `mesh` with a vertex per chart corner and uvs laid out per chart at
// one texel density (uv area equal to surface area), not yet packed.
// `visibility` is per vertex of the input, 0 hidden .. 1 exposed; empty means
// all equally visible. Triangle order is kept.
PartsUnwrapResult unwrap_by_parts(Mesh& mesh, const std::vector<float>& visibility,
                                  const PartsUnwrapOptions& opts);

} // namespace rd
