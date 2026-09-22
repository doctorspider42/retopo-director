# Retopo Director

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

High poly in, console-budget low poly out.

Every line of geometry, UV and bake work is done by deterministic code. A
language model acts as **art director**: it looks at renders, names the parts of
the model, decides how the triangle budget is split between them, decides what
gets modelled and what gets painted into the diffuse, and judges the result. It
never touches a vertex, and it cannot overrule a hard limit.

```
high poly ──► analysis ──► segmentation ──► [ director names the regions ]
                                                      │
                             [ director allocates the budget ]
                                                      │
                                     knob panel (JSON)
                                                      │
              density field ──► retopo ──► hard rules ──► bake ──► validate
                                                      │
                            pass? ──► [ director reviews renders ] ──┐
                            fail? ──► [ director gets the numbers ] ─┤
                                                      │              │
                                                      └──────────────┘
                                                      │
                                              export + report
```

## Status

First version. The whole loop works end to end: load, analyse, segment, name,
allocate, build, bake, validate, iterate, export, report.

Measured on a 20 480 triangle procedural test sculpt against `ps2_character`:

| | deterministic run | with the director |
|---|---|---|
| wall clock | 2.1 s | 3 m 46 s (3 iterations, model latency dominates) |
| result | 1 118 tri, 702 vtx | converged, verdict *accept* on iteration 3 |
| validation | 0 errors, 0 warnings | 0 errors, 0 warnings |
| silhouette error | 0.0087 mean (profile wants under 0.020) | same |
| strips | 10.2 triangles each | same |
| quad coverage | 81 % | same |

The director costs minutes and buys judgement about *where* the triangles go.
The deterministic half is what guarantees the result is legal, and it runs in
two seconds with `--no-llm` if that is all you want.

## Building

Requires CMake 3.24+, Ninja and a C++20 compiler. Everything else is fetched and
compiled by the build; there is nothing to install.

```bash
cmake --preset mingw-release
cmake --build --preset mingw-release
```

Presets for MinGW (debug and release) and MSVC are in `CMakePresets.json`. The
MinGW build links the runtime statically, so the result is a single executable
you can hand to an artist.

On Windows, `build.cmd` does the same thing in one step, and `-run` starts the
result when the build succeeds. `-run` and `-help` are the script's own; every
other argument is passed to the application, whose exit code comes back out:

```bat
build.cmd
build.cmd -run
build.cmd -run --mesh sculpt.glb --run
```

## Running

```bash
# windowed
./build/mingw-release/bin/retopo-director.exe

# windowed, load a mesh and start immediately
./build/mingw-release/bin/retopo-director.exe --mesh sculpt.glb --run

# batch, no window, no director: just the deterministic half
./build/mingw-release/bin/retopo-director.exe --headless --no-llm \
    --mesh sculpt.glb --project ./out --verbose
```

| flag | meaning |
|---|---|
| `--mesh <file>` | high poly to load (`.obj`, `.gltf`, `.glb`) |
| `--profile <file>` | target profile to load |
| `--project <dir>` | where renders, bakes, reports and exports are written |
| `--headless` | run the pipeline without a window, then exit |
| `--no-llm` | skip the director entirely |
| `--run` | start the pipeline as soon as the window opens |
| `--verbose` | mirror the log to stderr |
| `--backend <name>` | force `auto`, `quad_field` or `quadric` |
| `--no-gpu` | do not create a GL context at all |
| `--segmenter <name>` | region split: `geometric`, `sam` or `auto` |
| `--sam-checkpoint <file>` | SAM weights the sidecar should load |
| `--sam-device <name>` | `auto`, `cuda`, `cpu` or `mps` |
| `--screenshot <file>` | write a png of the window; no focus needed |
| `--screenshot-delay <sec>` | when to take it; 0 waits for the run to finish |
| `--exit-after <sec>` | close the window automatically |

The exit code is 0 when validation passes, 3 when it fails, 1 or 2 on an error.

**Headless is a complete run, not a degraded one.** It creates a hidden OpenGL
context, so the director still gets its renders, the region overlay and the
silhouette metric — a batch run and a windowed run produce identical results.
`--no-gpu` forces the old render-free behaviour for a machine with no usable
driver; the geometry half does not need one either way.

