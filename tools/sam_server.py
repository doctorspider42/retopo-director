#!/usr/bin/env python3
"""Segment Anything sidecar for Retopo Director.

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

    pip install torch torchvision segment-anything pillow numpy

and fetch a checkpoint (vit_b is 375 MB and enough for this) from
https://github.com/facebookresearch/segment-anything#model-checkpoints
"""

import json
import os
import sys
import time

PROTOCOL = 1
# A view with more masks than this is speckle, and the payload stops being
# worth sending: the director merges them down to a few dozen regions anyway.
MAX_MASKS_PER_VIEW = 64


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

    try:
        import numpy as np
        import torch
        from PIL import Image
        from segment_anything import SamAutomaticMaskGenerator, sam_model_registry
    except ImportError as exc:
        fail("%s; pip install torch torchvision segment-anything pillow numpy" % exc)

    model_type = req.get("model_type", "vit_b")
    if model_type not in sam_model_registry:
        fail("unknown model type '%s'" % model_type)

    device = pick_device(req.get("device", "auto"), torch)
    started = time.time()

    try:
        sam = sam_model_registry[model_type](checkpoint=checkpoint)
        sam.to(device=device)
    except Exception as exc:                                  # noqa: BLE001
        fail("cannot load %s from %s: %s" % (model_type, checkpoint, exc))

    generator = SamAutomaticMaskGenerator(
        sam,
        points_per_side=int(req.get("points_per_side", 16)),
        # The director merges patches later, so a mask that is only probably
        # right is still useful; upstream defaults are tuned for standalone use.
        pred_iou_thresh=0.86,
        stability_score_thresh=0.90,
        min_mask_region_area=64,
    )

    min_share = float(req.get("min_mask_area", 0.0015))
    max_share = float(req.get("max_mask_area", 0.5))

    out_views = []
    for view in req.get("views", []):
        path = view.get("file", "")
        if not os.path.isfile(path):
            print("skipping missing view %s" % path, file=sys.stderr)
            continue

        image = np.array(Image.open(path).convert("RGB"))
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

        out_views.append({
            "name": view.get("name", ""),
            "index": int(view.get("index", -1)),
            "width": int(width),
            "height": int(height),
            "masks": masks,
        })
        print("%s: %d masks" % (view.get("name", "?"), len(masks)), file=sys.stderr)

    json.dump({
        "ok": True,
        "device": device,
        "model_type": model_type,
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
