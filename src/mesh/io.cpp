#include "mesh/io.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

#include <cgltf.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace rd::meshio {
namespace {

// ---------------------------------------------------------------------------
// OBJ
// ---------------------------------------------------------------------------
struct ObjKey {
    int v = 0, t = 0, n = 0;
    bool operator==(const ObjKey& o) const { return v == o.v && t == o.t && n == o.n; }
};

struct ObjKeyHash {
    size_t operator()(const ObjKey& k) const
    {
        return (size_t(uint32_t(k.v)) * 0x9E3779B1u) ^
               (size_t(uint32_t(k.t)) * 0x85EBCA6Bu) ^
               (size_t(uint32_t(k.n)) * 0xC2B2AE35u);
    }
};

// Fast float scan that does not care about the locale, unlike strtof.
const char* skip_ws(const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
    return p;
}

const char* parse_float(const char* p, const char* end, float& out)
{
    p = skip_ws(p, end);
    const char* start = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
    if (p == start) { out = 0.0f; return p; }

    // from_chars on floats is the one place MinGW's libstdc++ is reliably fast.
    double value = 0.0;
    const auto res = std::from_chars(start, p, value);
    out = (res.ec == std::errc()) ? static_cast<float>(value) : 0.0f;
    return p;
}

// Parses "12", "12/3", "12//4", "12/3/4"; indices may be negative (relative).
const char* parse_obj_index(const char* p, const char* end, ObjKey& key)
{
    p = skip_ws(p, end);
    auto read_int = [&](int& out) -> bool {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p == start) return false;
        int value = 0;
        std::from_chars(start, p, value);
        out = value;
        return true;
    };

    key = ObjKey{};
    if (!read_int(key.v)) return p;
    if (p < end && *p == '/') {
        ++p;
        if (p < end && *p != '/') read_int(key.t);
        if (p < end && *p == '/') { ++p; read_int(key.n); }
    }
    return p;
}

