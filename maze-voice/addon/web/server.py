#!/usr/bin/env python3
"""Maze Voice web control panel — serves the UI (web/index.html, ported
verbatim in style/layout from schwung-maze's own web_ui.html) and bridges
its knob/switch/button actions to maze_host's Unix control socket
(SET/GET/DESCRIBE/NOTE - see src/maze_host.cpp's header comment for the
protocol).

Deliberately stdlib-only (http.server + socket): no pip install step needed
on-device, matching this project's other web panels
(force-acid/web/server.py, ~/.claude/skills/mockbamod-module-creator/
references/web-gui.md).

Run: python3 server.py [--port N] [--ctrl-sock PATH]
"""
import json
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

WEB_DIR = Path(__file__).resolve().parent
CTRL_SOCK = "/tmp/maze_ctrl.sock"
SOCK_TIMEOUT = 1.0


def ctrl_request(line: str):
    """Send one line to maze_host's control socket, return its reply (or
    None if the engine isn't reachable). One connection per request -
    these are user-interaction-rate calls (knob turns, button presses),
    nowhere near the audio thread, so the per-call connect cost is fine.

    `with` guarantees the socket closes on every exit path, including a
    connect/send/recv timeout - an earlier version only closed it on the
    success path, leaking one fd per failed/timed-out request. Under a
    sustained burst of calls (a knob drag fires one HTTP request per
    mousemove, easily 50-100/sec - see index.html's THROTTLE_MS) that leak
    was the likely cause of the panel going unresponsive after a while:
    each leaked fd is also a lingering half-open connection maze_host's
    single-threaded accept() loop has to get through before it can serve
    the next real request, so throughput degrades as the leak grows."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(SOCK_TIMEOUT)
            s.connect(CTRL_SOCK)
            s.sendall((line.strip("\n") + "\n").encode("utf-8"))
            reply = s.recv(8192)
            return reply.decode("utf-8", errors="replace").strip("\n")
    except OSError:
        return None


class Handler(BaseHTTPRequestHandler):
    server_version = "MazeVoiceWeb/0.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[maze-web] " + (fmt % args) + "\n")

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
            elif reply == "ERR":
                self._text(404, "unknown param")
            else:
                self._text(200, reply)
            return

        if path == "/describe":
            reply = ctrl_request("DESCRIBE")
            if reply is None:
                self._json(503, {})
            else:
                # already JSON text from maze_voice.c's own chain_params
                self._text(200, reply, "application/json; charset=utf-8")
            return

        if path == "/status":
            self._json(200, {"engine_running": ctrl_request("DESCRIBE") is not None})
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

        if path == "/note":
            note = int(body.get("note", 60))
            vel = int(body.get("velocity", 100))
            reply = ctrl_request(f"NOTE {note} {vel}")
            self._json(200 if reply == "OK" else 503, {"ok": reply == "OK"})
            return

        self.send_error(404, "not found")


def main():
    global CTRL_SOCK
    port = 8304  # next free slot after force-acid's 8303; see gotchas.md on port collisions
    args = sys.argv[1:]
    if "--port" in args:
        port = int(args[args.index("--port") + 1])
    if "--ctrl-sock" in args:
        CTRL_SOCK = args[args.index("--ctrl-sock") + 1]

    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[maze-web] serving on http://0.0.0.0:{port}  (control socket: {CTRL_SOCK})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
