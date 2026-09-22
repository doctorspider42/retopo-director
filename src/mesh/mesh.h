#pragma once

// Indexed triangle mesh in struct-of-arrays form. Every stage of the pipeline
// speaks this type: the loader fills it, the retopo engines rewrite it, the
// baker decorates it, the exporter serialises it.

#include "core/math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rd {

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
// Sentinel for Mesh::tri_region: this triangle belongs to no region yet.
constexpr uint16_t kNoRegion     = 0xFFFFu;

// ---------------------------------------------------------------------------
// Armature
// ---------------------------------------------------------------------------
struct Joint {
    std::string name;
    int32_t     parent = -1;      // index into Armature::joints, -1 for roots
    Vec3        bind_position;    // model space, rest pose
    float       influence_radius = 0.0f; // filled by analysis, not by the loader
};

struct Armature {
    std::vector<Joint> joints;

    bool   empty() const { return joints.empty(); }
    size_t size()  const { return joints.size(); }

    // Bone segment i is (joints[i].bind_position -> parent bind_position).
    // Roots have no segment and are skipped.
    bool  bone_segment(size_t joint, Vec3& a, Vec3& b) const;
    int   find(std::string_view name) const;
    Aabb  bounds() const;
};

// ---------------------------------------------------------------------------
// Skinning
// ---------------------------------------------------------------------------
struct SkinVertex {
    uint16_t joints[4]  = {0, 0, 0, 0};
    float    weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    uint16_t dominant_joint() const
    {
        int best = 0;
        for (int i = 1; i < 4; ++i)
            if (weights[i] > weights[best]) best = i;
        return weights[best] > 0.0f ? joints[best] : 0;
    }
};

// ---------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------
struct Mesh {
    std::string              name;
    std::vector<Vec3>        positions;
    std::vector<Vec3>        normals;   // empty or positions.size()
    std::vector<Vec2>        uvs;       // empty or positions.size()
    std::vector<Vec4>        colors;    // empty or positions.size(), linear RGBA
    std::vector<SkinVertex>  skin;      // empty or positions.size()
    std::vector<uint32_t>    indices;   // 3 per triangle

    // Optional per triangle region assignment produced by the segmenter.
    std::vector<uint16_t>    tri_region;

    Armature                 armature;

    // Transform applied by the loader to normalise scale/orientation, kept so
    // exports can be written back in the original space.
    Mat4                     import_transform = Mat4::identity();
    float                    import_scale     = 1.0f;

    size_t vertex_count()   const { return positions.size(); }
    size_t triangle_count() const { return indices.size() / 3; }
    bool   empty()          const { return positions.empty() || indices.size() < 3; }

    bool has_normals() const { return normals.size() == positions.size() && !normals.empty(); }
    bool has_uvs()     const { return uvs.size()     == positions.size() && !uvs.empty(); }
    bool has_colors()  const { return colors.size()  == positions.size() && !colors.empty(); }
    bool has_skin()    const { return skin.size()    == positions.size() && !skin.empty(); }

    void clear();

    Vec3 tri_vertex(size_t tri, int corner) const { return positions[indices[tri * 3 + corner]]; }
    void tri_positions(size_t tri, Vec3& a, Vec3& b, Vec3& c) const
    {
        a = positions[indices[tri * 3 + 0]];
        b = positions[indices[tri * 3 + 1]];
        c = positions[indices[tri * 3 + 2]];
    }

    Vec3  triangle_normal(size_t tri) const;
    float triangle_area(size_t tri) const;
    Vec3  triangle_centroid(size_t tri) const;

    Aabb  bounds() const;
    float surface_area() const;
    Vec3  centroid_area_weighted() const;

    // Angle weighted vertex normals. Safe to call on a mesh with none.
    void compute_normals(float sharp_angle_degrees = 180.0f);

    // Welds vertices closer than `epsilon` (absolute, model space) and drops
    // degenerate triangles. Returns the number of vertices removed.
    size_t weld(float epsilon);

    // Removes triangles with zero area or repeated corners.
    size_t remove_degenerate(float area_epsilon = 1e-12f);

    // Drops vertices no triangle references.
    size_t compact();

    // Scales/translates so the mesh fits a unit sphere centred on the origin,
    // recording the inverse in import_transform.
    void normalise_to_unit(bool recentre = true);

    // Statistics used all over the UI and the validator.
    struct Stats {
        size_t vertices        = 0;
        size_t triangles       = 0;
        size_t edges           = 0;
        size_t boundary_edges  = 0;
        size_t nonmanifold_edges = 0;
        size_t isolated_vertices = 0;
        size_t shells          = 0;
        float  surface_area    = 0.0f;
        float  min_edge        = 0.0f;
        float  max_edge        = 0.0f;
        float  mean_edge       = 0.0f;
        float  min_quality     = 0.0f;  // triangle shape, 0 = sliver
        Aabb   bounds;
        bool   manifold        = true;
        bool   closed          = true;
    };

    Stats compute_stats() const;
};

// Appends `src` into `dst`, offsetting indices. Armatures must match or be empty.
void mesh_append(Mesh& dst, const Mesh& src);

// Extracts the triangles whose value in `mask` is true into a standalone mesh.
// `vertex_map` receives, per source vertex, the new index or kInvalidIndex.
Mesh mesh_extract(const Mesh& src, const std::vector<bool>& tri_mask,
                  std::vector<uint32_t>* vertex_map = nullptr);

} // namespace rd