LoadReport load_obj(const fs::path& path, Mesh& out, const LoadOptions& opts)
{
    LoadReport rep;
    rep.format = "obj";

    std::string text;
    if (!paths::read_file(path, text)) {
        rep.error = "cannot read file";
        return rep;
    }

    std::vector<Vec3> raw_pos;
    std::vector<Vec3> raw_nrm;
    std::vector<Vec2> raw_uv;
    std::vector<Vec4> raw_col;      // OBJ vertex colour extension: "v x y z r g b"
    bool              saw_colors = false;

    raw_pos.reserve(text.size() / 48);

    std::unordered_map<ObjKey, uint32_t, ObjKeyHash> lookup;
    lookup.reserve(text.size() / 64);

    std::vector<ObjKey> face;
    face.reserve(16);

    const char* p   = text.data();
    const char* end = text.data() + text.size();

    auto line_end = [&](const char* q) {
        while (q < end && *q != '\n') ++q;
        return q;
    };

    while (p < end) {
        const char* eol = line_end(p);
        const char* c   = skip_ws(p, eol);

        if (c < eol && *c == 'v') {
            ++c;
            if (c < eol && *c == ' ') {
                float x = 0, y = 0, z = 0;
                c = parse_float(c, eol, x);
                c = parse_float(c, eol, y);
                c = parse_float(c, eol, z);
                raw_pos.push_back({x, y, z});

                const char* probe = skip_ws(c, eol);
                if (probe < eol) {
                    float r = 1, g = 1, b = 1;
                    c = parse_float(c, eol, r);
                    c = parse_float(c, eol, g);
                    c = parse_float(c, eol, b);
                    raw_col.push_back({r, g, b, 1.0f});
                    saw_colors = true;
                } else {
                    raw_col.push_back({1, 1, 1, 1});
                }
            } else if (c < eol && *c == 'n') {
                ++c;
                float x = 0, y = 0, z = 0;
                c = parse_float(c, eol, x);
                c = parse_float(c, eol, y);
                c = parse_float(c, eol, z);
                raw_nrm.push_back({x, y, z});
            } else if (c < eol && *c == 't') {
                ++c;
                float u = 0, v = 0;
                c = parse_float(c, eol, u);
                c = parse_float(c, eol, v);
                raw_uv.push_back({u, v});
            }
        } else if (c < eol && *c == 'f' && c + 1 < eol && (c[1] == ' ' || c[1] == '\t')) {
            ++c;
            face.clear();
            while (c < eol) {
                const char* before = skip_ws(c, eol);
                if (before >= eol) break;
                ObjKey key;
                c = parse_obj_index(c, eol, key);
                if (c == before) break;
                if (key.v == 0) break;
                face.push_back(key);
            }

            if (face.size() >= 3) {
                auto resolve = [&](ObjKey k) -> uint32_t {
                    // Normalise relative indices before caching.
                    k.v = k.v > 0 ? k.v - 1 : static_cast<int>(raw_pos.size()) + k.v;
                    k.t = k.t > 0 ? k.t - 1 : (k.t < 0 ? static_cast<int>(raw_uv.size()) + k.t : -1);
                    k.n = k.n > 0 ? k.n - 1 : (k.n < 0 ? static_cast<int>(raw_nrm.size()) + k.n : -1);

                    const auto it = lookup.find(k);
                    if (it != lookup.end()) return it->second;

                    const uint32_t idx = static_cast<uint32_t>(out.positions.size());
                    if (k.v >= 0 && static_cast<size_t>(k.v) < raw_pos.size())
                        out.positions.push_back(raw_pos[k.v]);
                    else
                        out.positions.push_back({});
                    if (!raw_nrm.empty())
                        out.normals.push_back(k.n >= 0 && static_cast<size_t>(k.n) < raw_nrm.size()
                                                  ? raw_nrm[k.n] : Vec3{});
                    if (!raw_uv.empty())
                        out.uvs.push_back(k.t >= 0 && static_cast<size_t>(k.t) < raw_uv.size()
                                              ? raw_uv[k.t] : Vec2{});
                    if (saw_colors)
                        out.colors.push_back(k.v >= 0 && static_cast<size_t>(k.v) < raw_col.size()
                                                 ? raw_col[k.v] : Vec4{1, 1, 1, 1});
                    lookup.emplace(k, idx);
                    return idx;
                };

                const uint32_t v0 = resolve(face[0]);
                for (size_t i = 1; i + 1 < face.size(); ++i) {
                    out.indices.push_back(v0);
                    out.indices.push_back(resolve(face[i]));
                    out.indices.push_back(resolve(face[i + 1]));
                }
            }
        }

        p = eol < end ? eol + 1 : end;
    }

    rep.source_vertices  = out.positions.size();
    rep.source_triangles = out.triangle_count();
    rep.primitives       = 1;
    rep.ok               = !out.empty();
    if (!rep.ok) rep.error = "no triangles found";
    return rep;
}

// ---------------------------------------------------------------------------
// glTF
// ---------------------------------------------------------------------------
Mat4 mat4_from_gltf(const cgltf_float m[16])
{
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(m[i]);
    return r;
}

bool read_accessor_vec(const cgltf_accessor* acc, int components, std::vector<float>& out)
{
    if (!acc) return false;
    const cgltf_size count = acc->count;
    out.assign(count * components, 0.0f);
    std::vector<cgltf_float> tmp(static_cast<size_t>(cgltf_num_components(acc->type)));
    for (cgltf_size i = 0; i < count; ++i) {
        if (!cgltf_accessor_read_float(acc, i, tmp.data(), tmp.size())) return false;
        for (int c = 0; c < components; ++c)
            out[i * components + c] = static_cast<float>(
                c < static_cast<int>(tmp.size()) ? tmp[c] : 0.0f);
    }
    return true;
}

