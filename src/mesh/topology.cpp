#include "mesh/topology.h"

#include "core/log.h"
#include "core/util.h"

#include <algorithm>
#include <unordered_map>

namespace rd {
namespace {

inline uint64_t edge_code(uint32_t a, uint32_t b)
{
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

} // namespace

void MeshTopology::clear()
{
    opposite.clear();
    edges.clear();
    half_edge_to_edge.clear();
    vert_tri_begin.clear();
    vert_tri.clear();
}

void MeshTopology::build(const Mesh& mesh)
{
    clear();
    const size_t tri_count = mesh.triangle_count();
    const size_t vtx_count = mesh.vertex_count();
    if (tri_count == 0 || vtx_count == 0) return;

    const size_t he_count = tri_count * 3;
    opposite.assign(he_count, kInvalidIndex);
    half_edge_to_edge.assign(he_count, kInvalidIndex);

    // --- unique edges ------------------------------------------------------
    std::unordered_map<uint64_t, uint32_t> edge_lookup;
    edge_lookup.reserve(he_count);
    edges.reserve(he_count / 2 + 8);

    for (size_t h = 0; h < he_count; ++h) {
        const uint32_t tri    = static_cast<uint32_t>(h / 3);
        const uint32_t corner = static_cast<uint32_t>(h % 3);
        const uint32_t a = mesh.indices[tri * 3 + corner];
        const uint32_t b = mesh.indices[tri * 3 + (corner + 1) % 3];
        const uint64_t code = edge_code(a, b);

        auto it = edge_lookup.find(code);
        if (it == edge_lookup.end()) {
            const uint32_t idx = static_cast<uint32_t>(edges.size());
            Edge e;
            e.v0   = std::min(a, b);
            e.v1   = std::max(a, b);
            e.tri0 = tri;
            e.uses = 1;
            edges.push_back(e);
            edge_lookup.emplace(code, idx);
            half_edge_to_edge[h] = idx;
        } else {
            Edge& e = edges[it->second];
            if (e.uses == 1) e.tri1 = tri;
            if (e.uses < 255) ++e.uses;
            half_edge_to_edge[h] = it->second;
        }
    }

    // --- half-edge opposites ----------------------------------------------
    // Directed lookup, so a properly oriented manifold pairs up exactly; edges
    // used by more than two faces are simply left unpaired.
    std::unordered_map<uint64_t, uint32_t> directed;
    directed.reserve(he_count);
    for (size_t h = 0; h < he_count; ++h) {
        const uint32_t tri    = static_cast<uint32_t>(h / 3);
        const uint32_t corner = static_cast<uint32_t>(h % 3);
        const uint32_t a = mesh.indices[tri * 3 + corner];
        const uint32_t b = mesh.indices[tri * 3 + (corner + 1) % 3];
        directed.emplace((uint64_t(a) << 32) | b, static_cast<uint32_t>(h));
    }
    for (size_t h = 0; h < he_count; ++h) {
        if (opposite[h] != kInvalidIndex) continue;
        const uint32_t tri    = static_cast<uint32_t>(h / 3);
        const uint32_t corner = static_cast<uint32_t>(h % 3);
        const uint32_t a = mesh.indices[tri * 3 + corner];
        const uint32_t b = mesh.indices[tri * 3 + (corner + 1) % 3];
        const auto it = directed.find((uint64_t(b) << 32) | a);
        if (it == directed.end()) continue;
        if (edges[half_edge_to_edge[h]].uses > 2) continue; // non manifold, leave open
        opposite[h]         = it->second;
        opposite[it->second] = static_cast<uint32_t>(h);
    }

    // --- vertex to triangle CSR -------------------------------------------
    vert_tri_begin.assign(vtx_count + 1, 0);
    for (uint32_t i : mesh.indices)
        if (i < vtx_count) ++vert_tri_begin[i + 1];
    for (size_t v = 0; v < vtx_count; ++v) vert_tri_begin[v + 1] += vert_tri_begin[v];

    vert_tri.assign(vert_tri_begin.back(), 0);
    std::vector<uint32_t> cursor(vert_tri_begin.begin(), vert_tri_begin.end() - 1);
    for (size_t t = 0; t < tri_count; ++t)
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = mesh.indices[t * 3 + c];
            if (v < vtx_count) vert_tri[cursor[v]++] = static_cast<uint32_t>(t);
        }
}

float MeshTopology::dihedral_angle(const Mesh& mesh, size_t edge) const
{
    const Edge& e = edges[edge];
    if (e.tri1 == kInvalidIndex) return 0.0f;

    const Vec3 n0 = mesh.triangle_normal(e.tri0);
    const Vec3 n1 = mesh.triangle_normal(e.tri1);
    const float c = clampf(dot(n0, n1), -1.0f, 1.0f);
    const float angle = std::acos(c);

    // Convexity test: does the edge bulge out relative to the two face centres?
    const Vec3 edge_dir = normalize(mesh.positions[e.v1] - mesh.positions[e.v0]);
    const Vec3 sign_vec = cross(n0, n1);
    return dot(sign_vec, edge_dir) >= 0.0f ? angle : -angle;
}

void MeshTopology::vertex_neighbours(const Mesh& mesh, uint32_t v,
                                     std::vector<uint32_t>& out) const
{
    out.clear();
    if (v + 1 >= vert_tri_begin.size()) return;
    for (uint32_t i = vert_tri_begin[v]; i < vert_tri_begin[v + 1]; ++i) {
        const uint32_t t = vert_tri[i];
        for (int c = 0; c < 3; ++c) {
            const uint32_t n = mesh.indices[t * 3 + c];
            if (n != v && std::find(out.begin(), out.end(), n) == out.end())
                out.push_back(n);
        }
    }
}

bool MeshTopology::manifold() const
{
    for (const Edge& e : edges)
        if (e.uses > 2) return false;
    return true;
}

} // namespace rd
