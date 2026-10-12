#!/usr/bin/env python3
"""Relay-host E2E (docs/TESTING.md "Relay E2E"): dev relay server + engine (--no-audio, alternate ports, temp data
dir) + two python controllers over the relay. Checks: the engine links and comes online as a host; hello over the
relay answers trust "remote"; load_preset round trip (reply + state); a set_param from controller 1 is echoed as
`param` to controller 2 (and the local client) but not back to the sender; forbidden messages answer
`error forbidden` and change nothing; telemetry reaches controllers flagged lossy. Every process it starts is
stopped at the end (never uses the installed engine's ports 7340/7341).

  python scripts/dev_relay_e2e.py [--engine build-relay/bin/Release/keysynth-engine.exe] [--idle 50 --server-ping 1000]
Requires `python -m pip install websockets`.
"""
import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

import websockets

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOKEN = "ksd_" + "e2eDevRelayToken0123456789abcdefghijklmnopq"[:43]


def wait_http(url, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with urllib.request.urlopen(url, timeout=1) as r:
                if r.status == 200:
                    return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("no answer from " + url)


class Client:
    """A websocket with a background reader; `expect` waits for a matching message."""

    def __init__(self, ws, name, relay_host=None):
        self.ws, self.name, self.host = ws, name, relay_host
        self.inbox, self.cond, self.next_id = [], asyncio.Condition(), 100
        self.task = asyncio.ensure_future(self._reader())

    async def _reader(self):
        try:
            async for text in self.ws:
                m = json.loads(text)
                async with self.cond:
                    self.inbox.append(m)
                    self.cond.notify_all()
        except websockets.ConnectionClosed:
            pass

    async def send(self, msg):
        if self.host is not None:  # relay envelope
            await self.ws.send(json.dumps({"t": "msg", "host": self.host, "msg": msg}))
        else:
            await self.ws.send(json.dumps(msg))

    def inner(self, m):
        return m.get("msg") if self.host is not None and m.get("t") == "msg" else m

    async def expect(self, pred, timeout=10.0, what="message"):
        end = time.monotonic() + timeout
        async with self.cond:
            while True:
                for i, m in enumerate(self.inbox):
                    if pred(m):
                        del self.inbox[: i + 1]
                        return m
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"{self.name}: timed out waiting for {what}")
                try:
                    await asyncio.wait_for(self.cond.wait(), left)
                except asyncio.TimeoutError:
                    pass

    async def request(self, msg, timeout=10.0):
        self.next_id += 1
        rid = self.next_id
        await self.send(dict(msg, id=rid))
        m = await self.expect(lambda m: (self.inner(m) or {}).get("id") == rid, timeout, f"reply to {msg['type']}")
        return self.inner(m)

    async def none_of(self, pred, seconds):
        await asyncio.sleep(seconds)
        async with self.cond:
            return not any(pred(m) for m in self.inbox)

    async def close(self):
        await self.ws.close()
        self.task.cancel()


async def probe(port):
    ws = await asyncio.wait_for(websockets.connect(f"ws://127.0.0.1:{port}"), 1)
    await ws.close()


def check(cond, what):
    if not cond:
        raise AssertionError(what)
    print("  ok:", what, flush=True)


