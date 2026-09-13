# force-maze

Port of [`schwung-maze`](https://github.com/sd88me/schwung-maze)'s **Maze
Voice** — a monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder
/ state-variable filter voice, originally a native DSP synth module for
Ableton Move — to the **Akai Force** running
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).

Unlike [`force-acid`](https://github.com/sd88me/force-acid) (a MIDI-FX
*generator* port, output is MIDI), this is a real **audio DSP synth**: it
renders actual samples and mixes them into what the Force's own app reads
from its audio-in capture device, so the voice comes out on a normal
Audio-In track.

## How it works

```
Mockba Maze:In (virtual MIDI port, notes + CC)
        │
        ▼
maze_host  ──renders──▶  maze_voice.c (verbatim DSP core, Schwung's plugin API v2)
        │
        ▼  (float32 stereo, shared-memory ring: forceAudioInject.h)
forceAudioIn.so  (LD_PRELOAD'd into /usr/bin/MPC)
        │  interposes snd_pcm_readi - mixes the ring's audio into whatever
        │  MPC reads from its capture device (hw:2, confirmed live)
        ▼
Audio-In track on the Force
```

`maze_host` is a standalone process playing the role Move's chain host plays
for the DSP core (see `src/maze_host.cpp`'s header comment) - same porting
pattern `force-acid/src/host_shim.cpp` uses for a MIDI-FX module, but for
audio: RtMidi in, a timer thread standing in for the SPI audio callback, and
its render output going into a shared-memory ring instead of MIDI out.

`forceAudioIn.so` is the *inverse* of MockbaMod's `forceStream.so`
(bundled with the `ForceLinkAudio` addon, which taps `snd_pcm_writei` to
**extract** what MPC plays): this taps `snd_pcm_readi` to **inject** synthesized
audio into what MPC reads from its capture device. It's a general-purpose
mechanism, not maze-voice-specific, and lives in its own repo,
[`force-audioin`](https://github.com/sd88me/force-audioin) (split out of this
repo on 2026-09-13 — its source used to live here), deployed as its own
standalone addon in the MockbaMod fork's
[`SD/AddOns/ForceAudioIn`](https://github.com/sd88me/MockbaMod/tree/main/SD/AddOns/ForceAudioIn)
— this repo depends on it rather than bundling it, so several voice addons
can share one tap instead of each racing to arm their own copy. That repo's
`injectTone.c` is a fixed-tone stand-in producer used to prove the
injection path works before wiring up a real DSP engine.

## Layout

```
src/
  maze_voice.c         schwung-maze's DSP core, byte-for-byte verbatim
  maze_host.cpp         RtMidi in, timer-driven render, writes to the ring
  forceAudioInject.h      shared-memory ring layout — vendored from the
                         separate ForceAudioIn repo (its own canonical copy);
                         keep byte-for-byte identical, it's a shared ABI
  include/plugin_api_v1.h Schwung's plugin ABI (v1 host_api, v1/v2 plugin API)
  rtmidi/                 vendored RtMidi 6 (ALSA backend)
addon/                  MockbaMod addon (the ENGINE): manage.sh (no longer
                        touches LD_PRELOAD/acvs - see below), prebuilt
                        maze_host, module.json, NSMODULE.json (nodeServer
                        Modules-page descriptor, Autoload deliberately
                        disabled), Force Maze Control.xtk (Q-Link track
                        template), web/ (bundled copy of the web GUI below
                        - a separate addon in its own right). Does NOT
                        bundle forceAudioIn.so/injectTone - see the
                        separate ForceAudioIn repo/addon.
web/                    a SEPARATE addon (the web panel), independently
                        enabled - see "Deploy / enable"
  index.html            control panel - ported verbatim in style/layout from
                        schwung-maze's own web_ui.html (the "rack" look,
                        SVG knobs, section layout all unchanged); only the
                        transport (Move's schwungRemote/postMessage API) is
                        swapped for fetch() calls to server.py
  server.py             stdlib-only HTTP server bridging the page to
                        maze_host's Unix control socket (SET/GET/DESCRIBE/NOTE)
  manage.sh, run_maze_web.sh   its own ENABLE/DISABLE, PID-file based
                                (same split + reasoning as force-acid's
                                web/manage.sh + run_forceacidweb.sh)
scripts/
  Dockerfile, build.sh          armhf-native (QEMU) build for maze_host,
                                same toolchain as force-acid
  build_xtk.py, xtk-seed.json    generates addon/Force Maze Control.xtk
docs/
  CC-MAP.md                      the 16 Q-Link knobs' CC assignments
  capture-xtk.md                 the .xtk format's reverse-engineering notes
nodeserver-integration/  patches for the SEPARATE nodeServer addon (home-page
                        link + confirms the Modules-page entry needs no
                        patch, just NSMODULE.json) - see its own README.md
```

## Build

```bash
./scripts/build.sh                       # maze_host, via Docker/QEMU armhf-native
```

Writes straight into `addon/`, ready to deploy as-is. `forceAudioIn.so`/
`injectTone` are no longer built from this repo at all — see the separate
[`force-audioin`](https://github.com/sd88me/force-audioin) repo's own
`scripts/build.sh`.

## Deploy / enable

Two independent things need to be on the device, in order:

1. **The shared tap** - the separate [`force-audioin`](https://github.com/sd88me/force-audioin)
   addon (already bundled in the MockbaMod fork at
   [`SD/AddOns/ForceAudioIn`](https://github.com/sd88me/MockbaMod/tree/main/SD/AddOns/ForceAudioIn)),
   enabled once (`manage.sh ENABLE`). This is what actually arms
   `LD_PRELOAD`, and it only ever attaches zero voices at boot - see its
   own README for why. If it's already enabled (e.g. another voice addon
   needs it too), nothing more to do here.
2. **This addon** (the engine):
   ```
   ssh root@<force-ip> 'rm -rf /media/662522/AddOns/ForceMazeVoice'   # see note below
   scp -r addon root@<force-ip>:/media/662522/AddOns/ForceMazeVoice
   ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/manage.sh ENABLE'
   ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/web/manage.sh ENABLE'   # the web panel
   ```
   `ENABLE` here does **not** touch `LD_PRELOAD` or restart `acvs` - it
   never has to, since the tap is ForceAudioIn's job now. Start `maze_host`
   itself from the nodeServer Modules page (`/moduler`) once both addons
   are in place - never at boot (Autoload is deliberately unavailable for
   this module - see NSMODULE.json and DESIGN.md's "hard rule": an `acvs`
   restart while a voice is attached reliably kills pads/buttons).

**The `rm -rf` first matters**: `scp -r addon dest` copies `addon` itself
as a subdirectory of `dest` if `dest` already exists (`dest/addon/...`)
rather than merging its contents into `dest` - hit this live while
redeploying (2026-09-13). Safe to skip only when deploying to a path that
doesn't exist yet.

**The engine and the web panel are two separate addons** (`addon/`'s own
`manage.sh` vs. `addon/web/manage.sh`) - enabling one does not enable the
other. The web panel has no `LD_PRELOAD`/`acvs` involvement at all and can
stay always-on (like `force-acid`'s own web panel) even while the engine
is stopped - every control on the page just answers 503 until
`maze_host`'s control socket exists.

Then: route a MIDI track to `Mockba Maze:In` for notes, and monitor/record
from whichever Audio-In track corresponds to the Force's `hw:2` capture
device (confirmed live - may enumerate differently if your USB device order
differs). Open `http://<force-ip>:8304` for the web control panel (or use
nodeServer's home-page "Force Maze Voice" link / Modules page, if
nodeServer is installed - see `nodeserver-integration/README.md`) - the
`Audition` strip at the bottom plays a note straight from the page, no MIDI
keyboard needed to hear a change take effect.

For physical knob control: a MIDI track named `MAZE CTRL` → `Mockba Maze:In`
ch 1, with `addon/Force Maze Control.xtk` loaded onto it for 16 pre-named
Q-Link knobs. See `docs/CC-MAP.md` for the full assignment (16 of
`maze_voice.c`'s ~30 params - the rest stay web-only) and setup steps.

## Status

Working end-to-end and hardware-verified: DSP core ported, note-in (+ CC-in
on a control channel), synthesized audio audible on a real Audio-In track,
full web control panel (every `chain_params` knob/switch from
`module.json`, styled and laid out exactly like the original Move version,
now an independently-always-on addon), a nodeServer home-page link +
Modules-page entry, plus a Q-Link track template for the 16 most-used
params on physical knobs (structurally valid, not yet visually confirmed on
a real screen - see `docs/capture-xtk.md`). Audio timing needed real tuning
(see `DESIGN.md` for the full story) — currently a fixed clock-rate
correction plus a generous ~100/200ms ring buffer, not yet a fully "locked"
adaptive solution.

**The engine (`addon/manage.sh`) is currently DISABLED on the test device**
pending a fix for a confirmed boot-time race condition shared with
MockbaMod's own `mockbaMagic`/`MidiLoop` addons (see `DESIGN.md`'s "Boot-time
LD_PRELOAD race" section) - re-enabling it before that's fixed risks the
same intermittent dead-pads/dead-WiFi symptom on reboot. The web panel has
no such dependency and stays enabled.

## License

Inherits `schwung-maze`'s terms for `maze_voice.c`.
