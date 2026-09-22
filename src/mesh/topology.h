#pragma once

// Static connectivity built once per mesh: half-edge opposites, a unique edge
// list and a CSR vertex-to-triangle map. Read only, cache friendly, rebuilt
// from scratch whenever geometry changes (it is cheap compared to everything
// that consumes it).

#include "mesh/mesh.h"

#include <cstdint>
#include <vector>

namespace rd {

struct MeshTopology {
    // Half-edge h belongs to triangle h/3 and starts at corner h%3.
    // opposite[h] is the half-edge running the other way, or kInvalidIndex.
    std::vector<uint32_t> opposite;

    struct Edge {
        uint32_t v0 = 0, v1 = 0;          // v0 < v1
        uint32_t tri0 = kInvalidIndex;    // always valid
        uint32_t tri1 = kInvalidIndex;    // kInvalidIndex on a boundary
        uint8_t  uses = 0;                // >2 means non manifold
    };
    std::vector<Edge>     edges;
    std::vector<uint32_t> half_edge_to_edge;   // size 3 * triangle_count

    // CSR: triangles touching vertex v are
    // vert_tri[vert_tri_begin[v] .. vert_tri_begin[v+1])
    std::vector<uint32_t> vert_tri_begin;
    std::vector<uint32_t> vert_tri;

    size_t triangle_count() const { return opposite.size() / 3; }
    size_t vertex_count()   const { return vert_tri_begin.empty() ? 0 : vert_tri_begin.size() - 1; }

    void build(const Mesh& mesh);
    void clear();

    uint32_t half_edge_start(const Mesh& m, uint32_t h) const { return m.indices[h]; }
    uint32_t half_edge_end(const Mesh& m, uint32_t h) const
    {
        const uint32_t tri = h / 3, corner = h % 3;
        return m.indices[tri * 3 + (corner + 1) % 3];
    }

    // Triangle adjacent across corner c of triangle t, or kInvalidIndex.
    uint32_t neighbour(uint32_t tri, int corner) const
    {
        const uint32_t o = opposite[tri * 3 + corner];
        return o == kInvalidIndex ? kInvalidIndex : o / 3;
    }

    bool is_boundary_edge(size_t edge) const { return edges[edge].tri1 == kInvalidIndex; }
    bool is_manifold_edge(size_t edge) const { return edges[edge].uses <= 2; }

    // Signed dihedral angle in radians across an interior edge. Positive means
    // convex (a ridge), negative means concave (a valley). Zero on boundaries.
    float dihedral_angle(const Mesh& mesh, size_t edge) const;

    // Vertices sharing an edge with v.
    void vertex_neighbours(const Mesh& mesh, uint32_t v, std::vector<uint32_t>& out) const;

    bool manifold() const;
};

} // namespace rd
