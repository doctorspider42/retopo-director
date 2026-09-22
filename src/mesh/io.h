#pragma once

// Mesh import/export. OBJ is hand rolled (it is trivial and we want control
// over triangulation); glTF/GLB goes through cgltf, FBX through ufbx.
//
// Import only for FBX: the format is Autodesk's, the writers that matter are
// theirs, and nothing in this pipeline needs to hand one back.

#include "mesh/mesh.h"

#include <filesystem>
#include <string>

namespace rd::meshio {

namespace fs = std::filesystem;

struct LoadOptions {
    // Merge every primitive/node into a single mesh. The pipeline works on one
    // mesh; keeping parts separate would only push the problem downstream.
    bool  merge_all      = true;
    // Weld vertices that a DCC split purely for shading/UV reasons. The epsilon
    // is relative to the bounding box diagonal.
    bool  weld           = true;
    float weld_epsilon_rel = 1e-6f;
    // Rescale into a unit sphere at the origin. Keeps every distance threshold
    // in the pipeline resolution independent.
    bool  normalise      = true;
    // Recompute normals even if the file carries them.
    bool  force_recompute_normals = false;
    float sharp_angle_degrees     = 60.0f;
    // Import the skeleton when the file has one.
    bool  load_armature  = true;
};

struct LoadReport {
    bool        ok = false;
    std::string error;
    std::string format;
    size_t      source_vertices  = 0;
    size_t      source_triangles = 0;
    size_t      welded_vertices  = 0;
    size_t      dropped_triangles = 0;
    size_t      primitives       = 0;
    size_t      joints           = 0;
    double      seconds          = 0.0;
};

LoadReport load(const fs::path& path, Mesh& out, const LoadOptions& opts = {});

// Exporters. `restore_import_transform` writes the mesh back in the space the
// source file used, which is what an engine pipeline expects.
struct SaveOptions {
    bool restore_import_transform = true;
    bool write_normals            = true;
    bool write_uvs                = true;
    bool write_colors             = true;
    bool binary                   = false;       // glTF only: emit .glb
    std::string texture_file;                    // referenced from the material
};

bool save_obj(const fs::path& path, const Mesh& mesh, const SaveOptions& opts = {},
              std::string* error = nullptr);
bool save_gltf(const fs::path& path, const Mesh& mesh, const SaveOptions& opts = {},
               std::string* error = nullptr);

// Dispatches on the extension.
bool save(const fs::path& path, const Mesh& mesh, const SaveOptions& opts = {},
          std::string* error = nullptr);

bool is_supported_extension(const std::string& ext_lower);
const char* supported_extensions_filter();

} // namespace rd::meshio
