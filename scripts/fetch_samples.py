#!/usr/bin/env python3
"""Download and extract the SFZ sample libraries listed in assets/samples.json.

Stdlib only. Downloads are streamed to <repo>/.deps/downloads (resumable via HTTP Range),
size-checked against the manifest, then extracted into assets/samples/<id>/.
A marker file (.fetched.json) in each library dir makes re-runs skip finished libraries.

Usage:
  python scripts/fetch_samples.py               # fetch everything in the manifest
  python scripts/fetch_samples.py --list        # show libraries and their state
  python scripts/fetch_samples.py --only salamander,jrhodes3c
  python scripts/fetch_samples.py --force       # re-extract even if marker exists
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "assets" / "samples.json"
DOWNLOADS = REPO / ".deps" / "downloads" / "samples"
MARKER = ".fetched.json"
UA = "keysynth-fetch-samples/1.0 (+python urllib)"
CHUNK = 1 << 20


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1000
    return f"{n:.1f} GB"


def load_manifest() -> dict:
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)


def archive_filename(lib_id: str, idx: int, arc: dict) -> str:
    if arc.get("filename"):
        return arc["filename"]
    ext = {"zip": ".zip", "tar.gz": ".tar.gz", "tar.xz": ".tar.xz", "tar.bz2": ".tar.bz2"}[arc["archive"]]
    return f"{lib_id}-{idx}{ext}"


def download(url: str, dest: Path, expected: int | None, retries: int = 5) -> None:
    """Stream url to dest, resuming from dest.part when the server supports ranges."""
    if dest.exists() and (expected is None or dest.stat().st_size == expected):
        print(f"  have {dest.name} ({human(dest.stat().st_size)})")
        return
    if dest.exists():
        print(f"  {dest.name}: size {dest.stat().st_size} != expected {expected}, re-downloading")
        dest.unlink()
    part = dest.with_name(dest.name + ".part")
    dest.parent.mkdir(parents=True, exist_ok=True)

    for attempt in range(1, retries + 1):
        have = part.stat().st_size if part.exists() else 0
        if expected is not None and have > expected:
            part.unlink()
            have = 0
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        # Only try to resume when the expected size is known (GitHub archive zips are generated
        # on the fly, have no Content-Length, and do not reliably honour Range).
        if have and expected is not None:
            req.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                status = resp.status
                if have and status != 206:
                    have = 0  # server ignored Range; start over
                total = expected
                cl = resp.headers.get("Content-Length")
                if total is None and cl and status == 200:
                    total = int(cl)
                mode = "ab" if have else "wb"
                done = have
                t0 = time.time()
                last = 0.0
                with open(part, mode) as out:
                    while True:
                        buf = resp.read(CHUNK)
                        if not buf:
                            break
                        out.write(buf)
                        done += len(buf)
                        now = time.time()
                        if now - last >= 5:
                            last = now
                            rate = (done - have) / max(now - t0, 1e-3)
                            pct = f"{100 * done / total:5.1f}%" if total else "  ?  "
                            print(f"    {dest.name}: {pct} {human(done)}"
                                  f"{' / ' + human(total) if total else ''} @ {human(rate)}/s", flush=True)
            size = part.stat().st_size
            if expected is not None and size != expected:
                raise IOError(f"size mismatch: got {size}, expected {expected}")
            part.replace(dest)
            print(f"  downloaded {dest.name} ({human(size)})")
            return
        except (urllib.error.URLError, IOError, TimeoutError, ConnectionError) as e:
            print(f"  attempt {attempt}/{retries} failed: {e}", flush=True)
            if attempt == retries:
                raise
            time.sleep(min(30, 3 * attempt))


def _safe_target(root: Path, name: str) -> Path:
    target = (root / name).resolve()
    if not str(target).startswith(str(root.resolve())):
        raise ValueError(f"unsafe path in archive: {name}")
    return target


def extract(archive: Path, kind: str, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    print(f"  extracting {archive.name} -> {dest.relative_to(REPO)}", flush=True)
    if kind == "zip":
        with zipfile.ZipFile(archive) as z:
            members = []
            for info in z.infolist():
                _safe_target(dest, info.filename)
                parts = info.filename.replace("\\", "/").split("/")
                # Drop macOS resource-fork junk (__MACOSX/, ._foo, .DS_Store); ._foo.sfz would look like an sfz.
                if "__MACOSX" in parts or parts[-1].startswith("._") or parts[-1] == ".DS_Store":
                    continue
                members.append(info)
            z.extractall(dest, members)
    elif kind.startswith("tar"):
        with tarfile.open(archive, "r:*") as t:
            try:
                t.extractall(dest, filter="data")
            except TypeError:  # Python < 3.12
                for m in t.getmembers():
                    _safe_target(dest, m.name)
                t.extractall(dest)
    else:
        raise ValueError(f"unsupported archive type {kind}")


def apply_patches(lib: dict, root: Path) -> None:
    """Apply manifest `patches` ({file, find, replace}) to fix upstream sfz bugs after extraction."""
    for p in lib.get("patches", []):
        f = root / p["file"]
        with open(f, encoding="utf-8", errors="surrogateescape", newline="") as fh:
            text = fh.read()
        n = text.count(p["find"])
        if n == 0:
            raise ValueError(f"patch target not found in {p['file']}: {p['find']!r}")
        with open(f, "w", encoding="utf-8", errors="surrogateescape", newline="") as fh:
            fh.write(text.replace(p["find"], p["replace"]))
        print(f"  patched {p['file']} ({n}x {p['find']!r} -> {p['replace']!r})")


def lib_dir(lib: dict) -> Path:
    return REPO / lib["dir"]


def is_done(lib: dict) -> bool:
    return (lib_dir(lib) / MARKER).exists()


def fetch(lib: dict, force: bool) -> None:
    d = lib_dir(lib)
    print(f"[{lib['id']}] {lib['name']}")
    if is_done(lib) and not force:
        print("  already fetched (marker present), skipping")
        return
    if force and d.exists():
        shutil.rmtree(d)
    staged = []
    for i, arc in enumerate(lib["archives"]):
        dest = DOWNLOADS / archive_filename(lib["id"], i, arc)
        download(arc["url"], dest, arc.get("size"))
        staged.append((dest, arc))
    # Extract into a temp dir, then move into place, so an interrupted extract never looks done.
    tmp = d.with_name(d.name + ".extracting")
    if tmp.exists():
        shutil.rmtree(tmp)
    for dest, arc in staged:
        extract(dest, arc["archive"], tmp / arc.get("subdir", ""))
    apply_patches(lib, tmp)
    if d.exists():
        shutil.rmtree(d)
    tmp.replace(d)
    marker = {
        "id": lib["id"],
        "archives": [{"file": p.name, "size": p.stat().st_size} for p, _ in staged],
        "fetched_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    (d / MARKER).write_text(json.dumps(marker, indent=2), encoding="utf-8")
    print("  done")


def dir_size(p: Path) -> int:
    return sum(f.stat().st_size for f in p.rglob("*") if f.is_file())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated library ids")
    ap.add_argument("--list", action="store_true", help="list libraries and status")
    ap.add_argument("--force", action="store_true", help="re-extract even if already fetched")
    args = ap.parse_args()

    libs = load_manifest()["libraries"]
    if args.only:
        wanted = [s.strip() for s in args.only.split(",") if s.strip()]
        known = {l["id"] for l in libs}
        bad = [w for w in wanted if w not in known]
        if bad:
            print(f"unknown id(s): {', '.join(bad)}; known: {', '.join(sorted(known))}", file=sys.stderr)
            return 2
        libs = [l for l in libs if l["id"] in wanted]

    if args.list:
        for l in libs:
            dl = sum(a.get("size") or a.get("approx_size") or 0 for a in l["archives"])
            state = "fetched" if is_done(l) else "missing"
            disk = f", {human(dir_size(lib_dir(l)))} on disk" if is_done(l) else ""
            print(f"{l['id']:<22} {state:<8} ~{human(dl)} download{disk}  "
                  f"[{l['licence']}] {l['name']}")
        return 0

    failed = []
    for l in libs:
        try:
            fetch(l, args.force)
        except Exception as e:  # keep going with the other libraries
            print(f"  FAILED: {e}", file=sys.stderr, flush=True)
            failed.append(l["id"])
    if failed:
        print(f"failed: {', '.join(failed)}", file=sys.stderr)
        return 1
    print("all done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
