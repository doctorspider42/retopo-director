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
    // Occluders nearer than this, relative to the bbox diagonal, do not count
    // towards ambient occlusion. A sculpted source is full of millimetre
    // grooves - veins, muscle striations - that occlude correctly and read, at
    // 256 texels for a whole character, as thin black scribbles all over the
    // skin. The occlusion a low poly texture can use is the armpit and the
    // crotch, not the pore; this is the line between them.
    float ao_min_distance_rel = 0.004f;
    // How far to search for the high poly when a texel sits off the surface.
    float projection_distance_rel = 0.05f;
    // Palette size; 0 takes the profile value, negative disables quantisation.
    int   palette_colors = 0;
    bool  dither         = true;
    bool  write_indexed  = true;
};

struct BakeResult {
    // Page 0: the profile's main atlas.
    Texture      diffuse;
    CoverageMask coverage;
    Palette      palette;

    // The profile's extra pages, in order; page i + 1 of the mesh's tri_page.
    // Each has its own palette, as each would have its own CLUT on the target.
    struct Page {
        std::string  name;
        Texture      diffuse;
        CoverageMask coverage;
        Palette      palette;
        size_t       triangles = 0;
    };
    std::vector<Page> extra_pages;
    size_t page0_triangles = 0;

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

// Which page each triangle of `mesh` belongs on, from the profile's extra
// pages: the regions a page's camera sees most of go to that page, whole, so
// the back of the head travels with the face rather than being cut off by a
// seam. Empty when the profile has no extra page or nothing qualifies.
std::vector<uint8_t> assign_texture_pages(const Mesh& mesh, const TargetProfile& profile);

// Everything the viewport and the candidate renders need to show a multi page
// bake with one texture: the pages side by side at the height of the tallest,
// and a copy of the mesh with its uvs moved into that layout. With one page it
// is the diffuse and the mesh as they are. Display only - nothing ships this.
Texture display_atlas(const Mesh& mesh, const BakeResult& bake, Mesh& display_mesh);

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

// Carries the source uv layout onto `mesh` by projection, so the low poly reads
// the original artwork through the coordinates it was painted in rather than
// through a freshly invented atlas.
//
// The vertex buffer is rebuilt: a corner needs its own uv whenever the corners
// around a vertex come from different parts of the source layout, which is most
// of them. `mesh.tri_material` is filled at the same time, because the uv only
// means anything paired with the material it indexes into.
struct UvTransferResult {
    bool   ok = false;
    size_t triangles_mapped   = 0;
    size_t triangles_fallback = 0;  // corners disagreed, extrapolated from one source triangle
    size_t triangles_unmapped = 0;  // nothing of the source within reach
    size_t vertices_before    = 0;
    size_t vertices_after     = 0;
    std::string error;
};
UvTransferResult transfer_source_uvs(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                                     float search_distance);

// Packs an existing uv layout into one atlas page without re-parameterising it.
// `carried` is any per vertex array that must follow the rebuilt vertex buffer,
// which is how the source uvs survive the repack to be sampled from.
UnwrapResult repack_uvs(Mesh& mesh, std::vector<Vec2>* carried, int width, int height,
                        int padding);


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
