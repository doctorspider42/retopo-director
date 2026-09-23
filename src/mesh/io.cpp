#include "mesh/io.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

#include <cgltf.h>
#include <ufbx.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <sstream>
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

// Defined with the rest of the material helpers, below the obj reader.
void downscale_to(Texture& tex, int limit);

// An mtl is a flat list of "newmtl <name>" blocks. Only map_Kd and Kd matter
// here: everything else describes shading this target does not have.
std::shared_ptr<MaterialSet> load_mtl(const fs::path& mtl_path, int size_limit,
                                      std::unordered_map<std::string, uint16_t>& by_name,
                                      size_t* textured)
{
    std::string text;
    if (!paths::read_file(mtl_path, text)) return nullptr;

    auto set = std::make_shared<MaterialSet>();
    const fs::path base_dir = mtl_path.parent_path();

    SourceMaterial* current = nullptr;
    std::string     map_file;

    auto flush = [&] {
        if (!current) return;
        if (!map_file.empty()) {
            if (current->base_color.load_png(base_dir / map_file) ||
                current->base_color.load_png(map_file)) {
                downscale_to(current->base_color, size_limit);
                if (textured) ++*textured;
            } else {
                RD_WARN("mtl material '%s' points at %s, which will not open",
                        current->name.c_str(), map_file.c_str());
            }
        }
        map_file.clear();
    };

    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        if (t.rfind("newmtl", 0) == 0) {
            flush();
            set->materials.push_back({});
            current = &set->materials.back();
            current->name = trim(t.substr(6));
            by_name[current->name] = uint16_t(set->materials.size() - 1);
        } else if (current && t.rfind("map_Kd", 0) == 0) {
            // The last token is the filename; the ones before it are options
            // like -s or -o that this pipeline has no use for.
            const std::string rest = trim(t.substr(6));
            const size_t      sp   = rest.find_last_of(" \t");
            map_file = (sp == std::string::npos) ? rest : trim(rest.substr(sp + 1));
        } else if (current && t.rfind("Kd", 0) == 0) {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (std::sscanf(t.c_str() + 2, "%f %f %f", &r, &g, &b) == 3)
                current->base_factor = {r, g, b, 1.0f};
        }
    }
    flush();
    return set->materials.empty() ? nullptr : set;
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

    // Material assignment is positional in an obj: "usemtl" applies to every
    // face after it. Names are collected per triangle and resolved against the
    // mtl at the end, because the mtllib line is not required to come first.
    std::string              mtl_name, pending_material;
    std::vector<std::string> face_materials;

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
                // OBJ puts v = 0 at the bottom of the image; the pipeline at the top.
                raw_uv.push_back({u, 1.0f - v});
            }
        } else if (opts.load_materials && c + 6 < eol && std::strncmp(c, "mtllib", 6) == 0) {
            mtl_name = trim(std::string(c + 6, eol));
        } else if (opts.load_materials && c + 6 < eol && std::strncmp(c, "usemtl", 6) == 0) {
            pending_material = trim(std::string(c + 6, eol));
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
                    if (opts.load_materials) face_materials.push_back(pending_material);
                }
            }
        }

        p = eol < end ? eol + 1 : end;
    }

    if (opts.load_materials && !mtl_name.empty()) {
        std::unordered_map<std::string, uint16_t> by_name;
        out.materials = load_mtl(path.parent_path() / mtl_name,
                                 opts.max_material_texture_size, by_name, &rep.materials);
        if (out.materials) {
            out.tri_material.assign(out.triangle_count(), 0);
            for (size_t t = 0; t < out.triangle_count() && t < face_materials.size(); ++t) {
                const auto it = by_name.find(face_materials[t]);
                if (it != by_name.end()) out.tri_material[t] = it->second;
            }
        }
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


// ---------------------------------------------------------------------------
// Source materials
// ---------------------------------------------------------------------------
// A source map is routinely 2k or 4k and the atlas it will be resampled into is
// 256. Keeping the full resolution in memory buys nothing: the bake reads each
// texel through a closest point query, so it is already sampling far below the
// source rate. Box filter down to `limit` on the longest side.
void downscale_to(Texture& tex, int limit)
{
    if (limit <= 0 || tex.empty()) return;
    const int longest = std::max(tex.width, tex.height);
    if (longest <= limit) return;

    const float scale = float(limit) / float(longest);
    const int   nw = std::max(1, int(std::lround(tex.width * scale)));
    const int   nh = std::max(1, int(std::lround(tex.height * scale)));

    Texture out;
    out.resize(nw, nh, 4);
    for (int y = 0; y < nh; ++y) {
        const int y0 = y * tex.height / nh, y1 = std::max(y0 + 1, (y + 1) * tex.height / nh);
        for (int x = 0; x < nw; ++x) {
            const int x0 = x * tex.width / nw, x1 = std::max(x0 + 1, (x + 1) * tex.width / nw);
            Vec4 sum{0, 0, 0, 0};
            int  n = 0;
            for (int sy = y0; sy < y1 && sy < tex.height; ++sy)
                for (int sx = x0; sx < x1 && sx < tex.width; ++sx) { sum = sum + tex.get(sx, sy); ++n; }
            out.set(x, y, n > 0 ? sum * (1.0f / float(n)) : Vec4{1, 1, 1, 1});
        }
    }
    tex = std::move(out);
}

bool decode_data_uri(const char* uri, Texture& tex)
{
    const char* comma = std::strchr(uri, ',');
    if (!comma) return false;
    if (std::strstr(uri, "base64") == nullptr || std::strstr(uri, "base64") > comma) return false;

    static const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<uint8_t> bytes;
    uint32_t acc = 0;
    int      bits = 0;
    for (const char* p = comma + 1; *p; ++p) {
        const int v = value(*p);
        if (v < 0) continue;                       // '=' padding and whitespace
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back(uint8_t((acc >> bits) & 0xFF)); }
    }
    return tex.load_memory(bytes.data(), bytes.size());
}

