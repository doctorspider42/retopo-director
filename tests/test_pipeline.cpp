// The deterministic half end to end, in process, with no renderer: what a CI box
// with no GPU can still say about a run.

#include "test.h"

#include "core/paths.h"
#include "mesh/io.h"
#include "pipeline/pipeline.h"

#include <filesystem>

using namespace rd;
namespace fs = std::filesystem;

namespace {

struct RunOutcome {
    bool   finished = false;
    Stage  stage    = Stage::Idle;
    size_t triangles = 0, vertices = 0, source_triangles = 0;
    bool   passed = false;
    std::vector<std::string> failed;
    size_t tri_region = 0, colors = 0, uvs = 0;
    int    budget = 0, vertex_budget = 0;
};

RunOutcome run_on(const Mesh& source, const char* name, const TargetProfile& profile)
{
    const fs::path dir = fs::temp_directory_path() / "rd_test_pipeline" / name;
    fs::create_directories(dir);
    paths::set_project_dir(dir);
    const fs::path mesh_path = dir / (std::string(name) + ".obj");
    meshio::save_obj(mesh_path, source);

    PipelineSettings s;
    s.profile      = profile;
    s.use_llm      = false;
    s.auto_export  = false;
    s.write_report = false;

    Pipeline p;
    RunOutcome out;
    if (!p.start(mesh_path, s)) return out;
    p.join();
    out.finished = true;
    out.stage    = p.stage();
    p.with_results([&](const PipelineResults& r) {
        out.triangles        = r.lowpoly.triangle_count();
        out.vertices         = r.lowpoly.vertex_count();
        out.source_triangles = r.highpoly.triangle_count();
        out.passed           = r.validation.passed;
        for (const auto& c : r.validation.checks)
            if (!c.passed && c.severity == Severity::Error) out.failed.push_back(c.id);
        out.tri_region = r.lowpoly.tri_region.size();
        out.colors     = r.lowpoly.colors.size();
        out.uvs        = r.lowpoly.uvs.size();
    });
    out.budget        = profile.max_triangles;
    out.vertex_budget = profile.max_vertices;
    return out;
}

void expect_good_run(const RunOutcome& o)
{
    CHECK(o.finished);
    CHECK(o.stage == Stage::Done);
    CHECK(o.triangles > 0);
    CHECK(o.triangles <= size_t(o.budget));
    CHECK(o.vertices <= size_t(o.vertex_budget));
    for (const std::string& id : o.failed) rdtest::fail(__FILE__, __LINE__, "failed check " + id);
    CHECK(o.passed);
    // The unwrap rebuilds the vertex buffer: everything indexed by vertex must
    // have been re-derived against the new one, everything by triangle kept.
    CHECK_EQ(o.tri_region, o.triangles);
    CHECK(o.colors == 0 || o.colors == o.vertices);
    CHECK_EQ(o.uvs, o.vertices);
}

} // namespace

TEST(pipeline_sphere_without_renderer)
{
    Mesh m = rdtest::icosphere(5);
    for (Vec3& p : m.positions) p = p * (1.0f + 0.05f * std::sin(6 * p.y));
    const RunOutcome o = run_on(m, "sphere", TargetProfile::ps2_character_default());
    expect_good_run(o);
    // Most of the budget should be spent on a shape this simple; a run that
    // leaves half of it on the table is not doing its job.
    CHECK(double(o.triangles) >= 0.6 * o.budget);
}

TEST(pipeline_torus_without_renderer)
{
    const Mesh m = rdtest::torus(160, 48);
    const RunOutcome o = run_on(m, "torus", TargetProfile::ps2_character_default());
    expect_good_run(o);
}

// A lopsided source cannot come out mirror symmetric, and the validator must
// not fail the run for faithfully keeping the shape it was given.
TEST(pipeline_asymmetric_source_is_not_failed_for_asymmetry)
{
    Mesh m = rdtest::icosphere(5);
    for (Vec3& p : m.positions) p = p * (1.0f + 0.3f * std::max(0.0f, p.x + 0.6f * p.y + 0.4f * p.z));
    TargetProfile profile = TargetProfile::ps2_character_default();
    profile.require_symmetry = true;
    const RunOutcome o = run_on(m, "lopsided", profile);
    expect_good_run(o);
}
