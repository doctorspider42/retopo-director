#!/usr/bin/env python3
"""Segment Anything sidecar for PS2 Retopo Director.

Reads one JSON request from stdin, writes one JSON reply to stdout, exits.
Nothing else is ever written to stdout: Torch, timm and friends are chatty, and
the C++ side parses this stream.

    request  {"protocol": 1, "checkpoint": "sam_vit_b.pth", "model_type": "vit_b",
              "device": "auto", "points_per_side": 16,
              "min_mask_area": 0.0015, "max_mask_area": 0.5,
              "views": [{"name": "front", "index": 0, "file": "/abs/front.png"}]}

    reply    {"ok": true, "device": "cuda", "seconds": 12.3,
              "views": [{"name": "front", "index": 0, "width": 640, "height": 640,
                         "masks": [{"score": 0.93, "area_share": 0.11,
                                    "rle": [120, 40, 600, 40, ...]}]}]}

The mask bitmap is run length encoded over the row major pixels, starting with
a run of background. That is a few kilobytes per mask instead of a few hundred,
and it decodes in a dozen lines on the other side.

A failure that we can explain - no Torch, no checkpoint, no CUDA - comes back as
{"ok": false, "error": "..."} with exit code 0, so the director can print it and
fall back to the geometric split. A non-zero exit means something we did not
anticipate, and the C++ side reports stderr instead.

Install what it needs with:

    pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
    pip install segment-anything opencv-python-headless pillow numpy

and fetch a checkpoint (vit_b is 375 MB and enough for this) from
https://github.com/facebookresearch/segment-anything#model-checkpoints
"""

import json
import hashlib
import os
import sys
import time

PROTOCOL = 1
# A view with more masks than this is speckle, and the payload stops being
# worth sending: the director merges them down to a few dozen regions anyway.
MAX_MASKS_PER_VIEW = 64

# Generator settings. The director merges patches later, so a mask that is
# only probably right is still useful; upstream defaults are tuned for
# standalone use. 64 points a batch is upstream's: 256 ran ten times slower on
# an 8 GB card.
GENERATOR = {
    "pred_iou_thresh": 0.86,
    "stability_score_thresh": 0.90,
    "min_mask_region_area": 64,
    "points_per_batch": 64,
}

# Masks for one view are kept by what produced them: the image, the weights
# and every setting above. Generating them is the whole cost - about 1.7 s a
# view on an RTX 2070, twenty seconds a run, against eight for importing Torch
# and loading vit_b - and a rebuild, a re-run or a benchmark renders the same
# views of the same mesh every time. A run whose views all hit never imports
# Torch at all.
CACHE_LIMIT = 4000   # files; the oldest go first


def cache_key(image_path, req, checkpoint):
    h = hashlib.sha1()
    with open(image_path, "rb") as f:
        h.update(f.read())
    settings = {
        "model_type": req.get("model_type", "vit_b"),
        "checkpoint": os.path.basename(checkpoint),
        "checkpoint_bytes": os.path.getsize(checkpoint),
        "points_per_side": int(req.get("points_per_side", 16)),
        "min_mask_area": float(req.get("min_mask_area", 0.0015)),
        "max_mask_area": float(req.get("max_mask_area", 0.5)),
        "max_masks": MAX_MASKS_PER_VIEW,
        "generator": GENERATOR,
    }
    h.update(json.dumps(settings, sort_keys=True).encode("utf-8"))
    return h.hexdigest()


