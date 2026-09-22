# Roadmap

What this version does on purpose, and how to take each part further. Written
so that whoever picks a line item up knows where the seam is before they start.

---

## 1 · Segmentation on the GPU (SAM)

**Today.** Regions come from a deterministic multi-source Dijkstra over the dual
graph, seeded at joints and by farthest-point sampling, with edge weights that
punish crossing creases and bone boundaries. It needs no model, no download and
no GPU, it runs in single-digit milliseconds, and every region maps exactly onto
a set of source triangles. The language model then names the patches and says
which ones are one part.

This was chosen over Segment Anything because SAM on a CPU is roughly fifteen
seconds *per view*, and a 2.4 GB checkpoint is a lot to ask before the first
triangle moves.

**Why SAM is still worth having.** The geometric split follows geometry, which
is exactly its limitation: it cannot see that a painted belt and the torso under
it are different *things*, or that a strap crossing a chest is one object. SAM
segments what a person would call a part, not what the curvature says.

**The seam is already there.** `segment_mesh` fills a `Segmentation`
(`tri_region` plus a `Region` list). Anything that can produce that structure is
a drop-in replacement. The intended shape:

```cpp
// segment/segment.h
struct ISegmenter {
    virtual Segmentation run(const Mesh&, const MeshAnalysis&, const ViewSet&) = 0;
};
```

**What the GPU path looks like.**

1. *Sidecar process.* A Python process (`tools/sam_server.py`) holding the model
   resident, talking JSON over stdin/stdout — `llm/process.h` already spawns and
   pipes processes with timeouts and cancellation, so there is nothing new to
   write on the C++ side. Keeping it out of process also keeps the licence
   surface clean and means a crash in Torch does not take the editor with it.
2. *Input.* The renderer already writes the view set the model sees. Feed SAM
   the same PNGs, plus the depth buffer if `render_mask` is extended to emit it.
   Automatic mask generation (`SamAutomaticMaskGenerator`) on eight turntable
   views is the obvious starting point.
3. *Back onto the mesh.* For each view, for each mask, cast a ray per covered
   pixel through the camera into the existing `Bvh` and vote for the hit
   triangle. `Bvh::intersect` is already parallel-ready through the shared
   thread pool. Triangles collect votes across views; the winner takes the
   triangle. A triangle with no votes (never visible) inherits from its
   neighbours by the same Dijkstra that runs today, so the two approaches
   compose rather than compete.
4. *Cleanup.* Reuse the existing sliver absorption and the dense renumbering at
   the end of `segment_mesh`.

**Hardware.** SAM ViT-H needs roughly 8 GB of VRAM; ViT-B fits in 4 GB and is
good enough for this, because the masks are only a starting point that the
director then merges. On a machine with no CUDA device the sidecar should report
that at startup and the pipeline should fall back to the geometric split rather
than stalling. `MobileSAM` or `FastSAM` are the sensible middle ground: a few
hundred milliseconds per view on a mid-range GPU, small enough to ship.

**Cost of doing it.** The C++ side is perhaps 400 lines: a sidecar client, the
mask-to-triangle projection, and a settings page. The real cost is the Python
dependency and the checkpoint, which is exactly why it is optional and behind an
interface rather than in the critical path.

---

## 2 · Instant Meshes as a third backend

**Today.** The quad path is implemented here: a density-driven isotropic remesh
followed by greedy quad pairing. It is self-contained, has no dependencies, and
respects the per-region budgets the director sets.

Instant Meshes (BSD-3) and QuadriFlow (BSD-3) would give better edge flow on
organic shapes, particularly around joints. Both pull in Eigen, which is MPL2 —
weak copyleft at file granularity. That is almost certainly fine for a
commercial product, but it is a decision to make deliberately rather than
inherit, which is why it is not vendored by default.

`geom/retopo.h` dispatches on `RetopoBackend`; adding a third value and a
`geom/instant_meshes.cpp` that converts the density field into their per-vertex
scale field is a contained job. The interface it has to satisfy is already the
one `quad_field_retopo` satisfies.

---

## 3 · Differentiable rendering

The obvious next step for the optimisation loop: instead of the director nudging
knobs between iterations, optimise vertex positions directly against the
rendered difference from the high poly.

PyTorch3D and Mitsuba 3 are both BSD. **nvdiffrast and nvdiffmodeling are
NVIDIA-licensed, non-commercial only** — usable for research, not for anything
shipped.

This would sit after the retopo stage as a position-only refinement: topology
fixed, vertices free, silhouette and shading error as the loss, hard rules
re-applied afterwards. The existing `SilhouetteError` metric is already the
right objective; it just needs a gradient.

---

## 4 · Smaller things worth doing

**UV seams are the binding constraint on the quadric path.** Its triangle fans
unwrap into dozens of small charts, and 30 to 45 per cent of vertices end up
existing only to carry a seam, which is what forces the triangle budget down —
not the triangle limit itself. The quad field path already gets this to 20 per
cent by being regular. Closing the gap for hard surface work is worth doing:

- Seam-aware chart seeding that prefers cutting along creases the director
  already marked as hard edges, rather than wherever the parameterisation likes.
- A post-unwrap pass that merges charts whose parameterisations agree, which is
  cheaper than making xatlas produce fewer of them in the first place.

**Slivers.** The worst triangle quality after mirroring sits around 0.005. They
are harmless on screen but ugly in a strip. A short edge-flip and smoothing pass
constrained to the symmetry seam would clear most of them.

**The quadric does not always reach its budget.** Flip rejections dominate on
high-curvature areas; the sweep loop mitigates it but does not remove it.
Allowing a bounded amount of normal deviation that grows with each sweep would
close the remaining gap.

**Skinned assets.** The transfer works and the influence limit is enforced, but
there is no bind-pose validation and no check that the deformation actually
survives. Rendering a couple of extreme poses and putting them in front of the
director is the natural extension, and the camera rig already supports adding
views.

**Texture streaming.** The palette quantiser is a median cut with
Floyd-Steinberg. A perceptual palette (weighting luminance error above chroma)
would look noticeably better at sixteen colours, which is where a 4-bit CLUT
target actually lives.

**More target profiles.** PS1, Saturn, N64 and Dreamcast all have interestingly
different constraints — affine texture mapping, no perspective correction,
4 KB texture caches. The profile structure already covers most of it; what is
missing is per-target quirks like N64's texture size limits driving the unwrap.
