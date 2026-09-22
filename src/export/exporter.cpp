#include "export/exporter.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "mesh/io.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cstring>

namespace rd {
namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
}

void put_f32(std::vector<uint8_t>& out, float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put_u32(out, bits);
}

} // namespace

std::vector<std::vector<uint32_t>> split_strips(const StripData& data)
{
    std::vector<std::vector<uint32_t>> out;
    std::vector<uint32_t> current;
    for (uint32_t i : data.indices) {
        if (i == data.restart_index) {
            if (current.size() >= 3) out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(i);
    }
    if (current.size() >= 3) out.push_back(current);
    return out;
}

OptimiseReport optimise_for_target(Mesh& mesh, const TargetProfile& profile)
{
    OptimiseReport rep;
    if (mesh.empty()) return rep;

    const size_t index_count  = mesh.indices.size();
    const size_t vertex_count = mesh.vertex_count();
    const unsigned cache_size = unsigned(std::clamp(profile.vertex_cache_size, 4, 64));

    rep.vertices_before = vertex_count;
    {
        const meshopt_VertexCacheStatistics before = meshopt_analyzeVertexCache(
            mesh.indices.data(), index_count, vertex_count, cache_size, 0, 0);
        rep.acmr_before = before.acmr;
        rep.atvr_before = before.atvr;
    }

    // 1. Cache friendly triangle order.
    std::vector<uint32_t> ordered(index_count);
    meshopt_optimizeVertexCacheStrip(ordered.data(), mesh.indices.data(), index_count,
                                     vertex_count);
    mesh.indices.swap(ordered);

    // 2. Overdraw: only worth doing when we actually have positions to sort by.
    {
        std::vector<uint32_t> overdrawn(index_count);
        meshopt_optimizeOverdraw(overdrawn.data(), mesh.indices.data(), index_count,
                                 &mesh.positions[0].x, vertex_count, sizeof(Vec3), 1.02f);
        mesh.indices.swap(overdrawn);
    }

    // 3. Fetch order: remap the vertices so the index buffer walks forward.
    {
        std::vector<uint32_t> remap(vertex_count);
        const size_t unique = meshopt_optimizeVertexFetchRemap(
            remap.data(), mesh.indices.data(), index_count, vertex_count);

        std::vector<uint32_t> new_indices(index_count);
        meshopt_remapIndexBuffer(new_indices.data(), mesh.indices.data(), index_count,
                                 remap.data());
        mesh.indices.swap(new_indices);

        auto remap_stream = [&](auto& stream) {
            using T = typename std::decay_t<decltype(stream)>::value_type;
            if (stream.size() != vertex_count) return;
            std::vector<T> out(unique);
            meshopt_remapVertexBuffer(out.data(), stream.data(), vertex_count, sizeof(T),
                                      remap.data());
            stream.swap(out);
        };
        remap_stream(mesh.positions);
        remap_stream(mesh.normals);
        remap_stream(mesh.uvs);
        remap_stream(mesh.colors);
        remap_stream(mesh.skin);
        rep.vertices_after = unique;
    }

    // tri_region is per face and the face order changed, so it is no longer
    // meaningful; dropping it is more honest than shipping a stale mapping.
    mesh.tri_region.clear();

    {
        const meshopt_VertexCacheStatistics after = meshopt_analyzeVertexCache(
            mesh.indices.data(), mesh.indices.size(), mesh.vertex_count(), cache_size, 0, 0);
        rep.acmr_after = after.acmr;
        rep.atvr_after = after.atvr;
    }
    {
        const meshopt_OverdrawStatistics od = meshopt_analyzeOverdraw(
            mesh.indices.data(), mesh.indices.size(), &mesh.positions[0].x,
            mesh.vertex_count(), sizeof(Vec3));
        rep.overdraw_after = od.overdraw;
    }

    RD_INFO("optimise: acmr %.3f -> %.3f, atvr %.3f -> %.3f, %zu -> %zu vertices",
            rep.acmr_before, rep.acmr_after, rep.atvr_before, rep.atvr_after,
            rep.vertices_before, rep.vertices_after);
    return rep;
}

StripData build_strips(const Mesh& mesh, const TargetProfile& profile)
{
    StripData data;
    if (mesh.empty()) return data;

    const size_t bound = meshopt_stripifyBound(mesh.indices.size());
    data.indices.resize(bound);
    const size_t count = meshopt_stripify(data.indices.data(), mesh.indices.data(),
                                          mesh.indices.size(), mesh.vertex_count(),
                                          data.restart_index);
    data.indices.resize(count);

    const auto runs = split_strips(data);
    data.strip_count = runs.size();

    size_t triangles = 0;
    for (const auto& run : runs) triangles += run.size() >= 3 ? run.size() - 2 : 0;
    data.average_length = data.strip_count ? float(triangles) / float(data.strip_count) : 0.0f;

    // Strips generated with restarts contain no degenerate stitching triangles,
    // but a run can still hold repeated indices where the source did.
    for (const auto& run : runs)
        for (size_t i = 0; i + 2 < run.size(); ++i)
            if (run[i] == run[i + 1] || run[i + 1] == run[i + 2] || run[i] == run[i + 2])
                ++data.degenerate_count;

    RD_INFO("strips: %zu runs, %.2f triangles per strip (profile wants >= %.2f)",
            data.strip_count, data.average_length, profile.min_average_strip_len);
    return data;
}

// ---------------------------------------------------------------------------
bool write_rdmesh(const fs::path& path, const Mesh& mesh, const StripData& strips,
                  const TargetProfile& profile, std::string* error)
{
    std::vector<uint8_t> out;
    out.reserve(mesh.vertex_count() * 48 + strips.indices.size() * 4 + 256);

    // --- header -------------------------------------------------------------
    const char magic[8] = {'R', 'D', 'M', 'E', 'S', 'H', '0', '1'};
    out.insert(out.end(), magic, magic + 8);

    uint32_t flags = 0;
    if (mesh.has_normals()) flags |= 1u << 0;
    if (mesh.has_uvs())     flags |= 1u << 1;
    if (mesh.has_colors())  flags |= 1u << 2;
    if (mesh.has_skin())    flags |= 1u << 3;
    if (!strips.indices.empty()) flags |= 1u << 4;

    put_u32(out, flags);
    put_u32(out, uint32_t(mesh.vertex_count()));
    put_u32(out, uint32_t(mesh.triangle_count()));
    put_u32(out, uint32_t(strips.indices.size()));
    put_u32(out, strips.restart_index);
    put_u32(out, uint32_t(std::max(0, profile.max_bone_influences)));
    put_f32(out, mesh.import_scale);

    const Aabb box = mesh.bounds();
    put_f32(out, box.lo.x); put_f32(out, box.lo.y); put_f32(out, box.lo.z);
    put_f32(out, box.hi.x); put_f32(out, box.hi.y); put_f32(out, box.hi.z);

    // --- streams ------------------------------------------------------------
    for (const Vec3& p : mesh.positions) { put_f32(out, p.x); put_f32(out, p.y); put_f32(out, p.z); }
    if (mesh.has_normals())
        for (const Vec3& n : mesh.normals) { put_f32(out, n.x); put_f32(out, n.y); put_f32(out, n.z); }
    if (mesh.has_uvs())
        for (const Vec2& t : mesh.uvs) { put_f32(out, t.x); put_f32(out, t.y); }
    if (mesh.has_colors())
        for (const Vec4& c : mesh.colors) {
            out.push_back(uint8_t(clampf(c.x * 255.0f + 0.5f, 0.0f, 255.0f)));
            out.push_back(uint8_t(clampf(c.y * 255.0f + 0.5f, 0.0f, 255.0f)));
            out.push_back(uint8_t(clampf(c.z * 255.0f + 0.5f, 0.0f, 255.0f)));
            out.push_back(uint8_t(clampf(c.w * 255.0f + 0.5f, 0.0f, 255.0f)));
        }
    if (mesh.has_skin())
        for (const SkinVertex& s : mesh.skin) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(uint8_t(s.joints[i] & 0xFFu));
                out.push_back(uint8_t((s.joints[i] >> 8) & 0xFFu));
            }
            for (int i = 0; i < 4; ++i)
                out.push_back(uint8_t(clampf(s.weights[i] * 255.0f + 0.5f, 0.0f, 255.0f)));
        }

    // --- indices ------------------------------------------------------------
    for (uint32_t i : mesh.indices) put_u32(out, i);
    for (uint32_t i : strips.indices) put_u32(out, i);

    // --- skeleton -----------------------------------------------------------
    put_u32(out, uint32_t(mesh.armature.size()));
    for (const Joint& j : mesh.armature.joints) {
        const uint32_t len = uint32_t(std::min<size_t>(j.name.size(), 255));
        put_u32(out, len);
        out.insert(out.end(), j.name.begin(), j.name.begin() + long(len));
        put_u32(out, uint32_t(int32_t(j.parent)));
        put_f32(out, j.bind_position.x);
        put_f32(out, j.bind_position.y);
        put_f32(out, j.bind_position.z);
    }

    if (!paths::write_file(path, out.data(), out.size())) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
