# Architecture

## The rule everything else follows

The language model fills in one JSON structure. Everything downstream of that
structure is deterministic: the same knob panel and the same high poly always
produce the same low poly, byte for byte. That is why the model is never handed
a vertex buffer, and why the validator sits between the geometry engine and the
model rather than after it.

## Module map

```
src/
  core/       logging, math, paths, thread pool, JSON, main-thread dispatcher
  mesh/       Mesh, loaders, half-edge topology, BVH, analysis
  segment/    region decomposition
  knobs/      target profile, knob panel
  geom/       density field, quadric simplifier, quad field remesher,
              hard rules, backend dispatch
  bake/       texture container and palette, UV unwrap, AO and lighting bake
  validate/   the hard checks
  llm/        process spawning, HTTPS, the three director backends
  render/     OpenGL loader, camera rig, offscreen renderer
  export/     cache order, strips, writers, the .rdmesh container
  pipeline/   prompts and the orchestrator
  ui/         theme, widgets, panels, application shell
```

Dependencies point one way: `ui` knows about everything, `pipeline` knows about
every engine module, engine modules know only `core` and `mesh`. Nothing in
`geom`, `bake` or `validate` knows that a language model exists.

## The stages

**1 · Load and analyse.** `meshio::load` normalises the mesh into a unit sphere
so every distance threshold downstream is resolution independent, welds the
duplicates a DCC left behind, and keeps the inverse transform so exports go back
into the original space. `analyse_mesh` then computes, once: a half-edge
topology, a BVH, per-vertex curvature from the cotangent Laplacian, dihedral
angles and crease flags, distance to the nearest bone and joint, a cheap ambient
term, and the dominant mirror plane.

Curvature is normalised against a high percentile rather than the maximum. One
pinched vertex on a sculpt would otherwise flatten the entire field to zero.

**2 · Render.** The camera rig comes from the profile: named cameras authored in
metres (`three metres behind the character, pivot at 1.1 m`) plus a turntable.
The mesh is normalised, so the profile's `reference_height_m` is what converts
between the two. The same renderer draws the viewport and the offscreen captures,
which means what the artist inspects and what the model judges cannot drift
apart.

**3 · Segment.** A multi-source Dijkstra over the dual graph. Edge weight is
distance, scaled up by normal deviation, by crossing a crease, and by crossing
between triangles skinned to different joints. Seeds are placed at joints when
there is an armature and by farthest-point sampling otherwise, then slivers are
absorbed into whichever neighbour they agree with best. Deterministic, and every
region maps exactly onto a set of source triangles.

The model does not draw these boundaries. It gets the patches, names them, and
says which ones are really one part.

**4 · Knob panel.** Region shares, detail priority, geometry-versus-texture,
hard edge angle, silhouette and boundary locks, symmetry lock, curvature bias.
Globally: backend, symmetry, joint loop density, merge aggressiveness, curvature
and silhouette weights, smoothing, quad dominance, and the bake settings.

`apply_patch` merges only the keys the model actually sent, so a reply with
three keys does not wipe the other forty. Unknown keys are dropped and logged.

**5 · Density field.** The translation layer. Per region: budget and area give a
base edge length. Per triangle that base is modulated by curvature, joint
proximity, silhouette membership, cavity occlusion and the region's fidelity
setting, then each region is calibrated in a few passes so the field actually
predicts its budget instead of merely pointing in the right direction.

Neither retopo backend knows what a region or a budget is. They read this field.

**6 · Retopology.** Two paths.

*Quadric* is a Garland-Heckbert edge collapse where the cost is the quadric
error divided by the square of the locally desired edge length. A region the
director gave a fat budget resists collapse; one it wrote off melts away.
Boundaries, creases and region borders get constraint planes. Collapses run in
sweeps: an edge rejected for flipping a normal is dropped from the queue, but
later collapses can make it legal again, so a single pass stalls far above
budget.

