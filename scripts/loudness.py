#!/usr/bin/env python3
"""Factory preset loudness: measure, check and normalize (docs/TESTING.md, docs/research/LOUDNESS.md).

Every preset renders a standard phrase for its category with ks-render (KEYS: bass note + two chords + melody
with the sustain pedal down; leads: monophonic melody; basses: monophonic bass line; drums: two bars of a GM
groove), then gets ITU-R BS.1770-4 integrated loudness (K-weighted, 400 ms blocks / 75 % overlap, -70 LUFS
absolute and -10 LU relative gates) and true peak (4x polyphase oversampling, dBTP).

    python scripts/loudness.py                    # measure all factory presets, print a table
    python scripts/loudness.py --check            # exit 1 if any preset misses the target (CI / before merge)
    python scripts/loudness.py --apply --report docs/research/LOUDNESS.md   # normalize master volume_db
    python scripts/loudness.py --filter organ,doors --jobs 4

Normalization only changes the patch's master `volume_db` (applied before a trailing master limiter, ARCHITECTURE
§5.3); sound design is untouched. Sampler presets need their libraries (python scripts/fetch_samples.py or
$KS_ASSETS_DIR); missing ones are skipped. Dependencies (numpy, scipy) are pip-installed if missing.
"""
import argparse
import concurrent.futures as cf
import copy
import importlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


def ensure(pkgs):
    missing = []
    for mod, pip_name in pkgs:
        try:
            importlib.import_module(mod)
        except ImportError:
            missing.append(pip_name)
    if missing:
        print(f"installing {' '.join(missing)} ...", file=sys.stderr)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", *missing])


ensure([("numpy", "numpy"), ("scipy", "scipy")])

import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402
from scipy.io import wavfile  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TARGET_LUFS = -16.0
TOLERANCE_LU = 1.0
MAX_TRUE_PEAK = -1.0
MAX_VOLUME_DB, MIN_VOLUME_DB = 12.0, -96.0  # PatchModel::kMaxVolumeDb / kMinVolumeDb

# --- phrases ------------------------------------------------------------------------------------------------
# NOTE:start:dur:vel (ks-render --notes) + controller automation (--cc).
KEYS = (
    "C2:0:3.8:90,"                                        # bass
    "C4:0:1.9:80,E4:0:1.9:80,G4:0:1.9:80,"                # chord 1
    "A3:2:1.9:80,C4:2:1.9:80,F4:2:1.9:80,"                # chord 2
    "E5:0:0.45:95,D5:0.5:0.45:95,C5:1:0.45:95,G5:1.5:0.9:100,"  # melody
    "A5:2.5:0.45:95,G5:3:0.45:95,F5:3.5:0.45:95,E5:4:1.2:100",
    "sustain:127:0,sustain:0:4",
)
LEAD = (
    "C4:0:0.48:100,D4:0.5:0.48:100,E4:1:0.48:100,G4:1.5:0.48:100,A4:2:0.98:110,"
    "G4:3:0.48:100,E4:3.5:0.48:100,C5:4:1.2:110",
    "",
)
BASS = (
    "C2:0:0.45:105,C2:0.5:0.45:95,G1:1:0.45:105,Bb1:1.5:0.45:100,C2:2:0.95:110,"
    "Eb2:3:0.45:100,F2:3.5:0.45:100,C2:4:1.2:110",
    "",
)


def drums_phrase():
    notes = []
    for bar in range(2):
        t0 = bar * 2.0  # 120 BPM, 4/4
        for i in range(8):
            notes.append(f"42:{t0 + 0.25 * i:g}:0.1:{90 if i % 2 == 0 else 70}")  # closed hat 8ths
        for b in (0, 2):
            notes.append(f"36:{t0 + 0.5 * b:g}:0.1:110")  # kick 1, 3 (+ the "and" of 3)
        notes.append(f"36:{t0 + 1.25:g}:0.1:90")
        for b in (1, 3):
            notes.append(f"38:{t0 + 0.5 * b:g}:0.1:105")  # snare 2, 4
    notes.append("49:0:0.1:110")  # crash on the one
    return (",".join(notes), "")


