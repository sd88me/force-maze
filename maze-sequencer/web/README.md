# Force Maze Sequencer — web control panel

Browser UI ported from `schwung-maze`'s own `web_ui.html` (see `../DESIGN.md`'s
"Web GUI" section for exactly what changed and what didn't). Unlike
Maze Voice's panel there's no "Audition" strip — this module only generates
step sequences, it doesn't play notes on demand — but the two 8-step bits
strips are genuinely live: they poll `GET /state` every ~200ms so the
play-head and step pattern track the running sequencer in real time, and
clicking any step LED flips that step immediately (`s{1,2}_flip`).

**Engine on/off** — the topbar's ENGINE indicator/button (green LED + START/
STOP) starts and stops `maze_seq_host` itself via `server.py`'s `/engine`
endpoint (`Popen`/`killall`, same pattern as `force-acid`'s web panel). This
page is always up regardless of whether the engine is — every control just
answers 503 until it's running — so this is the "is it actually there"
indicator that replaces a `statusText` stuck forever on "Connecting…".

**Sequencer A / Sequencer B** — the Sequencer section's top and bottom rows
are now explicitly captioned, with the shared Trig Mix/Reset Both controls
in an unlabeled row between them.

**Global dropdowns** — Scale/Key/Note Rate/Note Len are native `<select>`
elements (like `force-acid`'s web panel's enum controls) instead of the
click-to-cycle stepper box used elsewhere — picking straight from a
6-12-option list beats clicking through them one at a time. The per-
sequencer Channel selectors and Reset Both keep the stepper style.

## Run it

**Autolaunch (survives reboot), independent of the engine:**
```sh
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeSeq/web/manage.sh ENABLE'
```
Copies `run_maze_seq_web.sh` to the top-level `AddOns/` folder and starts it
immediately. `DISABLE`/`UNINSTALL` stop it and remove the autolaunch entry.
Independent of `addon/manage.sh` (the engine, `maze_seq_host`) — enabling
this does not enable the engine, and vice versa. Unlike Maze Voice, the
engine here has no boot-race concern and is `AUTOLAUNCHABLE: true` — but the
web panel still stays a separate addon, same pattern as every other module
in this repo/force-acid, so the panel can be iterated on or restarted
without touching the running engine.

The launcher tracks its PID in `.maze_seq_web.pid` next to `server.py` — not
`killall python3`/a name-based `pgrep`, since this device runs other
python3 processes (nodeServer's tooling, the other modules' own web panels)
that a name match would also take down.

**Manual (dev loop, no autolaunch):**
```sh
ssh root@<force-ip> 'python3 /media/662522/AddOns/ForceMazeSeq/web/server.py &'
# then open http://<force-ip>:8305
```

## Port

**8305.** `force-acid`'s web panel owns 8303, Maze Voice's owns 8304;
nodeServer owns 8080/443 (historically 80); DrmVncServer owns 5900. See
`~/.claude/skills/mockbamod-module-creator/references/web-gui.md` on
picking a port and checking for collisions before changing this.

## Why /param's GET has a fallback

`maze_seq_core.c`'s `get_param()` only implements a handful of fixed keys
(`running`/`module_id`/`state`/`s1_state`/`s2_state`) — every knob param is
`set_param`-only, exactly as it was on Move. `server.py`'s `/param` GET
falls back to parsing the `state` JSON blob whenever a plain `GET <key>` on
the control socket comes back `ERR`, so `getParam()` in `index.html` still
works uniformly for every key without the page needing to know the
difference. See `server.py`'s own header comment.

## nodeServer integration

A home-page quick-link and a Modules-page entry both exist — see
`../nodeserver-integration/README.md` for what's patched and where.

## Not yet done

- No auth - anyone on the LAN who knows the IP:port can control it. Fine
  for a home studio, worth a look before exposing more broadly.
- No mobile home-screen icon / PWA manifest yet.
