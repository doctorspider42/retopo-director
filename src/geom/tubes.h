#pragma once

// Thin tubes - tails, antennae, whiskers of geometry - swept as prisms.
//
// The quad field is isotropic: it spends the same edge length round a tube as
// along it. A tail whose circumference holds fewer than four or five of the
// region's edges cannot be a tube at all under that rule, and comes back as a
// ribbon twisting between two or three vertices per slice, with notches where
// the twists pass through edge-on that read as holes. What such a tube can
// afford is a few sides and rings spaced along it, which is exactly what a
// hand modeller builds, so that is what this does: it finds the thin end of a
// region, cuts it off the low poly and sweeps a prism along the source's
// centreline in its place, spending the same triangles.

#include "mesh/analysis.h"
#include "mesh/mesh.h"
#include "segment/segment.h"

#include <string>
#include <vector>

namespace rd {

struct TubeOptions {
    bool  enabled = true;
    // Swept when fewer than this many of the low poly's edges fit round the
    // tube. Above it the remesher can already hold the tube open.
    float max_edges_around = 4.5f;
    // And only when it is at least this many diameters long; a stub is a bump.
    float min_length_diameters = 4.0f;
    // Rings a swept tube gets even when the triangles it replaces are fewer.
    int   min_rings = 3;
};

struct TubeReport {
    int                      swept = 0;
    std::vector<std::string> notes;
};

// Fills Region::tube_aspect for every region that is the tip of a long thin
// tube, whatever the low poly will make of it. The regions are what the
// director names and budgets, and a tail's tip region is a small cap of 0.6%
// of the surface that it has called a neck and an ear.
void measure_region_tubes(const Mesh& source, const MeshAnalysis& analysis, Segmentation& seg,
                          const TubeOptions& opts);

// `symmetric` says the hard rules will mirror the mesh afterwards; a tube lying
// on the plane is then given a cross-section that survives the cut.
TubeReport sweep_thin_tubes(Mesh& low, const Mesh& source, const MeshAnalysis& analysis,
                            const Segmentation& seg, bool symmetric, const TubeOptions& opts);

} // namespace rd