*Quad field* runs a coarse quadric pass to get into range, then remeshes
incrementally against the density field — split, collapse, flip toward valence
six, tangential relaxation, reprojected onto the high poly every pass — then
pairs adjacent triangles into quads where the pair is well shaped and
re-triangulates each quad along its better diagonal. The console still eats
triangles; the quad step is what makes them line up.

`Auto` measures what fraction of edges are creases. Below 18 per cent, or on
anything skinned, it treats the model as organic and uses the quad field;
above, as hard surface and uses the quadric.

That threshold is not only about edge flow. The regular topology the quad field
produces unwraps into a handful of large charts, while the triangle fans a
quadric collapse leaves behind shatter the atlas into dozens of small ones — and
every chart border duplicates its vertices. On the test sculpt the two paths
differ by more than the edge flow suggests:

| | quadric | quad field |
|---|---|---|
| triangles / vertices | 1 326 / 1 054 | 1 118 / 702 |
| uv charts | 64 | 9 |
| vertices existing only for a seam | 37 % | 20 % |
| triangles per strip | 8.1 | 10.2 |
| worst uv stretch | 19.6x | 2.0x |
| silhouette error | 0.0505 | 0.0087 |
| budget re-fit attempts needed | 4 | 1 |

On a console budget the vertex count is usually the limit that actually binds,
so the unwrap behaviour matters as much as the topology.

**7 · Hard rules.** Unconditional. Symmetry by cutting at the plane, keeping the
larger half and mirroring it. Non-manifold fins removed. Shell count capped.
Deformation loops inserted around joints that do not have enough vertices to
hold a crease. Orientation made consistent across shared edges, with one global
sign per shell from its signed volume.

If the model asked for something a rule forbids, the rule wins and the report
says the model was overruled.

**8 · Bake.** xatlas unwraps, deliberately without being given our shading
normals: they are hard-edged for creases, and xatlas would cut a chart at every
one of them. Charts are then rasterised, each texel is projected onto the high
poly along the low poly normal, and ambient occlusion plus a fixed three-point
light rig is evaluated there. On hardware with no per pixel lighting this is not
a shortcut; it is the entire shading model, so the viewport shader uses the same
rig and shows baked colours without lighting them a second time.

**9 · Validate.** Counts come from the buffer that ships. Connectivity does not:
the unwrap splits a vertex at every chart border, which would otherwise be
reported as a hundred shells with open edges, so shells, manifoldness and
boundary are measured on a position-welded copy. Failures are phrased as
numbers — *region head is 340 triangles over budget* — because that is what the
model is given when the result is rejected.

**10 · Export.** meshoptimizer for cache order, overdraw and fetch order, then
strips. OBJ, glTF, GLB, and a documented binary container.

## Budget fitting

The unwrap duplicates vertices along every seam, so a triangle count that fits
can still blow the vertex limit, and collapses the link condition refuses leave
a few triangles over. Neither is a question of taste. The engine therefore
shrinks its own budget and rebuilds, up to six times, keeping the best attempt
rather than the last one — the seam count is not a smooth function of the
triangle budget, so the last attempt is often not the best. Only then, if it is
still illegal, does the director hear about it.

## Threading

The pipeline runs on a worker thread. The GL context lives on the thread that
made the window, so every render the pipeline needs is posted to
`MainThreadDispatcher` and the worker blocks until the main loop runs it. The UI
reads results under a mutex and only re-uploads to the GPU when a version
counter changes.

Headless uses exactly the same arrangement. It creates a window with
`GLFW_VISIBLE` off purely to obtain a context, and its loop drains the
dispatcher the same way the windowed loop does. There is no second rendering
path to keep in step, which is the point: a batch run and an interactive run
produce the same images and therefore the same silhouette numbers.

Data-parallel loops — BVH queries, AO rays, curvature, silhouette tests — go
through one shared thread pool, so the thread count is tuned in one place.

## Determinism

Same input, same knobs, same output. The RNG is a seeded PCG32, region ids are
renumbered densely and deterministically, ties in budget rounding break on
priority and then on id, and every map iteration that affects geometry is done
over a sorted key list. The only non-deterministic input is the director itself,
and its answer is written to disk so any run can be replayed exactly.
