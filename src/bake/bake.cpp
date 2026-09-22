#include "bake/bake.h"

#include "core/log.h"
#include "core/thread_pool.h"
#include "core/util.h"

#include <xatlas.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace rd {
namespace {

struct TexelJob {
    int      x, y;
    Vec3     position;
    Vec3     normal;
    uint32_t triangle;
};

Vec3 sample_source_color(const Mesh& source, const RayHit& hit)
{
    if (!source.has_colors() || !hit.hit()) return Vec3{1.0f, 1.0f, 1.0f};
    const uint32_t i0 = source.indices[hit.triangle * 3 + 0];
    const uint32_t i1 = source.indices[hit.triangle * 3 + 1];
    const uint32_t i2 = source.indices[hit.triangle * 3 + 2];
    const float w = 1.0f - hit.u - hit.v;
    const Vec4 c = source.colors[i0] * w + source.colors[i1] * hit.u + source.colors[i2] * hit.v;
    return {saturate(c.x), saturate(c.y), saturate(c.z)};
}

Vec3 sample_source_color_at(const Mesh& source, const ClosestHit& hit)
{
    if (!source.has_colors() || !hit.hit()) return Vec3{1.0f, 1.0f, 1.0f};
    const uint32_t i0 = source.indices[hit.triangle * 3 + 0];
    const uint32_t i1 = source.indices[hit.triangle * 3 + 1];
    const uint32_t i2 = source.indices[hit.triangle * 3 + 2];

    const Vec3 a = source.positions[i0], b = source.positions[i1], c = source.positions[i2];
    const Vec3 v0 = b - a, v1 = c - a, v2 = hit.point - a;
    const float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
    const float d20 = dot(v2, v0), d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    float u = 0.0f, v = 0.0f;
    if (std::fabs(denom) > 1e-16f) {
        u = (d11 * d20 - d01 * d21) / denom;
        v = (d00 * d21 - d01 * d20) / denom;
    }
    const float w = 1.0f - u - v;
    const Vec4 col = source.colors[i0] * saturate(w) + source.colors[i1] * saturate(u) +
                     source.colors[i2] * saturate(v);
    return {saturate(col.x), saturate(col.y), saturate(col.z)};
}

// Half space triangle rasteriser over the UV domain, with a conservative
// half texel bleed so chart edges are covered.
template <typename Fn>
void rasterise_uv_triangle(const Vec2& a, const Vec2& b, const Vec2& c,
                           int width, int height, const Fn& emit)
{
    const float min_x = std::floor(std::min({a.x, b.x, c.x})) - 1.0f;
    const float max_x = std::ceil(std::max({a.x, b.x, c.x})) + 1.0f;
    const float min_y = std::floor(std::min({a.y, b.y, c.y})) - 1.0f;
    const float max_y = std::ceil(std::max({a.y, b.y, c.y})) + 1.0f;

    const int x0 = std::max(0, int(min_x));
    const int x1 = std::min(width - 1, int(max_x));
    const int y0 = std::max(0, int(min_y));
    const int y1 = std::min(height - 1, int(max_y));
    if (x1 < x0 || y1 < y0) return;

    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-12f) return;
    const float inv_area = 1.0f / area;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const Vec2 p{float(x) + 0.5f, float(y) + 0.5f};
            float w0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) * inv_area;
            float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            // The tolerance is what covers the seam texels.
            const float tol = -0.75f / std::max(1.0f, std::fabs(area));
            if (w0 < tol || w1 < tol || w2 < tol) continue;
            w0 = saturate(w0); w1 = saturate(w1); w2 = saturate(w2);
            const float sum = w0 + w1 + w2;
            if (sum <= kEps) continue;
            emit(x, y, w0 / sum, w1 / sum, w2 / sum);
        }
    }
}

} // namespace

std::vector<BakeLight> default_light_rig()
{
    // Key from the upper front left, cool fill from the opposite side, warm
    // bounce from below. Standard three point setup, baked flat.
    // Intensities are chosen so key + fill + rim + ambient peaks at roughly 1.0
    // on a surface facing the key. Anything hotter clips to white and throws
    // away exactly the shading information the bake exists to capture.
    return {
        {normalize(Vec3{-0.45f, 0.75f, 0.50f}), {1.00f, 0.97f, 0.90f}, 0.62f, true},
        {normalize(Vec3{0.70f, 0.20f, 0.35f}),  {0.62f, 0.70f, 0.85f}, 0.24f, false},
        {normalize(Vec3{0.05f, -0.80f, -0.40f}), {0.85f, 0.75f, 0.62f}, 0.14f, false},
    };
}

