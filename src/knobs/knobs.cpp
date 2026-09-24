#include "knobs/knobs.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <cmath>

namespace rd {
namespace {

std::string bool_str(bool v)   { return v ? "true" : "false"; }
std::string num_str(float v)   { return format("%.3g", v); }
std::string num_str(int v)     { return format("%d", v); }

void push_diff(std::vector<KnobDiffEntry>& out, const char* scope, const char* field,
               std::string before, std::string after)
{
    if (before == after) return;
    out.push_back({scope, field, std::move(before), std::move(after)});
}

} // namespace

// ---------------------------------------------------------------------------
const char* fidelity_name(Fidelity f)
{
    switch (f) {
    case Fidelity::Geometry: return "geometry";
    case Fidelity::Texture:  return "texture";
    default:                 return "balanced";
    }
}

Fidelity fidelity_from_name(std::string_view s, Fidelity fallback)
{
    if (iequals(s, "geometry") || iequals(s, "model") || iequals(s, "geo")) return Fidelity::Geometry;
    if (iequals(s, "texture")  || iequals(s, "bake"))                       return Fidelity::Texture;
    if (iequals(s, "balanced") || iequals(s, "mixed"))                      return Fidelity::Balanced;
    return fallback;
}

const char* backend_name(RetopoBackend b)
{
    switch (b) {
    case RetopoBackend::QuadField: return "quad_field";
    case RetopoBackend::Quadric:   return "quadric";
    default:                       return "auto";
    }
}

RetopoBackend backend_from_name(std::string_view s, RetopoBackend fallback)
{
    if (iequals(s, "quad_field") || iequals(s, "quad") || iequals(s, "retopo"))
        return RetopoBackend::QuadField;
    if (iequals(s, "quadric") || iequals(s, "decimate") || iequals(s, "simplify"))
        return RetopoBackend::Quadric;
    if (iequals(s, "auto")) return RetopoBackend::Auto;
    return fallback;
}

// ---------------------------------------------------------------------------
// RegionKnobs
// ---------------------------------------------------------------------------
Json RegionKnobs::to_json() const
{
    Json j;
    j["id"]                  = id;
    j["name"]                = name;
    if (!role.empty()) j["role"] = role;
    j["share"]               = share;
    j["triangle_budget"]     = triangle_budget;
    j["detail_priority"]     = detail_priority;
    j["fidelity"]            = fidelity_name(fidelity);
    j["hard_edge_degrees"]   = hard_edge_degrees;
    j["preserve_silhouette"] = preserve_silhouette;
    j["preserve_boundary"]   = preserve_boundary;
    j["symmetry_lock"]       = symmetry_lock;
    j["curvature_bias"]      = curvature_bias;
    j["texel_weight"]        = texel_weight;
    if (!rationale.empty()) j["rationale"] = rationale;
    return j;
}

RegionKnobs RegionKnobs::from_json(const Json& j)
{
    RegionKnobs r;
    r.id                  = static_cast<uint16_t>(json_get<int>(j, "id", 0));
    r.name                = json_get<std::string>(j, "name", r.name);
    r.role                = json_get<std::string>(j, "role", "");
    r.share               = json_get<float>(j, "share", r.share);
    r.triangle_budget     = json_get<int>(j, "triangle_budget", 0);
    r.detail_priority     = json_get<float>(j, "detail_priority", r.detail_priority);
    r.fidelity            = fidelity_from_name(json_get<std::string>(j, "fidelity", "balanced"));
    r.hard_edge_degrees   = json_get<float>(j, "hard_edge_degrees", r.hard_edge_degrees);
    r.preserve_silhouette = json_get<bool>(j, "preserve_silhouette", r.preserve_silhouette);
    r.preserve_boundary   = json_get<bool>(j, "preserve_boundary", r.preserve_boundary);
    r.symmetry_lock       = json_get<float>(j, "symmetry_lock", r.symmetry_lock);
    r.curvature_bias      = json_get<float>(j, "curvature_bias", r.curvature_bias);
    r.texel_weight        = json_get<float>(j, "texel_weight", r.texel_weight);
    r.rationale           = json_get<std::string>(j, "rationale", "");
    r.clamp();
    return r;
}

void RegionKnobs::clamp()
{
    share             = clampf(share, 0.0f, 1000.0f);
    detail_priority   = saturate(detail_priority);
    hard_edge_degrees = clampf(hard_edge_degrees, 0.0f, 180.0f);
    symmetry_lock     = saturate(symmetry_lock);
    curvature_bias    = saturate(curvature_bias);
    texel_weight      = clampf(texel_weight, 0.5f, 2.0f);
    triangle_budget   = std::max(0, triangle_budget);
    if (name.empty()) name = format("region_%u", unsigned(id));
    if (rationale.size() > 600) rationale.resize(600);
}

// ---------------------------------------------------------------------------
// GlobalKnobs
// ---------------------------------------------------------------------------
Json GlobalKnobs::to_json() const
{
    Json j;
    j["backend"]                   = backend_name(backend);
    j["enforce_symmetry"]          = enforce_symmetry;
    j["joint_loop_density"]        = joint_loop_density;
    j["merge_aggressiveness"]      = merge_aggressiveness;
    j["curvature_influence"]       = curvature_influence;
    j["silhouette_weight"]         = silhouette_weight;
    j["smoothing_iterations"]      = smoothing_iterations;
    j["quad_dominance"]            = quad_dominance;
    j["reuse_source_uvs"]          = reuse_source_uvs;
    j["bake_ambient_occlusion"]    = bake_ambient_occlusion;
    j["ao_intensity"]              = ao_intensity;
    j["bake_vertex_colors"]        = bake_vertex_colors;
    j["uv_padding_texels"]         = uv_padding_texels;
    j["uv_stretch_tolerance"]      = uv_stretch_tolerance;
    j["uv_seam_hiding"]            = uv_seam_hiding;
    j["request_another_iteration"] = request_another_iteration;
    if (!notes.empty()) j["notes"] = notes;
    return j;
}

GlobalKnobs GlobalKnobs::from_json(const Json& j)
{
    GlobalKnobs g;
    g.backend                   = backend_from_name(json_get<std::string>(j, "backend", "auto"));
    g.enforce_symmetry          = json_get<bool>(j, "enforce_symmetry", g.enforce_symmetry);
    g.joint_loop_density        = json_get<float>(j, "joint_loop_density", g.joint_loop_density);
    g.merge_aggressiveness      = json_get<float>(j, "merge_aggressiveness", g.merge_aggressiveness);
    g.curvature_influence       = json_get<float>(j, "curvature_influence", g.curvature_influence);
    g.silhouette_weight         = json_get<float>(j, "silhouette_weight", g.silhouette_weight);
    g.smoothing_iterations      = json_get<int>(j, "smoothing_iterations", g.smoothing_iterations);
    g.quad_dominance            = json_get<float>(j, "quad_dominance", g.quad_dominance);
    g.reuse_source_uvs          = json_get<bool>(j, "reuse_source_uvs", g.reuse_source_uvs);
    g.bake_ambient_occlusion    = json_get<bool>(j, "bake_ambient_occlusion", g.bake_ambient_occlusion);
    g.ao_intensity              = json_get<float>(j, "ao_intensity", g.ao_intensity);
    g.bake_vertex_colors        = json_get<bool>(j, "bake_vertex_colors", g.bake_vertex_colors);
    g.uv_padding_texels         = json_get<float>(j, "uv_padding_texels", g.uv_padding_texels);
    g.uv_stretch_tolerance      = json_get<float>(j, "uv_stretch_tolerance", g.uv_stretch_tolerance);
    g.uv_seam_hiding            = json_get<float>(j, "uv_seam_hiding", g.uv_seam_hiding);
    g.request_another_iteration = json_get<bool>(j, "request_another_iteration", false);
    g.notes                     = json_get<std::string>(j, "notes", "");
    g.clamp();
    return g;
}

void GlobalKnobs::clamp()
{
    joint_loop_density   = clampf(joint_loop_density, 0.0f, 3.0f);
    merge_aggressiveness = saturate(merge_aggressiveness);
    curvature_influence  = saturate(curvature_influence);
    silhouette_weight    = saturate(silhouette_weight);
    smoothing_iterations = std::clamp(smoothing_iterations, 0, 32);
    quad_dominance       = saturate(quad_dominance);
    ao_intensity         = saturate(ao_intensity);
    uv_padding_texels    = clampf(uv_padding_texels, 0.0f, 32.0f);
    uv_stretch_tolerance = clampf(uv_stretch_tolerance, 0.0f, 1.0f);
    uv_seam_hiding       = saturate(uv_seam_hiding);
    if (notes.size() > 2000) notes.resize(2000);
}

// ---------------------------------------------------------------------------
// KnobPanel
// ---------------------------------------------------------------------------
RegionKnobs* KnobPanel::find(uint16_t id)
{
    for (RegionKnobs& r : regions)
        if (r.id == id) return &r;
    return nullptr;
}

const RegionKnobs* KnobPanel::find(uint16_t id) const
{
    return const_cast<KnobPanel*>(this)->find(id);
}

int KnobPanel::total_budget() const
{
    int total = 0;
    for (const RegionKnobs& r : regions) total += r.triangle_budget;
    return total;
}

int KnobPanel::resolve_budgets(int total_triangles, int min_per_region)
{
    if (regions.empty()) return 0;

    const int region_count = static_cast<int>(regions.size());
    min_per_region = std::max(0, min_per_region);

    // If the floor alone would blow the budget, drop it proportionally instead
    // of refusing: the validator will complain about the total, not us.
    if (static_cast<long long>(min_per_region) * region_count > total_triangles)
        min_per_region = std::max(0, total_triangles / std::max(1, region_count));

    const int floor_total = min_per_region * region_count;
    int       remaining   = std::max(0, total_triangles - floor_total);

    double share_sum = 0.0;
    for (const RegionKnobs& r : regions) share_sum += std::max(0.0f, r.share);

    if (share_sum <= 1e-6) {
        // No usable shares: spread evenly.
        for (RegionKnobs& r : regions)
            r.triangle_budget = min_per_region + remaining / region_count;
        return total_budget();
    }

    // Largest remainder method, so the budgets sum exactly to the target.
    std::vector<double> exact(regions.size());
    std::vector<int>    floors(regions.size());
    int                 handed_out = 0;

    for (size_t i = 0; i < regions.size(); ++i) {
        exact[i]  = remaining * (std::max(0.0f, regions[i].share) / share_sum);
        floors[i] = static_cast<int>(std::floor(exact[i]));
        handed_out += floors[i];
    }

    int leftover = remaining - handed_out;
    std::vector<size_t> order(regions.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const double fa = exact[a] - floors[a];
        const double fb = exact[b] - floors[b];
        if (fa != fb) return fa > fb;
        // Deterministic tie break: higher priority first, then lower id.
        if (regions[a].detail_priority != regions[b].detail_priority)
            return regions[a].detail_priority > regions[b].detail_priority;
        return regions[a].id < regions[b].id;
    });
    for (size_t k = 0; k < order.size() && leftover > 0; ++k, --leftover)
        ++floors[order[k]];

