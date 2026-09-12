# Force Maze Voice — web control panel

Browser UI for every `chain_params` knob/switch in `module.json`, styled and
laid out exactly like `schwung-maze`'s own `web_ui.html` (see `DESIGN.md`'s
"Web GUI" section for what changed and what didn't). An "Audition" strip of
note buttons at the bottom plays the voice straight from the page - no MIDI
track needed to hear a change take effect.

## Run it

**Autolaunch (survives reboot), independent of the engine:**
```sh
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/web/manage.sh ENABLE'
```
Copies `run_maze_web.sh` to the top-level `AddOns/` folder and starts it
immediately. `DISABLE`/`UNINSTALL` stop it and remove the autolaunch entry.
This is independent of `addon/manage.sh` (the engine, `maze_host` +
`forceAudioIn.so`'s `LD_PRELOAD` tap) - enabling this does not enable the
engine, and vice versa. The page itself works either way: every control
just answers 503 until `maze_host`'s control socket exists.

The launcher tracks its PID in `.maze_web.pid` next to `server.py` - not
`killall python3`/a name-based `pgrep`, since this device runs other
python3 processes (nodeServer's tooling, force-acid's own web panel) that a
name match would also take down.

**Manual (dev loop, no autolaunch):**
```sh
ssh root@<force-ip> 'python3 /media/662522/AddOns/ForceMazeVoice/web/server.py &'
# then open http://<force-ip>:8304
```

## Port

**8304.** `force-acid`'s web panel already owns 8303; nodeServer owns
8080/443 (historically 80); DrmVncServer owns 5900. See
`~/.claude/skills/mockbamod-module-creator/references/web-gui.md` on
picking a port and checking for collisions before changing this.

## nodeServer integration

A home-page quick-link and a Modules-page entry both exist - see
`../nodeserver-integration/README.md` for what's patched and where.

## Not yet done

- No auth - anyone on the LAN who knows the IP:port can control it. Fine
  for a home studio, worth a look before exposing more broadly.
- No mobile home-screen icon / PWA manifest yet.
