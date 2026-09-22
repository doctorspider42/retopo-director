#pragma once

// Segment Anything, out of process.
//
// A Python sidecar holds the model, reads one JSON request off stdin and writes
// one JSON reply to stdout: the renders the director already has go in, per
// view mask bitmaps come back. Out of process on purpose - the licence surface
// stays clean, the 2.4 GB checkpoint stays optional, and a crash inside Torch
// does not take the editor with it.
//
// Back onto the mesh: one ray per (strided) pixel through the same camera that
// rendered the view, into the BVH the analysis already built. Each mask votes
// for the triangles its pixels hit, the mask with the most votes takes the
// triangle, and triangles no camera ever saw are filled in by the same Dijkstra
// the geometric split uses. The two approaches compose rather than compete.

#include "segment/segmenter.h"

#include <memory>

namespace rd {

// Never null. Reports through ISegmenter::available() when the interpreter,
// the script or the checkpoint is missing, so the caller can fall back.
std::unique_ptr<ISegmenter> make_sam_segmenter(const SamOptions& opts);

// Where the sidecar script is looked for when SamOptions::script is empty:
// next to the executable first, then the source tree, so a build directory run
// finds it without staging. Empty when nothing was found.
std::string find_sam_script();

} // namespace rd
