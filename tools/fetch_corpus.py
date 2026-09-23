#!/usr/bin/env python3
"""Fetch the downloadable half of the benchmark corpus.

Everything here is CC0 from Poly Haven (https://polyhaven.com/license), so it
can be kept, shared and checked into a private corpus without a second thought.
The meshes are scans with real sculpted detail: a bust, a standing figure, an
animal, a creature. That is the shape of input the pipeline exists for, and
the kind a subdivided sphere does not exercise.

    python tools/fetch_corpus.py <corpus-root>

Files land in <corpus-root>/PolyHaven/<asset>/. Already present files are
skipped, so running it twice costs two HTTP requests per asset.
"""

import json
import os
import sys
import urllib.request

# Chosen for shape, not polish: a face, a whole standing figure with thin limbs
# and cloth, a quadruped, a head with deep carved hair, a small creature with a
# tail. 1k textures, because the bake downsamples to 256 anyway.
ASSETS = ["marble_bust_01", "gothic_statue", "horse_statue_01", "lion_head", "street_rat"]
RESOLUTION = "1k"
API = "https://api.polyhaven.com/files/"
USER_AGENT = "retopo-director-corpus/1.0"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def fetch(asset, root):
    files = json.loads(get(API + asset))
    entry = files["gltf"][RESOLUTION]["gltf"]
    out_dir = os.path.join(root, "PolyHaven", asset)
    os.makedirs(out_dir, exist_ok=True)

    todo = [(os.path.basename(entry["url"]), entry["url"])]
    # The include map is keyed by the path the .gltf refers to, which is where
    # the file has to go for the relative references inside it to resolve.
    for rel, inc in entry.get("include", {}).items():
        todo.append((rel, inc["url"]))

    for rel, url in todo:
        dest = os.path.join(out_dir, rel)
        if os.path.exists(dest):
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        data = get(url)
        with open(dest, "wb") as f:
            f.write(data)
        print(f"  {asset}/{rel}  {len(data) / 1e6:.1f} MB")

    with open(os.path.join(out_dir, "LICENSE.txt"), "w") as f:
        f.write(f"{asset} from https://polyhaven.com/a/{asset}\n"
                "License: CC0 1.0 Universal (https://polyhaven.com/license)\n")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    for asset in ASSETS:
        print(asset)
        fetch(asset, root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
