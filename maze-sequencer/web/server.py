#!/usr/bin/env python3
"""Maze Sequencer web control panel — serves the UI (web/index.html, ported
from schwung-maze's own web_ui.html) and bridges its knob/stepper/bits-strip
actions to host_shim's Unix control socket (SET/GET - see host_shim.cpp's
header comment for the protocol).

One thing this bridge does that ../../maze-voice/web/server.py's doesn't
need to: maze_seq_core.c's get_param() only implements a fixed handful of
keys (running/module_id/state/s1_state/s2_state) - every individual knob
param (s1_corrupt, s2_channel, scale, ...) is set_param-only, exactly as it
was on Move (its own manager apparently flattened get_param("state")'s JSON
into individually-addressable params server-side - see web_ui.html's own
comment on "overtake tools get a real adaptive server-side poll"). So
/param's GET falls back to parsing the "state" JSON blob whenever the
control socket itself answers ERR for a plain `GET <key>`.

Deliberately stdlib-only (http.server + socket): no pip install step needed
on-device, matching this project's other web panels
(force-acid/web/server.py, ../../maze-voice/web/server.py).

This panel also starts/stops the engine process itself (Popen/killall),
same as force-acid's web panel and for the same reason: it's always-on
(its own addon, independent of the engine's), so the page needs a real
online/offline indicator and a Start/Stop control rather than assuming the
engine is running. See engine_start()/engine_stop() below.

Run: python3 server.py [--port N] [--ctrl-sock PATH]
"""
import json
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

WEB_DIR = Path(__file__).resolve().parent
ADDON_DIR = WEB_DIR.parent          # .../AddOns/ForceMazeSeq on a deployed device
ENGINE_BIN = ADDON_DIR / "maze_seq_host"
CTRL_SOCK = "/tmp/maze_seq_ctrl.sock"
SOCK_TIMEOUT = 1.0


def ctrl_request(line: str):
    """Send one line to host_shim's control socket, return its reply (or
    None if the engine isn't reachable). `with` guarantees the socket closes
    on every exit path, including a connect/send/recv timeout - see
    ../../maze-voice/web/server.py's own comment for why that matters under
    a knob-drag-rate burst of calls."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(SOCK_TIMEOUT)
            s.connect(CTRL_SOCK)
            s.sendall((line.strip("\n") + "\n").encode("utf-8"))
            reply = s.recv(8192)
            return reply.decode("utf-8", errors="replace").strip("\n")
    except OSError:
        return None


def get_param_via_state(key: str):
    """Fallback for the many knob params get_param() doesn't implement
    directly - parse the "state" JSON blob and pull the field out of it."""
    reply = ctrl_request("GET state")
    if reply is None or reply == "ERR":
        return None
    try:
        state = json.loads(reply)
    except json.JSONDecodeError:
        return None
    return state.get(key)


def engine_present() -> bool:
    """Is maze_seq_host actually up and answering? The control socket is a
    direct, sufficient proxy for this - no need to check ALSA MIDI ports
    the way force-acid's web/server.py does (that one has no control socket
    at all, only MIDI, so port presence IS its liveness check)."""
    return ctrl_request("GET running") is not None


def engine_start():
    if engine_present():
        return True, "already running"
    if not ENGINE_BIN.exists():
        return False, f"binary not found: {ENGINE_BIN}"
    args = [
        str(ENGINE_BIN),
        "--module-dir", str(ADDON_DIR),
        "--ctrl-sock", CTRL_SOCK,
        "--control-channel", "1",
    ]
    try:
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
    except Exception as e:
        return False, str(e)
    # Reap it whenever it exits (killall from /engine stop, a crash, or the
    # process outliving this server) - otherwise it zombies forever, since
    # nothing else waits on it. Same pattern as force-acid/web/server.py.
    threading.Thread(target=proc.wait, daemon=True).start()
    for _ in range(30):  # up to ~3s for the control socket to come up
        time.sleep(0.1)
        if engine_present():
            return True, "started"
    return False, "launched but control socket did not respond in time"


def engine_stop():
    subprocess.run(["killall", "maze_seq_host"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return True, "stopped"


class Handler(BaseHTTPRequestHandler):
    server_version = "MazeSeqWeb/0.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[maze-seq-web] " + (fmt % args) + "\n")

    def _text(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code, obj):
        self._text(code, json.dumps(obj), "application/json; charset=utf-8")

    def _file(self, relpath, ctype):
        path = WEB_DIR / relpath
        try:
            data = path.read_bytes()
        except OSError:
            self.send_error(404, "not found")
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path

        if path in ("/", "/index.html"):
            self._file("index.html", "text/html; charset=utf-8")
            return

        if path == "/param":
            qs = parse_qs(urlparse(self.path).query)
            key = (qs.get("key") or [""])[0]
            if not key:
                self._text(400, "missing key")
                return
            reply = ctrl_request(f"GET {key}")
            if reply is None:
                self._text(503, "engine not running")
                return
            if reply == "ERR":
                val = get_param_via_state(key)
                if val is None:
                    self._text(404, "unknown param")
                else:
                    self._text(200, str(val))
                return
            self._text(200, reply)
            return

        if path == "/state":
            reply = ctrl_request("GET state")
            if reply is None or reply == "ERR":
                self._json(503, {})
            else:
                # already JSON text from maze_seq_core.c's build_state_json
                self._text(200, reply, "application/json; charset=utf-8")
            return

        if path == "/status":
            self._json(200, {"engine_running": engine_present()})
            return

        self.send_error(404, "not found")

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            self._json(400, {"ok": False, "error": "bad json"})
            return

        if path == "/param":
            key, value = body.get("key"), body.get("value")
            if not key or value is None:
                self._json(400, {"ok": False, "error": "missing key/value"})
                return
            reply = ctrl_request(f"SET {key} {value}")
            self._json(200 if reply == "OK" else 503, {"ok": reply == "OK"})
            return

        if path == "/engine":
            action = body.get("action")
            if action == "start":
                ok, msg = engine_start()
            elif action == "stop":
                ok, msg = engine_stop()
            else:
                self._json(400, {"ok": False, "error": "action must be start|stop"})
                return
            self._json(200 if ok else 503, {"ok": ok, "message": msg})
            return

        self.send_error(404, "not found")


def main():
    global CTRL_SOCK
    port = 8305  # next free slot after force-acid's 8303 and maze-voice's 8304
    args = sys.argv[1:]
    if "--port" in args:
        port = int(args[args.index("--port") + 1])
    if "--ctrl-sock" in args:
        CTRL_SOCK = args[args.index("--ctrl-sock") + 1]

    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[maze-seq-web] serving on http://0.0.0.0:{port}  (control socket: {CTRL_SOCK})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
