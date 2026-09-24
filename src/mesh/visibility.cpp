#include "mesh/visibility.h"

#include "core/log.h"
#include "core/thread_pool.h"
#include "core/util.h"
#include "mesh/bvh.h"

#include <cmath>

namespace rd {

void find_enclosed_triangles(const Mesh& mesh, const Bvh& bvh, std::vector<uint8_t>& hidden,
                             const EnclosedOptions& opts)
{
    const size_t tcount = mesh.triangle_count();
    hidden.assign(tcount, 0);
    if (tcount == 0 || opts.rays <= 0) return;

    const float diag   = std::max(mesh.bounds().diagonal(), 1e-6f);
    const float offset = diag * 1e-4f;
    const float far    = diag * 10.0f;

    // A Fibonacci spiral over the hemisphere, in the frame of the normal. The
    // same set for every face, so the result does not depend on a seed.
    std::vector<Vec3> local(size_t(opts.rays));
    const float golden = 2.39996323f;
    for (int i = 0; i < opts.rays; ++i) {
        const float z = 1.0f - (float(i) + 0.5f) / float(opts.rays);   // (0, 1]
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float a = golden * float(i);
        local[size_t(i)] = {r * std::cos(a), r * std::sin(a), z};
    }

    static const float kSample[4][3] = {{1.f / 3, 1.f / 3, 1.f / 3},
                                        {4.f / 6, 1.f / 6, 1.f / 6},
                                        {1.f / 6, 4.f / 6, 1.f / 6},
                                        {1.f / 6, 1.f / 6, 4.f / 6}};

    ThreadPool::shared().parallel_ranges(tcount, 64, [&](size_t b, size_t e, unsigned) {
        for (size_t t = b; t < e; ++t) {
            Vec3 pa, pb, pc;
            mesh.tri_positions(t, pa, pb, pc);
            const Vec3 cr = cross(pb - pa, pc - pa);
            const float len = length(cr);
            if (len <= 0.0f) continue;   // degenerate: leave it to the hygiene passes
            const Vec3 n = cr / len;
            Vec3 tangent, bitangent;
            basis_from_normal(n, tangent, bitangent);

            // Both sides. Half the assets this sees are double sided and a
            // fair share are wound inconsistently, so which way the normal
            // points says nothing about which side is the outside.
            bool seen = false;
            for (int side = 0; side < 2 && !seen; ++side) {
                const float sgn = side == 0 ? 1.0f : -1.0f;
                for (const auto& w : kSample) {
                    const Vec3 p = pa * w[0] + pb * w[1] + pc * w[2] + n * (offset * sgn);
                    for (const Vec3& d : local) {
                        const Vec3 dir = (tangent * d.x + bitangent * d.y + n * d.z) * sgn;
                        if (!bvh.occluded(p, dir, offset, far)) { seen = true; break; }
                    }
                    if (seen) break;
                }
            }
            hidden[t] = seen ? 0 : 1;
        }
    });
}

uint32_t label_shells(const Mesh& mesh, std::vector<uint32_t>& tri_shell)
{
    const size_t vcount = mesh.vertex_count(), tcount = mesh.triangle_count();
    std::vector<uint32_t> parent(vcount);
    for (uint32_t i = 0; i < vcount; ++i) parent[i] = i;
    auto find = [&](uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t a = find(mesh.indices[t * 3]);
        const uint32_t b = find(mesh.indices[t * 3 + 1]);
        const uint32_t c = find(mesh.indices[t * 3 + 2]);
        parent[b] = a;
        parent[find(c)] = a;
    }
    std::vector<uint32_t> dense(vcount, UINT32_MAX);
    uint32_t next = 0;
    tri_shell.assign(tcount, 0);
    for (size_t t = 0; t < tcount; ++t) {
        const uint32_t root = find(mesh.indices[t * 3]);
        if (dense[root] == UINT32_MAX) dense[root] = next++;
        tri_shell[t] = dense[root];
    }
    return next;
}

} // namespace rd