ExportResult export_asset(Mesh& mesh, const Texture& diffuse, const Palette& palette,
                          const TargetProfile& profile, const fs::path& directory,
                          const ExportOptions& opts)
{
    Stopwatch watch;
    ExportResult result;

    if (mesh.empty()) {
        result.error = "nothing to export";
        return result;
    }
    if (!paths::ensure_dir(directory)) {
        result.error = "cannot create " + directory.string();
        return result;
    }

    if (opts.optimise) result.optimisation = optimise_for_target(mesh, profile);
    if (profile.require_strips) result.strips = build_strips(mesh, profile);

    const std::string base = opts.base_name.empty() ? "lowpoly" : opts.base_name;
    std::string texture_name;

    // --- texture ------------------------------------------------------------
    if (opts.write_texture && !diffuse.empty()) {
        const fs::path tex_path = directory / (base + "_diffuse.png");
        if (diffuse.save_png(tex_path)) {
            result.files.push_back(tex_path);
            texture_name = tex_path.filename().string();
        }
        if (opts.write_indexed_texture && !palette.colors.empty()) {
            std::string err;
            if (save_indexed(tex_path, diffuse, palette, &err)) {
                result.files.push_back(directory / (base + "_diffuse_index.png"));
                result.files.push_back(directory / (base + "_diffuse_clut.json"));
            } else {
                RD_WARN("indexed texture not written: %s", err.c_str());
            }
        }
    }

    meshio::SaveOptions save;
    save.restore_import_transform = true;
    save.write_normals = true;
    save.write_uvs     = mesh.has_uvs();
    save.write_colors  = mesh.has_colors();
    save.texture_file  = texture_name;

    std::string err;
    if (opts.write_obj) {
        const fs::path p = directory / (base + ".obj");
        if (meshio::save_obj(p, mesh, save, &err)) {
            result.files.push_back(p);
            if (!texture_name.empty()) result.files.push_back(directory / (base + ".mtl"));
        } else {
            RD_WARN("obj export failed: %s", err.c_str());
        }
    }
    if (opts.write_gltf) {
        const fs::path p = directory / (base + ".gltf");
        if (meshio::save_gltf(p, mesh, save, &err)) {
            result.files.push_back(p);
            result.files.push_back(directory / (base + ".bin"));
        } else {
            RD_WARN("gltf export failed: %s", err.c_str());
        }
    }
    if (opts.write_glb) {
        meshio::SaveOptions binary = save;
        binary.binary = true;
        const fs::path p = directory / (base + ".glb");
        if (meshio::save_gltf(p, mesh, binary, &err)) result.files.push_back(p);
        else RD_WARN("glb export failed: %s", err.c_str());
    }
    if (opts.write_binary) {
        const fs::path p = directory / (base + ".rdmesh");
        if (write_rdmesh(p, mesh, result.strips, profile, &err)) result.files.push_back(p);
        else RD_WARN("rdmesh export failed: %s", err.c_str());
    }

    for (const fs::path& f : result.files) {
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        if (!ec) result.total_bytes += size;
    }

    result.ok      = !result.files.empty();
    result.seconds = watch.seconds();
    if (!result.ok) result.error = "no files were written";

    RD_INFO("export: %zu files, %s, in %s", result.files.size(),
            paths::format_bytes(result.total_bytes).c_str(),
            format_duration(result.seconds).c_str());
    return result;
}

} // namespace rd
