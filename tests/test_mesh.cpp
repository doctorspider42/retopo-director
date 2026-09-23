// BVH, topology, welding, winding and the segmentation bookkeeping: each of the
// "things that will bite you" in CLAUDE.md, pinned down by a test.

#include "test.h"

#include "geom/hard_rules.h"
#include "mesh/analysis.h"
#include "mesh/bvh.h"
#include "mesh/io.h"
#include "mesh/topology.h"
#include "mesh/visibility.h"
#include "segment/segment.h"

#include <filesystem>
#include <random>

using namespace rd;

namespace {

bool ray_triangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, float& t)
{
    const Vec3  e1 = b - a, e2 = c - a, p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vec3  s = o - a;
    const float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    const Vec3  q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t > 1e-4f;
}

// Ericson, Real-Time Collision Detection 5.1.5.
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

Mesh bumpy_sphere()
{
    Mesh m = rdtest::icosphere(4);
    for (Vec3& p : m.positions) {
        const float r = 1.0f + 0.15f * std::sin(5 * p.y) * std::cos(3 * p.x) + 0.1f * p.x;
        p = p * r + Vec3(0.3f, -0.2f, 0.1f);
    }
    return m;
}

} // namespace

// The BVH is the thing that "silently returns nearly-correct nonsense" when a
// child index is wrong, so it is checked against brute force, not against
// itself: every ray and every closest point query must agree exactly.
TEST(bvh_matches_brute_force)
{
    const Mesh m = bumpy_sphere();
    Bvh bvh;
    bvh.build(m);
    REQUIRE(!bvh.empty());
    REQUIRE(bvh.mesh() == &m);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> U(-1.6f, 1.6f);
    int ray_mismatch = 0, closest_mismatch = 0;
    for (int i = 0; i < 400; ++i) {
        const Vec3 o{U(rng) * 2, U(rng) * 2, U(rng) * 2};
        const Vec3 d = normalize(Vec3{U(rng), U(rng), U(rng)} - o * 0.3f);

        float best = 1e30f;
        for (size_t t = 0; t < m.triangle_count(); ++t) {
            Vec3 a, b, c;
            m.tri_positions(t, a, b, c);
            float tt;
            if (ray_triangle(o, d, a, b, c, tt) && tt < best) best = tt;
        }
        const RayHit hit = bvh.intersect(o, d);
        if ((best < 1e30f) != hit.hit()) ++ray_mismatch;
        else if (hit.hit() && std::fabs(hit.t - best) > 1e-3f) ++ray_mismatch;

        const Vec3 p{U(rng), U(rng), U(rng)};
        float best_d2 = 1e30f;
        for (size_t t = 0; t < m.triangle_count(); ++t) {
            Vec3 a, b, c;
            m.tri_positions(t, a, b, c);
            best_d2 = std::min(best_d2, length2(closest_on_triangle(p, a, b, c) - p));
        }
        const ClosestHit ch = bvh.closest_point(p);
        if (!ch.hit() || std::fabs(std::sqrt(ch.distance2) - std::sqrt(best_d2)) > 1e-4f)
            ++closest_mismatch;
    }
    CHECK_EQ(ray_mismatch, 0);
    CHECK_EQ(closest_mismatch, 0);

    CHECK(bvh.inside(Vec3(0.3f, -0.2f, 0.1f)));
    CHECK(!bvh.inside(Vec3(5.0f, 5.0f, 5.0f)));
}

TEST(topology_of_closed_shapes)
{
    const Mesh s = rdtest::icosphere(3);
    MeshTopology ts;
    ts.build(s);
    CHECK(ts.manifold());
    CHECK_EQ(ts.edges.size(), s.triangle_count() * 3 / 2);
    size_t boundary = 0;
    for (size_t e = 0; e < ts.edges.size(); ++e) boundary += ts.is_boundary_edge(e);
    CHECK_EQ(boundary, size_t(0));

    const Mesh t = rdtest::torus(48, 16);
    const Mesh::Stats st = t.compute_stats();
    CHECK_EQ(st.shells, size_t(1));
    CHECK(st.closed);
    // Euler characteristic 0 for genus one.
    CHECK_EQ(long(st.vertices) - long(st.edges) + long(st.triangles), 0L);
}