    for (size_t i = 0; i < regions.size(); ++i)
        regions[i].triangle_budget = min_per_region + floors[i];

    return total_budget();
}

Json KnobPanel::to_json() const
{
    Json j;
    j["global"] = global.to_json();
    Json arr    = Json::array();
    for (const RegionKnobs& r : regions) arr.push_back(r.to_json());
    j["regions"] = arr;
    return j;
}

KnobPanel KnobPanel::from_json(const Json& j)
{
    KnobPanel p;
    p.global = GlobalKnobs::from_json(json_object_or_empty(j, "global"));
    for (const Json& r : json_array_or_empty(j, "regions"))
        p.regions.push_back(RegionKnobs::from_json(r));
    return p;
}

void KnobPanel::clamp()
{
    global.clamp();
    for (RegionKnobs& r : regions) r.clamp();
}

KnobPanel::ApplyReport KnobPanel::apply_patch(const Json& patch)
{
    ApplyReport rep;
    if (!patch.is_object()) return rep;

    // --- global ------------------------------------------------------------
    const auto git = patch.find("global");
    if (git != patch.end() && git->is_object()) {
        const Json before = global.to_json();
        Json merged = before;
        for (auto it = git->begin(); it != git->end(); ++it) {
            if (before.contains(it.key())) merged[it.key()] = it.value();
            else rep.ignored_keys.push_back("global." + it.key());
        }
        global = GlobalKnobs::from_json(merged);
        const Json after = global.to_json();
        for (auto it = after.begin(); it != after.end(); ++it)
            if (!before.contains(it.key()) || before[it.key()] != it.value()) ++rep.fields_changed;
    }

    // --- regions -----------------------------------------------------------
    const auto rit = patch.find("regions");
    if (rit != patch.end() && rit->is_array()) {
        for (const Json& entry : *rit) {
            if (!entry.is_object()) continue;

            RegionKnobs* target = nullptr;
            if (entry.contains("id")) {
                target = find(static_cast<uint16_t>(json_get<int>(entry, "id", -1)));
            }
            if (!target && entry.contains("name")) {
                const std::string wanted = json_get<std::string>(entry, "name", "");
                for (RegionKnobs& r : regions)
                    if (iequals(r.name, wanted)) { target = &r; break; }
            }
            if (!target) { ++rep.unknown_regions; continue; }

            const Json before = target->to_json();
            Json merged = before;
            for (auto it = entry.begin(); it != entry.end(); ++it) {
                if (it.key() == "id") continue;
                if (before.contains(it.key()) || it.key() == "role" || it.key() == "rationale")
                    merged[it.key()] = it.value();
                else
                    rep.ignored_keys.push_back("regions[" + target->name + "]." + it.key());
            }
            const uint16_t keep_id = target->id;
            *target    = RegionKnobs::from_json(merged);
            target->id = keep_id;

            const Json after = target->to_json();
            int changed = 0;
            for (auto it = after.begin(); it != after.end(); ++it)
                if (!before.contains(it.key()) || before[it.key()] != it.value()) ++changed;
            if (changed) { ++rep.regions_touched; rep.fields_changed += changed; }
        }
    }

    clamp();
    return rep;
}

