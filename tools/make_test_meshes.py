#!/usr/bin/env python3
"""Synthetic high poly meshes for the tests and the benchmark.

    python tools/make_test_meshes.py <out-dir>

Four shapes, each chosen to break a different assumption:

  sphere.obj   displaced icosphere, 20k triangles. The easy case, and the one
               every stage has been developed against.
  torus.obj    genus one. A shell with a hole through it: anything that assumes
               a sphere-like surface (a single boundary-free chart, a Euler
               characteristic of 2) shows up here.
  blob.obj     no symmetry at all. Symmetry detection must say so and the
               mirror enforcement must stay out of the way.
  figure.obj   a standing A-pose figure built from a smooth union of capsules,
               one closed shell, mirror symmetric in X, Y up, 1.75 m tall. Thin
               limbs, a neck, a crotch and armpits: the places a character
               retopology actually goes wrong.

Deterministic: the same script writes byte-identical files, so a baseline taken
on one machine means the same thing on another. Requires numpy.
"""

import math
import os
import sys

import numpy as np

# Bump when a shape changes, so cached copies are regenerated.
VERSION = 2


# --------------------------------------------------------------------------
def write_obj(path, verts, faces):
    with open(path, "w", newline="\n") as f:
        f.write(f"# retopo-director synthetic mesh v{VERSION}\n")
        for v in verts:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for a, b, c in faces:
            f.write(f"f {a + 1} {b + 1} {c + 1}\n")


def icosphere(levels):
    t = (1 + 5 ** 0.5) / 2
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t),
             (0, -1, -t), (0, 1, -t), (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9),
             (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8), (3, 9, 4), (3, 4, 2),
             (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
             (8, 6, 7), (9, 8, 1)]
    verts = [tuple(np.array(v) / np.linalg.norm(v)) for v in verts]
    for _ in range(levels):
        cache = {}
        out = []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                p = (np.array(verts[a]) + np.array(verts[b])) / 2
                verts.append(tuple(p / np.linalg.norm(p)))
                cache[key] = len(verts) - 1
            return cache[key]

        for a, b, c in faces:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            out += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = out
    return np.array(verts), faces


def sphere():
    v, f = icosphere(5)
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    r = 1 + 0.08 * np.sin(6 * y) * np.cos(4 * np.abs(x)) + 0.05 * np.cos(5 * z)
    return v * r[:, None], f


def blob():
    v, f = icosphere(5)
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    # Every term odd in x or offset, so no mirror plane survives.
    r = (1 + 0.22 * np.exp(-((x - 0.6) ** 2 + (y - 0.3) ** 2) * 6)
         + 0.12 * np.sin(3 * x + 1.3) * np.cos(2 * z)
         + 0.06 * np.sin(7 * y + 2 * x))
    v = v * r[:, None]
    v[:, 1] *= 0.8
    return v, f


def torus(major=1.0, minor=0.35, nu=200, nv=64):
    verts, faces = [], []
    for i in range(nu):
        u = 2 * math.pi * i / nu
        for j in range(nv):
            w = 2 * math.pi * j / nv
            r = minor * (1 + 0.1 * math.sin(5 * u) * math.cos(3 * w))
            verts.append(((major + r * math.cos(w)) * math.cos(u),
                          r * math.sin(w),
                          (major + r * math.cos(w)) * math.sin(u)))
    for i in range(nu):
        for j in range(nv):
            a = i * nv + j
            b = ((i + 1) % nu) * nv + j
            c = ((i + 1) % nu) * nv + (j + 1) % nv
            d = i * nv + (j + 1) % nv
            faces += [(a, d, c), (a, c, b)]
    return np.array(verts), faces


# --------------------------------------------------------------------------
# The figure: a signed distance field polygonised with marching tetrahedra.
# Tetrahedra rather than cubes because the six-tet split of a cube along its
# main diagonal is the same in every cell, so neighbouring cells agree on how
# their shared faces are cut and the result is watertight without the
# ambiguity tables marching cubes needs.
def _capsule(p, a, b, r):
    a, b = np.array(a), np.array(b)
    pa, ba = p - a, b - a
    h = np.clip((pa @ ba) / (ba @ ba), 0, 1)
    return np.linalg.norm(pa - h[..., None] * ba, axis=-1) - r


def _smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b + (a - b) * h - k * h * (1 - h)


