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
mechanism, not maze-voice-specific — `injectTone.c` is a fixed-tone stand-in
producer used to prove the injection path works before wiring up a real DSP
engine.

## Layout

```
src/
  maze_voice.c         schwung-maze's DSP core, byte-for-byte verbatim
  maze_host.cpp         RtMidi in, timer-driven render, writes to the ring
  forceAudioIn.c         LD_PRELOAD tap: mixes the ring into MPC's capture reads
  injectTone.c           fixed-tone test producer (smoke-test only)
  forceAudioInject.h      shared-memory ring layout, used by all three above
  include/plugin_api_v1.h Schwung's plugin ABI (v1 host_api, v1/v2 plugin API)
  rtmidi/                 vendored RtMidi 6 (ALSA backend)
addon/                  MockbaMod addon: manage.sh, run_ForceMazeVoice.sh,
                        prebuilt forceAudioIn.so + maze_host, module.json,
                        web/ (bundled copy of the web GUI below),
                        Force Maze Control.xtk (Q-Link track template)
web/
  index.html            control panel - ported verbatim in style/layout from
                        schwung-maze's own web_ui.html (the "rack" look,
                        SVG knobs, section layout all unchanged); only the
                        transport (Move's schwungRemote/postMessage API) is
                        swapped for fetch() calls to server.py
  server.py             stdlib-only HTTP server bridging the page to
                        maze_host's Unix control socket (SET/GET/DESCRIBE/NOTE)
scripts/
  Dockerfile, build.sh          armhf-native (QEMU) build for maze_host,
                                same toolchain as force-acid
  build_audiotap.sh             zig cross-build for forceAudioIn.so/injectTone
                                (no Docker needed - see script header)
  build_xtk.py, xtk-seed.json    generates addon/Force Maze Control.xtk
docs/
  CC-MAP.md                      the 16 Q-Link knobs' CC assignments
  capture-xtk.md                 the .xtk format's reverse-engineering notes
```

## Build

```bash
./scripts/build.sh                       # maze_host, via Docker/QEMU armhf-native
ZIG=/path/to/zig ./scripts/build_audiotap.sh   # forceAudioIn.so + injectTone, via zig
```

Both write straight into `addon/`, which is then ready to deploy as-is.

## Deploy / enable

```
scp -r addon root@<force-ip>:/media/662522/AddOns/ForceMazeVoice
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/manage.sh ENABLE'
```

`ENABLE`/`DISABLE` restart the Force's `acvs` service (the main app) to
arm/disarm the `LD_PRELOAD` tap - see `DESIGN.md` for why, and for two
real-hardware incidents worth reading before touching this again.

Then: route a MIDI track to `Mockba Maze:In` for notes, and monitor/record
from whichever Audio-In track corresponds to the Force's `hw:2` capture
device (confirmed live - may enumerate differently if your USB device order
differs). Open `http://<force-ip>:8304` for the web control panel - the
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
`module.json`, styled and laid out exactly like the original Move version),
plus a Q-Link track template for the 16 most-used params on physical knobs
(structurally valid, not yet visually confirmed on a real screen - see
`docs/capture-xtk.md`). Audio timing needed real tuning (see `DESIGN.md`
for the full story) — currently a fixed clock-rate
correction plus a generous ~100/200ms ring buffer, not yet a fully "locked"
adaptive solution.

## License

Inherits `schwung-maze`'s terms for `maze_voice.c`.