KnobPanel KnobPanel::seed_from_regions(const std::vector<uint16_t>& ids,
                                       const std::vector<std::string>& names,
                                       const std::vector<float>& area_share)
{
    KnobPanel p;
    p.regions.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        RegionKnobs r;
        r.id    = ids[i];
        r.name  = i < names.size() ? names[i] : format("region_%u", unsigned(ids[i]));
        // Surface area is the only defensible prior before anyone has looked at
        // the model: bigger patch, more triangles.
        r.share = i < area_share.size() ? std::max(area_share[i], 1e-4f) : 1.0f;
        r.clamp();
        p.regions.push_back(std::move(r));
    }
    return p;
}

std::string KnobPanel::json_schema_text()
{
    return R"SCHEMA({
  "global": {
    "backend":                   "auto | quad_field | quadric",
    "enforce_symmetry":          "bool - mirror the result across the detected plane",
    "joint_loop_density":        "0.0 .. 3.0 - extra edge rings around deforming joints",
    "merge_aggressiveness":      "0.0 .. 1.0 - how eagerly flat areas collapse",
    "curvature_influence":       "0.0 .. 1.0 - global weight of the curvature term",
    "silhouette_weight":         "0.0 .. 1.0 - protection for outline forming triangles",
    "smoothing_iterations":      "0 .. 32 - tangential relaxation passes",
    "quad_dominance":            "0.0 .. 1.0 - how hard to pair triangles into quads",
    "reuse_source_uvs":          "bool, keep the source uv layout instead of unwrapping fresh",
    "bake_ambient_occlusion":    "bool",
    "ao_intensity":              "0.0 .. 1.0",
    "bake_vertex_colors":        "bool",
    "uv_padding_texels":         "0.0 .. 32.0",
    "uv_stretch_tolerance":      "0.0 .. 1.0 - has no measurable effect on this unwrap; leave it",
    "uv_seam_hiding":            "0.0 .. 1.0 - how hard uv seams avoid visible surface (0 shortest cut, 1 hide them in crevices and underneath)",
    "request_another_iteration": "bool - true if you want to see the result and adjust again",
    "notes":                     "string - short explanation of the overall strategy"
  },
  "regions": [
    {
      "id":                  "integer - must match a region id from the region table",
      "name":                "string - short human name, e.g. 'head' or 'left hand'",
      "role":                "string - what this part does for the read of the model",
      "share":               "number >= 0 - relative slice of the triangle budget",
      "detail_priority":     "0.0 .. 1.0 - who wins when the budget is tight",
      "fidelity":            "geometry | balanced | texture",
      "hard_edge_degrees":   "0 .. 180 - creases sharper than this stay sharp",
      "preserve_silhouette": "bool",
      "preserve_boundary":   "bool",
      "symmetry_lock":       "0.0 .. 1.0",
      "curvature_bias":      "0.0 .. 1.0 - follow curvature vs stay uniform",
      "texel_weight":        "0.5 .. 2.0 - share of the texture per unit of surface, relative to the rest (2 = twice the texel density)",
      "rationale":           "string - one sentence, why this region gets this treatment"
    }
  ]
})SCHEMA";
}

