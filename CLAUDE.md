# Working on Retopo Director

## Build and test

```bash
cmake --preset mingw-release
cmake --build --preset mingw-release

# unit tests plus whole runs on the synthetic meshes, ~25 seconds
ctest --preset mingw-release

# one mesh, the whole deterministic pipeline, no model, a couple of seconds
./build/mingw-release/bin/retopo-director.exe --headless --no-llm --no-settings \
    --verbose --mesh <high.obj> --project /tmp/run

# the benchmark: every corpus mesh, compared against a baseline
python tools/bench.py --root <corpus> --baseline <previous>/metrics.json
```

`--no-settings` matters. Without it a headless run reads settings.json, which
holds whatever the window last had selected - profile, segmenter, director -
and two runs of the same command stop meaning the same thing. The benchmark
and the tests always pass it, with an explicit `--profile`.

Exit code 0 means validation passed, 3 means it failed. `--verbose` mirrors the
log to stderr, which is the only way to see what the engine is doing in headless
mode.

Headless creates a hidden GL context, so this exercises the renderer, the
region overlay and the silhouette metric too — it is the same run the windowed
build does. Use `--no-gpu` only to test the render-free fallback.

To look at the interface without needing it focused or even visible:

```bash
./build/mingw-release/bin/retopo-director.exe --mesh <high.obj> --run --no-llm \
    --screenshot shot.png --exit-after 20
```

The capture comes off the back buffer, so it contains the application window and
nothing else. F12 does the same thing interactively.

There is no test mesh in the repository. `tools/make_test_meshes.py <dir>`
writes four (sphere, torus, lopsided blob, an A-pose figure with thin limbs),
and `tools/fetch_corpus.py <corpus>` fetches five CC0 scans and a coffee cart
from Poly Haven.
`bench/corpus.json` lists those plus CC0 character packs found under `--root`.
Judge a change by the benchmark table, not by one mesh: nearly every change in
the geometry layer helps some assets and hurts others.

The number to read is `px`, the mean outline offset in pixels. The silhouette
fraction depends on the shape - thin limbs are all perimeter - so 0.06 on a
figure with its arms out can be a better fit than 0.02 on a bust.

To repeat a run with the director without paying for the model:
`--replay <that run>/reports/prompts`. Every reply is recorded there as
`NN_<label>_text.txt`, and the Replay backend hands them back in order.

Sanitizers need Linux: the `linux-asan` and `linux-tsan` presets build GLFW
with its null platform, so they need no X11, and CI runs both on every PR.

## Conventions

- C++20. No exceptions in the engine path: functions return a result struct with
  an `ok` flag and an `error` string.
- Struct-of-arrays meshes. `Mesh` is the only geometry type that crosses a
  module boundary.
- Every long-running stage takes a `std::function<void(float, const char*)>`
  progress callback and checks a cancellation flag.
- Anything that can fail on malformed input from the model goes through
  `json_get` / `json_parse_lenient`, which never throw and always have a
  fallback.
- Comments explain why, not what. If a constant looks arbitrary, say where it
  came from.

## Things that will bite you

**The BVH stores the right child index, not the left.** The left child is always
`node_index + 1`; `Node::first` is the right child for an interior node and the
first primitive for a leaf. Getting this wrong produces a BVH that silently
returns nearly-correct nonsense: rays still hit, closest-point queries still
return a triangle, and everything downstream quietly degrades.

**`MeshAnalysis::bvh` holds a pointer to the mesh it was built from.** Any stage
that copies the mesh out of the pipeline results must rebuild the BVH against
its own copy. `stage_iterate` does this at the top of every iteration.

**The unwrap rebuilds the vertex buffer.** Region ids, skin weights and anything
else indexed by vertex must be re-derived afterwards. Vertex counts jump 30 to
45 per cent at this point, which is why the budget re-fit loop exists.

**Validation measures connectivity on a welded copy.** Shell count, manifoldness
and boundary edges on the post-unwrap buffer are meaningless: every chart border
looks like a separate shell with open edges.

**Orientation must be fixed topologically.** Flipping individual triangles
against a geometric reference leaves the surface inconsistently wound, which
makes xatlas shatter the atlas. Propagate across shared edges, then pick one
global sign per shell.

**A segmenter that only labels triangles is not finished.** `segment_mesh` and
the SAM path both end in `finalize_segmentation`, which does the dense
renumbering, the sliver absorption and the statistics, and `refresh_statistics`
keys off the region ids: renumber the regions without renumbering
`tri_region` in the same pass and every region silently reports somebody else's
area. Fill the gaps with `grow_unassigned_regions` before that.