// ---------------------------------------------------------------------------
UnwrapResult unwrap_uvs(Mesh& mesh, int width, int height, int padding,
                        float stretch_tolerance)
{
    UnwrapResult res;
    if (mesh.empty()) {
        res.error = "mesh is empty";
        return res;
    }

    xatlas::Atlas* atlas = xatlas::Create();

    xatlas::MeshDecl decl;
    decl.vertexCount          = uint32_t(mesh.vertex_count());
    decl.vertexPositionData   = mesh.positions.data();
    decl.vertexPositionStride = sizeof(Vec3);
    // Normals are deliberately withheld. Ours are hard-edged for shading, and
    // xatlas would treat every crease as a normal seam and cut a chart there.
    // On a thousand-triangle mesh that is most of the edges.
    decl.indexCount  = uint32_t(mesh.indices.size());
    decl.indexData   = mesh.indices.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;

    const xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        res.error = std::string("xatlas rejected the mesh: ") + xatlas::StringForEnum(err);
        xatlas::Destroy(atlas);
        return res;
    }
    xatlas::AddMeshJoin(atlas);

    xatlas::ChartOptions chart;
    // A console budget mesh has a thousand triangles, not a million. xatlas'
    // defaults are tuned for dense scans and would shatter this into a hundred
    // ten-triangle charts, and every chart border duplicates its vertices - which
    // is what actually blows the vertex budget. Big charts, few seams.
    chart.maxCost               = 8.0f + 160.0f * clampf(stretch_tolerance, 0.01f, 1.0f);
    chart.maxIterations         = 4;
    chart.normalDeviationWeight = 0.5f;
    chart.roundnessWeight       = 0.02f;
    chart.straightnessWeight    = 1.0f;
    chart.normalSeamWeight      = 0.1f;
    chart.textureSeamWeight     = 0.25f;

    xatlas::PackOptions pack;
    pack.padding    = uint32_t(std::max(0, padding));
    pack.resolution = uint32_t(std::max(8, width));
    pack.bilinear   = true;
    pack.blockAlign = false;
    pack.bruteForce = true;

    xatlas::Generate(atlas, chart, pack);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) {
        res.error = "xatlas produced an empty atlas";
        xatlas::Destroy(atlas);
        return res;
    }

    const xatlas::Mesh& out = atlas->meshes[0];

    Mesh rebuilt;
    rebuilt.name             = mesh.name;
    rebuilt.armature         = mesh.armature;
    rebuilt.import_transform = mesh.import_transform;
    rebuilt.import_scale     = mesh.import_scale;
    rebuilt.positions.resize(out.vertexCount);
    rebuilt.uvs.resize(out.vertexCount);
    if (mesh.has_normals()) rebuilt.normals.resize(out.vertexCount);
    if (mesh.has_colors())  rebuilt.colors.resize(out.vertexCount);
    if (mesh.has_skin())    rebuilt.skin.resize(out.vertexCount);

    const float inv_w = 1.0f / float(atlas->width);
    const float inv_h = 1.0f / float(atlas->height);

    for (uint32_t v = 0; v < out.vertexCount; ++v) {
        const xatlas::Vertex& xv = out.vertexArray[v];
        const uint32_t src = xv.xref;
        rebuilt.positions[v] = mesh.positions[src];
        rebuilt.uvs[v]       = {xv.uv[0] * inv_w, xv.uv[1] * inv_h};
        if (!rebuilt.normals.empty()) rebuilt.normals[v] = mesh.normals[src];
        if (!rebuilt.colors.empty())  rebuilt.colors[v]  = mesh.colors[src];
        if (!rebuilt.skin.empty())    rebuilt.skin[v]    = mesh.skin[src];
    }

    rebuilt.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    if (mesh.tri_region.size() == mesh.triangle_count())
        rebuilt.tri_region = mesh.tri_region;   // face order is preserved by xatlas

    // xatlas splits a vertex per chart it belongs to, but a vertex interior to
    // one chart can still come back duplicated. Every duplicate is a vertex the
    // console has to transform, and on this budget that is the binding limit,
    // so merge back anything whose position and uv are identical.
    {
        struct Key {
            uint32_t src;
            float    u, v;
            bool operator==(const Key& o) const { return src == o.src && u == o.u && v == o.v; }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const
            {
                uint32_t bu, bv;
                std::memcpy(&bu, &k.u, 4);
                std::memcpy(&bv, &k.v, 4);
                return (size_t(k.src) * 0x9E3779B97F4A7C15ull) ^
                       (size_t(bu) * 0xC2B2AE3D27D4EB4Full) ^ (size_t(bv) * 0x165667B19E3779F9ull);
            }
        };

        std::unordered_map<Key, uint32_t, KeyHash> unique;
        unique.reserve(rebuilt.positions.size());
        std::vector<uint32_t> remap(rebuilt.positions.size(), kInvalidIndex);

        Mesh merged;
        merged.name             = rebuilt.name;
        merged.armature         = rebuilt.armature;
        merged.import_transform = rebuilt.import_transform;
        merged.import_scale     = rebuilt.import_scale;
        merged.tri_region       = rebuilt.tri_region;

        const bool keep_n = !rebuilt.normals.empty();
        const bool keep_c = !rebuilt.colors.empty();
        const bool keep_s = !rebuilt.skin.empty();

        for (uint32_t v = 0; v < rebuilt.positions.size(); ++v) {
            const Key key{out.vertexArray[v].xref, rebuilt.uvs[v].x, rebuilt.uvs[v].y};
            const auto it = unique.find(key);
            if (it != unique.end()) { remap[v] = it->second; continue; }

            const uint32_t nv = uint32_t(merged.positions.size());
            remap[v] = nv;
            unique.emplace(key, nv);
            merged.positions.push_back(rebuilt.positions[v]);
            merged.uvs.push_back(rebuilt.uvs[v]);
            if (keep_n) merged.normals.push_back(rebuilt.normals[v]);
            if (keep_c) merged.colors.push_back(rebuilt.colors[v]);
            if (keep_s) merged.skin.push_back(rebuilt.skin[v]);
        }

        if (merged.positions.size() < rebuilt.positions.size()) {
            merged.indices.reserve(rebuilt.indices.size());
            for (uint32_t i : rebuilt.indices) merged.indices.push_back(remap[i]);
            RD_DEBUG("uv weld: %zu -> %zu vertices", rebuilt.positions.size(),
                     merged.positions.size());
            rebuilt = std::move(merged);
        }
    }

    res.charts         = int(out.chartCount);
    res.atlas_count    = int(atlas->atlasCount);
    res.utilisation    = atlas->atlasCount > 0 ? atlas->utilization[0] : 0.0f;
    res.added_vertices = out.vertexCount > mesh.vertex_count()
                             ? out.vertexCount - mesh.vertex_count() : 0;
    res.ok = true;

    xatlas::Destroy(atlas);
    mesh = std::move(rebuilt);

    RD_INFO("uv unwrap: %d charts, %d atlas pages, %.1f%% utilised, +%zu seam vertices",
            res.charts, res.atlas_count, res.utilisation * 100.0f, res.added_vertices);
    return res;
}

