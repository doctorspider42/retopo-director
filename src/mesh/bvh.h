#pragma once

// Binned SAH bounding volume hierarchy over triangles.
// Used for ambient occlusion rays, high-poly projection during remeshing and
// the silhouette error metric. Build is parallel over the top levels; queries
// are branch-light and allocation free.

#include "core/math.h"
#include "mesh/mesh.h"

#include <cstdint>
#include <vector>

namespace rd {

struct RayHit {
    float    t        = 0.0f;
    float    u        = 0.0f;
    float    v        = 0.0f;
    uint32_t triangle = kInvalidIndex;
    bool     hit() const { return triangle != kInvalidIndex; }
};

struct ClosestHit {
    Vec3     point;
    float    distance2 = 0.0f;
    uint32_t triangle  = kInvalidIndex;
    bool     hit() const { return triangle != kInvalidIndex; }
};

class Bvh {
public:
    void build(const Mesh& mesh);
    void clear();

    bool   empty()  const { return nodes_.empty(); }
    Aabb   bounds() const { return nodes_.empty() ? Aabb{} : nodes_[0].box; }
    size_t node_count() const { return nodes_.size(); }
    size_t depth() const { return max_depth_; }

    // Nearest hit along the ray. `dir` need not be normalised, but t is then
    // expressed in units of |dir|.
    RayHit intersect(Vec3 origin, Vec3 dir, float tmin = 1e-4f,
                     float tmax = std::numeric_limits<float>::max()) const;

    // Early-out visibility test, roughly twice as fast as intersect().
    bool occluded(Vec3 origin, Vec3 dir, float tmin, float tmax) const;

    // Nearest point on the surface. `max_distance` prunes the search.
    ClosestHit closest_point(Vec3 p,
                             float max_distance = std::numeric_limits<float>::max()) const;

    // Interpolated normal at a hit, using vertex normals when present.
    Vec3 shading_normal(const RayHit& hit) const;
    Vec3 geometric_normal(uint32_t triangle) const;

    // Signed distance sign test via ray parity. Slower than closest_point,
    // only used when we genuinely need inside/outside.
    bool inside(Vec3 p) const;

    const Mesh* mesh() const { return mesh_; }

private:
    struct Node {
        Aabb     box;
        // Leaf (count > 0): index of the first primitive in prims_.
        // Interior (count == 0): index of the RIGHT child. The left child is
        // always the node right after this one, because build_range emits it
        // immediately; the right child lands wherever the left subtree ends.
        uint32_t first = 0;
        uint32_t count = 0;
    };

    uint32_t build_range(uint32_t begin, uint32_t end, uint32_t depth);

    const Mesh*           mesh_ = nullptr;
    std::vector<Node>     nodes_;
    std::vector<uint32_t> prims_;      // triangle indices, reordered
    std::vector<Aabb>     prim_boxes_;
    std::vector<Vec3>     prim_centroids_;
    size_t                max_depth_ = 0;
};

} // namespace rd