LoadReport load_gltf(const fs::path& path, Mesh& out, const LoadOptions& opts)
{
    LoadReport rep;
    rep.format = paths::extension_of(path);

    cgltf_options options{};
    cgltf_data*   data = nullptr;
    const std::string path_utf8 = path.string();

    cgltf_result res = cgltf_parse_file(&options, path_utf8.c_str(), &data);
    if (res != cgltf_result_success) {
        rep.error = format("cgltf_parse_file failed (code %d)", static_cast<int>(res));
        return rep;
    }

    res = cgltf_load_buffers(&options, data, path_utf8.c_str());
    if (res != cgltf_result_success) {
        cgltf_free(data);
        rep.error = format("cgltf_load_buffers failed (code %d)", static_cast<int>(res));
        return rep;
    }

    // --- skeleton ----------------------------------------------------------
    std::unordered_map<const cgltf_node*, int> joint_index;
    if (opts.load_armature && data->skins_count > 0) {
        const cgltf_skin& skin = data->skins[0];
        out.armature.joints.reserve(skin.joints_count);

        std::vector<float> ibm;
        const bool have_ibm = skin.inverse_bind_matrices &&
                              read_accessor_vec(skin.inverse_bind_matrices, 16, ibm);

        for (cgltf_size j = 0; j < skin.joints_count; ++j) {
            const cgltf_node* node = skin.joints[j];
            Joint joint;
            joint.name = node->name ? node->name : format("joint_%zu", size_t(j));

            if (have_ibm && (j + 1) * 16 <= ibm.size()) {
                Mat4 m;
                for (int i = 0; i < 16; ++i) m.m[i] = ibm[j * 16 + i];
                const Mat4 bind = inverse(m);
                joint.bind_position = {bind.at(3, 0), bind.at(3, 1), bind.at(3, 2)};
            } else {
                cgltf_float world[16];
                cgltf_node_transform_world(node, world);
                const Mat4 m = mat4_from_gltf(world);
                joint.bind_position = {m.at(3, 0), m.at(3, 1), m.at(3, 2)};
            }

            joint_index.emplace(node, static_cast<int>(j));
            out.armature.joints.push_back(std::move(joint));
        }

        for (cgltf_size j = 0; j < skin.joints_count; ++j) {
            const cgltf_node* parent = skin.joints[j]->parent;
            const auto it = parent ? joint_index.find(parent) : joint_index.end();
            out.armature.joints[j].parent = it != joint_index.end() ? it->second : -1;
        }
        rep.joints = out.armature.joints.size();
    }

    // --- geometry ----------------------------------------------------------
    bool any_normals = false, any_uvs = false, any_colors = false, any_skin = false;

    // First pass just to learn which streams exist, so the merged mesh keeps
    // attribute arrays dense instead of ragged.
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                switch (prim.attributes[a].type) {
                case cgltf_attribute_type_normal:   any_normals = true; break;
                case cgltf_attribute_type_texcoord: if (prim.attributes[a].index == 0) any_uvs = true; break;
                case cgltf_attribute_type_color:    if (prim.attributes[a].index == 0) any_colors = true; break;
                case cgltf_attribute_type_joints:   any_skin = true; break;
                default: break;
                }
            }
        }
    }

    std::vector<float> pos, nrm, uv, col, joints_f, weights_f;

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;

        cgltf_float world[16];
        cgltf_node_transform_world(&node, world);
        const Mat4 xform = mat4_from_gltf(world);

        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            pos.clear(); nrm.clear(); uv.clear(); col.clear();
            joints_f.clear(); weights_f.clear();

            const cgltf_accessor* acc_joints = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                switch (attr.type) {
                case cgltf_attribute_type_position: read_accessor_vec(attr.data, 3, pos); break;
                case cgltf_attribute_type_normal:   read_accessor_vec(attr.data, 3, nrm); break;
                case cgltf_attribute_type_texcoord:
                    if (attr.index == 0) read_accessor_vec(attr.data, 2, uv);
                    break;
                case cgltf_attribute_type_color:
                    if (attr.index == 0) read_accessor_vec(attr.data, 4, col);
                    break;
                case cgltf_attribute_type_joints:
                    if (attr.index == 0) acc_joints = attr.data;
                    break;
                case cgltf_attribute_type_weights:
                    if (attr.index == 0) read_accessor_vec(attr.data, 4, weights_f);
                    break;
                default: break;
                }
            }

            const size_t vcount = pos.size() / 3;
            if (vcount == 0) continue;
            ++rep.primitives;

            const uint32_t base = static_cast<uint32_t>(out.positions.size());
            out.positions.reserve(base + vcount);
            for (size_t v = 0; v < vcount; ++v)
                out.positions.push_back(
                    transform_point(xform, {pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]}));

            if (any_normals) {
                out.normals.reserve(base + vcount);
                for (size_t v = 0; v < vcount; ++v) {
                    Vec3 nv{0, 1, 0};
                    if (v * 3 + 2 < nrm.size())
                        nv = normalize(transform_dir(xform, {nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]}));
                    out.normals.push_back(nv);
                }
            }
            if (any_uvs) {
                out.uvs.reserve(base + vcount);
                for (size_t v = 0; v < vcount; ++v)
                    out.uvs.push_back(v * 2 + 1 < uv.size() ? Vec2{uv[v * 2], uv[v * 2 + 1]} : Vec2{});
            }
            if (any_colors) {
                out.colors.reserve(base + vcount);
                for (size_t v = 0; v < vcount; ++v)
                    out.colors.push_back(v * 4 + 3 < col.size()
                                             ? Vec4{col[v * 4], col[v * 4 + 1], col[v * 4 + 2], col[v * 4 + 3]}
                                             : Vec4{1, 1, 1, 1});
            }
            if (any_skin) {
                out.skin.reserve(base + vcount);
                for (size_t v = 0; v < vcount; ++v) {
                    SkinVertex sv;
                    if (acc_joints) {
                        cgltf_uint tmp[4] = {0, 0, 0, 0};
                        if (cgltf_accessor_read_uint(acc_joints, v, tmp, 4))
                            for (int c = 0; c < 4; ++c) sv.joints[c] = static_cast<uint16_t>(tmp[c]);
                    }
                    for (int c = 0; c < 4; ++c)
                        sv.weights[c] = (v * 4 + c) < weights_f.size() ? weights_f[v * 4 + c] : 0.0f;
                    const float sum = sv.weights[0] + sv.weights[1] + sv.weights[2] + sv.weights[3];
                    if (sum > kEps) for (float& w : sv.weights) w /= sum;
                    else            sv.weights[0] = 1.0f;
                    out.skin.push_back(sv);
                }
            }

            if (prim.indices) {
                const cgltf_size ic = prim.indices->count;
                out.indices.reserve(out.indices.size() + ic);
                for (cgltf_size i = 0; i < ic; ++i)
                    out.indices.push_back(base +
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            } else {
                out.indices.reserve(out.indices.size() + vcount);
                for (size_t v = 0; v < vcount; ++v)
                    out.indices.push_back(base + static_cast<uint32_t>(v));
            }
        }
    }

    if (data->meshes_count > 0 && data->meshes[0].name) out.name = data->meshes[0].name;
    cgltf_free(data);

    rep.source_vertices  = out.positions.size();
    rep.source_triangles = out.triangle_count();
    rep.ok               = !out.empty();
    if (!rep.ok) rep.error = "no triangle primitives found";
    return rep;
}

} // namespace

