#!/usr/bin/env python3
"""Tiny local stand-in for the keysynth API (docs/specs/2026-10-12-cloud-api.md subset) for engine + UI E2E tests
without the VDS. Standard library only. Not secure, not for production.

Implements (base http://127.0.0.1:<port>/keysynth/api):
  POST   /api/devices            dev convenience: mints a device token (no cookie auth), body {deviceId, name}
  GET    /api/devices/me         DeviceMe            DELETE /api/devices/me
  GET    /api/samples/manifest   index.json + ETag / If-None-Match -> 304
  GET    /api/samples/files/{lib}/{path}   Range / If-Range / ETag (only paths listed in the index)
  GET    /api/presets            GET/PUT/DELETE /api/presets/{slug} (revision, If-Match, If-None-Match: *, tombstones)

Usage:
  python scripts/dev_cloud_server.py --samples assets/samples --libs salamander-lite --port 8765
  keysynth-engine --no-audio --cloud-api http://127.0.0.1:8765/keysynth/api
  (the printed token goes into `cloud_link`; the UI's link flow can also POST /api/devices on this server)

The index is read from --index FILE, or generated from --samples DIR: every sub-directory is a library, every file
in it is listed (sha256 computed at startup), every .sfz is an entry that references all files of its library
(an over-approximation of the real SFZ-aware builder, scripts/build_sample_index.py).
"""
import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import sys
import threading
import urllib.parse
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PREFIX = "/keysynth/api"
SLUG = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def build_index(samples_dir, libs_filter):
    libraries = []
    for lib in sorted(os.listdir(samples_dir)):
        d = os.path.join(samples_dir, lib)
        if not os.path.isdir(d) or not re.match(r"^[a-z0-9][a-z0-9-]*$", lib):
            continue
        if libs_filter and lib not in libs_filter:
            continue
        files, sfz = [], []
        for base, _, names in os.walk(d):
            for n in sorted(names):
                full = os.path.join(base, n)
                rel = os.path.relpath(full, d).replace(os.sep, "/")
                h = hashlib.sha256()
                with open(full, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                files.append({"path": rel, "bytes": os.path.getsize(full), "sha256": h.hexdigest()})
                if rel.lower().endswith(".sfz"):
                    sfz.append(rel)
        print(f"  {lib}: {len(files)} files", file=sys.stderr)
        idx = list(range(len(files)))
        total = sum(f["bytes"] for f in files)
        entries = [{"id": f"{lib}/{s}", "sfz": s, "name": os.path.splitext(os.path.basename(s))[0], "files": idx,
                    "bytes": total} for s in sfz]
        libraries.append({"id": lib, "name": lib, "group": "dev", "licence": "dev", "author": "", "attribution": "",
                          "bytes": total, "files": files, "entries": entries})
    return {"format": 1, "generatedAt": now_iso(), "groups": [{"id": "dev", "name": "Dev samples"}],
            "libraries": libraries}


class State:
    def __init__(self, index_bytes, samples_dir, token, data_file):
        self.lock = threading.Lock()
        self.index_bytes = index_bytes
        self.index = json.loads(index_bytes)
        self.etag = '"' + hashlib.sha256(index_bytes).hexdigest()[:16] + '"'
        self.samples_dir = samples_dir
        self.paths = {(lib["id"], f["path"]) for lib in self.index.get("libraries", []) for f in lib.get("files", [])}
        self.tokens = {token: "1"} if token else {}
        self.data_file = data_file
        self.presets = {}
        if data_file and os.path.exists(data_file):
            with open(data_file, "r", encoding="utf-8") as f:
                self.presets = json.load(f)

    def save(self):
        if self.data_file:
            with open(self.data_file + ".tmp", "w", encoding="utf-8") as f:
                json.dump(self.presets, f, indent=1)
            os.replace(self.data_file + ".tmp", self.data_file)


def new_token():
    return "ksd_" + base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")


class Handler(BaseHTTPRequestHandler):
    server_version = "keysynth-dev-api/1"
    state: State = None

    def log_message(self, fmt, *args):
        sys.stderr.write("[dev-api] " + (fmt % args) + "\n")

    def send_json(self, status, obj, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def account(self):
        auth = self.headers.get("Authorization", "")
        tok = auth[7:] if auth.startswith("Bearer ") else ""
        return self.state.tokens.get(tok)

    def route(self):
        path = urllib.parse.urlsplit(self.path).path
        if not path.startswith(PREFIX):
            return None
        return path[len(PREFIX):]

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def meta(self, slug, p):
        return {"slug": slug, "revision": p["revision"], "updatedAt": p["updatedAt"], "deleted": p["deleted"]}

    def do_POST(self):
        path = self.route()
        if path == "/api/devices":
            req = json.loads(self.body() or b"{}")
            tok = new_token()
            with self.state.lock:
                self.state.tokens[tok] = "1"
            exp = (datetime.now(timezone.utc) + timedelta(days=180)).strftime("%Y-%m-%dT%H:%M:%SZ")
            return self.send_json(201, {"deviceId": req.get("deviceId", ""), "token": tok, "expiresAt": exp,
                                        "accountId": "1", "accountName": "Dev"})
        self.send_json(404, {"error": "not_found"})

    def do_GET(self):
        path = self.route()
        if path is None:
            return self.send_json(404, {"error": "not_found"})
        if path == "/health":
            return self.send_json(200, {"status": "ok"})
        acct = self.account()
        if not acct:
            return self.send_json(401, {"error": "token_invalid"}, {"WWW-Authenticate": 'Bearer error="token_invalid"'})
        if path == "/api/devices/me":
            return self.send_json(200, {"deviceId": "dev", "name": "dev", "accountId": acct, "accountName": "Dev",
                                        "expiresAt": "2099-01-01T00:00:00Z"})
        if path == "/api/samples/manifest":
            if self.headers.get("If-None-Match") == self.state.etag:
                self.send_response(304)
                self.send_header("ETag", self.state.etag)
                self.end_headers()
                return
            body = self.state.index_bytes
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("ETag", self.state.etag)
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path.startswith("/api/samples/files/"):
            rest = path[len("/api/samples/files/"):]
            lib, _, rel = rest.partition("/")
            lib, rel = urllib.parse.unquote(lib), urllib.parse.unquote(rel)
            if (lib, rel) not in self.state.paths or not self.state.samples_dir:
                return self.send_json(404, {"error": "not_found"})
            full = os.path.join(self.state.samples_dir, lib, *rel.split("/"))
            if not os.path.isfile(full):
                return self.send_json(404, {"error": "not_found"})
            st = os.stat(full)
            etag = '"%x-%x"' % (int(st.st_mtime), st.st_size)
            size, start = st.st_size, 0
            status = 200
            rng = self.headers.get("Range")
            if rng and (self.headers.get("If-Range") in (None, etag)):
                m = re.match(r"bytes=(\d+)-$", rng)
                if m:
                    start = int(m.group(1))
                    if start >= size:
                        self.send_response(416)
                        self.send_header("Content-Range", f"bytes */{size}")
                        self.end_headers()
                        return
                    status = 206
            self.send_response(status)
            self.send_header("ETag", etag)
            self.send_header("Content-Length", str(size - start))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
            self.end_headers()
            with open(full, "rb") as f:
                f.seek(start)
                for chunk in iter(lambda: f.read(1 << 16), b""):
                    self.wfile.write(chunk)
            return
        if path == "/api/presets":
            with self.state.lock:
                items = [self.meta(s, p) for s, p in sorted(self.state.presets.get(acct, {}).items())]
            return self.send_json(200, {"items": items})
        if path.startswith("/api/presets/"):
            slug = path[len("/api/presets/"):]
            with self.state.lock:
                p = self.state.presets.get(acct, {}).get(slug)
            if not SLUG.match(slug):
                return self.send_json(400, {"error": "invalid_slug"})
            if not p:
                return self.send_json(404, {"error": "not_found"})
            return self.send_json(200, dict(self.meta(slug, p), patch=p["patch"]), {"ETag": f'"{p["revision"]}"'})
        self.send_json(404, {"error": "not_found"})

    def do_PUT(self):
        path = self.route() or ""
        acct = self.account()
        if not acct:
            return self.send_json(401, {"error": "token_invalid"})
        if not path.startswith("/api/presets/"):
            return self.send_json(404, {"error": "not_found"})
        slug = path[len("/api/presets/"):]
        if not SLUG.match(slug) or len(slug) > 64:
            return self.send_json(400, {"error": "invalid_slug"})
        raw = self.body()
        if len(raw) > 256 * 1024 + 64:
            return self.send_json(413, {"error": "payload_too_large"})
        try:
            patch = json.loads(raw)["patch"]
            assert isinstance(patch, dict) and isinstance(patch.get("format"), (int, float))
        except Exception:
            return self.send_json(400, {"error": "invalid_patch"})
        if_match, if_none = self.headers.get("If-Match"), self.headers.get("If-None-Match")
        with self.state.lock:
            mine = self.state.presets.setdefault(acct, {})
            cur = mine.get(slug)
            if if_none == "*":
                if cur:
                    return self.send_json(412, {"error": "revision_mismatch", "current": self.meta(slug, cur)})
                mine[slug] = {"revision": 1, "updatedAt": now_iso(), "deleted": False, "patch": patch}
                status = 201
            elif if_match:
                if not cur or if_match != f'"{cur["revision"]}"':
                    return self.send_json(412, {"error": "revision_mismatch",
                                                "current": self.meta(slug, cur) if cur else None})
                mine[slug] = {"revision": cur["revision"] + 1, "updatedAt": now_iso(), "deleted": False, "patch": patch}
                status = 200
            else:
                return self.send_json(428, {"error": "precondition_required"})
            p = mine[slug]
            self.state.save()
        self.send_json(status, self.meta(slug, p), {"ETag": f'"{p["revision"]}"'})

    def do_DELETE(self):
        path = self.route() or ""
        acct = self.account()
        if not acct:
            return self.send_json(401, {"error": "token_invalid"})
        if path == "/api/devices/me":
            auth = self.headers.get("Authorization", "")[7:]
            with self.state.lock:
                self.state.tokens.pop(auth, None)
            self.send_response(204)
            self.end_headers()
            return
        if not path.startswith("/api/presets/"):
            return self.send_json(404, {"error": "not_found"})
        slug = path[len("/api/presets/"):]
        with self.state.lock:
            cur = self.state.presets.get(acct, {}).get(slug)
            if not cur:
                return self.send_json(404, {"error": "not_found"})
            if self.headers.get("If-Match") != f'"{cur["revision"]}"':
                return self.send_json(412, {"error": "revision_mismatch", "current": self.meta(slug, cur)})
            cur.update(revision=cur["revision"] + 1, updatedAt=now_iso(), deleted=True, patch=None)
            self.state.save()
            p = dict(cur)
        self.send_json(200, self.meta(slug, p), {"ETag": f'"{p["revision"]}"'})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--samples", help="directory with <lib>/... (e.g. assets/samples)")
    ap.add_argument("--index", help="index.json to serve (default: generated from --samples)")
    ap.add_argument("--libs", default="", help="comma-separated library ids to include when generating")
    ap.add_argument("--token", default="", help="accept this device token (default: a new one is printed)")
    ap.add_argument("--data", default="", help="JSON file to persist presets (default: memory only)")
    a = ap.parse_args()
    if a.index:
        with open(a.index, "rb") as f:
            index_bytes = f.read()
    elif a.samples:
        print("building index ...", file=sys.stderr)
        libs = {x for x in a.libs.split(",") if x}
        index_bytes = json.dumps(build_index(a.samples, libs), indent=1).encode()
    else:
        index_bytes = json.dumps({"format": 1, "groups": [], "libraries": []}).encode()
    token = a.token or new_token()
    Handler.state = State(index_bytes, a.samples, token, a.data or None)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"dev cloud API on http://127.0.0.1:{a.port}{PREFIX}", flush=True)
    print(f"device token: {token}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