TEST(weld_merges_split_vertices_and_counts_shells)
{
    // Two spheres, one with every triangle's corners unshared, as an exporter
    // that splits on every uv seam would write it.
    Mesh a = rdtest::icosphere(2);
    Mesh b = rdtest::icosphere(2, 0.5f);
    for (Vec3& p : b.positions) p = p + Vec3(3, 0, 0);

    Mesh split;
    for (size_t t = 0; t < b.triangle_count(); ++t)
        for (int c = 0; c < 3; ++c) {
            split.positions.push_back(b.tri_vertex(t, c));
            split.indices.push_back(uint32_t(split.positions.size() - 1));
        }
    mesh_append(a, split);

    const size_t removed = a.weld(1e-5f);
    CHECK(removed > 0);
    const Mesh::Stats st = a.compute_stats();
    CHECK_EQ(st.shells, size_t(2));
    CHECK(st.closed);
    CHECK(st.manifold);
}

// Orientation has to be fixed topologically, then signed per shell. Flip a
// random third of the faces and the fix must give back a consistently wound,
// outward facing surface.
TEST(fix_winding_restores_a_scrambled_sphere)
{
    Mesh m = rdtest::icosphere(3);
    const double v0 = rdtest::signed_volume(m);
    REQUIRE(v0 > 0);
    Bvh ref;
    const Mesh reference = m;
    ref.build(reference);

    std::mt19937 rng(7);
    for (size_t t = 0; t < m.triangle_count(); ++t)
        if (rng() % 3 == 0) std::swap(m.indices[t * 3 + 1], m.indices[t * 3 + 2]);
    // And invert the whole thing, so a fix that only makes the winding
    // consistent without choosing the global sign fails too.
    for (size_t t = 0; t < m.triangle_count(); ++t) std::swap(m.indices[t * 3 + 1], m.indices[t * 3 + 2]);

    fix_winding(m, ref);
    CHECK_NEAR(rdtest::signed_volume(m), v0, 1e-4);

    MeshTopology topo;
    topo.build(m);
    size_t inconsistent = 0;
    for (size_t h = 0; h < topo.opposite.size(); ++h) {
        const uint32_t o = topo.opposite[h];
        if (o == kInvalidIndex) continue;
        // A consistently wound pair walks the shared edge in opposite directions.
        if (topo.half_edge_start(m, uint32_t(h)) != topo.half_edge_end(m, o)) ++inconsistent;
    }
    CHECK_EQ(inconsistent, size_t(0));
}

// renumber the regions without renumbering tri_region in the same pass and
// every region silently reports somebody else's area.
TEST(finalize_segmentation_keeps_ids_and_areas_in_step)
{
    const Mesh m = bumpy_sphere();
    MeshAnalysis an;
    AnalysisOptions ao;
    ao.ambient_rays = 0;
    analyse_mesh(m, an, ao);
    REQUIRE(an.valid());

    // Sparse, out of order ids, split by octant.
    Segmentation seg;
    seg.tri_region.resize(m.triangle_count());
    const uint16_t ids[8] = {900, 3, 77, 12, 5000, 41, 8, 260};
    for (size_t t = 0; t < m.triangle_count(); ++t) {
        const Vec3 c = m.triangle_centroid(t) - Vec3(0.3f, -0.2f, 0.1f);
        seg.tri_region[t] = ids[(c.x > 0) | ((c.y > 0) << 1) | ((c.z > 0) << 2)];
    }
    SegmentationOptions so;
    so.min_area_share = 0.0f;
    finalize_segmentation(m, an, so, seg);
    REQUIRE(seg.valid());

    // Dense.
    for (size_t i = 0; i < seg.regions.size(); ++i) CHECK_EQ(size_t(seg.regions[i].id), i);
    // And each region's area is the area of the triangles that carry its id.
    std::vector<double> area(seg.regions.size(), 0.0);
    bool out_of_range = false;
    for (size_t t = 0; t < m.triangle_count(); ++t) {
        if (seg.tri_region[t] >= area.size()) { out_of_range = true; continue; }
        area[seg.tri_region[t]] += m.triangle_area(t);
    }
    CHECK(!out_of_range);
    for (size_t i = 0; i < seg.regions.size(); ++i)
        CHECK_NEAR(seg.regions[i].area, area[i], 1e-3 * (area[i] + 1e-6));
}

