#!/usr/bin/env python3
"""Local stand-in for the keysynth relay hub (docs/specs/2026-10-12-remote.md §3 + §10 envelope) on top of the dev
cloud API (scripts/dev_cloud_server.py: devices/me, manifest, presets), for engine relay-host E2E tests without the
VDS. Standard library only (RFC 6455 framing implemented here). Not secure, not for production.

  GET /keysynth/api/relay?role=host          Authorization: Bearer <device token> (else 401 before the upgrade)
  GET /keysynth/api/relay?role=controller&cid=<id>   no auth (dev); cid generated when missing

Envelope (JSON text frames):
  host -> server        {t:"hello", host:{deviceId,name,version}}  {t:"msg", to:"<cid>"|"*", except?, lossy?, msg}
  server -> host        {t:"controller", cid, joined}  {t:"msg", from:"<cid>", msg}  {t:"pause"|"resume", cid}
  controller -> server  {t:"msg", host:"<deviceId>", msg}  {t:"pause"} {t:"resume"}
  server -> controller  {t:"hosts", hosts:[{deviceId,name,online,version}]}  {t:"msg", host, msg}  {t:"error", code}
  both ways             {t:"ping"} / {t:"pong"} (server pings every --ping s; idle sockets are dropped after 45 s)

Usage:
  python scripts/dev_relay_server.py --port 8766 --token ksd_<43 chars>
  keysynth-engine --no-audio --cloud-api http://127.0.0.1:8766/keysynth   (then cloud_link {token} on the local WS)
E2E: scripts/dev_relay_e2e.py (starts both).
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_cloud_server as api  # noqa: E402

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HOST_FRAME = 1 << 20
MAX_CONTROLLER_FRAME = 128 << 10
SILENCE_S = 45


class WsConn:
    """Server side of one WebSocket (blocking reads on the handler thread, locked writes)."""

    def __init__(self, sock, rfile, max_frame):
        self.sock, self.rfile, self.max_frame = sock, rfile, max_frame
        self.wlock = threading.Lock()
        self.closed = False
        self.last_rx = time.monotonic()

    def _read(self, n):
        b = self.rfile.read(n)
        if b is None or len(b) < n:
            raise ConnectionError("eof")
        return b

    def recv(self):
        """Next text message (str) or None when the connection closed."""
        parts, total = [], 0
        while True:
            h = self._read(2)
            fin, op, masked, ln = h[0] & 0x80, h[0] & 0x0F, h[1] & 0x80, h[1] & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read(8))[0]
            if total + ln > self.max_frame:
                self.close(1009, "too big")
                return None
            mask = self._read(4) if masked else b"\0\0\0\0"
            data = bytearray(self._read(ln))
            for i in range(ln):
                data[i] ^= mask[i & 3]
            self.last_rx = time.monotonic()
            if op == 0x8:  # close
                code = struct.unpack(">H", bytes(data[:2]))[0] if ln >= 2 else 1005
                self.close(code, "")
                return None
            if op == 0x9:  # ping
                self._send_frame(0xA, bytes(data))
                continue
            if op == 0xA:  # pong
                continue
            if op in (0x1, 0x2, 0x0):
                parts.append(bytes(data))
                total += ln
                if fin:
                    msg = b"".join(parts)
                    parts, total = [], 0
                    if op == 0x2:
                        continue
                    return msg.decode("utf-8", "replace")

    def _send_frame(self, op, payload):
        n = len(payload)
        if n < 126:
            head = struct.pack(">BB", 0x80 | op, n)
        elif n < 65536:
            head = struct.pack(">BBH", 0x80 | op, 126, n)
        else:
            head = struct.pack(">BBQ", 0x80 | op, 127, n)
        with self.wlock:
            if self.closed and op != 0x8:
                return False
            try:
                self.sock.sendall(head + payload)
                return True
            except OSError:
                self.closed = True
                return False

    def send(self, obj):
        return self._send_frame(0x1, json.dumps(obj).encode())

    def close(self, code=1000, reason=""):
        if self.closed:
            return
        self._send_frame(0x8, struct.pack(">H", code) + reason.encode()[:120])
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class Hub:
    def __init__(self):
        self.lock = threading.Lock()
        self.hosts = {}  # deviceId -> {"conn", "name", "version"}
        self.controllers = {}  # cid -> {"conn", "paused"}
        self.stats = {"host_frames": 0, "controller_frames": 0, "lossy": 0}

    def hosts_msg(self):
        return {"t": "hosts", "hosts": [{"deviceId": d, "name": h["name"], "version": h["version"], "online": True}
                                        for d, h in self.hosts.items()]}

    def broadcast_hosts(self):
        with self.lock:
            msg, ctls = self.hosts_msg(), [c["conn"] for c in self.controllers.values()]
        for c in ctls:
            c.send(msg)

    def to_hosts(self, obj):
        with self.lock:
            hs = [h["conn"] for h in self.hosts.values()]
        for h in hs:
            h.send(obj)


HUB = Hub()


def log(*a):
    print("[dev-relay]", *a, file=sys.stderr, flush=True)


class Handler(api.Handler):
    server_version = "keysynth-dev-relay/1"

    def do_GET(self):
        if self.route() == "/api/relay":
            return self.relay()
        return super().do_GET()

    def relay(self):
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        role = (q.get("role") or [""])[0]
        if self.headers.get("Upgrade", "").lower() != "websocket" or not self.headers.get("Sec-WebSocket-Key"):
            return self.send_json(400, {"error": "websocket_required"})
        if role == "host":
            if not self.account():
                return self.send_json(401, {"error": "token_invalid"})
        elif role != "controller":
            return self.send_json(400, {"error": "bad_role"})
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + WS_GUID).encode()).digest())
        # Written by hand: BaseHTTPRequestHandler speaks HTTP/1.0, browsers/websockets require a 1.1 status line.
        self.log_request(101)
        crlf = chr(13) + chr(10)
        self.wfile.write((f"HTTP/1.1 101 Switching Protocols{crlf}Upgrade: websocket{crlf}Connection: Upgrade{crlf}"
                          f"Sec-WebSocket-Accept: {accept.decode()}{crlf}{crlf}").encode())
        self.wfile.flush()
        self.close_connection = True
        conn = WsConn(self.connection, self.rfile, MAX_HOST_FRAME if role == "host" else MAX_CONTROLLER_FRAME)
        try:
            if role == "host":
                self.run_host(conn)
            else:
                self.run_controller(conn, (q.get("cid") or [str(uuid.uuid4())])[0])
        except (ConnectionError, OSError):
            pass
        finally:
            conn.closed = True

    def run_host(self, conn):
        device = None
        try:
            while True:
                text = conn.recv()
                if text is None:
                    break
                try:
                    m = json.loads(text)
                except ValueError:
                    continue
                t = m.get("t")
                if t == "ping":
                    conn.send({"t": "pong"})
                elif t == "hello":
                    h = m.get("host") or {}
                    device = h.get("deviceId") or "unknown"
                    with HUB.lock:
                        old = HUB.hosts.get(device)
                        HUB.hosts[device] = {"conn": conn, "name": h.get("name", ""), "version": h.get("version", "")}
                        cids = list(HUB.controllers)
                    if old and old["conn"] is not conn:
                        old["conn"].close(4000, "replaced")
                    log("host online", device, h.get("version"))
                    for cid in cids:
                        conn.send({"t": "controller", "cid": cid, "joined": True})
                    HUB.broadcast_hosts()
                elif t == "msg" and device:
                    HUB.stats["host_frames"] += 1
                    if m.get("lossy"):
                        HUB.stats["lossy"] += 1
                    to, exc, out = m.get("to"), m.get("except"), {"t": "msg", "host": device, "msg": m.get("msg")}
                    with HUB.lock:
                        targets = [c["conn"] for cid, c in HUB.controllers.items()
                                   if (to == "*" and cid != exc) or cid == to]
                    for c in targets:
                        c.send(out)
        finally:
            if device:
                with HUB.lock:
                    if HUB.hosts.get(device, {}).get("conn") is conn:
                        del HUB.hosts[device]
                log("host offline", device)
                HUB.broadcast_hosts()

    def run_controller(self, conn, cid):
        with HUB.lock:
            old = HUB.controllers.get(cid)
            HUB.controllers[cid] = {"conn": conn, "paused": False}
            hosts = HUB.hosts_msg()
        if old:
            old["conn"].close(4000, "resumed elsewhere")
        conn.send(hosts)
        HUB.to_hosts({"t": "controller", "cid": cid, "joined": True})
        log("controller joined", cid)
        try:
            while True:
                text = conn.recv()
                if text is None:
                    break
                try:
                    m = json.loads(text)
                except ValueError:
                    continue
                t = m.get("t")
                if t == "ping":
                    conn.send({"t": "pong"})
                elif t in ("pause", "resume"):
                    HUB.to_hosts({"t": t, "cid": cid})
                elif t == "msg":
                    HUB.stats["controller_frames"] += 1
                    with HUB.lock:
                        h = HUB.hosts.get(m.get("host"))
                    if not h:
                        conn.send({"t": "error", "code": "host_offline"})
                    else:
                        h["conn"].send({"t": "msg", "from": cid, "msg": m.get("msg")})
        finally:
            with HUB.lock:
                if HUB.controllers.get(cid, {}).get("conn") is conn:
                    del HUB.controllers[cid]
                    gone = True
                else:
                    gone = False
            if gone:
                HUB.to_hosts({"t": "controller", "cid": cid, "joined": False})
            log("controller left", cid)


def pinger(interval):
    while True:
        time.sleep(interval)
        now = time.monotonic()
        with HUB.lock:
            conns = [h["conn"] for h in HUB.hosts.values()] + [c["conn"] for c in HUB.controllers.values()]
        for c in conns:
            if now - c.last_rx > SILENCE_S:
                c.close(4001, "silence")
            else:
                c.send({"t": "ping"})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--token", default="", help="accept this device token (default: a new one is printed)")
    ap.add_argument("--ping", type=float, default=20.0, help="server ping interval, seconds")
    a = ap.parse_args()
    token = a.token or api.new_token()
    index = json.dumps({"format": 1, "groups": [], "libraries": []}).encode()
    Handler.state = api.State(index, None, token, None)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=pinger, args=(a.ping,), daemon=True).start()
    print(f"dev relay + cloud API on http://127.0.0.1:{a.port}{api.PREFIX}", flush=True)
    print(f"device token: {token}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