**`max_shells` is a demolition charge, not a check.** The shell limit does not
reject a mesh with too many pieces, it deletes pieces until the count fits,
largest first. A character authored as a body plus hood, belts, boots and
straps is sixty five shells, so `max_shells: 1` silently reduces it to a torso
and the validator then reports "1 shell, ok" over the wreckage.

`ps2_character` allows 32. A skinned body is already around sixteen shells on
its own - torso, head, two upper arms, two forearms, two hands, pelvis, two
thighs, two shins, two boots - so a limit near that number spends the whole
budget on the body and demolishes every costume piece. Measured on a costumed
character, quadric scores the same at 16, 32 and 48 and falls apart at 64,
where shell debris starts surviving; `min_shell_area_share` is what removes
that debris, and `0` disables the rule entirely, debris included, so it scores
worse than any of them.

**`std::filesystem::exists` cannot see a Windows app execution alias.** The
Microsoft Store installs python as a reparse point that only CreateProcess can
resolve; opening it fails, so `exists()` says no and the interpreter looks
missing on a machine where typing `python` works. `which()` uses
GetFileAttributes for exactly this reason.

**Every loader hands back the same convention.** Y up, right handed, metres,
one mesh. OBJ and glTF are already there; FBX is converted by ufbx at load
(`target_axes`, `target_unit_meters`), because it is authored in centimetres as
often as in metres and Z up as often as Y up. Do not add a second conversion
downstream - the profile frames its cameras in metres and would move.

**Never fill a `std::vector<bool>` from a parallel loop.** It packs 64
entries into a word, so two lanes writing neighbouring entries lose each
other's bits. `MeshAnalysis::edge_sharp` was one, and creases came and went
between runs of the same mesh until TSan named it. Use bytes.

**`normalize()` has an absolute epsilon and the loader works in metres.** A
small asset's millimetre triangles, and the slivers every scan is full of, have
cross products below it. Anything that judges a triangle by its normalised
normal - fold tests above all - must compare raw area vectors against their
own length instead, or it quietly refuses every operation on small geometry.

**Inside the pipeline, uv v = 0 is the top row of the image.** That is glTF's
convention, the bake rasteriser's and the GL upload's. OBJ and FBX put v = 0 at
the bottom, so their loaders and the OBJ writer convert at the file boundary
and nothing else may. The sampler used to flip instead, which read every glTF
texture upside down in the bake.

**Bake occlusion is traced from the low poly, not from the source.** Sculpted
sources run through themselves at every fold, and a texel that lands on such a
point sees nothing from there: that drew black scribbles over every character.

**A low poly can have more than one texture page.** `Mesh::tri_page` says
which; `ps2_character` gives the head a 512 page of its own, claimed by the
`cutscene_face` camera. Each page is its own unwrap, bake and palette, the
exporter keeps pages contiguous and never strips across them, and anything
that shows the result with one texture goes through `display_atlas`.

**A source's vertex colours are not lighting.** `Mesh::colors_prelit` says
whether they carry baked light, and only the bake sets it. Assets ship colour
channels as shader masks (every Quaternius character has COLOR_0 all white),
and drawing those unlit makes the reference renders a white cut-out.

**The run hands back its best iteration, not its last.** The director's next
panel is a guess and is often worse. `PipelineResults::kept_iteration` says
which one is in the results, and the panel restored with it is the one that
built it.

**MinGW links the runtime dynamically by default**, which makes the executable
unusable outside a shell that has the toolchain on `PATH`. The build passes
`-static`.

**Long quoted heredocs get mangled by some shells here.** Write patch scripts to
a file and run them rather than piping them in.

## Where to change what

| you want to | go to |
|---|---|
| change what the model is told | `src/pipeline/director.cpp` — all prompt text is there |
| add a knob | `src/knobs/knobs.h`, then read it in `src/geom/density.cpp` |
| change what the model may argue about | `src/knobs/advice.h` |
| add a hard check | `src/validate/validator.cpp` |
| add a retopo backend | implement the shape of `quad_field_retopo`, dispatch in `src/geom/retopo.cpp` |
| add a segmenter | implement `ISegmenter`, dispatch in `make_segmenter` (`src/segment/segmenter.cpp`) |
| change the look | `src/ui/theme.cpp` for the palette and metrics, `src/ui/widgets.cpp` for the components |
| add an LLM backend | implement `ILlmBackend`, register in `make_llm_backend` |

Do not add geometry decisions to the prompt layer, and do not let the geometry
layer learn that a language model exists. The knob panel is the only interface
the model can move. The one thing it can do besides filling knobs is object to
the brief through `profile_advice`, and that is advisory by construction: the
pipeline records proposals and a human applies them from the profile panel.
Nothing in the run ever reads them back.
