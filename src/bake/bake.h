#pragma once

// UV unwrap, then transfer everything the low poly cannot model into pixels
// and vertex colours: ambient occlusion, a fixed light rig and whatever base
// colour the high poly carried. On hardware with no per pixel lighting this is
// not a shortcut, it is the entire shading model.

#include "bake/texture.h"
#include "knobs/knobs.h"
#include "knobs/profile.h"
#include "mesh/analysis.h"
#include "mesh/bvh.h"
#include "mesh/mesh.h"

#include <functional>
#include <string>
#include <vector>

namespace rd {

struct BakeOptions {
    int   texture_width  = 0;      // 0 takes the profile value
    int   texture_height = 0;
    int   padding_texels = 4;
    int   ao_rays        = 96;
    float ao_distance_rel = 0.30f; // of the bounding box diagonal
    float ao_intensity    = 0.75f;
    bool  bake_ao         = true;
    bool  bake_lighting   = true;
    bool  bake_vertex_colors = true;
    // Ray offset along the normal, relative to the bbox diagonal, to stop the
    // low poly surface from shadowing itself against the high poly.
    float ray_bias_rel = 3e-4f;
    // How far to search for the high poly when a texel sits off the surface.
    float projection_distance_rel = 0.05f;
    // Palette size; 0 takes the profile value, negative disables quantisation.
    int   palette_colors = 0;
    bool  dither         = true;
    bool  write_indexed  = true;
};

struct BakeResult {
    Texture      diffuse;
    CoverageMask coverage;
    Palette      palette;

    bool   ok = false;
    int    charts = 0;
    int    atlas_count = 0;
    float  uv_utilisation = 0.0f;
    float  uv_max_stretch = 0.0f;
    size_t texels_baked  = 0;
    size_t rays_cast     = 0;
    double seconds       = 0.0;
    std::string error;
    std::vector<std::string> messages;
};

// Unwraps `mesh` in place (this adds vertices along the seams), then bakes.
// `source` / `source_bvh` are the high poly the detail comes from.
BakeResult bake_all(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                    const MeshAnalysis& source_analysis, const TargetProfile& profile,
                    const GlobalKnobs& knobs, const BakeOptions& opts = {},
                    const std::function<void(float, const char*)>& progress = nullptr);

// Unwrap on its own, for the UV preview in the viewport.
struct UnwrapResult {
    bool   ok = false;
    int    charts = 0;
    int    atlas_count = 0;
    float  utilisation = 0.0f;
    size_t added_vertices = 0;
    std::string error;
};
UnwrapResult unwrap_uvs(Mesh& mesh, int width, int height, int padding,
                        float stretch_tolerance);

// The light rig used by the bake. Deterministic and documented so the result is
// reproducible and an artist can match it in their DCC.
struct BakeLight {
    Vec3  direction{0.0f, 0.0f, 1.0f};   // points from the surface toward the light
    Vec3  color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool  casts_shadow = false;
};
std::vector<BakeLight> default_light_rig();

} // namespace rd