def cache_load(cache_dir, key):
    if not cache_dir:
        return None
    try:
        with open(os.path.join(cache_dir, key + ".json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def cache_store(cache_dir, key, view):
    if not cache_dir:
        return
    try:
        os.makedirs(cache_dir, exist_ok=True)
        path = os.path.join(cache_dir, key + ".json")
        # Written aside and renamed, so a run that dies halfway never leaves a
        # truncated entry for the next one to trust.
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(view, f)
        os.replace(path + ".tmp", path)
    except OSError as exc:
        print("mask cache: cannot write %s: %s" % (key, exc), file=sys.stderr)


def cache_prune(cache_dir):
    try:
        entries = [os.path.join(cache_dir, n) for n in os.listdir(cache_dir) if n.endswith(".json")]
    except OSError:
        return
    if len(entries) <= CACHE_LIMIT:
        return
    entries.sort(key=lambda p: os.path.getmtime(p))
    for path in entries[:len(entries) - CACHE_LIMIT]:
        try:
            os.remove(path)
        except OSError:
            pass


def fail(message):
    json.dump({"ok": False, "error": str(message)}, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()
    sys.exit(0)


def pick_device(requested, torch):
    requested = (requested or "auto").lower()
    if requested == "auto":
        if torch.cuda.is_available():
            return "cuda"
        if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
            return "mps"
        return "cpu"
    if requested == "cuda" and not torch.cuda.is_available():
        # Explicitly asked for the GPU and there is not one. Say so rather than
        # silently spending fifteen seconds a view on the CPU.
        fail("cuda was requested but torch reports no CUDA device")
    return requested


def encode_rle(mask):
    """Row major run length encoding, alternating background / foreground."""
    import numpy as np

    flat = np.asarray(mask, dtype=bool).reshape(-1)
    if flat.size == 0:
        return []

    # Boundaries where the value changes, plus the two ends.
    change = np.flatnonzero(flat[1:] != flat[:-1]) + 1
    bounds = np.concatenate(([0], change, [flat.size]))
    runs = np.diff(bounds).tolist()

    # The encoding always starts on background; if the image does not, lead
    # with an empty run.
    if flat[0]:
        runs.insert(0, 0)
    return [int(r) for r in runs]


def main():
    raw = sys.stdin.read()
    if not raw.strip():
        fail("empty request")
    try:
        req = json.loads(raw)
    except ValueError as exc:
        fail("cannot parse the request: %s" % exc)

    if int(req.get("protocol", 0)) != PROTOCOL:
        fail("protocol %s is not supported, this sidecar speaks %d"
             % (req.get("protocol"), PROTOCOL))

    checkpoint = req.get("checkpoint", "")
    if not checkpoint or not os.path.isfile(checkpoint):
        fail("checkpoint '%s' is not a file" % checkpoint)

    # Beside the weights unless the request says otherwise: the masks belong
    # to the machine, like the checkpoint, not to one project.
    cache_dir = req.get("cache_dir")
    if cache_dir is None:
        cache_dir = os.path.join(os.path.dirname(os.path.abspath(checkpoint)), "mask_cache")

    started = time.time()
    model_type = req.get("model_type", "vit_b")

    views = []
    for view in req.get("views", []):
        path = view.get("file", "")
        if not os.path.isfile(path):
            print("skipping missing view %s" % path, file=sys.stderr)
            continue
        key = cache_key(path, req, checkpoint)
        views.append((view, key, cache_load(cache_dir, key)))

    misses = [v for v in views if v[2] is None]
    device = "cache"
    generator = None
    if misses:
        try:
            import numpy as np
            import torch
            from PIL import Image
            from segment_anything import SamAutomaticMaskGenerator, sam_model_registry
        except ImportError as exc:
            fail("%s; pip install torch torchvision segment-anything opencv-python-headless "
                 "pillow numpy" % exc)

        if model_type not in sam_model_registry:
            fail("unknown model type '%s'" % model_type)

        device = pick_device(req.get("device", "auto"), torch)
        try:
            sam = sam_model_registry[model_type](checkpoint=checkpoint)
            sam.to(device=device)
        except Exception as exc:                                  # noqa: BLE001
            fail("cannot load %s from %s: %s" % (model_type, checkpoint, exc))

        generator = SamAutomaticMaskGenerator(
            sam, points_per_side=int(req.get("points_per_side", 16)), **GENERATOR)

    min_share = float(req.get("min_mask_area", 0.0015))
    max_share = float(req.get("max_mask_area", 0.5))

    out_views = []
    for view, key, cached in views:
        if cached is not None:
            # The name and index are this request's; the masks are the image's.
            cached["name"]  = view.get("name", "")
            cached["index"] = int(view.get("index", -1))
            out_views.append(cached)
            print("%s: %d masks (cached)" % (view.get("name", "?"), len(cached["masks"])),
                  file=sys.stderr)
            continue

        image = np.array(Image.open(view["file"]).convert("RGB"))
        height, width = image.shape[:2]
        frame = float(width * height)

        masks = []
        for record in generator.generate(image):
            share = float(record.get("area", 0)) / frame if frame else 0.0
            if share < min_share or share > max_share:
                continue
            masks.append({
                "score": float(record.get("predicted_iou", 0.0)),
                "area_share": share,
                "rle": encode_rle(record["segmentation"]),
            })

        masks.sort(key=lambda m: m["area_share"], reverse=True)
        del masks[MAX_MASKS_PER_VIEW:]

        entry = {
            "name": view.get("name", ""),
            "index": int(view.get("index", -1)),
            "width": int(width),
            "height": int(height),
            "masks": masks,
        }
        cache_store(cache_dir, key, entry)
        out_views.append(entry)
        print("%s: %d masks" % (view.get("name", "?"), len(masks)), file=sys.stderr)

    if misses and cache_dir:
        cache_prune(cache_dir)

    json.dump({
        "ok": True,
        "device": device,
        "model_type": model_type,
        "cached_views": len(views) - len(misses),
        "seconds": time.time() - started,
        "views": out_views,
    }, sys.stdout)
    sys.stdout.write("\n")
    sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:                                  # noqa: BLE001
        # Unexpected: let stderr and a non-zero exit carry it.
        print("sam_server: %s" % exc, file=sys.stderr)
        sys.exit(1)
