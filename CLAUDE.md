# Working on Retopo Director

## Build and test

```bash
cmake --preset mingw-release
cmake --build --preset mingw-release

# fast regression: the whole deterministic pipeline, no model, a couple of seconds
./build/mingw-release/bin/retopo-director.exe --headless --no-llm --verbose \
    --mesh <high.obj> --project /tmp/run
```

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

There is no test mesh in the repository. Generate one with any subdivided,
displaced sphere; the pipeline expects a single closed shell and detects
symmetry automatically.

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