std::shared_ptr<MaterialSet> load_gltf_materials(const cgltf_data* data, const fs::path& base_dir,
                                                 int size_limit, size_t* textured)
{
    if (!data || data->materials_count == 0) return nullptr;

    auto set = std::make_shared<MaterialSet>();
    set->materials.resize(data->materials_count);

    for (cgltf_size m = 0; m < data->materials_count; ++m) {
        const cgltf_material& src = data->materials[m];
        SourceMaterial&       dst = set->materials[m];
        dst.name = src.name ? src.name : format("material_%zu", size_t(m));

        const cgltf_texture_view* view = nullptr;
        if (src.has_pbr_metallic_roughness) {
            view = &src.pbr_metallic_roughness.base_color_texture;
            const float* f = src.pbr_metallic_roughness.base_color_factor;
            dst.base_factor = {f[0], f[1], f[2], f[3]};
        } else if (src.has_pbr_specular_glossiness) {
            view = &src.pbr_specular_glossiness.diffuse_texture;
            const float* f = src.pbr_specular_glossiness.diffuse_factor;
            dst.base_factor = {f[0], f[1], f[2], f[3]};
        }
        if (!view || !view->texture || !view->texture->image) continue;

        const cgltf_image& img = *view->texture->image;
        bool ok = false;
        if (img.buffer_view && img.buffer_view->buffer && img.buffer_view->buffer->data) {
            // The glb case: the png is sitting in the binary chunk.
            const uint8_t* bytes = static_cast<const uint8_t*>(img.buffer_view->buffer->data) +
                                   img.buffer_view->offset;
            ok = dst.base_color.load_memory(bytes, img.buffer_view->size);
        } else if (img.uri) {
            if (std::strncmp(img.uri, "data:", 5) == 0) {
                ok = decode_data_uri(img.uri, dst.base_color);
            } else {
                std::string uri = img.uri;
                cgltf_decode_uri(uri.data());
                ok = dst.base_color.load_png(base_dir / uri.c_str());
                if (!ok) RD_WARN("material '%s' points at %s, which will not open",
                                 dst.name.c_str(), uri.c_str());
            }
        }
        if (ok) {
            downscale_to(dst.base_color, size_limit);
            if (textured) ++*textured;
        }
    }
    return set;
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

            const size_t tris_before = out.triangle_count();
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

            if (opts.load_materials) {
                const uint16_t mat =
                    prim.material ? uint16_t(cgltf_material_index(data, prim.material)) : 0;
                out.tri_material.resize(out.triangle_count(), mat);
                for (size_t t = tris_before; t < out.triangle_count(); ++t)
                    out.tri_material[t] = mat;
            }
        }
    }

    if (opts.load_materials) {
        out.materials = load_gltf_materials(data, path.parent_path(),
                                            opts.max_material_texture_size, &rep.materials);
        if (out.materials && out.tri_material.size() != out.triangle_count())
            out.tri_material.assign(out.triangle_count(), 0);
    }

    if (data->meshes_count > 0 && data->meshes[0].name) out.name = data->meshes[0].name;
    cgltf_free(data);

    rep.source_vertices  = out.positions.size();
    rep.source_triangles = out.triangle_count();
    rep.ok               = !out.empty();
    if (!rep.ok) rep.error = "no triangle primitives found";
    return rep;
}


