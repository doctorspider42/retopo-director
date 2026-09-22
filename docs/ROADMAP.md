# Roadmap

What this version does on purpose, and how to take each part further. Written
so that whoever picks a line item up knows where the seam is before they start.

---

## 1 · Segmentation on the GPU (SAM) — *shipped, with edges left*

**Today.** Two segmenters sit behind `ISegmenter` (`segment/segmenter.h`) and
the pipeline picks one with `SegmenterOptions::kind`:

- **Geometric**, the default and the fallback. A multi source Dijkstra over the
  dual graph, seeded at joints and by farthest point sampling, with edge weights
  that punish crossing creases and bone boundaries. No model, no download and no
  GPU, single digit milliseconds, every region maps onto source triangles.
- **SAM** (`segment/sam.cpp` plus `tools/sam_server.py`). A Python sidecar holds
  the model and speaks one JSON request / one JSON reply over stdin and stdout,
  through the same `run_process` the CLI backends use. The renders the director
  already has go in; per view masks come back run length encoded. Each mask is
  projected with one ray per strided pixel through the camera that rendered the
  view into the existing `Bvh`, votes for the triangles it hits, and the mask
  with the most votes takes the triangle. Labels are then split into their
  connected components, triangles no camera ever saw are filled in by the same
  Dijkstra, and both paths finish through the shared sliver absorption,
  renumbering and statistics (`finalize_segmentation`).

Everything that can go wrong - no checkpoint, no CUDA device, no Torch, a
sidecar that falls over, a `--no-gpu` run with no renders - costs a warning and
falls back to the geometric split rather than the run.

**What is deliberately still missing.**

1. *The sidecar is not resident.* It loads the model, answers once and exits, so
   a run pays the model load once and an interactive re-segment pays it again.
   Keeping it alive needs a long lived pipe rather than `run_process`, which is
   a change to `llm/process.h`, not to the segmenter.
2. *Automatic masks only.* `SamAutomaticMaskGenerator` on the turntable views,
   no prompting. Point or box prompts, seeded from the regions the director
   already named, are the obvious next step and would let a person correct a
   split by clicking.
3. *No depth.* `render_mask` emits coverage, not depth. Feeding SAM the depth
   buffer as a fourth channel separates a strap from the chest behind it in
   cases where the shaded render does not.
4. *Masks do not carry identity across views.* A mask in the front view and the
   one in the back view that cover the same belt are two labels that happen to
   meet. Matching them (by projected triangle overlap, which is already
   computed) would cut the region count before the merge down to the budget
   does it by area.

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