// ---------------------------------------------------------------------------
BakeResult bake_all(Mesh& mesh, const Mesh& source, const Bvh& source_bvh,
                    const MeshAnalysis& source_analysis, const TargetProfile& profile,
                    const GlobalKnobs& knobs, const BakeOptions& opts,
                    const std::function<void(float, const char*)>& progress)
{
    Stopwatch watch;
    BakeResult result;

    if (mesh.empty()) {
        result.error = "no low poly to bake";
        return result;
    }

    auto report = [&](float f, const char* what) { if (progress) progress(f, what); };

    const int width   = opts.texture_width  > 0 ? opts.texture_width  : profile.texture.width;
    const int height  = opts.texture_height > 0 ? opts.texture_height : profile.texture.height;
    const int padding = std::max(0, opts.padding_texels > 0
                                        ? opts.padding_texels
                                        : int(std::lround(knobs.uv_padding_texels)));

    // --- 1. unwrap ----------------------------------------------------------
    report(0.02f, "unwrapping uvs");
    const UnwrapResult uv = unwrap_uvs(mesh, width, height, padding, knobs.uv_stretch_tolerance);
    if (!uv.ok) {
        result.error = uv.error;
        return result;
    }
    result.charts         = uv.charts;
    result.atlas_count    = uv.atlas_count;
    result.uv_utilisation = uv.utilisation;
    if (uv.atlas_count > profile.texture.count)
        result.messages.push_back(
            format("unwrap needed %d atlas pages but the profile allows %d",
                   uv.atlas_count, profile.texture.count));

    mesh.compute_normals(60.0f);

    // --- 2. collect texel jobs ---------------------------------------------
    report(0.15f, "rasterising the atlas");
    result.diffuse.resize(width, height, 4);
    result.coverage.resize(width, height);

    std::vector<TexelJob> jobs;
    jobs.reserve(size_t(width) * height / 3 + 64);

    const size_t tcount = mesh.triangle_count();
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t i0 = mesh.indices[t * 3 + 0];
        const uint32_t i1 = mesh.indices[t * 3 + 1];
        const uint32_t i2 = mesh.indices[t * 3 + 2];
        const Vec2 a{mesh.uvs[i0].x * width, mesh.uvs[i0].y * height};
        const Vec2 b{mesh.uvs[i1].x * width, mesh.uvs[i1].y * height};
        const Vec2 c{mesh.uvs[i2].x * width, mesh.uvs[i2].y * height};

        rasterise_uv_triangle(a, b, c, width, height,
                              [&](int x, int y, float w0, float w1, float w2) {
                                  if (result.coverage.at(x, y)) return;
                                  result.coverage.set(x, y);
                                  TexelJob job;
                                  job.x = x;
                                  job.y = y;
                                  job.position = mesh.positions[i0] * w0 +
                                                 mesh.positions[i1] * w1 +
                                                 mesh.positions[i2] * w2;
                                  job.normal = normalize(mesh.normals[i0] * w0 +
                                                         mesh.normals[i1] * w1 +
                                                         mesh.normals[i2] * w2);
                                  job.triangle = uint32_t(t);
                                  jobs.push_back(job);
                              });
    }
    result.texels_baked = jobs.size();

    if (jobs.empty()) {
        result.error = "the unwrap produced no covered texels";
        return result;
    }

    // --- 3. shade -----------------------------------------------------------
    report(0.25f, "casting occlusion rays");

    const float diag        = std::max(source_analysis.bbox_diagonal, kEps);
    const float bias        = diag * opts.ray_bias_rel;
    const float ao_distance = diag * clampf(opts.ao_distance_rel, 0.01f, 2.0f);
    const float search      = diag * clampf(opts.projection_distance_rel, 0.001f, 1.0f);
    const int   ao_rays     = opts.bake_ao && knobs.bake_ambient_occlusion
                                  ? std::max(1, opts.ao_rays) : 0;
    const float ao_strength = clampf(opts.ao_intensity > 0.0f ? opts.ao_intensity
                                                              : knobs.ao_intensity,
                                     0.0f, 1.0f);
    const std::vector<BakeLight> lights = default_light_rig();
    const bool do_lighting = opts.bake_lighting && profile.bake_lighting_to_diffuse;

    std::vector<Vec3> shaded(jobs.size(), Vec3{1.0f, 1.0f, 1.0f});
    std::atomic<uint64_t> rays{0};
    std::atomic<size_t>   done{0};

    ThreadPool::shared().parallel_ranges(
        jobs.size(), 64, [&](size_t begin, size_t end, unsigned lane) {
            Rng rng(0xBA4Eu + uint32_t(begin) * 2654435761u);
            uint64_t local_rays = 0;

            for (size_t i = begin; i < end; ++i) {
                const TexelJob& job = jobs[i];

                // Find where this texel sits on the high poly. Ray along the
                // normal first (that keeps overhangs honest), closest point as
                // a fallback.
                Vec3  base_color = Vec3{1.0f, 1.0f, 1.0f};
                Vec3  surface    = job.position;
                Vec3  normal     = job.normal;

                const RayHit out_hit = source_bvh.intersect(job.position + job.normal * bias,
                                                            job.normal, bias, search);
                const RayHit in_hit  = source_bvh.intersect(job.position - job.normal * bias,
                                                            -job.normal, bias, search);
                local_rays += 2;

                const RayHit* best = nullptr;
                if (out_hit.hit() && in_hit.hit()) best = out_hit.t <= in_hit.t ? &out_hit : &in_hit;
                else if (out_hit.hit())            best = &out_hit;
                else if (in_hit.hit())             best = &in_hit;

                if (best) {
                    const Vec3 dir = (best == &out_hit) ? job.normal : -job.normal;
                    surface    = job.position + dir * best->t;
                    normal     = source_bvh.shading_normal(*best);
                    if (dot(normal, job.normal) < 0.0f) normal = -normal;
                    base_color = sample_source_color(source, *best);
                } else {
                    const ClosestHit hit = source_bvh.closest_point(job.position, search);
                    if (hit.hit()) {
                        surface    = hit.point;
                        normal     = source_bvh.geometric_normal(hit.triangle);
                        if (dot(normal, job.normal) < 0.0f) normal = -normal;
                        base_color = sample_source_color_at(source, hit);
                    }
                }

                const Vec3 origin = surface + normal * bias;

                // Ambient occlusion.
                float ao = 1.0f;
                if (ao_rays > 0) {
                    Vec3 tangent, bitangent;
                    basis_from_normal(normal, tangent, bitangent);
                    int open = 0;
                    for (int r = 0; r < ao_rays; ++r) {
                        const Vec3 local = sample_cosine_hemisphere(rng.next_float(),
                                                                    rng.next_float());
                        const Vec3 dir = tangent * local.x + bitangent * local.y + normal * local.z;
                        if (!source_bvh.occluded(origin, dir, bias, ao_distance)) ++open;
                    }
                    local_rays += uint64_t(ao_rays);
                    ao = float(open) / float(ao_rays);
                    ao = lerpf(1.0f, ao, ao_strength);
                }

                // Direct lighting, flattened into the albedo.
                Vec3 light_sum{0.0f, 0.0f, 0.0f};
                if (do_lighting) {
                    for (const BakeLight& l : lights) {
                        const float ndl = saturate(dot(normal, l.direction));
                        if (ndl <= 0.0f) continue;
                        float shadow = 1.0f;
                        if (l.casts_shadow) {
                            shadow = source_bvh.occluded(origin, l.direction, bias, ao_distance * 2.0f)
                                         ? 0.25f : 1.0f;
                            ++local_rays;
                        }
                        light_sum += l.color * (l.intensity * ndl * shadow);
                    }
                    // Ambient term so nothing goes fully black on a console.
                    light_sum += Vec3{0.17f, 0.19f, 0.23f} * ao;
                } else {
                    light_sum = Vec3{1.0f, 1.0f, 1.0f};
                }

                Vec3 c = base_color * light_sum * ao;
                shaded[i] = {saturate(c.x), saturate(c.y), saturate(c.z)};
            }

            rays.fetch_add(local_rays, std::memory_order_relaxed);
            const size_t finished = done.fetch_add(end - begin, std::memory_order_relaxed) +
                                    (end - begin);
            if ((finished & 0x3FFF) == 0)
                report(0.25f + 0.50f * float(finished) / float(jobs.size()), "baking");
        });

    result.rays_cast = size_t(rays.load());

    for (size_t i = 0; i < jobs.size(); ++i)
        result.diffuse.set(jobs[i].x, jobs[i].y, Vec4{shaded[i], 1.0f});

    // --- 4. dilate ----------------------------------------------------------
    report(0.80f, "dilating seams");
    dilate(result.diffuse, result.coverage, std::max(1, padding));

    // --- 5. vertex colours --------------------------------------------------
    if (opts.bake_vertex_colors && knobs.bake_vertex_colors) {
        report(0.86f, "vertex colours");
        const size_t vcount = mesh.vertex_count();
        mesh.colors.assign(vcount, Vec4{1, 1, 1, 1});

        ThreadPool::shared().parallel_ranges(vcount, 64, [&](size_t b, size_t e, unsigned) {
            Rng rng(0xC01Fu + uint32_t(b) * 40503u);
            const int rays_per_vertex = ao_rays > 0 ? std::max(8, ao_rays / 3) : 0;

            for (size_t v = b; v < e; ++v) {
                const Vec3 p = mesh.positions[v];
                Vec3 n = mesh.has_normals() ? mesh.normals[v] : Vec3{0, 1, 0};

                Vec3 base{1, 1, 1};
                const ClosestHit hit = source_bvh.closest_point(p, search * 2.0f);
                Vec3 surface = p;
                if (hit.hit()) {
                    surface = hit.point;
                    const Vec3 sn = source_bvh.geometric_normal(hit.triangle);
                    if (dot(sn, n) > 0.0f) n = normalize(n + sn * 0.5f);
                    base = sample_source_color_at(source, hit);
                }
                const Vec3 origin = surface + n * bias;

                float ao = 1.0f;
                if (rays_per_vertex > 0) {
                    Vec3 tangent, bitangent;
                    basis_from_normal(n, tangent, bitangent);
                    int open = 0;
                    for (int r = 0; r < rays_per_vertex; ++r) {
                        const Vec3 local = sample_cosine_hemisphere(rng.next_float(),
                                                                    rng.next_float());
                        const Vec3 dir = tangent * local.x + bitangent * local.y + n * local.z;
                        if (!source_bvh.occluded(origin, dir, bias, ao_distance)) ++open;
                    }
                    ao = lerpf(1.0f, float(open) / float(rays_per_vertex), ao_strength);
                }

                Vec3 light_sum{0, 0, 0};
                if (do_lighting) {
                    for (const BakeLight& l : lights)
                        light_sum += l.color * (l.intensity * saturate(dot(n, l.direction)));
                    light_sum += Vec3{0.17f, 0.19f, 0.23f} * ao;
                } else {
                    light_sum = Vec3{1, 1, 1};
                }

                const Vec3 c = base * light_sum * ao;
                mesh.colors[v] = {saturate(c.x), saturate(c.y), saturate(c.z), 1.0f};
            }
        });
    }

    // --- 6. palette ---------------------------------------------------------
    const int palette_size = opts.palette_colors != 0 ? opts.palette_colors
                                                      : profile.texture.palette_colors;
    if (palette_size >= 2) {
        report(0.94f, "quantising to the palette");
        result.palette = build_palette(result.diffuse, result.coverage, palette_size);
        apply_palette(result.diffuse, result.coverage, result.palette,
                      opts.dither && profile.texture.dithering);
        result.messages.push_back(format("quantised to %zu colours", result.palette.size()));
    }

    // --- 7. uv stretch metric ----------------------------------------------
    {
        float worst = 0.0f;
        for (size_t t = 0; t < tcount; ++t) {
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            const float area3 = mesh.triangle_area(t);
            const Vec2 a = mesh.uvs[i0], b = mesh.uvs[i1], c = mesh.uvs[i2];
            const float area2 = 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) -
                                                 (b.y - a.y) * (c.x - a.x));
            if (area3 < 1e-12f || area2 < 1e-12f) continue;
            const float ratio = (area2 / area3);
            worst = std::max(worst, ratio);
        }
        // Normalise against the median-ish scale so the number means "how much
        // worse than the average texel density", not raw units.
        float mean_ratio = 0.0f;
        int   counted = 0;
        for (size_t t = 0; t < tcount; ++t) {
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            const float area3 = mesh.triangle_area(t);
            const Vec2 a = mesh.uvs[i0], b = mesh.uvs[i1], c = mesh.uvs[i2];
            const float area2 = 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) -
                                                 (b.y - a.y) * (c.x - a.x));
            if (area3 < 1e-12f || area2 < 1e-12f) continue;
            mean_ratio += area2 / area3;
            ++counted;
        }
        if (counted > 0 && mean_ratio > 0.0f)
            result.uv_max_stretch = worst / (mean_ratio / float(counted));
    }

    result.ok      = true;
    result.seconds = watch.seconds();
    report(1.0f, "done");

    RD_INFO("bake: %dx%d, %zu texels, %zu rays, %d charts, %.1f%% utilised, %s",
            width, height, result.texels_baked, result.rays_cast, result.charts,
            result.uv_utilisation * 100.0f, format_duration(result.seconds).c_str());
    return result;
}

} // namespace rd