TEST(segment_mesh_labels_every_triangle)
{
    const Mesh m = bumpy_sphere();
    MeshAnalysis an;
    AnalysisOptions ao;
    ao.ambient_rays = 0;
    analyse_mesh(m, an, ao);
    Segmentation seg;
    segment_mesh(m, an, seg);
    REQUIRE(seg.valid());
    CHECK_EQ(seg.tri_region.size(), m.triangle_count());
    size_t unlabelled = 0;
    for (uint16_t r : seg.tri_region) unlabelled += r >= seg.regions.size();
    CHECK_EQ(unlabelled, size_t(0));
    double share = 0;
    for (const Region& r : seg.regions) share += r.area_share;
    CHECK_NEAR(share, 1.0, 1e-3);
}

TEST(symmetry_is_found_on_a_mirrored_shape_and_not_on_a_lopsided_one)
{
    Mesh sym = rdtest::icosphere(4);
    for (Vec3& p : sym.positions) p = p * (1.0f + 0.2f * std::cos(4 * p.y) * p.z * p.z);
    MeshAnalysis a;
    AnalysisOptions ao;
    ao.ambient_rays = 0;
    analyse_mesh(sym, a, ao);
    CHECK(a.symmetry.accepted);
    CHECK(std::fabs(a.symmetry.normal.x) > 0.99f);

    Mesh lop = rdtest::icosphere(4);
    for (Vec3& p : lop.positions) p = p * (1.0f + 0.35f * std::max(0.0f, p.x + p.y + p.z * 0.5f));
    MeshAnalysis b;
    analyse_mesh(lop, b, ao);
    CHECK(!b.symmetry.accepted);
}

TEST(obj_round_trip)
{
    const Mesh m = bumpy_sphere();
    const auto path = std::filesystem::temp_directory_path() / "rd_test_roundtrip.obj";
    std::string err;
    REQUIRE(meshio::save_obj(path, m, {}, &err));
    Mesh back;
    const meshio::LoadReport rep = meshio::load(path, back);
    std::filesystem::remove(path);
    REQUIRE(rep.ok);
    CHECK_EQ(back.triangle_count(), m.triangle_count());
    CHECK_EQ(back.compute_stats().shells, size_t(1));
}

// An icosahedron inscribed in a sphere has every face beneath the surface and
// about 60% of its volume. Fitting the faces must win most of that back, keep
// the shape closed, and not move it off centre.
TEST(fit_to_surface_undoes_inscribed_shrinkage)
{
    const Mesh high = rdtest::icosphere(5);
    Bvh bvh;
    bvh.build(high);
    Mesh low = rdtest::icosphere(1);
    const double v_high = rdtest::signed_volume(high);
    const double before = rdtest::signed_volume(low) / v_high;

    fit_to_surface(low, bvh, 6, nullptr, 1e-4f);
    const double after = rdtest::signed_volume(low) / v_high;
    CHECK(before < 0.9);
    CHECK(std::fabs(after - 1.0) < std::fabs(before - 1.0) * 0.5);
    CHECK(after < 1.1);
    CHECK(low.compute_stats().closed);
    Vec3 c{};
    for (const Vec3& p : low.positions) c = c + p;
    CHECK(length(c / float(low.vertex_count())) < 1e-3f);
}