Screenshots are read out of the back buffer before the swap, so they work with
the window unfocused, partly covered, or on another desktop, and they capture
only the application. **F12** takes one at any time into
`<project>/screenshots/`.

## Choosing the director backend

Three interchangeable backends, picked in the **Director** panel or in
`settings.json`:

- **Claude CLI** — runs `claude -p --output-format json`. Images are passed as
  paths on disk; the CLI opens them itself.
- **Codex CLI** — runs `codex exec -`. The argument list is configurable,
  because that tool moves faster than this one.
- **OpenAI API** — any OpenAI-compatible endpoint. Images are inlined as base64.
  The key comes from the settings or from `OPENAI_API_KEY`, and is never written
  to disk.

If the chosen backend is unavailable the run continues without it: the knob
panel keeps its area-proportional defaults and the geometry is built anyway.

## Choosing how the regions are drawn

The split into regions decides what the director is asked to name, and there are
two ways to get one. Pick it in the **Director → Region split** card, in
`settings.json`, or with `--segmenter`.

**Geometric** is the default and the floor. A multi source Dijkstra over the
dual graph, seeded at joints and by farthest point sampling, with edge weights
that punish crossing creases and bone boundaries. No model, no download, no GPU,
single digit milliseconds, and every region maps exactly onto a set of source
triangles.

**SAM** segments what a person would call a part rather than what the curvature
says: it can see that a painted belt and the torso under it are different
things. It runs out of process, in a Python sidecar that holds the model:

```bash
pip install torch torchvision segment-anything pillow numpy
# then fetch a checkpoint; vit_b is 375 MB and enough for this
retopo-director --mesh high.obj --segmenter sam --sam-checkpoint sam_vit_b_01ec64.pth
```

The reference renders the director is already looking at go to the sidecar, the
masks come back, and one ray per pixel through the same camera puts them onto
the mesh: each mask votes for the triangles its pixels hit, the mask with the
most votes takes the triangle, and triangles no camera ever saw are filled in by
the same Dijkstra the geometric split uses. The two approaches compose rather
than compete, and the sliver absorption and statistics at the end are shared.

Nothing about this is required. A missing checkpoint, a machine with no CUDA
device, a sidecar that falls over or a `--no-gpu` run with no renders to feed it
all cost a warning in the log, and the run continues on the geometric split.
`auto` says so quietly, `sam` says so loudly. ViT-H wants roughly 8 GB of VRAM
and ViT-B fits in 4 GB; MobileSAM and FastSAM are the sensible middle ground at
a few hundred milliseconds a view.

## What the model is and is not allowed to do

| the director decides | the code decides |
|---|---|
| what each region is called and what it is for | where region boundaries fall |
| how the triangle budget is split | which edges collapse, in what order |
| geometry or texture, per region | UV layout, atlas packing, palette |
| which parts must hold their silhouette | symmetry, manifold, joint loops |
| whether the result is good enough | whether the result is legal |

The knob panel is the only interface between the two. It is plain JSON, it is
written to every iteration directory, and it can be edited by hand in the
**Knob panel → JSON** tab and re-applied.

## Target profiles

A profile is the hard contract: triangle and vertex limits, shell count, bone
influences, texture size and palette depth, whether strips are required and how
long they must average, plus the cameras the asset will actually be seen
through and a paragraph of art direction in plain language.

Two ship in `profiles/`: `ps2_character.json` and `ps2_prop.json`. Edit them in
the **Target profile** panel and save your own.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the modules fit together
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — what is deliberately simple, and how to
  deepen it (including the GPU segmentation path)
- [`docs/FORMATS.md`](docs/FORMATS.md) — the `.rdmesh` container and the CLUT sidecar
- [`docs/LICENSES.md`](docs/LICENSES.md) — third party licences, and what was
  deliberately avoided

## Third party

All permissive, all compiled from source by the build:
meshoptimizer (MIT), xatlas (MIT), Dear ImGui (MIT), GLFW (zlib), cgltf (MIT),
stb (MIT / public domain), nlohmann/json (MIT).

Nothing copyleft is linked. See [`docs/LICENSES.md`](docs/LICENSES.md).

## Licence

Apache License 2.0. See [`LICENSE`](LICENSE).

Apache-2.0 rather than MIT because it comes with an explicit patent grant, which
matters for a tool built out of mesh processing algorithms, and because it is
compatible with everything this project links against.