def figure_sdf(p):
    parts = [
        _capsule(p, (0, 0.95, 0), (0, 1.33, 0), 0.16),            # torso
        _capsule(p, (0, 1.36, 0), (0, 1.52, 0), 0.05),            # neck
        _capsule(p, (0, 1.60, 0.01), (0, 1.66, 0.0), 0.10),       # head
        _capsule(p, (0, 1.58, 0.07), (0, 1.60, 0.11), 0.018),     # nose
    ]
    for s in (-1, 1):
        parts += [
            _capsule(p, (s * 0.19, 1.37, 0), (s * 0.40, 1.16, 0.02), 0.055),   # upper arm
            _capsule(p, (s * 0.40, 1.16, 0.02), (s * 0.58, 0.98, 0.06), 0.045),  # forearm
            _capsule(p, (s * 0.60, 0.96, 0.07), (s * 0.64, 0.90, 0.08), 0.035),  # hand
            _capsule(p, (s * 0.10, 0.92, 0), (s * 0.12, 0.50, 0.01), 0.075),   # thigh
            _capsule(p, (s * 0.12, 0.50, 0.01), (s * 0.13, 0.10, 0), 0.058),   # shin
            _capsule(p, (s * 0.13, 0.05, -0.02), (s * 0.13, 0.05, 0.13), 0.045),  # foot
        ]
    d = parts[0]
    for q in parts[1:]:
        d = _smin(d, q, 0.04)
    return d


_TETS = [(0, 1, 3, 7), (0, 3, 2, 7), (0, 2, 6, 7), (0, 6, 4, 7), (0, 4, 5, 7), (0, 5, 1, 7)]
_CORNER = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (0, 0, 1), (1, 0, 1), (0, 1, 1), (1, 1, 1)]


def marching_tets(sdf, lo, hi, cell):
    lo, hi = np.array(lo, float), np.array(hi, float)
    n = np.ceil((hi - lo) / cell).astype(int) + 1
    xs = [lo[i] + cell * np.arange(n[i]) for i in range(3)]
    grid = np.stack(np.meshgrid(*xs, indexing="ij"), axis=-1)
    f = sdf(grid.reshape(-1, 3)).reshape(n)
    # A sample exactly on the surface would put a vertex on a grid point and
    # collapse triangles; nudging it off the surface keeps every cut inside an edge.
    f[np.abs(f) < 1e-9] = 1e-9

    def gid(i, j, k):
        return (i * n[1] + j) * n[2] + k

    flat = f.ravel()
    pos = grid.reshape(-1, 3)

    # Cells whose corners disagree on sign; everything else is empty space.
    c = np.stack([f[dx:n[0] - 1 + dx, dy:n[1] - 1 + dy, dz:n[2] - 1 + dz]
                  for dx, dy, dz in _CORNER], axis=-1)
    cells = np.argwhere((c.min(axis=-1) < 0) & (c.max(axis=-1) > 0))

    verts, index, faces = [], {}, []

    def edge_vertex(a, b):
        key = (a, b) if a < b else (b, a)
        v = index.get(key)
        if v is None:
            fa, fb = flat[a], flat[b]
            t = min(max(fa / (fa - fb), 0.02), 0.98)
            verts.append(pos[a] + t * (pos[b] - pos[a]))
            v = index[key] = len(verts) - 1
        return v

    def emit(tri, inside, outside):
        a, b, c = (verts[i] for i in tri)
        normal = np.cross(b - a, c - a)
        away = pos[outside].mean(axis=0) - pos[inside].mean(axis=0)
        faces.append(tri if normal @ away > 0 else (tri[0], tri[2], tri[1]))

    for i, j, k in cells:
        ids = [gid(i + dx, j + dy, k + dz) for dx, dy, dz in _CORNER]
        for tet in _TETS:
            t = [ids[q] for q in tet]
            ins = [q for q in t if flat[q] < 0]
            outs = [q for q in t if flat[q] >= 0]
            if len(ins) == 0 or len(ins) == 4:
                continue
            if len(ins) == 1:
                a = ins[0]
                emit(tuple(edge_vertex(a, o) for o in outs), ins, outs)
            elif len(ins) == 3:
                d = outs[0]
                emit(tuple(edge_vertex(d, q) for q in ins), ins, outs)
            else:
                a, b = ins
                c_, d = outs
                q0, q1 = edge_vertex(a, c_), edge_vertex(a, d)
                q2, q3 = edge_vertex(b, d), edge_vertex(b, c_)
                emit((q0, q1, q2), ins, outs)
                emit((q0, q2, q3), ins, outs)
    return np.array(verts), faces


def figure():
    return marching_tets(figure_sdf, (-0.72, -0.02, -0.2), (0.72, 1.8, 0.26), 0.014)


SHAPES = {"sphere": sphere, "torus": torus, "blob": blob, "figure": figure}


def write_all(out_dir, names=None):
    os.makedirs(out_dir, exist_ok=True)
    written = {}
    for name, make in SHAPES.items():
        if names and name not in names:
            continue
        path = os.path.join(out_dir, name + ".obj")
        stamp = f"# retopo-director synthetic mesh v{VERSION}"
        if os.path.exists(path):
            with open(path) as f:
                if f.readline().strip() == stamp:
                    written[name] = path
                    continue
        v, faces = make()
        write_obj(path, v, faces)
        written[name] = path
    return written


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    for name, path in write_all(sys.argv[1]).items():
        print(path)
