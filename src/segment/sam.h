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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace rd {

// The checkpoints the sidecar can load, which is what the `segment_anything`
// package publishes: one file of trained weights per backbone size. The code is
// small; everything the model knows lives in these.
//
// Apache-2.0, the same licence as this project, downloaded from the address
// Meta documents in the Segment Anything repository. Nothing here is mirrored
// or redistributed - the application fetches the file on request, which is the
// same thing the user would otherwise do with curl. (The SA-1B *dataset* is
// under a separate research licence; it is not involved.)
struct SamCheckpoint {
    const char* model_type;   // what the sidecar passes to sam_model_registry
    const char* label;
    const char* file;
    const char* url;
    uint64_t    bytes;
    const char* note;         // what it costs to run
};

const std::vector<SamCheckpoint>& sam_checkpoints();

// Where a downloaded checkpoint is kept: beside the settings, not in the
// project, because it belongs to the machine rather than to one asset.
std::filesystem::path sam_checkpoint_dir();
std::filesystem::path sam_checkpoint_path(const SamCheckpoint& entry);

// Never null. Reports through ISegmenter::available() when the interpreter,
// the script or the checkpoint is missing, so the caller can fall back.
std::unique_ptr<ISegmenter> make_sam_segmenter(const SamOptions& opts);

// Where the sidecar script is looked for when SamOptions::script is empty:
// next to the executable first, then the source tree, so a build directory run
// finds it without staging. Empty when nothing was found.
std::string find_sam_script();

} // namespace rd