async def scenario(a):
    relay = f"ws://127.0.0.1:{a.relay_port}/keysynth/api/relay"
    local = Client(await websockets.connect(f"ws://127.0.0.1:{a.ws_port}"), "local")
    st = await local.request({"type": "hello", "client": "e2e", "version": "0"})
    check(st["type"] == "state" and st["trust"] == "local", "local hello")
    r = await local.request({"type": "cloud_link", "token": TOKEN}, timeout=15)
    check(r["type"] == "cloud_status" and r["linked"], "cloud_link to the dev server")
    device = r["deviceId"]

    c1 = Client(await websockets.connect(relay + "?role=controller&cid=c1"), "c1", device)
    await c1.expect(lambda m: m.get("t") == "hosts" and any(h["deviceId"] == device and h["online"]
                                                             for h in m["hosts"]), 15, "host online")
    check(True, "engine registered as relay host (hello)")
    st = await c1.request({"type": "hello", "client": "ui", "version": "e2e"})
    check(st["type"] == "state" and st["trust"] == "remote", "hello over the relay -> trust remote")
    check(st["cloud"]["deviceId"] == device, "state.cloud.deviceId = host deviceId")

    lst = await c1.request({"type": "list_presets"})
    factory = [p for p in lst["presets"] if p["factory"]]
    check(len(factory) > 0, f"list_presets over the relay ({len(lst['presets'])} presets)")
    target = factory[min(1, len(factory) - 1)]["path"]
    ok = await c1.request({"type": "load_preset", "path": target}, timeout=20)
    check(ok["type"] == "load_preset_ok", "load_preset reply")
    s = await c1.expect(lambda m: (c1.inner(m) or {}).get("type") == "state" and c1.inner(m).get("presetPath") == target,
                        10, "state after load_preset")
    check(s.get("to") is None and s.get("host") == device, "state broadcast arrives from the host")
    await local.expect(lambda m: m.get("type") == "state" and m.get("presetPath") == target, 10, "local state")
    check(True, "local client sees the remote load_preset")

    c2 = Client(await websockets.connect(relay + "?role=controller&cid=c2"), "c2", device)
    await c2.expect(lambda m: m.get("t") == "hosts", 5, "hosts")
    st2 = await c2.request({"type": "hello"})
    check(st2["presetPath"] == target, "second controller hello shows the same preset")
    await c1.send({"type": "set_param", "node": 0, "param": "volume_db", "value": -9.5})
    is_param = lambda c: (lambda m: (c.inner(m) or {}).get("type") == "param" and c.inner(m).get("param") == "volume_db")
    p = await c2.expect(is_param(c2), 5, "param echo on c2")
    check(abs(p["msg"]["value"] + 9.5) < 1e-6, "param echoed to the second controller")
    await local.expect(lambda m: m.get("type") == "param" and m.get("param") == "volume_db", 5, "param on local")
    check(True, "param echoed to the local client")
    check(await c1.none_of(is_param(c1), 0.5), "no param echo back to the sender (except)")

    for t in ("cloud_unlink", "set_audio_device", "reload_plugin", "samples_delete", "song_load"):
        e = await c1.request({"type": t})
        check(e["type"] == "error" and e["code"] == "forbidden", f"{t} -> forbidden")
    st = await local.request({"type": "hello"})
    check(st["cloud"]["linked"], "engine still linked after the denied cloud_unlink")

    tele = await c1.expect(lambda m: (c1.inner(m) or {}).get("type") == "telemetry", 5, "telemetry")
    check(tele.get("t") == "msg", "telemetry reaches controllers")
    await c1.ws.send(json.dumps({"t": "pause"}))
    await asyncio.sleep(0.5)
    async with c1.cond:
        c1.inbox.clear()
    check(await c1.none_of(lambda m: (c1.inner(m) or {}).get("type") == "telemetry", 1.0),
          "paused controller gets no telemetry")
    await c2.expect(lambda m: (c2.inner(m) or {}).get("type") == "telemetry", 5, "telemetry c2")
    check(True, "unpaused controller still gets telemetry")
    await c1.ws.send(json.dumps({"t": "resume"}))
    await c1.expect(lambda m: (c1.inner(m) or {}).get("type") == "telemetry", 5, "telemetry after resume")
    check(True, "telemetry resumes")

    if a.idle > 0:
        print(f"  idling {a.idle} s (heartbeat / receive timeouts) ...", flush=True)
        await asyncio.sleep(a.idle)
        r = await c1.request({"type": "get_patch"}, timeout=10)
        check(r["type"] == "state", f"host still answers after {a.idle} s")

    for c in (c1, c2, local):
        await c.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=os.path.join(REPO, "build-relay", "bin", "Release", "keysynth-engine.exe"))
    ap.add_argument("--relay-port", type=int, default=8766)
    ap.add_argument("--ws-port", type=int, default=7351)
    ap.add_argument("--idle", type=float, default=0, help="extra idle seconds before a last request")
    ap.add_argument("--server-ping", type=float, default=5, help="dev server ping interval (1000 = rely on the "
                                                                  "engine's own 20 s heartbeat)")
    a = ap.parse_args()
    if a.ws_port in (7340, 7341):
        sys.exit("ports 7340/7341 belong to the installed engine")
    data = tempfile.mkdtemp(prefix="ks-relay-e2e-")
    procs = []
    logs = []
    try:
        slog = open(os.path.join(data, "relay.log"), "w")
        logs.append(slog)
        procs.append(subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "dev_relay_server.py"),
                                       "--port", str(a.relay_port), "--token", TOKEN, "--ping", str(a.server_ping)],
                                      stdout=slog, stderr=subprocess.STDOUT))
        wait_http(f"http://127.0.0.1:{a.relay_port}/keysynth/health")
        env = dict(os.environ, KS_DATA_DIR=os.path.join(data, "engine"))
        env.pop("KS_CLOUD_API", None)
        elog = open(os.path.join(data, "engine.log"), "w")
        logs.append(elog)
        procs.append(subprocess.Popen([a.engine, "--no-audio", "--no-midi", "--no-plugins", "--no-tray",
                                       "--port", str(a.ws_port), "--http-port", "0",
                                       "--cloud-api", f"http://127.0.0.1:{a.relay_port}/keysynth"],
                                      stdout=elog, stderr=subprocess.STDOUT, env=env, cwd=REPO))
        end = time.time() + 30
        while True:
            try:
                asyncio.run(probe(a.ws_port))
                break
            except Exception:
                if time.time() > end or procs[-1].poll() is not None:
                    raise RuntimeError("engine did not start")
                time.sleep(0.3)
        asyncio.run(scenario(a))
        print("relay E2E passed", flush=True)
        return 0
    except Exception as e:  # noqa: BLE001
        print("relay E2E FAILED:", e, flush=True)
        for f in logs:
            f.flush()
        for name in ("engine.log", "relay.log"):
            p = os.path.join(data, name)
            if os.path.exists(p):
                print(f"--- {name} (tail) ---")
                with open(p, errors="replace") as f:
                    print("".join(f.readlines()[-40:]))
        return 1
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        for f in logs:
            f.close()
        shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
