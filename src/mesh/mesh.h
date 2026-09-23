#pragma once

// Indexed triangle mesh in struct-of-arrays form. Every stage of the pipeline
// speaks this type: the loader fills it, the retopo engines rewrite it, the
// baker decorates it, the exporter serialises it.

#include "core/image.h"
#include "core/math.h"

#include <cstdint>
#include <memory>
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
// Source materials
// ---------------------------------------------------------------------------
// What the asset was authored with, carried only so the bake can read the
// original artwork back out. Nothing in the geometry path looks at these: they
// exist to be sampled through a uv, and the low poly the pipeline produces has
// one material of its own.
struct SourceMaterial {
    std::string name;
    Texture     base_color;                     // empty when the material had none
    Vec4        base_factor{1.0f, 1.0f, 1.0f, 1.0f};  // multiplied onto the texture
};

// Held behind a shared_ptr on the Mesh because a Mesh is copied at nearly every
// stage of the pipeline and a character's material set is tens of megabytes.
// Nothing mutates a material set after load, so sharing is free.
struct MaterialSet {
    std::vector<SourceMaterial> materials;

    const SourceMaterial* find(uint16_t index) const
    {
        return index < materials.size() ? &materials[index] : nullptr;
    }
    size_t textured_count() const
    {
        size_t n = 0;
        for (const SourceMaterial& m : materials) n += m.base_color.empty() ? 0 : 1;
        return n;
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
    // True when `colors` already carry baked lighting - set by the bake, never
    // by a loader. A source's vertex colours are albedo or, as often, a mask
    // channel the artist painted for a shader (Quaternius ships every character
    // with COLOR_0 all white), and drawing those unlit turned the reference
    // renders the director judges against into a flat white cut-out.
    bool                     colors_prelit = false;
    std::vector<SkinVertex>  skin;      // empty or positions.size()
    std::vector<uint32_t>    indices;   // 3 per triangle

    // Optional per triangle region assignment produced by the segmenter.
    std::vector<uint16_t>    tri_region;

    // Optional per triangle material index into `materials`, filled by the
    // loader. Kept per triangle rather than per vertex because that is how
    // every source format expresses it, and because a vertex on a material
    // border belongs to both.
    std::vector<uint16_t>    tri_material;
    std::shared_ptr<const MaterialSet> materials;

    // Texture coordinates per corner rather than per vertex, snapshotted by the
    // loader before welding.
    //
    // The weld merges by position alone, deliberately: shell count, symmetry
    // and watertightness all have to see the surface the way the modeller built
    // it, not the way the uv layout cuts it up. But a uv seam is exactly a
    // vertex the modeller split for uv reasons, so welding it throws one of the
    // two coordinates away, and a triangle on the far side of the seam ends up
    // reading from the wrong place in the map. Keeping both here costs one
    // Vec2 per corner and leaves the topology untouched.
    std::vector<Vec2>        corner_uvs;   // empty or 3 * triangle_count()

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
    bool has_corner_uvs() const
    {
        return corner_uvs.size() == indices.size() && !corner_uvs.empty();
    }
    // The uv of corner `c` of triangle `t`, from the per corner snapshot when
    // there is one and from the welded vertex otherwise.
    Vec2 corner_uv(size_t t, int c) const
    {
        if (has_corner_uvs()) return corner_uvs[t * 3 + size_t(c)];
        const uint32_t v = indices[t * 3 + size_t(c)];
        return v < uvs.size() ? uvs[v] : Vec2{};
    }
    bool has_materials() const
    {
        return materials && !materials->materials.empty() &&
               tri_material.size() == triangle_count();
    }
    // The material a triangle was authored with, or null when the mesh carries
    // no material set - which is every mesh this pipeline generates itself.
    const SourceMaterial* material_for(size_t tri) const
    {
        if (!has_materials() || tri >= tri_material.size()) return nullptr;
        return materials->find(tri_material[tri]);
    }

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
