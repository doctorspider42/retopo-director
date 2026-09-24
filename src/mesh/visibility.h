#pragma once

// Geometry nobody can see, and pieces of a mesh.
//
// The analysis uses these to tell which small pieces of a source are hidden -
// the back two thirds of an eyeball behind its lids - so the hard rules can
// leave them out of the low poly and let the bake paint what shows. Removing
// hidden triangles from the source itself was tried and is worse: it opens
// holes in the main surface (armpits, the inside of a mouth), which changes
// the backend choice and costs far more than it saves.

#include "mesh/mesh.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rd {

struct EnclosedOptions {
    // Rays per sample point, spread over the hemisphere around the face
    // normal. A face is kept the moment one of them leaves the model.
    int   rays = 48;
};

class Bvh;

// Marks the triangles from which no ray - from the centroid or from a point
// toward any corner, off either side - escapes the model. `bvh` must be built
// over `mesh`. `hidden` gets one flag per triangle.
void find_enclosed_triangles(const Mesh& mesh, const Bvh& bvh, std::vector<uint8_t>& hidden,
                             const EnclosedOptions& opts = {});

// Connected pieces of a mesh, by shared vertex: one label per triangle, dense
// from 0. Returns the number of pieces.
uint32_t label_shells(const Mesh& mesh, std::vector<uint32_t>& tri_shell);

} // namespace rd