// ---------------------------------------------------------------------------
LoadReport load(const fs::path& path, Mesh& out, const LoadOptions& opts)
{
    Stopwatch watch;
    out.clear();

    const std::string ext = paths::extension_of(path);
    LoadReport rep;

    if (ext == "obj")                          rep = load_obj(path, out, opts);
    else if (ext == "gltf" || ext == "glb")    rep = load_gltf(path, out, opts);
    else {
        rep.error = "unsupported extension ." + ext;
        return rep;
    }

    if (!rep.ok) {
        out.clear();
        rep.seconds = watch.seconds();
        return rep;
    }

    if (out.name.empty()) out.name = path.stem().string();

    rep.dropped_triangles = out.remove_degenerate();

    if (opts.weld) {
        const Aabb  box = out.bounds();
        const float eps = std::max(box.diagonal() * opts.weld_epsilon_rel, 1e-9f);
        rep.welded_vertices = out.weld(eps);
    }

    out.compact();

    if (opts.normalise) out.normalise_to_unit(true);

    if (opts.force_recompute_normals || !out.has_normals())
        out.compute_normals(opts.sharp_angle_degrees);

    rep.seconds = watch.seconds();
    RD_INFO("loaded %s: %zu tri, %zu vtx, %zu joints in %s",
            path.filename().string().c_str(), out.triangle_count(), out.vertex_count(),
            out.armature.size(), format_duration(rep.seconds).c_str());
    return rep;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------
namespace {

Mesh prepared_for_save(const Mesh& mesh, const SaveOptions& opts)
{
    Mesh copy = mesh;
    if (opts.restore_import_transform) {
        for (Vec3& p : copy.positions) p = transform_point(mesh.import_transform, p);
        for (Joint& j : copy.armature.joints)
            j.bind_position = transform_point(mesh.import_transform, j.bind_position);
    }
    if (!opts.write_normals) copy.normals.clear();
    if (!opts.write_uvs)     copy.uvs.clear();
    if (!opts.write_colors)  copy.colors.clear();
    return copy;
}

} // namespace

bool save_obj(const fs::path& path, const Mesh& mesh, const SaveOptions& opts,
              std::string* error)
{
    const Mesh m = prepared_for_save(mesh, opts);

    std::string text;
    text.reserve(m.vertex_count() * 48 + m.indices.size() * 12);
    text += "# Retopo Director " RD_VERSION_STRING "\n";
    if (!opts.texture_file.empty())
        text += "mtllib " + fs::path(opts.texture_file).stem().string() + ".mtl\n";
    text += "o " + (m.name.empty() ? std::string("lowpoly") : m.name) + "\n";

    char buf[192];
    const bool with_colors = m.has_colors();
    for (size_t v = 0; v < m.vertex_count(); ++v) {
        const Vec3& p = m.positions[v];
        if (with_colors) {
            const Vec4& c = m.colors[v];
            std::snprintf(buf, sizeof(buf), "v %.6g %.6g %.6g %.4f %.4f %.4f\n",
                          p.x, p.y, p.z, c.x, c.y, c.z);
        } else {
            std::snprintf(buf, sizeof(buf), "v %.6g %.6g %.6g\n", p.x, p.y, p.z);
        }
        text += buf;
    }
    if (m.has_uvs())
        for (const Vec2& t : m.uvs) {
            std::snprintf(buf, sizeof(buf), "vt %.6g %.6g\n", t.x, t.y);
            text += buf;
        }
    if (m.has_normals())
        for (const Vec3& n : m.normals) {
            std::snprintf(buf, sizeof(buf), "vn %.5g %.5g %.5g\n", n.x, n.y, n.z);
            text += buf;
        }

    if (!opts.texture_file.empty()) text += "usemtl lowpoly\n";

    const bool ht = m.has_uvs(), hn = m.has_normals();
    for (size_t t = 0; t < m.triangle_count(); ++t) {
        const uint32_t a = m.indices[t * 3] + 1;
        const uint32_t b = m.indices[t * 3 + 1] + 1;
        const uint32_t c = m.indices[t * 3 + 2] + 1;
        if (ht && hn)       std::snprintf(buf, sizeof(buf), "f %u/%u/%u %u/%u/%u %u/%u/%u\n",
                                          a, a, a, b, b, b, c, c, c);
        else if (ht)        std::snprintf(buf, sizeof(buf), "f %u/%u %u/%u %u/%u\n", a, a, b, b, c, c);
        else if (hn)        std::snprintf(buf, sizeof(buf), "f %u//%u %u//%u %u//%u\n", a, a, b, b, c, c);
        else                std::snprintf(buf, sizeof(buf), "f %u %u %u\n", a, b, c);
        text += buf;
    }

    if (!paths::write_file(path, text)) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }

    if (!opts.texture_file.empty()) {
        const std::string mtl =
            "newmtl lowpoly\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nd 1\nillum 1\nmap_Kd " +
            fs::path(opts.texture_file).filename().string() + "\n";
        paths::write_file(path.parent_path() / (path.stem().string() + ".mtl"), mtl);
    }
    return true;
}

