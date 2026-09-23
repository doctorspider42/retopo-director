#pragma once

// The knob panel: the only surface the language model is allowed to touch.
// It never sees a vertex. It fills this structure, the geometry engine turns it
// into a density field, and the hard rules override anything unsafe.

#include "core/json.h"
#include "core/math.h"
#include "knobs/profile.h"

#include <string>
#include <vector>

namespace rd {

enum class Fidelity : uint8_t {
    Geometry = 0,  // model it: the shape must exist in the silhouette
    Balanced,      // some geometry, some baked detail
    Texture        // bake it: flat panels carrying the detail in the diffuse
};

const char* fidelity_name(Fidelity f);
Fidelity    fidelity_from_name(std::string_view s, Fidelity fallback = Fidelity::Balanced);

enum class RetopoBackend : uint8_t {
    Auto = 0,     // quad field for skinned meshes, quadric for everything else
    QuadField,    // isotropic remesh guided by the density field, quad dominant
    Quadric       // meshoptimizer edge collapse with per region weights
};

const char*   backend_name(RetopoBackend b);
RetopoBackend backend_from_name(std::string_view s, RetopoBackend fallback = RetopoBackend::Auto);

// ---------------------------------------------------------------------------
// Per region
// ---------------------------------------------------------------------------
struct RegionKnobs {
    uint16_t    id   = 0;
    std::string name = "region";
    std::string role;              // free text: "face", "left hand", "belt buckle"

    // Budget. `share` is what the model fills in; `triangle_budget` is the
    // resolved absolute count after normalisation against the profile limit.
    float    share           = 1.0f;   // relative weight, >= 0
    int      triangle_budget = 0;      // resolved, read only for the model

    float    detail_priority = 0.5f;   // 0..1, breaks ties when the budget is tight
    Fidelity fidelity        = Fidelity::Balanced;

    // Geometry behaviour.
    float hard_edge_degrees   = 40.0f; // creases sharper than this stay sharp
    bool  preserve_silhouette = true;  // lock the outline, spend inside instead
    bool  preserve_boundary   = true;  // do not let the region border drift
    float symmetry_lock       = 1.0f;  // 0..1, how strictly to mirror this region
    float curvature_bias      = 0.5f;  // 0..1, follow curvature vs stay uniform

    // Filled by the model, shown in the report. Never read by the engine.
    std::string rationale;

    Json to_json() const;
    static RegionKnobs from_json(const Json& j);
    void clamp();
};

// ---------------------------------------------------------------------------
// Global
// ---------------------------------------------------------------------------
struct GlobalKnobs {
    RetopoBackend backend = RetopoBackend::Auto;

    bool  enforce_symmetry     = true;
    float joint_loop_density   = 1.0f;   // 0..3, extra rings around deforming joints
    float merge_aggressiveness = 0.5f;   // 0..1, how eagerly to collapse flat areas
    float curvature_influence  = 0.6f;   // 0..1, global weight of the curvature term
    float silhouette_weight    = 0.7f;   // 0..1, protect outline-forming triangles
    int   smoothing_iterations = 3;      // tangential relaxation passes
    float quad_dominance       = 0.7f;   // 0..1, how hard to pair triangles into quads

    // Baking.
    // Carry the source uv layout onto the new topology and repack it, instead
    // of unwrapping from scratch. Keeps the artwork where the artist put it;
    // needs a source that actually has uvs and materials, and falls back to a
    // fresh unwrap when it does not.
    bool  reuse_source_uvs       = true;
    bool  bake_ambient_occlusion = true;
    float ao_intensity           = 0.75f;
    bool  bake_vertex_colors     = true;
    float uv_padding_texels      = 4.0f;
    float uv_stretch_tolerance   = 0.15f;

    // The model may ask for another pass; the pipeline still caps it.
    bool        request_another_iteration = false;
    std::string notes;

    Json to_json() const;
    static GlobalKnobs from_json(const Json& j);
    void clamp();
};

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
struct KnobPanel {
    GlobalKnobs              global;
    std::vector<RegionKnobs> regions;

    RegionKnobs*       find(uint16_t id);
    const RegionKnobs* find(uint16_t id) const;

    // Turns relative shares into absolute triangle budgets that sum to
    // `total_triangles`, giving every region at least `min_per_region`.
    // Returns the number of triangles actually handed out.
    int resolve_budgets(int total_triangles, int min_per_region = 8);

    int total_budget() const;

    Json to_json() const;
    static KnobPanel from_json(const Json& j);
    void clamp();

    // Applies only the fields present in `patch`, leaving everything else
    // alone. This is what an LLM response goes through, so a model that
    // returns three keys does not wipe the other forty.
    struct ApplyReport {
        int         regions_touched = 0;
        int         fields_changed  = 0;
        int         unknown_regions = 0;
        std::vector<std::string> ignored_keys;
    };
    ApplyReport apply_patch(const Json& patch);

    // Machine readable description handed to the model so it knows the exact
    // shape and ranges it is allowed to produce.
    static std::string json_schema_text();

    // Seeds a panel from a freshly segmented mesh: equal shares, defaults
    // everywhere, names taken from the segmenter.
    static KnobPanel seed_from_regions(const std::vector<uint16_t>& ids,
                                       const std::vector<std::string>& names,
                                       const std::vector<float>& area_share);
};

// Human readable difference between two panels, for the report and the UI.
struct KnobDiffEntry {
    std::string scope;   // "global" or the region name
    std::string field;
    std::string before;
    std::string after;
};
std::vector<KnobDiffEntry> knob_diff(const KnobPanel& before, const KnobPanel& after);

} // namespace rd
