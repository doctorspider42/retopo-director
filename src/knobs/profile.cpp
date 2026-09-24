#include "knobs/profile.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

namespace rd {
namespace {

ProfileCamera camera_from_json(const Json& j)
{
    ProfileCamera c;
    c.name          = json_get<std::string>(j, "name", c.name);
    c.description   = json_get<std::string>(j, "description", "");
    c.yaw_degrees   = json_get<float>(j, "yaw_degrees", c.yaw_degrees);
    c.pitch_degrees = json_get<float>(j, "pitch_degrees", c.pitch_degrees);
    c.distance_m    = json_get<float>(j, "distance_m", c.distance_m);
    c.height_m      = json_get<float>(j, "height_m", c.height_m);
    c.fov_degrees   = json_get<float>(j, "fov_degrees", c.fov_degrees);
    c.fit_fraction  = json_get<float>(j, "fit_fraction", c.fit_fraction);
    c.primary       = json_get<bool>(j, "primary", c.primary);
    return c;
}

Json camera_to_json(const ProfileCamera& c)
{
    Json j;
    j["name"]          = c.name;
    if (!c.description.empty()) j["description"] = c.description;
    j["yaw_degrees"]   = c.yaw_degrees;
    j["pitch_degrees"] = c.pitch_degrees;
    j["distance_m"]    = c.distance_m;
    j["height_m"]      = c.height_m;
    j["fov_degrees"]   = c.fov_degrees;
    if (c.fit_fraction > 0.0f) j["fit_fraction"] = c.fit_fraction;
    j["primary"]       = c.primary;
    return j;
}

} // namespace

TargetProfile TargetProfile::ps2_character_default()
{
    TargetProfile p;
    p.name        = "ps2_character";
    p.description = "Playable third person character, PlayStation 2 class budget.";

    p.max_triangles       = 1400;
    // Roughly T/2 for a closed mesh, plus the vertices the uv seams duplicate.
    // On a thousand-triangle character that surcharge is 30 to 45 per cent, so
    // a limit of T/2 exactly is unreachable no matter how good the retopology.
    p.max_vertices        = 1200;
    p.max_shells          = 1;
    p.max_bone_influences = 2;
    p.require_manifold    = true;
    p.allow_ngons         = false;
    p.require_symmetry    = true;
    p.require_strips      = true;
    p.min_average_strip_len = 3.5f;
    p.vertex_cache_size   = 16;

    p.texture.width          = 256;
    p.texture.height         = 256;
    p.texture.palette_colors = 256;
    p.texture.dithering      = true;
    p.texture.count          = 1;

    p.require_uvs               = true;
    p.require_vertex_colors     = true;
    p.bake_lighting_to_diffuse  = true;

    p.target_silhouette_error = 0.020f;
    p.max_silhouette_error    = 0.050f;

    p.reference_height_m = 1.8f;
    p.turntable_views    = 8;

    p.cameras = {
        {"gameplay", "Third person follow camera, roughly three metres back",
         180.0f, 12.0f, 3.0f, 1.1f, 40.0f, 0.0f, true},
        {"cutscene_face", "Dialogue framing, head and shoulders",
         170.0f, 2.0f, 0.9f, 1.62f, 35.0f, 0.0f, true},
        {"silhouette_side", "Profile read, checks the body outline",
         90.0f, 0.0f, 3.2f, 1.0f, 38.0f, 0.0f, false},
        {"low_hero", "Low hero angle, exaggerates the shoulder line",
         200.0f, -18.0f, 2.4f, 0.7f, 45.0f, 0.0f, false},
    };

    p.art_direction =
        "Third person playable character. The face is visible in dialogue "
        "cutscenes, so the head deserves a disproportionate share of the budget. "
        "Hands are seen holding equipment but never in close up. The feet are in "
        "every gameplay shot and walk the ground: they need a readable heel, instep "
        "and toe box, about a hand's worth of triangles each, but no toes. The back "
        "of the legs is rarely framed.";
    p.max_iterations = 4;
    return p;
}

TargetProfile TargetProfile::ps2_prop_default()
{
    TargetProfile p;
    p.name        = "ps2_prop";
    p.description = "Static world prop, PlayStation 2 class budget.";

    p.max_triangles       = 400;
    p.max_vertices        = 380;
    p.max_shells          = 0;
    p.max_bone_influences = 0;
    p.require_symmetry    = false;
    p.require_strips      = true;
    p.min_average_strip_len = 4.0f;

    p.texture.width          = 128;
    p.texture.height         = 128;
    p.texture.palette_colors = 16;

    p.require_vertex_colors = true;

    p.target_silhouette_error = 0.025f;
    p.max_silhouette_error    = 0.060f;

    p.reference_height_m = 1.0f;
    p.turntable_views    = 6;
    p.cameras = {
        {"gameplay", "Standing player looking at the prop",
         180.0f, 20.0f, 2.0f, 0.5f, 45.0f, 0.0f, true},
    };
    p.art_direction =
        "Static prop seen from walking distance. Never inspected up close. "
        "Silhouette matters far more than surface detail.";
    p.max_iterations = 3;
    return p;
}

Json TargetProfile::to_json() const
{
    Json j;
    j["name"]        = name;
    j["description"] = description;

    Json g;
    g["max_triangles"]         = max_triangles;
    g["max_vertices"]          = max_vertices;
    g["budget_tolerance"]      = budget_tolerance;
    g["max_shells"]            = max_shells;
    g["max_bone_influences"]   = max_bone_influences;
    g["require_manifold"]      = require_manifold;
    g["allow_ngons"]           = allow_ngons;
    g["require_symmetry"]      = require_symmetry;
    g["require_strips"]        = require_strips;
    g["min_average_strip_len"] = min_average_strip_len;
    g["vertex_cache_size"]     = vertex_cache_size;
    j["geometry"] = g;

    Json t;
    t["width"]          = texture.width;
    t["height"]         = texture.height;
    t["palette_colors"] = texture.palette_colors;
    t["dithering"]      = texture.dithering;
    t["count"]          = texture.count;
    t["require_uvs"]              = require_uvs;
    t["require_vertex_colors"]    = require_vertex_colors;
    t["bake_lighting_to_diffuse"] = bake_lighting_to_diffuse;
    if (!texture.extra_pages.empty()) {
        Json pages = Json::array();
        for (const TexturePage& pg : texture.extra_pages)
            pages.push_back(Json{{"name", pg.name}, {"width", pg.width}, {"height", pg.height},
                                 {"camera", pg.camera}});
        t["pages"] = pages;
    }
    t["filtering"] = texture.bilinear ? "bilinear" : "nearest";
    t["uv_layout"] = texture.uv_layout;
    j["texture"] = t;

    Json q;
    q["target_silhouette_error"] = target_silhouette_error;
    q["max_silhouette_error"]    = max_silhouette_error;
    j["quality"] = q;

    Json f;
    f["reference_height_m"] = reference_height_m;
    f["turntable_views"]    = turntable_views;
    f["turntable_pitches"]  = turntable_pitches;
    Json cams = Json::array();
    for (const ProfileCamera& c : cameras) cams.push_back(camera_to_json(c));
    f["cameras"] = cams;
    j["framing"] = f;

    Json d;
    d["art_direction"]  = art_direction;
    d["max_iterations"] = max_iterations;
    j["director"] = d;

    return j;
}

TargetProfile TargetProfile::from_json(const Json& j, std::string* error)
{
    TargetProfile p;
    if (!j.is_object()) {
        if (error) *error = "profile root is not an object";
        return p;
    }

    p.name        = json_get<std::string>(j, "name", p.name);
    p.description = json_get<std::string>(j, "description", p.description);

    const Json& g = json_object_or_empty(j, "geometry");
    p.max_triangles         = json_get<int>(g, "max_triangles", p.max_triangles);
    p.max_vertices          = json_get<int>(g, "max_vertices", p.max_vertices);
    p.budget_tolerance      = json_get<float>(g, "budget_tolerance", p.budget_tolerance);
    p.max_shells            = json_get<int>(g, "max_shells", p.max_shells);
    p.max_bone_influences   = json_get<int>(g, "max_bone_influences", p.max_bone_influences);
    p.require_manifold      = json_get<bool>(g, "require_manifold", p.require_manifold);
    p.allow_ngons           = json_get<bool>(g, "allow_ngons", p.allow_ngons);
    p.require_symmetry      = json_get<bool>(g, "require_symmetry", p.require_symmetry);
    p.require_strips        = json_get<bool>(g, "require_strips", p.require_strips);
    p.min_average_strip_len = json_get<float>(g, "min_average_strip_len", p.min_average_strip_len);
    p.vertex_cache_size     = json_get<int>(g, "vertex_cache_size", p.vertex_cache_size);

    const Json& t = json_object_or_empty(j, "texture");
    p.texture.width          = json_get<int>(t, "width", p.texture.width);
    p.texture.height         = json_get<int>(t, "height", p.texture.height);
    p.texture.palette_colors = json_get<int>(t, "palette_colors", p.texture.palette_colors);
    p.texture.dithering      = json_get<bool>(t, "dithering", p.texture.dithering);
    p.texture.bilinear       = json_get<std::string>(t, "filtering", "bilinear") != "nearest";
    p.texture.uv_layout      = json_get<std::string>(t, "uv_layout", p.texture.uv_layout);
    p.texture.count          = json_get<int>(t, "count", p.texture.count);
    p.texture.extra_pages.clear();
    for (const Json& pg : json_array_or_empty(t, "pages")) {
        if (!pg.is_object()) continue;
        TexturePage page;
        page.name   = json_get<std::string>(pg, "name", page.name);
        page.width  = std::clamp(json_get<int>(pg, "width", page.width), 16, 4096);
        page.height = std::clamp(json_get<int>(pg, "height", page.height), 16, 4096);
        page.camera = json_get<std::string>(pg, "camera", page.camera);
        p.texture.extra_pages.push_back(page);
    }
    p.require_uvs               = json_get<bool>(t, "require_uvs", p.require_uvs);
    p.require_vertex_colors     = json_get<bool>(t, "require_vertex_colors", p.require_vertex_colors);
    p.bake_lighting_to_diffuse  = json_get<bool>(t, "bake_lighting_to_diffuse", p.bake_lighting_to_diffuse);

    const Json& q = json_object_or_empty(j, "quality");
    p.target_silhouette_error = json_get<float>(q, "target_silhouette_error", p.target_silhouette_error);
    p.max_silhouette_error    = json_get<float>(q, "max_silhouette_error", p.max_silhouette_error);

    const Json& f = json_object_or_empty(j, "framing");
    p.reference_height_m = json_get<float>(f, "reference_height_m", p.reference_height_m);
    p.turntable_views    = json_get<int>(f, "turntable_views", p.turntable_views);
    {
        std::vector<float> pitches;
        for (const Json& v : json_array_or_empty(f, "turntable_pitches"))
            if (v.is_number()) pitches.push_back(std::clamp(v.get<float>(), -80.0f, 80.0f));
        if (!pitches.empty()) p.turntable_pitches = pitches;
    }
    const Json& cams = json_array_or_empty(f, "cameras");
    if (!cams.empty()) {
        p.cameras.clear();
        for (const Json& c : cams) p.cameras.push_back(camera_from_json(c));
    }

    const Json& d = json_object_or_empty(j, "director");
    p.art_direction  = json_get<std::string>(d, "art_direction", p.art_direction);
    p.max_iterations = json_get<int>(d, "max_iterations", p.max_iterations);

    p.clamp();
    return p;
}

void TargetProfile::clamp()
{
    max_triangles       = std::clamp(max_triangles, 12, 2000000);
    max_vertices        = std::clamp(max_vertices, 8, 2000000);
    budget_tolerance    = std::clamp(budget_tolerance, 0.0f, 0.25f);
    max_shells          = std::max(0, max_shells);
    max_bone_influences = std::clamp(max_bone_influences, 0, 4);
    vertex_cache_size   = std::clamp(vertex_cache_size, 4, 64);
    min_average_strip_len = clampf(min_average_strip_len, 1.0f, 64.0f);

    texture.width  = std::clamp(texture.width, 8, 4096);
    texture.height = std::clamp(texture.height, 8, 4096);
    texture.count  = std::clamp(texture.count, 1, 8);
    // Palette sizes below 2 make no sense; 0 keeps truecolor.
    if (texture.palette_colors != 0)
        texture.palette_colors = std::clamp(texture.palette_colors, 2, 256);

    target_silhouette_error = clampf(target_silhouette_error, 0.0f, 1.0f);
    max_silhouette_error    = clampf(max_silhouette_error, target_silhouette_error, 1.0f);

    reference_height_m = std::max(reference_height_m, 0.01f);
    turntable_views    = std::clamp(turntable_views, 0, 32);
    max_iterations     = std::clamp(max_iterations, 0, 20);

    for (ProfileCamera& c : cameras) {
        c.fov_degrees  = clampf(c.fov_degrees, 5.0f, 150.0f);
        c.distance_m   = std::max(c.distance_m, 0.0f);
        c.fit_fraction = clampf(c.fit_fraction, 0.0f, 1.0f);
        if (c.distance_m <= 0.0f && c.fit_fraction <= 0.0f) c.fit_fraction = 0.9f;
    }
    if (cameras.empty() && turntable_views == 0) turntable_views = 6;
}

std::vector<std::string> TargetProfile::warnings() const
{
    std::vector<std::string> w;
    if (max_vertices > max_triangles)
        w.push_back("max_vertices exceeds max_triangles, which is unusual for a "
                    "closed mesh (expect roughly half as many vertices as triangles)");
    if (require_strips && min_average_strip_len < 2.0f)
        w.push_back("min_average_strip_len below 2 makes the strip requirement meaningless");
    if (texture.palette_colors != 0 && (texture.palette_colors & (texture.palette_colors - 1)))
        w.push_back("palette_colors is not a power of two; most console CLUT "
                    "formats will round it up");
    if (cameras.empty() && turntable_views == 0)
        w.push_back("no cameras and no turntable: the director would be blind");
    if (max_bone_influences == 0 && require_symmetry == false && max_shells == 1)
        w.push_back("single shell required for an unskinned asset; loose parts will fail");
    return w;
}

bool TargetProfile::load(const std::string& path, std::string* error)
{
    Json j;
    std::string err;
    if (!json_load_file(path, j, err)) {
        if (error) *error = err;
        return false;
    }
    *this = from_json(j, error);
    return true;
}

bool TargetProfile::save(const std::string& path, std::string* error) const
{
    std::string err;
    if (!json_save_file(path, to_json(), err)) {
        if (error) *error = err;
        return false;
    }
    return true;
}

} // namespace rd