DRUMS = drums_phrase()
PHRASES = {"Synth Lead": ("lead", LEAD), "Synth Bass": ("bass", BASS), "Drums": ("drums", DRUMS)}


def phrase_for(category):
    return PHRASES.get(category, ("keys", KEYS))


# --- BS.1770 ------------------------------------------------------------------------------------------------
def k_weighting(fs):
    """Two biquads (pre-filter high shelf + RLB high-pass) for any sample rate (BS.1770-4 Annex 1)."""
    g, f0, q = 3.999843853973347, 1681.974450955533, 0.7071752369554196
    k = math.tan(math.pi * f0 / fs)
    vh = 10.0 ** (g / 20.0)
    vb = vh ** 0.4996667741545416
    a0 = 1.0 + k / q + k * k
    b1 = [(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a1 = [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0]
    f0, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * f0 / fs)
    a0 = 1.0 + k / q + k * k
    b2 = [1.0, -2.0, 1.0]
    a2 = [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0]
    return (b1, a1), (b2, a2)


def integrated_lufs(x, fs):
    """x: (n, channels) float. Returns integrated loudness in LUFS (-inf if everything is gated)."""
    (b1, a1), (b2, a2) = k_weighting(fs)
    y = signal.lfilter(b2, a2, signal.lfilter(b1, a1, x, axis=0), axis=0)
    block, step = int(round(0.4 * fs)), int(round(0.1 * fs))
    if len(y) < block:
        return -math.inf
    sq = np.concatenate([np.zeros((1, y.shape[1])), np.cumsum(y * y, axis=0)])
    starts = np.arange(0, len(y) - block + 1, step)
    z = (sq[starts + block] - sq[starts]) / block  # mean square per block and channel
    zsum = z.sum(axis=1)  # channel weights 1 (L, R)
    with np.errstate(divide="ignore"):
        lj = -0.691 + 10.0 * np.log10(zsum)
    abs_gated = zsum[lj > -70.0]
    if abs_gated.size == 0:
        return -math.inf
    rel = -0.691 + 10.0 * math.log10(abs_gated.mean()) - 10.0
    gated = zsum[(lj > -70.0) & (lj > rel)]
    return -0.691 + 10.0 * math.log10(gated.mean()) if gated.size else -math.inf


def true_peak_db(x):
    """Approximate true peak: 4x polyphase oversampling (BS.1770-4 Annex 2), dBTP."""
    up = signal.resample_poly(x, 4, 1, axis=0)
    p = float(np.max(np.abs(up))) if up.size else 0.0
    return 20.0 * math.log10(p) if p > 0 else -math.inf


# --- presets ------------------------------------------------------------------------------------------------
def default_renderer():
    for cfg in ("Release", "RelWithDebInfo", "Debug"):
        for b in sorted(ROOT.glob("build*")):
            exe = b / "bin" / cfg / ("ks-render.exe" if os.name == "nt" else "ks-render")
            if exe.exists():
                return exe
    return None


def is_sampler(patch):
    return any(l.get("instrument", {}).get("type") == "sampler" for l in patch.get("layers", []))


def sample_paths_ok(patch):
    """True when every sampler layer's sfz is found (repo, $KS_ASSETS_DIR, main checkout; SamplePaths.h)."""
    parts = re.split(r"[\\/]\.claude[\\/]worktrees[\\/]", str(ROOT))
    main = Path(parts[0]) if len(parts) > 1 else None
    for l in patch.get("layers", []):
        ins = l.get("instrument", {})
        if ins.get("type") != "sampler":
            continue
        sfz = ins.get("state", {}).get("sfz", "")
        cands = [ROOT / sfz]
        if sfz.startswith("assets/"):
            if os.environ.get("KS_ASSETS_DIR"):
                cands.append(Path(os.environ["KS_ASSETS_DIR"]) / sfz[len("assets/"):])
            if main:
                cands.append(main / sfz)
        if not any(c.exists() for c in cands):
            return False
    return True


RENDER_LOCK = threading.Semaphore(2)  # sampler presets: at most two loading GBs at once


def measure(renderer, preset_path, patch, tmpdir, volume_override=None):
    """Renders the category phrase; returns (lufs, true_peak_db)."""
    _, (notes, ccs) = phrase_for(patch.get("meta", {}).get("category", ""))
    out = Path(tmpdir) / (re.sub(r"[^A-Za-z0-9]+", "_", str(preset_path)) + ".wav")
    cmd = [str(renderer)]
    pj = None
    if volume_override is None:
        cmd += ["--preset", str(preset_path)]
    else:
        p = copy.deepcopy(patch)
        p.setdefault("master", {})["volume_db"] = volume_override
        pj = out.with_suffix(".json")
        pj.write_text(json.dumps(p), encoding="utf-8")
        cmd += ["--patch-json", "@" + str(pj)]
    cmd += ["--notes", notes, "--tail", "2", "--sr", "48000", "--block", "64", "--out", str(out)]
    if ccs:
        cmd += ["--cc", ccs]
    sampler = is_sampler(patch)
    if sampler:
        RENDER_LOCK.acquire()
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    finally:
        if sampler:
            RENDER_LOCK.release()
    if r.returncode != 0:
        raise RuntimeError(f"ks-render failed for {preset_path}: {r.stderr.strip()[-400:]}")
    fs, data = wavfile.read(out)
    x = data.astype(np.float64)
    if x.ndim == 1:
        x = x[:, None]
    out.unlink(missing_ok=True)
    if pj:
        pj.unlink(missing_ok=True)
    return integrated_lufs(x, fs), true_peak_db(x)


def set_master_volume(path, text, value):
    """Rewrites master.volume_db in the file text, keeping its formatting (falls back to a 2-space re-dump)."""
    j = json.loads(text)
    want = copy.deepcopy(j)
    want.setdefault("master", {})["volume_db"] = value
    m = re.search(r'"master"\s*:\s*\{', text)
    if m:
        v = re.compile(r'("volume_db"\s*:\s*)(-?[0-9.eE+-]+)')
        mm = v.search(text, m.end())
        if mm:
            new = text[: mm.start(2)] + f"{value:g}" + text[mm.end(2):]
            try:
                if json.loads(new) == want:
                    return new
            except json.JSONDecodeError:
                pass
    return json.dumps(want, indent=2, ensure_ascii=False) + "\n"


def has_trailing_limiter(patch):
    # Engine (RackGraph::finalize) puts volume_db before the whole trailing run of `limiter` slots; a bypassed
    # last limiter does not hold peaks, so it does not count here.
    fx = patch.get("master", {}).get("fx", [])
    return bool(fx) and fx[-1].get("type") == "limiter" and not fx[-1].get("bypass", False)


def target_volume(vol, lufs, tp, target, max_tp, limited):
    """Next master volume_db guess for the target loudness. Without a trailing master limiter the true peak scales
    with the gain, so the gain is capped at max_tp (peak-limited preset); with one, the limiter holds the peak and
    the gain is backed off only if a measurement actually exceeded max_tp."""
    gain = target - lufs
    if tp > max_tp:
        gain = min(gain, max_tp - tp - 0.1)
    elif not limited and tp + gain > max_tp:
        gain = max_tp - tp - 0.1  # margin for the 0.1 dB rounding below
    return round(min(MAX_VOLUME_DB, max(MIN_VOLUME_DB, vol + gain)), 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ks-render", help="ks-render executable (default: first build*/bin/<cfg>/ks-render)")
    ap.add_argument("--filter", help="comma-separated substrings of preset paths")
    ap.add_argument("--target", type=float, default=TARGET_LUFS)
    ap.add_argument("--tolerance", type=float, default=TOLERANCE_LU)
    ap.add_argument("--max-true-peak", type=float, default=MAX_TRUE_PEAK)
    ap.add_argument("--check", action="store_true", help="exit 1 if a preset misses target/tolerance/true peak")
    ap.add_argument("--apply", action="store_true", help="rewrite master volume_db of each preset")
    ap.add_argument("--report", help="write a Markdown table (before/after with --apply)")
    ap.add_argument("--jobs", type=int, default=max(1, min(8, (os.cpu_count() or 4) // 2)))
    args = ap.parse_args()

    renderer = Path(args.ks_render) if args.ks_render else default_renderer()
    if not renderer or not renderer.exists():
        sys.exit("ks-render not found: build it (docs/TESTING.md) or pass --ks-render")
    filt = [f.strip().lower() for f in args.filter.split(",")] if args.filter else None
    files = sorted((ROOT / "presets" / "factory").rglob("*.json"))
    if filt:
        files = [f for f in files if any(s in f.as_posix().lower() for s in filt)]

    rows, skipped, errors = [], [], []
    tmp = tempfile.TemporaryDirectory(prefix="ks-loudness-")
    tmpdir = tmp.name

    def job(f):
        rel = f.relative_to(ROOT).as_posix()
        text = f.read_text(encoding="utf-8")
        patch = json.loads(text)
        if is_sampler(patch) and not sample_paths_ok(patch):
            return ("skip", rel, None)
        vol = float(patch.get("master", {}).get("volume_db", 0.0))
        lufs, tp = measure(renderer, rel, patch, tmpdir)
        row = {"preset": rel, "phrase": phrase_for(patch.get("meta", {}).get("category", ""))[0],
               "vol_before": vol, "lufs_before": lufs, "tp_before": tp}
        if args.apply:
            v = vol
            limited = has_trailing_limiter(patch)
            for _ in range(6):  # limiters / saturation make the gain non-linear: iterate
                if abs(lufs - args.target) <= 0.3 and tp <= args.max_true_peak:
                    break
                nv = target_volume(v, lufs, tp, args.target, args.max_true_peak, limited)
                if nv == v:
                    break
                v = nv
                lufs, tp = measure(renderer, rel, patch, tmpdir, volume_override=v)
            if v != vol:
                f.write_text(set_master_volume(f, text, v), encoding="utf-8", newline="")
            row.update({"vol_after": v, "lufs_after": lufs, "tp_after": tp})
        row["limiter"] = has_trailing_limiter(patch)
        return ("ok", rel, row)

    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(job, f): f for f in files}
        for fu in cf.as_completed(futs):
            try:
                kind, rel, row = fu.result()
            except Exception as e:  # noqa: BLE001
                errors.append(str(e))
                continue
            if kind == "skip":
                skipped.append(rel)
            else:
                rows.append(row)
                k = "after" if args.apply else "before"
                print(f"{row[f'lufs_{k}']:7.2f} LUFS {row[f'tp_{k}']:6.2f} dBTP  {rel}", flush=True)
    rows.sort(key=lambda r: r["preset"])
    tmp.cleanup()

    k = "after" if args.apply else "before"
    for r in rows:
        r["status"] = status(r[f"lufs_{k}"], r[f"tp_{k}"], r["limiter"], args)
    bad = [r for r in rows if r["status"] == "OUT"]
    peak = [r for r in rows if r["status"] == "peak-limited"]
    if args.report:
        write_report(Path(args.report), rows, args, skipped)
    for s in skipped:
        print(f"skipped (samples not installed): {s}")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{len(rows)} measured, {len(skipped)} skipped, {len(errors)} errors; "
          f"{len(bad)} outside {args.target:g} +-{args.tolerance:g} LUFS / <= {args.max_true_peak:g} dBTP, "
          f"{len(peak)} peak-limited")
    for r in peak:
        print(f"  peak-limited: {r['preset']}: {r[f'lufs_{k}']:.2f} LUFS, {r[f'tp_{k}']:.2f} dBTP")
    for r in bad:
        print(f"  OUT: {r['preset']}: {r[f'lufs_{k}']:.2f} LUFS, {r[f'tp_{k}']:.2f} dBTP")
    if errors or (args.check and bad):
        sys.exit(1)


def status(lufs, tp, limiter, args):
    """ok | peak-limited (no master limiter and the true peak already at the limit: volume_db alone cannot make
    this crest factor louder; accepted by --check) | OUT."""
    if tp <= args.max_true_peak and abs(lufs - args.target) <= args.tolerance:
        return "ok"
    if not limiter and lufs < args.target and args.max_true_peak - 0.5 <= tp <= args.max_true_peak:
        return "peak-limited"
    return "OUT"


NOTES_MARK = "<!-- notes: kept when the table is regenerated -->"


def write_report(path, rows, args, skipped):
    def f(v):
        return "-inf" if v == -math.inf else f"{v:.1f}"

    apply = "lufs_after" in (rows[0] if rows else {})
    lines = [
        "# Factory preset loudness",
        "",
        f"Generated by `python scripts/loudness.py{' --apply' if apply else ''} --report {path.as_posix()}` "
        "(docs/TESTING.md). Target "
        f"{args.target:g} LUFS ±{args.tolerance:g} LU integrated (ITU-R BS.1770-4, K-weighted, gated), true peak "
        f"≤ {args.max_true_peak:g} dBTP (4x oversampled). Only the master `volume_db` is changed. Phrases (48 kHz):",
        "",
        "- `keys`: C2 bass note, C and F/A chords, an eight-note melody (C5–A5), sustain pedal down 0–4 s.",
        "- `lead`: monophonic eight-note melody C4–C5. `bass`: monophonic bass line G1–F2. "
        "`drums`: two bars of kick / snare / closed hat + crash at 120 BPM.",
        "",
        "Each render is followed by a 2 s tail (inside the gated measurement). Presets ending in a master "
        "`limiter` may be driven into it; without one the gain stops at the true-peak limit (`peak-limited`, "
        "accepted by `--check`: only dynamics processing could make that crest factor louder). Anything else off "
        "target, e.g. needing more than the +12 dB `volume_db` limit, is `OUT` and fails `--check`.",
        "",
    ]
    if apply:
        lines += ["| Preset | Phrase | volume_db before → after | LUFS before | LUFS after | TP after (dBTP) "
                  "| Status |", "|---|---|---|---|---|---|---|"]
        for r in rows:
            lines.append(f"| `{r['preset'].removeprefix('presets/factory/')}` | {r['phrase']} | "
                         f"{r['vol_before']:g} → {r['vol_after']:g} | {f(r['lufs_before'])} | "
                         f"{f(r['lufs_after'])} | {f(r['tp_after'])} | {r['status']} |")
    else:
        lines += ["| Preset | Phrase | volume_db | LUFS | TP (dBTP) | Status |", "|---|---|---|---|---|---|"]
        for r in rows:
            lines.append(f"| `{r['preset'].removeprefix('presets/factory/')}` | {r['phrase']} | "
                         f"{r['vol_before']:g} | {f(r['lufs_before'])} | {f(r['tp_before'])} | {r['status']} |")
    if skipped:
        lines += ["", "Skipped (sample libraries not installed): " + ", ".join(f"`{s}`" for s in skipped)]
    if path.exists():  # keep hand-written notes below the marker
        old = path.read_text(encoding="utf-8")
        if NOTES_MARK in old:
            lines += ["", NOTES_MARK + old.split(NOTES_MARK, 1)[1].rstrip("\n")]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
