#!/usr/bin/env python3
"""Build the server sample catalogue `index.json` (docs/specs/2026-10-12-cloud-api.md §4).

Reads assets/samples.json and the extracted libraries under assets/samples/<lib>/, parses every listed
SFZ entry with the SFZ rules sfizz uses (#define, #include, default_path, sample= values with spaces,
// and /* */ comments), collects the .sfz + included files + samples of each entry, case-corrects the
paths to the real file names, hashes them (sha256) and writes index.json.

Fails (exit 1) on any missing file or invalid path.

    python scripts/build_sample_index.py                    # -> build/samples/index.json
    python scripts/build_sample_index.py -o out.json --lib salamander --no-hash
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
import unicodedata
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

GROUP_NAMES = {
    "piano": "Piano",
    "epiano": "Electric piano",
    "organ": "Organ",
    "clavinet": "Clavinet",
    "harpsichord": "Harpsichord",
    "mellotron": "Mellotron",
    "orchestra": "Orchestra",
    "drums": "Drums",
}

LIB_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}


class IndexError_(Exception):
    pass


def validate_path(p: str) -> None:
    """Path rules shared by builder, API and engine (cloud-api.md §4.1)."""
    if not p or len(p) > 240:
        raise IndexError_(f"invalid path (length): {p!r}")
    if p.startswith("/") or "\\" in p or ":" in p:
        raise IndexError_(f"invalid path (/, \\ or :): {p!r}")
    for ch in p:
        if ord(ch) < 0x20 or ord(ch) == 0x7F:
            raise IndexError_(f"invalid path (control char): {p!r}")
    for seg in p.split("/"):
        if seg in ("", ".", ".."):
            raise IndexError_(f"invalid path (segment {seg!r}): {p!r}")
        if seg.endswith(".") or seg.endswith(" "):
            raise IndexError_(f"invalid path (segment ends in . or space): {p!r}")
        if seg.split(".", 1)[0].upper() in RESERVED:
            raise IndexError_(f"invalid path (reserved name): {p!r}")


class CaseResolver:
    """Maps a case-insensitive relative path to the real on-disk names (the server is case-sensitive)."""

    def __init__(self, root: Path):
        self.root = root
        self._cache: dict[Path, dict[str, list[str]]] = {}

    def _listing(self, d: Path) -> dict[str, list[str]]:
        if d not in self._cache:
            m: dict[str, list[str]] = {}
            try:
                for name in os.listdir(d):
                    m.setdefault(name.casefold(), []).append(name)
            except OSError:
                pass
            self._cache[d] = m
        return self._cache[d]

    def resolve(self, rel: str) -> str | None:
        """rel: '/'-separated, may contain '.'/'..'. Returns the real relative path or None."""
        parts: list[str] = []
        for seg in rel.split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if not parts:
                    return None  # escapes the library dir
                parts.pop()
                continue
            names = self._listing(self.root.joinpath(*parts)).get(seg.casefold())
            if not names:
                return None
            parts.append(seg if seg in names else names[0])
        if not parts or not self.root.joinpath(*parts).is_file():
            return None
        return "/".join(parts)


_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_INCLUDE = re.compile(r'#include\s+"([^"]*)"')
_DEFINE = re.compile(r"^\s*#define\s+\$([A-Za-z0-9_]+)\s+(.*?)\s*$")
_TOKEN = re.compile(r"<([A-Za-z_]+)>|(?:(?<=\s)|(?<=>)|^)([A-Za-z_][A-Za-z0-9_]*)=")


def _strip_line_comment(line: str) -> str:
    i = line.find("//")
    return line if i < 0 else line[:i]


class SfzParser:
    def __init__(self, lib_root: Path, resolver: CaseResolver):
        self.lib_root = lib_root
        self.resolver = resolver

    def _expand(self, text: str, defines: dict[str, str]) -> str:
        for name in sorted(defines, key=len, reverse=True):
            text = text.replace("$" + name, defines[name])
        return text

    def _read(self, rel_real: str) -> str:
        data = (self.lib_root / rel_real).read_bytes()
        for enc in ("utf-8-sig", "cp1252"):
            try:
                return data.decode(enc)
            except UnicodeDecodeError:
                continue
        return data.decode("latin-1")

    def _preprocess(self, rel_real: str, base_dir: str, defines: dict[str, str], files: list[str],
                    depth: int = 0) -> str:
        if depth > 32:
            raise IndexError_(f"#include nesting too deep at {rel_real}")
        text = _BLOCK_COMMENT.sub(" ", self._read(rel_real))
        out: list[str] = []
        for raw in text.splitlines():
            line = _strip_line_comment(raw)
            m = _DEFINE.match(line)
            if m:
                defines[m.group(1)] = self._expand(m.group(2), defines)
                continue
            # #include may appear anywhere in a line (e.g. `<group> locc56=1 #include "x.sfz"`).
            while (m := _INCLUDE.search(line)) is not None:
                out.append(self._expand(line[:m.start()], defines))
                inc = self._expand(m.group(1), defines).replace("\\", "/")
                real = self.resolver.resolve(_join(base_dir, inc))
                if real is None:
                    raise IndexError_(f"{rel_real}: #include not found: {inc!r}")
                if real not in files:
                    files.append(real)
                out.append(self._preprocess(real, base_dir, defines, files, depth + 1))
                line = line[m.end():]
            out.append(self._expand(line, defines))
        return "\n".join(out)

    def entry_files(self, sfz_rel: str) -> list[str]:
        """Returns [sfz, includes..., samples...] as real relative paths."""
        sfz_real = self.resolver.resolve(sfz_rel)
        if sfz_real is None:
            raise IndexError_(f"sfz not found: {sfz_rel}")
        base_dir = sfz_real.rsplit("/", 1)[0] if "/" in sfz_real else ""
        files = [sfz_real]
        text = self._preprocess(sfz_real, base_dir, {}, files)
        default_path = ""
        tokens = list(_TOKEN.finditer(text))
        for i, tok in enumerate(tokens):
            if tok.group(1) is not None:
                continue
            opcode = tok.group(2)
            end = tokens[i + 1].start() if i + 1 < len(tokens) else len(text)
            value = text[tok.end():end]
            # A value runs to the next opcode/header or the end of the line (sample= may contain spaces).
            value = value.split("\n", 1)[0].strip()
            if opcode == "default_path":
                default_path = value.replace("\\", "/")
            elif opcode == "sample":
                if not value or value.startswith("*"):
                    continue  # generators: *sine, *silence, *noise ...
                rel = _join(base_dir, default_path + value.replace("\\", "/"))
                real = self.resolver.resolve(rel)
                if real is None:
                    raise IndexError_(f"{sfz_real}: sample not found: {value!r} (default_path {default_path!r})")
                if real not in files:
                    files.append(real)
        return files


def _join(base: str, rel: str) -> str:
    if rel.startswith("/"):
        rel = rel.lstrip("/")
    return f"{base}/{rel}" if base else rel


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def build(catalogue: Path, samples_root: Path, only: set[str] | None, do_hash: bool) -> dict:
    cat = json.loads(catalogue.read_text(encoding="utf-8"))
    groups: list[dict] = []
    libraries: list[dict] = []
    for lib in cat["libraries"]:
        lib_id = lib["id"]
        if only and lib_id not in only:
            continue
        if not LIB_ID_RE.match(lib_id):
            raise IndexError_(f"invalid library id {lib_id!r}")
        group = lib["category"]
        if group not in [g["id"] for g in groups]:
            groups.append({"id": group, "name": GROUP_NAMES.get(group, group.replace("-", " ").title())})
        lib_root = (REPO / lib["dir"]) if "dir" in lib else (samples_root / lib_id)
        if lib_root.resolve() != (samples_root / lib_id).resolve():
            raise IndexError_(f"{lib_id}: dir must be assets/samples/{lib_id}")
        if not lib_root.is_dir():
            raise IndexError_(f"{lib_id}: {lib_root} not found (run scripts/fetch_samples.py)")
        resolver = CaseResolver(lib_root)
        parser = SfzParser(lib_root, resolver)
        file_index: dict[str, int] = {}
        files: list[dict] = []
        entries: list[dict] = []
        for item in lib["sfz"]:
            sfz_rel, name = (item, None) if isinstance(item, str) else (item["path"], item.get("name"))
            ef = parser.entry_files(sfz_rel.replace("\\", "/"))
            idx = []
            for p in ef:
                p = unicodedata.normalize("NFC", p)
                validate_path(p)
                if p not in file_index:
                    file_index[p] = len(files)
                    full = lib_root / p
                    files.append({"path": p, "bytes": full.stat().st_size,
                                  "sha256": sha256_of(full) if do_hash else ""})
                idx.append(file_index[p])
            sfz_real = unicodedata.normalize("NFC", ef[0])
            entries.append({
                "id": f"{lib_id}/{sfz_real}",
                "sfz": sfz_real,
                "name": name or Path(sfz_real).stem,
                "files": idx,
                "bytes": sum(files[i]["bytes"] for i in idx),
            })
        libraries.append({
            "id": lib_id,
            "name": lib["name"],
            "group": group,
            "licence": lib.get("licence", ""),
            "author": lib.get("author", ""),
            "attribution": lib.get("attribution", ""),
            "bytes": sum(f["bytes"] for f in files),
            "files": files,
            "entries": entries,
        })
        print(f"{lib_id}: {len(entries)} entries, {len(files)} files, "
              f"{libraries[-1]['bytes'] / 1e6:.1f} MB", file=sys.stderr)
    return {
        "format": 1,
        "generatedAt": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "groups": groups,
        "libraries": libraries,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=REPO / "build" / "samples" / "index.json")
    ap.add_argument("--catalogue", type=Path, default=REPO / "assets" / "samples.json")
    ap.add_argument("--samples-root", type=Path, default=REPO / "assets" / "samples")
    ap.add_argument("--lib", action="append", help="only this library id (repeatable; for debugging)")
    ap.add_argument("--no-hash", action="store_true", help="skip sha256 (debugging only; output not uploadable)")
    a = ap.parse_args()
    try:
        index = build(a.catalogue, a.samples_root, set(a.lib) if a.lib else None, not a.no_hash)
    except (IndexError_, OSError, KeyError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    a.out.parent.mkdir(parents=True, exist_ok=True)
    tmp = a.out.with_suffix(".tmp")
    tmp.write_text(json.dumps(index, ensure_ascii=False, indent=1) + "\n", encoding="utf-8", newline="\n")
    os.replace(tmp, a.out)
    total = sum(lib["bytes"] for lib in index["libraries"])
    nfiles = sum(len(lib["files"]) for lib in index["libraries"])
    print(f"wrote {a.out}: {len(index['libraries'])} libraries, {nfiles} files, {total / 1e9:.2f} GB",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