bool save_gltf(const fs::path& path, const Mesh& mesh, const SaveOptions& opts,
               std::string* error)
{
    // Hand written glTF 2.0: a single buffer with interleaved-by-stream layout.
    // Going through cgltf_write would mean building the whole cgltf_data graph
    // by hand anyway, and this keeps the binary layout under our control.
    const Mesh m = prepared_for_save(mesh, opts);
    if (m.empty()) {
        if (error) *error = "mesh is empty";
        return false;
    }

    const bool wn = m.has_normals(), wt = m.has_uvs(), wc = m.has_colors();
    const size_t vcount = m.vertex_count();
    const size_t icount = m.indices.size();

    std::vector<uint8_t> bin;
    struct View { size_t offset, length; };
    std::vector<View> views;

    auto push_view = [&](const void* data, size_t bytes) {
        while (bin.size() % 4) bin.push_back(0);
        const size_t offset = bin.size();
        bin.insert(bin.end(), static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + bytes);
        views.push_back({offset, bytes});
        return views.size() - 1;
    };

    const size_t v_pos = push_view(m.positions.data(), vcount * sizeof(Vec3));
    size_t v_nrm = 0, v_uv = 0, v_col = 0;
    if (wn) v_nrm = push_view(m.normals.data(), vcount * sizeof(Vec3));
    if (wt) v_uv  = push_view(m.uvs.data(), vcount * sizeof(Vec2));
    if (wc) v_col = push_view(m.colors.data(), vcount * sizeof(Vec4));
    const size_t v_idx = push_view(m.indices.data(), icount * sizeof(uint32_t));

    const Aabb box = m.bounds();

    std::string json = "{\n  \"asset\": {\"version\": \"2.0\", \"generator\": "
                       "\"Retopo Director " RD_VERSION_STRING "\"},\n";
    json += "  \"scene\": 0,\n  \"scenes\": [{\"nodes\": [0]}],\n";
    json += "  \"nodes\": [{\"mesh\": 0, \"name\": \"" +
            (m.name.empty() ? std::string("lowpoly") : m.name) + "\"}],\n";

    json += "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0";
    int accessor = 1;
    int a_nrm = -1, a_uv = -1, a_col = -1;
    if (wn) { json += ", \"NORMAL\": " + std::to_string(accessor); a_nrm = accessor++; }
    if (wt) { json += ", \"TEXCOORD_0\": " + std::to_string(accessor); a_uv = accessor++; }
    if (wc) { json += ", \"COLOR_0\": " + std::to_string(accessor); a_col = accessor++; }
    const int a_idx = accessor++;
    json += "}, \"indices\": " + std::to_string(a_idx);
    if (!opts.texture_file.empty()) json += ", \"material\": 0";
    json += ", \"mode\": 4}]}],\n";

    if (!opts.texture_file.empty()) {
        const std::string tex = fs::path(opts.texture_file).filename().string();
        json += "  \"materials\": [{\"name\": \"lowpoly\", \"pbrMetallicRoughness\": "
                "{\"baseColorTexture\": {\"index\": 0}, \"metallicFactor\": 0.0, "
                "\"roughnessFactor\": 1.0}}],\n";
        json += "  \"textures\": [{\"source\": 0, \"sampler\": 0}],\n";
        json += "  \"samplers\": [{\"magFilter\": 9729, \"minFilter\": 9987, "
                "\"wrapS\": 10497, \"wrapT\": 10497}],\n";
        json += "  \"images\": [{\"uri\": \"" + tex + "\"}],\n";
    }

    char minmax[256];
    std::snprintf(minmax, sizeof(minmax),
                  "\"min\": [%.6g, %.6g, %.6g], \"max\": [%.6g, %.6g, %.6g]",
                  box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z);

    json += "  \"accessors\": [\n";
    json += "    {\"bufferView\": " + std::to_string(v_pos) +
            ", \"componentType\": 5126, \"count\": " + std::to_string(vcount) +
            ", \"type\": \"VEC3\", " + minmax + "}";
    if (wn) json += ",\n    {\"bufferView\": " + std::to_string(v_nrm) +
                    ", \"componentType\": 5126, \"count\": " + std::to_string(vcount) +
                    ", \"type\": \"VEC3\"}";
    if (wt) json += ",\n    {\"bufferView\": " + std::to_string(v_uv) +
                    ", \"componentType\": 5126, \"count\": " + std::to_string(vcount) +
                    ", \"type\": \"VEC2\"}";
    if (wc) json += ",\n    {\"bufferView\": " + std::to_string(v_col) +
                    ", \"componentType\": 5126, \"count\": " + std::to_string(vcount) +
                    ", \"type\": \"VEC4\"}";
    json += ",\n    {\"bufferView\": " + std::to_string(v_idx) +
            ", \"componentType\": 5125, \"count\": " + std::to_string(icount) +
            ", \"type\": \"SCALAR\"}\n  ],\n";
    (void)a_nrm; (void)a_uv; (void)a_col;

    json += "  \"bufferViews\": [\n";
    for (size_t i = 0; i < views.size(); ++i) {
        json += "    {\"buffer\": 0, \"byteOffset\": " + std::to_string(views[i].offset) +
                ", \"byteLength\": " + std::to_string(views[i].length) +
                ", \"target\": " + (i + 1 == views.size() ? "34963" : "34962") + "}";
        if (i + 1 < views.size()) json += ",";
        json += "\n";
    }
    json += "  ],\n";

    const std::string bin_name = path.stem().string() + ".bin";
    if (opts.binary) {
        json += "  \"buffers\": [{\"byteLength\": " + std::to_string(bin.size()) + "}]\n}";
    } else {
        json += "  \"buffers\": [{\"uri\": \"" + bin_name +
                "\", \"byteLength\": " + std::to_string(bin.size()) + "}]\n}";
    }

    if (!opts.binary) {
        if (!paths::write_file(path, json) ||
            !paths::write_file(path.parent_path() / bin_name, bin.data(), bin.size())) {
            if (error) *error = "cannot write glTF files";
            return false;
        }
        return true;
    }

    // GLB container.
    while (json.size() % 4) json.push_back(' ');
    while (bin.size() % 4)  bin.push_back(0);

    std::vector<uint8_t> glb;
    auto put_u32 = [&](uint32_t v) {
        glb.push_back(uint8_t(v));       glb.push_back(uint8_t(v >> 8));
        glb.push_back(uint8_t(v >> 16)); glb.push_back(uint8_t(v >> 24));
    };
    const uint32_t total = 12 + 8 + uint32_t(json.size()) + 8 + uint32_t(bin.size());
    put_u32(0x46546C67); put_u32(2); put_u32(total);
    put_u32(uint32_t(json.size())); put_u32(0x4E4F534A);
    glb.insert(glb.end(), json.begin(), json.end());
    put_u32(uint32_t(bin.size())); put_u32(0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());

    if (!paths::write_file(path, glb.data(), glb.size())) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool save(const fs::path& path, const Mesh& mesh, const SaveOptions& opts, std::string* error)
{
    const std::string ext = paths::extension_of(path);
    if (ext == "obj")  return save_obj(path, mesh, opts, error);
    if (ext == "gltf") return save_gltf(path, mesh, opts, error);
    if (ext == "glb") {
        SaveOptions o = opts;
        o.binary = true;
        return save_gltf(path, mesh, o, error);
    }
    if (error) *error = "unsupported extension ." + ext;
    return false;
}

bool is_supported_extension(const std::string& ext_lower)
{
    return ext_lower == "obj" || ext_lower == "gltf" || ext_lower == "glb";
}

const char* supported_extensions_filter() { return "obj,gltf,glb"; }

} // namespace rd::meshio
