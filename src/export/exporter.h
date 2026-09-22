#pragma once

// Everything that turns the finished low poly into files an engine can load.
// This is where meshoptimizer earns its keep: vertex cache ordering, fetch
// ordering and triangle strips, all of which the PS2 class target cares about
// far more than any modern GPU does.

#include "bake/texture.h"
#include "knobs/profile.h"
#include "mesh/mesh.h"

#include <filesystem>
#include <string>
#include <vector>

namespace rd {

namespace fs = std::filesystem;

struct StripData {
    // Strip indices with a restart sentinel between runs.
    std::vector<uint32_t> indices;
    uint32_t restart_index = 0xFFFFFFFFu;
    size_t   strip_count   = 0;
    float    average_length = 0.0f;   // triangles per strip
    size_t   degenerate_count = 0;
};

// Splits a restart-separated strip buffer into individual runs.
std::vector<std::vector<uint32_t>> split_strips(const StripData& data);

struct OptimiseReport {
    float acmr_before = 0.0f;   // average cache miss ratio
    float acmr_after  = 0.0f;
    float atvr_before = 0.0f;   // average transformed vertex ratio
    float atvr_after  = 0.0f;
    float overdraw_after = 0.0f;
    size_t vertices_before = 0;
    size_t vertices_after  = 0;
};

// Reorders indices for the cache and vertices for fetch locality, in place.
OptimiseReport optimise_for_target(Mesh& mesh, const TargetProfile& profile);

// Builds strips from an already optimised index buffer.
StripData build_strips(const Mesh& mesh, const TargetProfile& profile);

struct ExportOptions {
    bool write_obj    = true;
    bool write_gltf   = true;
    bool write_glb    = false;
    bool write_binary = true;      // the .rdmesh container with strips
    bool write_texture = true;
    bool write_indexed_texture = true;
    bool write_report  = true;
    bool optimise      = true;
    std::string base_name = "lowpoly";
};

struct ExportResult {
    bool                     ok = false;
    std::string              error;
    std::vector<fs::path>    files;
    OptimiseReport           optimisation;
    StripData                strips;
    uint64_t                 total_bytes = 0;
    double                   seconds = 0.0;
};

// `mesh` is modified in place when `optimise` is set, because the exported
// index order has to match the file that ships.
ExportResult export_asset(Mesh& mesh, const Texture& diffuse, const Palette& palette,
                          const TargetProfile& profile, const fs::path& directory,
                          const ExportOptions& opts = {});

// The .rdmesh container, documented in docs/FORMATS.md.
bool write_rdmesh(const fs::path& path, const Mesh& mesh, const StripData& strips,
                  const TargetProfile& profile, std::string* error = nullptr);

} // namespace rd
