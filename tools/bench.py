#!/usr/bin/env python3
"""Run the deterministic pipeline over a corpus and compare it with a baseline.

    python tools/bench.py --root <corpus-root> [--baseline bench/baseline.json]
                          [--save-baseline] [--only name,name] [--out dir]

Every entry in bench/corpus.json is run headless with --no-llm, one after the
other (each run already uses every core, and running two at once would make
the timings meaningless). The numbers a run is judged by are pulled out of its
report.json and written to <out>/metrics.json, and a table goes to stdout.

With a baseline the exit code means something: 0 nothing got worse, 1 at least
one mesh regressed, 2 the tool itself could not run. What counts as worse is
in REGRESSION below and is deliberately loose - a retopology changes a little
with every density tweak, and a gate that fires on noise gets switched off.

Synthetic meshes (tools/make_test_meshes.py) are regenerated into <out>/synthetic
on every run, so the corpus has something in it even on a machine with no
asset folder.
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import make_test_meshes  # noqa: E402

DEFAULT_EXE = os.path.join(REPO, "build", "mingw-release", "bin", "retopo-director.exe")
DEFAULT_CORPUS = os.path.join(REPO, "bench", "corpus.json")
DEFAULT_PROFILE = os.path.join("profiles", "ps2_character.json")

# metric: (direction, relative slack, absolute slack). "lower" means a bigger
# number is a regression. A change has to clear both slacks to count, so a
# silhouette going from 0.004 to 0.005 (25%, but a fifth of a pixel) is noise.
REGRESSION = {
    "silhouette_mean":  ("lower", 0.15, 0.003),
    "silhouette_worst": ("lower", 0.20, 0.005),
    "outline_px":       ("lower", 0.15, 0.1),
    "budget_use":       ("higher", 0.0, 0.05),
    "uv_stretch":       ("lower", 0.25, 0.25),
    "strip_length":     ("higher", 0.15, 0.3),
    "seconds":          ("lower", 0.60, 2.0),
}


def load_metrics(project):
    path = os.path.join(project, "reports", "report.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        rep = json.load(f)
    s = rep.get("summary", {})
    checks = {}
    for c in rep.get("validation", {}).get("checks", []):
        checks.setdefault(c["id"], c)

    def check(cid, key="value"):
        c = checks.get(cid)
        return None if c is None else c.get(key)

    m = {
        "highpoly_triangles": s.get("highpoly_triangles"),
        "triangles": s.get("lowpoly_triangles"),
        "vertices": s.get("lowpoly_vertices"),
        "budget_use": (s["lowpoly_triangles"] / s["max_triangles"])
        if s.get("max_triangles") else None,
        "regions": s.get("regions"),
        "silhouette_mean": s.get("silhouette_mean"),
        "silhouette_worst": s.get("silhouette_worst"),
        "outline_px": s.get("outline_px"),
        "validation_passed": s.get("validation_passed"),
        "errors": s.get("errors"),
        "warnings": s.get("warnings"),
        "shells": check("geometry.shells"),
        "uv_stretch": check("texture.stretch"),
        "strip_length": check("export.strips"),
        "atlas_use": check("texture.utilisation"),
        "worst_triangle": check("geometry.slivers"),
        "failed_checks": sorted({c["id"] for c in rep.get("validation", {}).get("checks", [])
                                 if not c["passed"] and c["severity"] == "error"}),
    }
    return m


def run_one(exe, entry, path, out_dir, extra):
    project = os.path.join(out_dir, "runs", entry["name"])
    os.makedirs(project, exist_ok=True)
    report = os.path.join(project, "reports", "report.json")
    if os.path.exists(report):
        os.remove(report)
    # Hermetic: no saved settings, an explicit profile and the geometric split,
    # so a run means the same thing on any machine whatever the window last did.
    cmd = [exe, "--headless", "--no-llm", "--no-settings", "--verbose",
           "--segmenter", "geometric", "--mesh", path, "--project", project,
           "--profile", os.path.join(REPO, entry.get("profile", DEFAULT_PROFILE))]
    cmd += entry.get("args", []) + extra
    t0 = time.perf_counter()
    with open(os.path.join(project, "run.log"), "w", encoding="utf-8", errors="replace") as log:
        try:
            rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=900).returncode
        except subprocess.TimeoutExpired:
            rc = -1
    seconds = time.perf_counter() - t0
    m = load_metrics(project) or {}
    m["rc"] = rc
    m["seconds"] = round(seconds, 2)
    return m


def compare(name, cur, base):
    problems = []
    if base is None:
        return problems
    if base.get("rc") == 0 and cur.get("rc") != 0:
        problems.append(f"exit code {base.get('rc')} -> {cur.get('rc')}")
    if base.get("validation_passed") and not cur.get("validation_passed"):
        problems.append("validation passed -> failed " + ",".join(cur.get("failed_checks", [])))
    for key, (direction, rel, absl) in REGRESSION.items():
        a, b = base.get(key), cur.get(key)
        if a is None or b is None:
            continue
        delta = (b - a) if direction == "lower" else (a - b)
        if delta > absl and delta > rel * abs(a):
            problems.append(f"{key} {a:.4g} -> {b:.4g}")
    return problems


def fmt(v, spec):
    return "-" if v is None else format(v, spec)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--root", default=os.environ.get("RD_CORPUS", ""),
                    help="folder the corpus paths are relative to (or $RD_CORPUS)")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "bench"))
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--save-baseline", action="store_true",
                    help="write the metrics of this run to --baseline")
    ap.add_argument("--only", default="", help="comma separated entry names")
    ap.add_argument("--synthetic-only", action="store_true")
    ap.add_argument("--require-pass", action="store_true",
                    help="exit 1 when any run fails validation (for ctest)")
    ap.add_argument("extra", nargs="*", help="passed through to the application")
    args = ap.parse_args()

    args.exe = os.path.abspath(args.exe)
    if not os.path.exists(args.exe):
        print(f"no executable at {args.exe}; build first", file=sys.stderr)
        return 2
    with open(args.corpus, encoding="utf-8") as f:
        corpus = json.load(f)

    synth_dir = os.path.join(args.out, "synthetic")
    make_test_meshes.write_all(synth_dir)

    only = {s for s in args.only.split(",") if s}
    entries = []
    for e in corpus["meshes"]:
        if only and e["name"] not in only:
            continue
        if e.get("synthetic"):
            path = os.path.join(synth_dir, e["path"])
        else:
            if args.synthetic_only:
                continue
            if not args.root:
                continue
            path = os.path.join(args.root, e["path"])
            if not os.path.exists(path):
                print(f"skip {e['name']}: {path} not found", file=sys.stderr)
                continue
        entries.append((e, path))

    base = {}
    if args.baseline and os.path.exists(args.baseline) and not args.save_baseline:
        with open(args.baseline, encoding="utf-8") as f:
            base = json.load(f).get("meshes", {})

    results = {}
    regressions = {}
    header = (f"{'mesh':<24}{'rc':>3}{'hi tris':>9}{'lo tris':>8}{'use':>6}{'regs':>5}"
              f"{'sil mean':>10}{'sil worst':>10}{'px':>6}{'stretch':>8}{'strip':>7}{'secs':>7}  notes")
    print(header)
    print("-" * len(header))
    for e, path in entries:
        m = run_one(args.exe, e, path, args.out, args.extra)
        results[e["name"]] = m
        problems = compare(e["name"], m, base.get(e["name"]))
        if problems:
            regressions[e["name"]] = problems
        notes = []
        if not m.get("validation_passed"):
            notes.append("FAILED " + ",".join(m.get("failed_checks", [])))
        notes += ["REGRESSION " + p for p in problems]
        print(f"{e['name']:<24}{m['rc']:>3}{fmt(m.get('highpoly_triangles'), 'd'):>9}"
              f"{fmt(m.get('triangles'), 'd'):>8}{fmt(m.get('budget_use'), '.0%'):>6}"
              f"{fmt(m.get('regions'), 'd'):>5}"
              f"{fmt(m.get('silhouette_mean'), '.4f'):>10}{fmt(m.get('silhouette_worst'), '.4f'):>10}"
              f"{fmt(m.get('outline_px'), '.2f'):>6}"
              f"{fmt(m.get('uv_stretch'), '.2f'):>8}{fmt(m.get('strip_length'), '.2f'):>7}"
              f"{m['seconds']:>7.1f}  {'; '.join(notes)}", flush=True)
        # A run that did not even produce a report died of something - a
        # sanitizer, an assert, a crash - and on CI the log is the only witness.
        if m["rc"] not in (0, 3):
            log_path = os.path.join(args.out, "runs", e["name"], "run.log")
            try:
                with open(log_path, encoding="utf-8", errors="replace") as f:
                    tail = f.readlines()[-60:]
                print("    | " + "    | ".join(tail), flush=True)
            except OSError:
                pass

    os.makedirs(args.out, exist_ok=True)
    doc = {"meshes": results}
    with open(os.path.join(args.out, "metrics.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
    if args.save_baseline and args.baseline:
        with open(args.baseline, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2, sort_keys=True)
        print(f"\nbaseline written to {args.baseline}")

    if regressions:
        print(f"\n{len(regressions)} mesh(es) regressed against the baseline")
        return 1
    if args.require_pass:
        failed = [n for n, m in results.items() if m.get("rc") != 0]
        if failed or not results:
            print(f"\nfailed: {', '.join(failed) or 'nothing ran'}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
