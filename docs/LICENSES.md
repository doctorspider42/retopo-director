# Licences

## This project

Apache License 2.0, in [`LICENSE`](../LICENSE) at the root. It grants patent
rights explicitly, which a pile of mesh processing algorithms ought to do, and
it is compatible with every dependency below — Apache-2.0 can consume MIT, BSD
and zlib code without friction, and downstream can consume this.

## Third party

Nothing copyleft is linked into the executable. Everything below is permissive
and is fetched and compiled from source by the build, pinned to an exact tag or
commit in `CMakeLists.txt`.

| library | licence | version | what it does here |
|---|---|---|---|
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | MIT | v0.25 | vertex cache order, overdraw, fetch order, triangle strips |
| [xatlas](https://github.com/jpcy/xatlas) | MIT | `f700c77` | UV unwrap and atlas packing |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | v1.92.9b-docking | the interface |
| [GLFW](https://github.com/glfw/glfw) | zlib | 3.4 | window, context and input |
| [cgltf](https://github.com/jkuhlmann/cgltf) | MIT | v1.15 | glTF and GLB loading |
| [ufbx](https://github.com/ufbx/ufbx) | MIT | v0.9.0 | FBX loading, binary and ASCII |
| [stb_image, stb_image_write, stb_image_resize](https://github.com/nothings/stb) | MIT / public domain | `2c980bb` | PNG in and out |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | v3.12.0 | every JSON document in the project |

## Deliberately avoided

These would have saved work. They were not used, and the reason is recorded here
so nobody re-litigates it by accident.

- **Autodesk FBX SDK** — proprietary, and its licence governs how the binaries
  may be redistributed, which rules it out of anything shipped. FBX comes in
  through ufbx instead: one C file, nothing to install, nothing to ship
  alongside.

- **MeshLab / PyMeshLab** — GPL. Would have covered most of the analysis and
  simplification.
- **CGAL** — mixed GPL and LGPL depending on the package. Its remeshing and
  shape-approximation packages are excellent and entirely off limits for a
  closed product.
- **libigl** — MPL2. Weak copyleft at file granularity, which in practice is
  fine for most commercial work, but it is a decision rather than a default.
- **Eigen** — MPL2, and the reason Instant Meshes and QuadriFlow are not
  vendored. See `ROADMAP.md`.
- **libimagequant / pngquant** — GPL. The palette quantiser here is a median cut
  written from scratch instead.
- **nvdiffrast, nvdiffmodeling** — NVIDIA source licence, **non-commercial use
  only**. Not copyleft, but unusable in anything shipped.
- **Blender** — GPL. It can be driven as a separate process and its output is
  not GPL, so it is a perfectly good preview or conversion tool. It is not part
  of this pipeline.

## Optional, at runtime, out of process

- **Segment Anything** (Apache-2.0, checkpoints Apache-2.0) and **PyTorch**
  (BSD-3) are used by `tools/sam_server.py`, which is a separate process the
  user installs and points at a checkpoint themselves. Nothing is vendored,
  nothing is downloaded, nothing links against them, and the product works
  without them. Keeping the model out of process is what keeps this paragraph
  short.

## Fonts

No fonts are bundled. The interface loads whatever the system provides (Segoe UI
on Windows, the platform equivalents elsewhere) and falls back to Dear ImGui's
built-in bitmap font if none is found.

## Written here, not borrowed

For the record, because these are the pieces someone might assume came from a
library: the BVH, the quadric simplifier, the isotropic remesher and quad
pairing, the segmentation, the symmetry cut and mirror, the AO and lighting
bake, the median-cut quantiser, the OpenGL loader, the HTTP client and the
process spawner are all in `src/` and owe nothing to anything outside the table
above.
