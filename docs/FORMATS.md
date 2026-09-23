# Output formats

Everything a run produces lands under the project directory:

```
<project>/
  renders/          reference shots of the high poly, plus the region overlay
  iterations/
    iter_01/        the low poly rendered from the same cameras, its knob
                    panel, its validation report and its baked texture
  bake/
  export/           the shipped asset
  reports/
    report.json     machine readable
    report.md       for a human
    prompts/        every prompt sent and every reply received, verbatim
```

## `.rdmesh`

A flat little container for engines that want strips and a CLUT rather than
glTF. Everything is little endian.

### Header

| offset | type | field |
|---|---|---|
| 0 | `char[8]` | magic, `RDMESH01` |
| 8 | `u32` | flags |
| 12 | `u32` | vertex count |
| 16 | `u32` | triangle count |
| 20 | `u32` | strip index count |
| 24 | `u32` | restart index (`0xFFFFFFFF`) |
| 28 | `u32` | max bone influences per vertex |
| 32 | `f32` | import scale (model units per source unit) |
| 36 | `f32[3]` | bounds min |
| 48 | `f32[3]` | bounds max |

Flags:

| bit | meaning |
|---|---|
| 0 | normals present |
| 1 | uvs present |
| 2 | vertex colours present |
| 3 | skin present |
| 4 | strips present |
| 5 | texture page table present |

### Streams, in order, each present only if its flag is set

| stream | layout |
|---|---|
| positions | `f32[3]` per vertex, always present |
| normals | `f32[3]` per vertex |
| uvs | `f32[2]` per vertex |
| colours | `u8[4]` RGBA per vertex |
| skin | `u16[4]` joint indices then `u8[4]` weights, per vertex |

### Indices

`u32` triangle list, `3 × triangle count` entries, in vertex-cache order.
Then, when the strips flag is set, `u32` strip indices, `strip index count`
entries, runs separated by the restart index.

With more than one texture page the triangles are grouped by page, in page
order, and no strip crosses from one page to the next.

### Texture pages

When bit 5 is set: `u32` page count, then per page `u32` first triangle,
`u32` triangle count, `u32` first strip index, `u32` strip index count. Page 0
is `<name>_diffuse.png`; page `p` is the profile's `texture.pages[p - 1]`,
written as `<name>_<page name>_diffuse.png` with its own index image and CLUT.
Uvs of a page's triangles point into that page's image.

### Skeleton

`u32` joint count, then per joint: `u32` name length, that many bytes of UTF-8,
`u32` parent index (`0xFFFFFFFF` for a root), `f32[3]` bind position in model
space.

## CLUT sidecar

When the profile asks for a palette, the exporter writes three files next to the
mesh:

- `<name>_diffuse.png` — the quantised RGBA image, for anything that just wants
  a texture
- `<name>_diffuse_index.png` — the same image as 8-bit palette indices
- `<name>_diffuse_clut.json` — `{ "width", "height", "bits", "palette": [[r,g,b], ...] }`

`bits` is 4 when the palette has sixteen entries or fewer, otherwise 8. The
palette is sorted by luminance, so the same input always produces the same table
and a diff between two runs is readable.

## glTF

A single buffer with one view per stream and a `mode: 4` primitive per texture
page, each with a material referencing that page's image. `--glb` packs it into one file.
Positions are written back through the import transform, so the asset comes out
in the space the source file used, not in the normalised space the pipeline
works in.