TEST(fit_to_surface_keeps_a_mirrored_mesh_mirrored)
{
    Mesh high = rdtest::icosphere(5);
    for (Vec3& p : high.positions) p = p * (1.0f + 0.2f * std::cos(3 * p.y) * std::fabs(p.x));
    Bvh bvh;
    bvh.build(high);
    Mesh low = rdtest::icosphere(2);
    for (Vec3& p : low.positions) p = p * (1.0f + 0.2f * std::cos(3 * p.y) * std::fabs(p.x));

    SymmetryPlane plane;
    plane.normal   = {1, 0, 0};
    plane.offset   = 0;
    plane.accepted = true;
    fit_to_surface(low, bvh, 4, &plane, 1e-4f);

    // Every vertex still has a partner at its reflection.
    size_t unmatched = 0;
    for (const Vec3& p : low.positions) {
        const Vec3 m = plane.mirror(p);
        float best = 1e30f;
        for (const Vec3& q : low.positions) best = std::min(best, length2(q - m));
        unmatched += best > 1e-8f;
    }
    CHECK_EQ(unmatched, size_t(0));
}

// Thickness is the distance through the model, so a capsule-ish tube of radius
// r reads about 2r along its sides, and a fat sphere reads its diameter.
TEST(analysis_thickness_measures_the_way_through)
{
    Mesh tube = rdtest::icosphere(4);
    for (Vec3& p : tube.positions) p = Vec3(p.x * 0.1f, p.y * 1.0f, p.z * 0.1f);
    MeshAnalysis a;
    AnalysisOptions ao;
    ao.ambient_rays = 0;
    analyse_mesh(tube, a, ao);
    REQUIRE(a.thickness.size() == tube.vertex_count());
    // Around the middle of the tube, where the sides are straight.
    double sum = 0;
    int n = 0;
    for (size_t v = 0; v < tube.vertex_count(); ++v)
        if (std::fabs(tube.positions[v].y) < 0.3f) { sum += a.thickness[v]; ++n; }
    REQUIRE(n > 0);
    // The radius at |y| < 0.3 is between 0.095 and 0.1.
    CHECK_NEAR(sum / n, 0.195, 0.02);
}

// An eyeball mostly sunk into the head and a patch lying on the skin are
// dropped from the low poly; a small piece standing clear of the body is not.
TEST(hidden_and_flat_pieces_are_dropped_and_standing_pieces_kept)
{
    Mesh src = rdtest::icosphere(4);                            // the body
    Mesh eye = rdtest::icosphere(2, 0.12f);
    for (Vec3& p : eye.positions) p = p + Vec3(0.0f, 0.95f, 0.0f);
    Mesh patch = rdtest::icosphere(1);   // on the front, clear of the eye
    for (Vec3& p : patch.positions) p = Vec3(p.x * 0.06f, p.y * 0.06f, 1.004f + p.z * 0.002f);
    Mesh knob = rdtest::icosphere(2, 0.1f);
    for (Vec3& p : knob.positions) p = p + Vec3(1.6f, 0.0f, 0.0f);
    mesh_append(src, eye);
    mesh_append(src, patch);
    mesh_append(src, knob);

    MeshAnalysis an;
    AnalysisOptions ao;
    ao.ambient_rays = 0;
    analyse_mesh(src, an, ao);
    REQUIRE(an.shell_area_share.size() == 4);

    HardRuleOptions rules;
    int droppable = 0;
    for (uint32_t s = 0; s < 4; ++s) droppable += source_shell_is_droppable(an, s, rules);
    CHECK_EQ(droppable, 2);
    CHECK(an.shell_visible_share[2] > 0.4f);   // the patch is seen, and dropped as flat

    Mesh low = src;
    const size_t removed = drop_hidden_shells(low, an.bvh, an, rules.hidden_visible_share,
                                              rules.hidden_max_area_share,
                                              rules.decal_max_offset_rel);
    CHECK_EQ(removed, eye.triangle_count() + patch.triangle_count());
    CHECK_EQ(low.compute_stats().shells, size_t(2));
}

TEST(visibility_sees_both_faces_of_a_thin_plate)
{
    Mesh patch = rdtest::icosphere(1);
    for (Vec3& p : patch.positions) p = Vec3(p.x * 0.06f, 1.004f + p.y * 0.002f, p.z * 0.06f);
    Bvh bvh;
    bvh.build(patch);
    std::vector<uint8_t> hidden;
    find_enclosed_triangles(patch, bvh, hidden);
    size_t n = 0;
    for (uint8_t h : hidden) n += h;
    CHECK_EQ(n, size_t(0));
}