// ---------------------------------------------------------------------------
// FBX (ufbx)
// ---------------------------------------------------------------------------
Mat4 mat4_from_ufbx(const ufbx_matrix& m)
{
    // ufbx keeps three basis columns plus a translation; the last row is implicit.
    Mat4 r = Mat4::identity();
    r.at(0, 0) = float(m.cols[0].x); r.at(0, 1) = float(m.cols[0].y); r.at(0, 2) = float(m.cols[0].z);
    r.at(1, 0) = float(m.cols[1].x); r.at(1, 1) = float(m.cols[1].y); r.at(1, 2) = float(m.cols[1].z);
    r.at(2, 0) = float(m.cols[2].x); r.at(2, 1) = float(m.cols[2].y); r.at(2, 2) = float(m.cols[2].z);
    r.at(3, 0) = float(m.cols[3].x); r.at(3, 1) = float(m.cols[3].y); r.at(3, 2) = float(m.cols[3].z);
    return r;
}

Vec3 vec3_from_ufbx(const ufbx_vec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

std::string string_from_ufbx(const ufbx_string& s)
{
    return s.length ? std::string(s.data, s.length) : std::string();
}

// The bones the file actually uses, in the order the skin clusters name them,
// so a weight's cluster index maps to a joint index without a second lookup.
struct FbxSkeleton {
    std::vector<const ufbx_node*>                 nodes;
    std::unordered_map<const ufbx_node*, int32_t> index;

    int32_t add(const ufbx_node* node)
    {
        if (!node) return -1;
        const auto it = index.find(node);
        if (it != index.end()) return it->second;
        const int32_t id = static_cast<int32_t>(nodes.size());
        nodes.push_back(node);
        index.emplace(node, id);
        return id;
    }

    // Nearest ancestor that is itself a joint. FBX rigs routinely hang bones off
    // null nodes, and those must not become gaps in the hierarchy.
    int32_t parent_of(const ufbx_node* node) const
    {
        for (const ufbx_node* p = node ? node->parent : nullptr; p; p = p->parent) {
            const auto it = index.find(p);
            if (it != index.end()) return it->second;
        }
        return -1;
    }
};

// FBX says "diffuse" where glTF says "base colour", and a file exported from a
// PBR tool carries both. Prefer the PBR slot and fall back to the legacy one.
const ufbx_texture* fbx_base_texture(const ufbx_material* mat)
{
    if (!mat) return nullptr;
    if (mat->pbr.base_color.texture)    return mat->pbr.base_color.texture;
    if (mat->fbx.diffuse_color.texture) return mat->fbx.diffuse_color.texture;
    return nullptr;
}

std::shared_ptr<MaterialSet> load_fbx_materials(const ufbx_scene* scene, const fs::path& base_dir,
                                                int size_limit, size_t* textured)
{
    if (!scene || scene->materials.count == 0) return nullptr;

    auto set = std::make_shared<MaterialSet>();
    set->materials.resize(scene->materials.count);

    for (size_t m = 0; m < scene->materials.count; ++m) {
        const ufbx_material* src = scene->materials.data[m];
        SourceMaterial&      dst = set->materials[m];
        dst.name = string_from_ufbx(src->name);
        if (dst.name.empty()) dst.name = format("material_%zu", m);

        const ufbx_material_map& factor =
            src->pbr.base_color.has_value ? src->pbr.base_color : src->fbx.diffuse_color;
        if (factor.has_value)
            dst.base_factor = {float(factor.value_vec4.x), float(factor.value_vec4.y),
                               float(factor.value_vec4.z), 1.0f};

        const ufbx_texture* tex = fbx_base_texture(src);
        if (!tex) continue;

        bool ok = false;
        if (tex->content.size > 0 && tex->content.data) {
            ok = dst.base_color.load_memory(static_cast<const uint8_t*>(tex->content.data),
                                            tex->content.size);
        }
        if (!ok) {
            // Exporters write an absolute path from the authoring machine, so
            // the file beside the model is the one that actually exists.
            for (const ufbx_string* candidate : {&tex->filename, &tex->absolute_filename,
                                                 &tex->relative_filename}) {
                const std::string name = string_from_ufbx(*candidate);
                if (name.empty()) continue;
                const fs::path direct = name;
                if (dst.base_color.load_png(direct)) { ok = true; break; }
                if (dst.base_color.load_png(base_dir / direct.filename())) { ok = true; break; }
            }
        }
        if (ok) {
            downscale_to(dst.base_color, size_limit);
            if (textured) ++*textured;
        } else {
            RD_DEBUG("material '%s' has a base colour texture that could not be read",
                     dst.name.c_str());
        }
    }
    return set;
}

LoadReport load_fbx(const fs::path& path, Mesh& out, const LoadOptions& opts)
{
    LoadReport rep;
    rep.format = paths::extension_of(path);

    ufbx_load_opts lo{};
    // Bring the file into the convention the rest of the pipeline assumes: Y up,
    // right handed, one unit is one metre. FBX is authored in centimetres about
    // as often as in metres and in Z up about as often as Y up, and the profile
    // frames its cameras in metres, so guessing here would move the pictures the
    // director is shown.
    lo.target_axes        = ufbx_axes_right_handed_y_up;
    lo.target_unit_meters = 1.0f;
    lo.space_conversion   = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    // Animation is never read and a character file carries megabytes of it.
    // Textures are read only when somebody is going to bake from them, which
    // is the usual case but not the cheap one: an fbx embeds its maps at full
    // authored resolution.
    lo.ignore_animation    = true;
    lo.ignore_embedded     = !opts.load_materials;
    lo.load_external_files = opts.load_materials;
    // Missing normals are computed at the end of load(), the same as for OBJ.
    lo.generate_missing_normals = false;

    ufbx_error  err{};
    ufbx_scene* scene = ufbx_load_file(path.string().c_str(), &lo, &err);
    if (!scene) {
        char buffer[512];
        ufbx_format_error(buffer, sizeof(buffer), &err);
        rep.error = "ufbx: " + trim(buffer);
        return rep;
    }

    if (opts.load_materials)
        out.materials = load_fbx_materials(scene, path.parent_path(),
                                           opts.max_material_texture_size, &rep.materials);

    // --- skeleton ----------------------------------------------------------
    FbxSkeleton skeleton;
    std::unordered_map<const ufbx_skin_deformer*, std::vector<int32_t>> cluster_to_joint;

    if (opts.load_armature) {
        // Skinned bones first: those are the ones the weights refer to.
        for (size_t i = 0; i < scene->skin_deformers.count; ++i) {
            const ufbx_skin_deformer* skin = scene->skin_deformers.data[i];
            std::vector<int32_t> mapping(skin->clusters.count, -1);
            for (size_t c = 0; c < skin->clusters.count; ++c)
                mapping[c] = skeleton.add(skin->clusters.data[c]->bone_node);
            cluster_to_joint.emplace(skin, std::move(mapping));
        }
        // Then any bone the file declares but never skins with. A rig exported
        // without weights is still a rig, and the segmenter seeds on the pivots.
        for (size_t i = 0; i < scene->bones.count; ++i) {
            const ufbx_bone* bone = scene->bones.data[i];
            for (size_t n = 0; n < bone->instances.count; ++n)
                skeleton.add(bone->instances.data[n]);
        }
    }

    out.armature.joints.reserve(skeleton.nodes.size());
    for (const ufbx_node* node : skeleton.nodes) {
        Joint joint;
        joint.name = string_from_ufbx(node->name);
        if (joint.name.empty()) joint.name = format("joint_%zu", out.armature.joints.size());
        // Animation was never loaded, so node_to_world is the rest pose.
        joint.bind_position = vec3_from_ufbx(node->node_to_world.cols[3]);
        out.armature.joints.push_back(std::move(joint));
    }
    for (size_t j = 0; j < skeleton.nodes.size(); ++j)
        out.armature.joints[j].parent = skeleton.parent_of(skeleton.nodes[j]);
    rep.joints = out.armature.joints.size();

    // --- geometry ----------------------------------------------------------
    // Which streams exist anywhere in the file, so the merged mesh keeps its
    // attribute arrays dense instead of ragged.
    bool any_normals = false, any_uvs = false, any_colors = false, any_skin = false;
    for (size_t n = 0; n < scene->nodes.count; ++n) {
        const ufbx_mesh* mesh = scene->nodes.data[n]->mesh;
        if (!mesh) continue;
        if (mesh->vertex_normal.exists) any_normals = true;
        if (mesh->vertex_uv.exists)     any_uvs     = true;
        if (mesh->vertex_color.exists)  any_colors  = true;
        if (mesh->skin_deformers.count > 0 && !cluster_to_joint.empty()) any_skin = true;
    }

    std::vector<uint32_t> fan;   // one face triangulated, reused across faces

    for (size_t n = 0; n < scene->nodes.count; ++n) {
        const ufbx_node* node = scene->nodes.data[n];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || mesh->num_faces == 0) continue;
        ++rep.primitives;

        const Mat4 xform = mat4_from_ufbx(node->geometry_to_world);

        // Weights are per logical vertex and the deformer is shared by every
        // instance of the mesh, so it is looked up once per node.
        const ufbx_skin_deformer*   skin    = nullptr;
        const std::vector<int32_t>* mapping = nullptr;
        if (any_skin && mesh->skin_deformers.count > 0) {
            const ufbx_skin_deformer* candidate = mesh->skin_deformers.data[0];
            const auto it = cluster_to_joint.find(candidate);
            if (it != cluster_to_joint.end()) {
                skin    = candidate;
                mapping = &it->second;
            }
        }

        fan.assign(std::max<size_t>(mesh->max_face_triangles, 1) * 3, 0);

        for (size_t f = 0; f < mesh->faces.count; ++f) {
            const ufbx_face face = mesh->faces.data[f];
            // Quads and n-gons are the norm in a file out of a DCC. ufbx fans
            // them, and returns zero triangles for the degenerate ones.
            const uint32_t tris = ufbx_triangulate_face(fan.data(), fan.size(), mesh, face);

            if (opts.load_materials) {
                // face_material indexes this mesh's own material list; the set
                // built above is indexed by the scene's, so hop through it.
                uint16_t mat = 0;
                if (f < mesh->face_material.count) {
                    const uint32_t local = mesh->face_material.data[f];
                    if (local < mesh->materials.count && mesh->materials.data[local])
                        mat = uint16_t(mesh->materials.data[local]->typed_id);
                }
                out.tri_material.insert(out.tri_material.end(), tris, mat);
            }

            for (uint32_t t = 0; t < tris * 3; ++t) {
                const uint32_t ix = fan[t];

                // One vertex per index, the same as the OBJ path: whatever the
                // file split for shading reasons is put back together by the
                // weld at the end of load().
                out.indices.push_back(static_cast<uint32_t>(out.positions.size()));
                out.positions.push_back(transform_point(
                    xform, vec3_from_ufbx(ufbx_get_vertex_vec3(&mesh->vertex_position, ix))));

                if (any_normals) {
                    Vec3 nv{0.0f, 1.0f, 0.0f};
                    if (mesh->vertex_normal.exists)
                        nv = normalize(transform_dir(
                            xform, vec3_from_ufbx(ufbx_get_vertex_vec3(&mesh->vertex_normal, ix))));
                    out.normals.push_back(nv);
                }
                if (any_uvs) {
                    Vec2 uv{};
                    if (mesh->vertex_uv.exists) {
                        const ufbx_vec2 t2 = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                        // FBX, like OBJ, has v = 0 at the bottom of the image.
                        uv = {float(t2.x), 1.0f - float(t2.y)};
                    }
                    out.uvs.push_back(uv);
                }
                if (any_colors) {
                    Vec4 c{1.0f, 1.0f, 1.0f, 1.0f};
                    if (mesh->vertex_color.exists) {
                        const ufbx_vec4 t4 = ufbx_get_vertex_vec4(&mesh->vertex_color, ix);
                        c = {float(t4.x), float(t4.y), float(t4.z), float(t4.w)};
                    }
                    out.colors.push_back(c);
                }
                if (any_skin) {
                    SkinVertex sv;
                    const uint32_t vid = ix < mesh->vertex_indices.count
                                             ? mesh->vertex_indices.data[ix] : UINT32_MAX;
                    if (skin && mapping && vid < skin->vertices.count) {
                        const ufbx_skin_vertex& sk = skin->vertices.data[vid];
                        // ufbx sorts a vertex's weights by decreasing weight, so
                        // the first four are the four that matter. FBX allows any
                        // number of influences; the rest are dropped here and the
                        // remainder renormalised, which is what the four-slot
                        // SkinVertex and every console target want anyway.
                        int slot = 0;
                        for (uint32_t w = 0; w < sk.num_weights && slot < 4; ++w) {
                            const ufbx_skin_weight& sw = skin->weights.data[sk.weight_begin + w];
                            if (sw.cluster_index >= mapping->size()) continue;
                            const int32_t joint = (*mapping)[sw.cluster_index];
                            if (joint < 0) continue;
                            sv.joints[slot]  = static_cast<uint16_t>(joint);
                            sv.weights[slot] = float(sw.weight);
                            ++slot;
                        }
                    }
                    const float sum = sv.weights[0] + sv.weights[1] + sv.weights[2] + sv.weights[3];
                    if (sum > kEps) for (float& w : sv.weights) w /= sum;
                    else            sv.weights[0] = 1.0f;
                    out.skin.push_back(sv);
                }
            }
        }

        if (out.name.empty()) out.name = string_from_ufbx(mesh->name);
    }

    // Counted before the scene goes away, so a file that holds only curves or
    // NURBS can say so instead of looking empty. Neither is tessellated here:
    // this pipeline retopologises dense polygon sculpts.
    const size_t nurbs  = scene->nurbs_surfaces.count;
    const size_t curves = scene->line_curves.count + scene->nurbs_curves.count;

    ufbx_free_scene(scene);

    rep.source_vertices  = out.positions.size();
    rep.source_triangles = out.triangle_count();
    rep.ok               = !out.empty();
    if (!rep.ok) {
        if (nurbs || curves)
            rep.error = format("no polygon meshes: %zu NURBS surface%s and %zu curve%s, "
                               "which are not tessellated here", nurbs, nurbs == 1 ? "" : "s",
                               curves, curves == 1 ? "" : "s");
        else
            rep.error = "no triangles found";
    }
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
    else if (ext == "fbx")                     rep = load_fbx(path, out, opts);
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

    // Before the weld, while both sides of every uv seam still exist.
    const size_t triangles_at_snapshot = out.triangle_count();
    if (opts.load_materials && out.has_uvs()) {
        out.corner_uvs.resize(out.indices.size());
        for (size_t i = 0; i < out.indices.size(); ++i) out.corner_uvs[i] = out.uvs[out.indices[i]];
    }

    if (opts.weld) {
        const Aabb  box = out.bounds();
        const float eps = std::max(box.diagonal() * opts.weld_epsilon_rel, 1e-9f);
        rep.welded_vertices = out.weld(eps);
    }

    out.compact();

    // Anything that dropped a triangle after the snapshot invalidates it; the
    // per vertex uvs are still there to fall back on.
    if (!out.corner_uvs.empty() && out.triangle_count() != triangles_at_snapshot) {
        RD_DEBUG("corner uvs dropped: the triangle count moved from %zu to %zu after welding",
                 triangles_at_snapshot, out.triangle_count());
        out.corner_uvs.clear();
    }

    if (opts.normalise) out.normalise_to_unit(true);

    if (opts.force_recompute_normals || !out.has_normals())
        out.compute_normals(opts.sharp_angle_degrees);

    rep.seconds = watch.seconds();
    std::string material_note;
    if (out.materials && !out.materials->materials.empty()) {
        size_t bytes = 0;
        for (const SourceMaterial& m : out.materials->materials) bytes += m.base_color.byte_size();
        material_note = format(", %zu of %zu materials textured (%.1f MB)", rep.materials,
                               out.materials->materials.size(), double(bytes) / (1024.0 * 1024.0));
    }
    RD_INFO("loaded %s: %zu tri, %zu vtx, %zu joints%s in %s",
            path.filename().string().c_str(), out.triangle_count(), out.vertex_count(),
            out.armature.size(), material_note.c_str(), format_duration(rep.seconds).c_str());
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
            // Back to OBJ's v = 0 at the bottom of the image.
            std::snprintf(buf, sizeof(buf), "vt %.6g %.6g\n", t.x, 1.0f - t.y);
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
    return ext_lower == "obj" || ext_lower == "gltf" || ext_lower == "glb" ||
           ext_lower == "fbx";
}

const char* supported_extensions_filter() { return "obj,gltf,glb,fbx"; }

} // namespace rd::meshio
