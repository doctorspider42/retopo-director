#include "validate/validator.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>

namespace rd {
namespace {

int count_influences(const Mesh& mesh)
{
    int worst = 0;
    if (!mesh.has_skin()) return 0;
    for (const SkinVertex& s : mesh.skin) {
        int n = 0;
        for (int i = 0; i < 4; ++i)
            if (s.weights[i] > 1e-4f) ++n;
        worst = std::max(worst, n);
    }
    return worst;
}

// Cheap self symmetry measure: mirror every vertex and look for a partner.
float measure_symmetry(const Mesh& mesh, const SymmetryPlane& plane, float tolerance)
{
    if (mesh.empty()) return 0.0f;
    Bvh bvh;
    bvh.build(mesh);
    if (bvh.empty()) return 0.0f;

    size_t matched = 0;
    const size_t stride = std::max<size_t>(1, mesh.vertex_count() / 2048);
    size_t tested = 0;

    for (size_t v = 0; v < mesh.vertex_count(); v += stride) {
        const Vec3 mirrored = plane.mirror(mesh.positions[v]);
        const ClosestHit hit = bvh.closest_point(mirrored, tolerance * 4.0f);
        ++tested;
        if (hit.hit() && std::sqrt(hit.distance2) <= tolerance) ++matched;
    }
    return tested ? float(double(matched) / double(tested)) : 0.0f;
}

} // namespace

const char* severity_name(Severity s)
{
    switch (s) {
    case Severity::Error:   return "error";
    case Severity::Warning: return "warning";
    default:                return "info";
    }
}

void ValidationReport::add(Check c)
{
    if (!c.passed) {
        if (c.severity == Severity::Error)   ++errors;
        else if (c.severity == Severity::Warning) ++warnings;
    }
    checks.push_back(std::move(c));
}

std::string ValidationReport::failure_text() const
{
    std::string out;
    int n = 0;
    for (const Check& c : checks) {
        if (c.passed || c.severity == Severity::Info) continue;
        out += format("%d. [%s] %s\n", ++n, severity_name(c.severity), c.detail.c_str());
    }
    if (out.empty()) out = "No failures.\n";
    return out;
}

std::string ValidationReport::full_text() const
{
    std::string out;
    out += format("Validation: %s (%d error%s, %d warning%s)\n",
                  passed ? "PASSED" : "FAILED",
                  errors, errors == 1 ? "" : "s",
                  warnings, warnings == 1 ? "" : "s");
    out += "------------------------------------------------------------\n";
    for (const Check& c : checks) {
        out += format("%-8s %-34s %s\n", c.passed ? "ok" : severity_name(c.severity),
                      c.title.c_str(), c.detail.c_str());
    }
    return out;
}

Json ValidationReport::to_json() const
{
    Json j;
    j["passed"]   = passed;
    j["errors"]   = errors;
    j["warnings"] = warnings;
    j["symmetry_score"] = symmetry_score;

    Json arr = Json::array();
    for (const Check& c : checks) {
        Json e;
        e["id"]       = c.id;
        e["title"]    = c.title;
        e["detail"]   = c.detail;
        e["severity"] = severity_name(c.severity);
        e["passed"]   = c.passed;
        e["value"]    = c.value;
        e["limit"]    = c.limit;
        if (!c.scope.empty()) e["scope"] = c.scope;
        arr.push_back(e);
    }
    j["checks"] = arr;
    return j;
}

// ---------------------------------------------------------------------------
ValidationReport validate(const ValidationInput& in)
{
    ValidationReport rep;
    if (!in.mesh || !in.profile) {
        rep.add({"input.missing", "Input", "nothing to validate", Severity::Error, false});
        return rep;
    }

    const Mesh&          mesh    = *in.mesh;
    const TargetProfile& profile = *in.profile;
    const Mesh::Stats    stats   = mesh.compute_stats();

    // Counts come from the buffer that actually ships, but connectivity does
    // not: the unwrap splits vertices along every chart border, which would
    // otherwise be reported as a hundred separate shells with open edges.
    Mesh welded = mesh;
    welded.weld(std::max(stats.bounds.diagonal() * 1e-5f, 1e-7f));
    welded.remove_degenerate();
    welded.compact();
    const Mesh::Stats topology = welded.compute_stats();

    auto check = [&](std::string id, std::string title, bool ok, Severity sev,
                     double value, double limit, std::string detail,
                     std::string scope = {}) {
        Check c;
        c.id       = std::move(id);
        c.title    = std::move(title);
        c.detail   = std::move(detail);
        c.severity = sev;
        c.passed   = ok;
        c.value    = value;
        c.limit    = limit;
        c.scope    = std::move(scope);
        rep.add(std::move(c));
    };

    // --- geometry budgets ---------------------------------------------------
    {
        const int tri = int(stats.triangles);
        const bool ok = tri <= profile.max_triangles;
        check("geometry.triangle_count", "Triangle count", ok, Severity::Error,
              tri, profile.max_triangles,
              ok ? format("%d of %d triangles used (%.0f%% of budget)", tri,
                          profile.max_triangles,
                          100.0 * tri / std::max(1, profile.max_triangles))
                 : format("triangle budget exceeded by %d (%d used, limit %d)",
                          tri - profile.max_triangles, tri, profile.max_triangles));
    }
    {
        const int vtx = int(stats.vertices);
        const bool ok = vtx <= profile.max_vertices;
        check("geometry.vertex_count", "Vertex count", ok, Severity::Error,
              vtx, profile.max_vertices,
              ok ? format("%d of %d vertices used", vtx, profile.max_vertices)
                 : format("vertex budget exceeded by %d (%d used, limit %d)",
                          vtx - profile.max_vertices, vtx, profile.max_vertices));
    }
    if (profile.max_shells > 0) {
        const bool ok = int(topology.shells) <= profile.max_shells;
        check("geometry.shells", "Connected shells", ok, Severity::Error,
              double(topology.shells), profile.max_shells,
              ok ? format("%zu shell%s", topology.shells, topology.shells == 1 ? "" : "s")
                 : format("%zu separate shells but the profile allows %d",
                          topology.shells, profile.max_shells));
    }
    if (profile.require_manifold) {
        const bool ok = topology.manifold;
        check("geometry.manifold", "Manifold", ok, Severity::Error,
              double(topology.nonmanifold_edges), 0.0,
              ok ? "no non manifold edges"
                 : format("%zu non manifold edges remain", topology.nonmanifold_edges));
    }
    {
        // Triangles only, by construction, but the check documents the promise.
        check("geometry.ngons", "Polygon type", true, Severity::Info, 3.0, 3.0,
              "triangles only");
    }
    if (topology.boundary_edges > 0) {
        check("geometry.boundary", "Open boundary", false, Severity::Warning,
              double(topology.boundary_edges), 0.0,
              format("%zu open boundary edges; the mesh is not watertight",
                     topology.boundary_edges));
    }
    {
        const bool ok = topology.min_quality > 0.02f;
        check("geometry.slivers", "Triangle shape", ok, Severity::Warning,
              topology.min_quality, 0.02,
              ok ? format("worst triangle quality %.3f", topology.min_quality)
                 : format("degenerate slivers present, worst quality %.4f",
                          topology.min_quality));
    }
    // Seam vertices are real cost, so say how much of the budget they take.
    if (stats.vertices > topology.vertices) {
        const size_t seam = stats.vertices - topology.vertices;
        check("texture.seam_vertices", "Seam vertices", true, Severity::Info,
              double(seam), double(stats.vertices),
              format("%zu of %zu vertices exist only to carry a uv seam (%.0f%%)",
                     seam, stats.vertices,
                     100.0 * double(seam) / double(std::max<size_t>(stats.vertices, 1))));
    }

    // --- symmetry -----------------------------------------------------------
    if (profile.require_symmetry) {
        SymmetryPlane plane;
        if (in.source_analysis && in.source_analysis->symmetry.accepted)
            plane = in.source_analysis->symmetry;
        else
            plane.offset = dot(plane.normal, mesh.bounds().center());

        const float tolerance = std::max(mesh.bounds().diagonal() * 0.005f, 1e-6f);
        rep.symmetry_score = measure_symmetry(mesh, plane, tolerance);

        const bool ok = rep.symmetry_score >= 0.97f;
        check("geometry.symmetry", "Symmetry", ok, Severity::Error,
              rep.symmetry_score, 0.97,
              ok ? format("%.1f%% of vertices mirror across %s",
                          rep.symmetry_score * 100.0f, plane.axis_name())
                 : format("only %.1f%% of vertices mirror across %s; the profile "
                          "requires a symmetric asset",
                          rep.symmetry_score * 100.0f, plane.axis_name()));
    }

    // --- skinning -----------------------------------------------------------
    if (profile.max_bone_influences > 0) {
        const int worst = count_influences(mesh);
        const bool ok = worst <= profile.max_bone_influences;
        check("skin.influences", "Bone influences", ok, Severity::Error,
              worst, profile.max_bone_influences,
              ok ? format("at most %d influences per vertex", worst)
                 : format("a vertex carries %d influences, the target allows %d",
                          worst, profile.max_bone_influences));
        if (!mesh.has_skin() && !mesh.armature.empty())
            check("skin.missing", "Skinning", false, Severity::Warning, 0, 1,
                  "the asset has an armature but no skin weights survived");
    }

    // --- strips -------------------------------------------------------------
    if (profile.require_strips) {
        if (!in.strips || in.strips->indices.empty()) {
            check("export.strips", "Triangle strips", false, Severity::Error, 0,
                  profile.min_average_strip_len,
                  "the profile requires strips but none were generated");
        } else {
            const bool ok = in.strips->average_length >= profile.min_average_strip_len;
            check("export.strips", "Triangle strips", ok, Severity::Error,
                  in.strips->average_length, profile.min_average_strip_len,
                  ok ? format("%zu strips, %.2f triangles each",
                              in.strips->strip_count, in.strips->average_length)
                     : format("average strip length %.2f is below the required %.2f "
                              "(%zu strips); the topology is too fragmented",
                              in.strips->average_length, profile.min_average_strip_len,
                              in.strips->strip_count));
        }
    }

    // --- texturing ----------------------------------------------------------
    if (profile.require_uvs) {
        const bool ok = mesh.has_uvs();
        check("texture.uvs", "UV coordinates", ok, Severity::Error, ok ? 1 : 0, 1,
              ok ? "uv set present" : "no uv coordinates on the low poly");
    }
    if (profile.require_vertex_colors) {
        const bool ok = mesh.has_colors();
        check("texture.vertex_colors", "Vertex colours", ok, Severity::Error, ok ? 1 : 0, 1,
              ok ? "vertex colours present" : "vertex colours are required but missing");
    }
    if (in.bake) {
        const BakeResult& bake = *in.bake;
        if (!bake.ok) {
            check("texture.bake", "Bake", false, Severity::Error, 0, 1,
                  bake.error.empty() ? "the bake failed" : bake.error);
        } else {
            {
                const bool ok = bake.diffuse.width <= profile.texture.width &&
                                bake.diffuse.height <= profile.texture.height;
                check("texture.size", "Texture size", ok, Severity::Error,
                      double(bake.diffuse.width), profile.texture.width,
                      ok ? format("%dx%d", bake.diffuse.width, bake.diffuse.height)
                         : format("baked %dx%d but the profile caps it at %dx%d",
                                  bake.diffuse.width, bake.diffuse.height,
                                  profile.texture.width, profile.texture.height));
            }
            if (profile.texture.palette_colors > 0) {
                const int used = int(bake.palette.size());
                const bool ok = used > 0 && used <= profile.texture.palette_colors;
                check("texture.palette", "Palette", ok, Severity::Error,
                      used, profile.texture.palette_colors,
                      ok ? format("%d colours of %d allowed", used,
                                  profile.texture.palette_colors)
                         : format("palette has %d entries, the target allows %d",
                                  used, profile.texture.palette_colors));
            }
            {
                const bool ok = bake.atlas_count <= profile.texture.count;
                check("texture.pages", "Atlas pages", ok, Severity::Error,
                      bake.atlas_count, profile.texture.count,
                      ok ? format("%d page%s", bake.atlas_count,
                                  bake.atlas_count == 1 ? "" : "s")
                         : format("unwrap needs %d atlas pages, the target allows %d",
                                  bake.atlas_count, profile.texture.count));
            }
            {
                const bool ok = bake.uv_utilisation >= 0.35f;
                check("texture.utilisation", "Atlas utilisation", ok, Severity::Warning,
                      bake.uv_utilisation, 0.35,
                      format("%.1f%% of the atlas carries geometry",
                             bake.uv_utilisation * 100.0f));
            }
            if (bake.uv_max_stretch > 0.0f) {
                const bool ok = bake.uv_max_stretch <= 4.0f;
                check("texture.stretch", "UV stretch", ok, Severity::Warning,
                      bake.uv_max_stretch, 4.0,
                      ok ? format("worst texel density ratio %.2f", bake.uv_max_stretch)
                         : format("one chart is %.1fx denser than average; expect visible "
                                  "blur or aliasing there", bake.uv_max_stretch));
            }
        }
    }

    // --- per region budgets -------------------------------------------------
    if (in.segmentation && in.panel && in.region_triangles) {
        const std::vector<int>& actual = *in.region_triangles;
        for (const Region& r : in.segmentation->regions) {
            const RegionKnobs* k = in.panel->find(r.id);
            if (!k || k->triangle_budget <= 0) continue;
            const int got = r.id < actual.size() ? actual[r.id] : 0;
            const int want = k->triangle_budget;
            const float miss = float(got - want) / float(std::max(1, want));
            if (std::fabs(miss) <= in.budget_tolerance) continue;

            const bool over = got > want;
            check("budget.region", "Region budget", false,
                  over ? Severity::Warning : Severity::Info,
                  got, want,
                  over ? format("region '%s' is %d triangles over budget (%d used, %d "
                                "allocated)", r.name.c_str(), got - want, got, want)
                       : format("region '%s' used only %d of its %d triangles; that %d "
                                "could go somewhere it shows",
                                r.name.c_str(), got, want, want - got),
                  r.name);
        }
    }

    rep.passed = rep.errors == 0;
    RD_INFO("validation: %s, %d errors, %d warnings", rep.passed ? "passed" : "failed",
            rep.errors, rep.warnings);
    return rep;
}

} // namespace rd
