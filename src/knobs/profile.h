#pragma once

// The target profile is the hard contract: what the console can actually eat.
// It is authored by a human, never by the model, and the validator treats every
// field in here as non negotiable.

#include "core/json.h"
#include "core/math.h"

#include <string>
#include <vector>

namespace rd {

// A camera the director will render from. Angles are authored in degrees and
// distances in metres, because that is how an art lead talks about framing.
struct ProfileCamera {
    std::string name        = "game";
    std::string description;
    float       yaw_degrees   = 180.0f;  // 0 looks along -Z at the model front
    float       pitch_degrees = 10.0f;   // positive looks down
    float       distance_m    = 3.0f;
    float       height_m      = 1.2f;    // pivot height above the feet
    float       fov_degrees   = 40.0f;
    // Fraction of the model height the framing should cover; used when
    // distance_m is zero so the camera auto-fits instead.
    float       fit_fraction  = 0.0f;
    bool        primary       = false;   // weighted higher in silhouette scoring
};

// A texture page beyond the first: a named part of the model that gets an atlas
// of its own. The part is whatever the named camera frames - a face camera's
// page is the head - so a cutscene close up can have the texels it needs
// without the whole body's atlas growing to match.
struct TexturePage {
    std::string name   = "face";
    int         width  = 512;
    int         height = 512;
    std::string camera = "cutscene_face";
};

struct TextureBudget {
    int  width          = 256;
    int  height         = 256;
    int  palette_colors = 256;   // 256 = 8 bit CLUT, 16 = 4 bit CLUT, 0 = truecolor
    bool dithering      = true;
    int  count          = 1;     // how many pages the target allows
    // Pages after the first. The first page is width x height and takes
    // everything no extra page claims.
    std::vector<TexturePage> extra_pages;
};

struct TargetProfile {
    std::string name        = "ps2_character";
    std::string description;

    // --- hard geometry limits ---------------------------------------------
    int  max_triangles       = 1200;
    int  max_vertices        = 900;
    // How far past the triangle and vertex limits a run may go, as a fraction
    // of them, before validation fails instead of warning. The engine still
    // aims at the limit itself; the margin is there for what comes after the
    // aim - closing a hole takes two triangles, and a run one triangle over
    // was otherwise judged the same as one a thousand over. 0 is a hard wall.
    float budget_tolerance   = 0.0f;
    int  triangle_ceiling() const { return int(float(max_triangles) * (1.0f + budget_tolerance)); }
    int  vertex_ceiling()   const { return int(float(max_vertices) * (1.0f + budget_tolerance)); }
    int  max_shells          = 1;      // 0 = unlimited
    int  max_bone_influences = 2;
    bool require_manifold    = true;
    bool allow_ngons         = false;  // PS2 wants triangles, full stop
    bool require_symmetry    = true;
    // Triangles must be emitted as strips; the exporter enforces it and the
    // validator checks the resulting average strip length.
    bool  require_strips        = true;
    float min_average_strip_len = 3.0f;
    int   vertex_cache_size     = 16;  // PS2 VU1 input buffer, in vertices

    // --- texturing ---------------------------------------------------------
    TextureBudget texture;
    bool          require_uvs           = true;
    bool          require_vertex_colors = true;
    // Baked lighting goes into the diffuse on hardware with no per pixel light.
    bool          bake_lighting_to_diffuse = true;

    // --- quality targets (soft, reported not enforced) ---------------------
    float target_silhouette_error = 0.02f;  // mean, fraction of frame height
    float max_silhouette_error    = 0.05f;  // any single view

    // --- framing -----------------------------------------------------------
    float                      reference_height_m = 1.8f;
    std::vector<ProfileCamera> cameras;
    int                        turntable_views = 8;

    // --- director guidance -------------------------------------------------
    // Free text handed to the model verbatim. This is where "3rd person, face
    // visible in cutscenes" lives.
    std::string art_direction;
    int         max_iterations = 4;

    static TargetProfile ps2_character_default();
    static TargetProfile ps2_prop_default();

    Json to_json() const;
    static TargetProfile from_json(const Json& j, std::string* error = nullptr);

    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;

    // Model units per metre, given the mesh was normalised to a unit sphere
    // with the supplied bounding box height.
    float units_per_metre(float model_height_units) const
    {
        return reference_height_m > kEps ? model_height_units / reference_height_m : 1.0f;
    }

    // Sanity clamp so a hand edited profile cannot wedge the pipeline.
    void clamp();
    std::vector<std::string> warnings() const;
};

} // namespace rd