// ---------------------------------------------------------------------------
std::vector<KnobDiffEntry> knob_diff(const KnobPanel& before, const KnobPanel& after)
{
    std::vector<KnobDiffEntry> out;

    const GlobalKnobs& a = before.global;
    const GlobalKnobs& b = after.global;
    push_diff(out, "global", "backend", backend_name(a.backend), backend_name(b.backend));
    push_diff(out, "global", "enforce_symmetry", bool_str(a.enforce_symmetry), bool_str(b.enforce_symmetry));
    push_diff(out, "global", "joint_loop_density", num_str(a.joint_loop_density), num_str(b.joint_loop_density));
    push_diff(out, "global", "merge_aggressiveness", num_str(a.merge_aggressiveness), num_str(b.merge_aggressiveness));
    push_diff(out, "global", "curvature_influence", num_str(a.curvature_influence), num_str(b.curvature_influence));
    push_diff(out, "global", "silhouette_weight", num_str(a.silhouette_weight), num_str(b.silhouette_weight));
    push_diff(out, "global", "smoothing_iterations", num_str(a.smoothing_iterations), num_str(b.smoothing_iterations));
    push_diff(out, "global", "quad_dominance", num_str(a.quad_dominance), num_str(b.quad_dominance));
    push_diff(out, "global", "ao_intensity", num_str(a.ao_intensity), num_str(b.ao_intensity));
    push_diff(out, "global", "uv_seam_hiding", num_str(a.uv_seam_hiding), num_str(b.uv_seam_hiding));

    for (const RegionKnobs& rb : after.regions) {
        const RegionKnobs* ra = before.find(rb.id);
        if (!ra) {
            out.push_back({rb.name, "region", "(absent)", "added"});
            continue;
        }
        const char* scope = rb.name.c_str();
        push_diff(out, scope, "share", num_str(ra->share), num_str(rb.share));
        push_diff(out, scope, "triangle_budget", num_str(ra->triangle_budget), num_str(rb.triangle_budget));
        push_diff(out, scope, "detail_priority", num_str(ra->detail_priority), num_str(rb.detail_priority));
        push_diff(out, scope, "fidelity", fidelity_name(ra->fidelity), fidelity_name(rb.fidelity));
        push_diff(out, scope, "hard_edge_degrees", num_str(ra->hard_edge_degrees), num_str(rb.hard_edge_degrees));
        push_diff(out, scope, "preserve_silhouette", bool_str(ra->preserve_silhouette), bool_str(rb.preserve_silhouette));
        push_diff(out, scope, "curvature_bias", num_str(ra->curvature_bias), num_str(rb.curvature_bias));
        push_diff(out, scope, "symmetry_lock", num_str(ra->symmetry_lock), num_str(rb.symmetry_lock));
        push_diff(out, scope, "texel_weight", num_str(ra->texel_weight), num_str(rb.texel_weight));
        if (ra->name != rb.name) push_diff(out, scope, "name", ra->name, rb.name);
    }
    return out;
}

} // namespace rd
