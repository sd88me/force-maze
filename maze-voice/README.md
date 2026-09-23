# Maze Voice

A monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder /
state-variable-filter synth voice, ported from
[`schwung-maze`](https://github.com/sd88me/schwung-maze) (originally a
chainable sound-generator module for Ableton Move) to the **Akai Force**
running [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).

Unlike a MIDI-FX or MIDI-generator addon, Maze Voice is a real **audio DSP
synth**: it renders actual samples and mixes them into what the Force's own
app reads from its audio-in capture device, so the voice comes out on a
normal Audio-In track — play it from a MIDI track routed to Maze Voice's
virtual input port, and monitor/record it like any other audio source.

## Features

- Two low-harmonic oscillators — a sine **VCO** and a wide-range triangle
  **MOD VCO** — coupled by **thru-zero FM** (stays in tune at any depth) and
  **ring modulation**.
- Variable-tone **noise** generator, morphing dark to bright.
- Diode/transistor-style **wavefolder** with a **Bias** control for
  asymmetric folding (even vs odd harmonic emphasis).
- Two-pole **state-variable filter** morphing continuously from lowpass to
  bandpass, with a nonlinear resonance stage that saturates and blooms as it
  approaches self-oscillation rather than ringing cleanly, plus a **Filt
  Drive** stage into the filter input for extra grit.
- Three-way **ORDER** routing (`VCW>VCF`, Parallel, `VCF>VCW`) with a
  bipolar **Blend** crossfader between the wavefolder and filter paths.
- Two decay-only envelopes — EG1 (modulation) and EG2 (VCA amplitude) — with
  a musically weighted, exponential time response.
- **Two LFOs**, each with a choice of saw/triangle/sine/square/sample-and-hold
  shapes, free-running or MIDI-clock-synced rate (with optional
  retrigger-on-note), and bipolar depth to eight destinations: VCO pitch,
  Mod pitch, FM depth, filter cutoff, Env1 decay, Env2 decay, filter drive,
  fold amount, and fold bias.
- **External Voice mode** — an alternate output tap that sends the raw,
  post-wavefolder oscillator mix out ungated and unfiltered (no blend, no
  VCA envelope, no filter), for feeding an external analogue filter or amp
  chain instead of using the built-in filter/VCA path.
- Per-channel warm overdrive (VCO, Mod, and Noise each break up
  independently above unity), a Boss-pedal-style overdrive stage on the
  final output (drive → asymmetric soft clip → passive tone), oversampling
  and anti-aliasing on every nonlinear stage, per-voice drift and detune,
  and a low analogue noise floor.
- Full web control panel, a Q-Link track template for physical-knob control,
  and an on-device touchscreen page (see below).

## Using Maze Voice

Route a MIDI track to `Mockba Maze:In` for notes, and monitor/record from
the Force's Audio-In track that corresponds to the audio-injection tap's
capture device (see Requirements below). Control it from any of:

- **The web control panel** at `http://<force-ip>:8304` — every parameter,
  laid out like the original Move panel (Oscillators / Mixer / Wavefolder →
  Filter / Mod / LFO). The "Audition" strip at the bottom plays a note
  straight from the page, no MIDI keyboard needed to hear a change take
  effect.
- **Physical Q-Link knobs** — a MIDI track named `MAZE CTRL` routed to
  `Mockba Maze:In` channel 1, with `addon/Force Maze Control.xtk` loaded
  onto it, gives 16 pre-named knobs for the most commonly tweaked
  parameters. See `docs/CC-MAP.md` for the full assignment (16 of
  `maze_voice.c`'s ~60 parameters — the rest stay web/touchscreen-only).
- **The touchscreen page** (see below).

For an external filter/amp chain, switch **VOICE MODE** to External on the
Voice page (web panel or touchscreen) — the filter, blend, and VCA envelope
are bypassed and the raw oscillator/wavefolder mix is sent out instead.

## Requirements

- An Akai Force running [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).
- [`force-audio-jack`](https://github.com/sd88me/force-audio-jack) — a separate,
  shared MockbaMod addon that injects synthesized audio into what the
  Force's app reads from its audio-in capture device. Maze Voice depends on
  it rather than bundling its own copy, so several voice addons can share
  one tap. Enable it once; it is already bundled in the
  [`sd88me/MockbaMod`](https://github.com/sd88me/MockbaMod) fork at
  `SD/AddOns/ForceAudioJack`.
- For the touchscreen page: [force-shadow](https://github.com/sd88me/force-shadow)
  v1.0.0 or later.

## Installation

1. **Enable the shared audio tap** (once, even if another voice addon
   already needs it):
   ```
   ssh root@<force-ip> '/media/662522/AddOns/ForceAudioJack/manage.sh ENABLE'
   ```
2. **Deploy this addon** — one command does the scp and enables both the
   engine and the web panel:
   ```bash
   scripts/deploy.sh root@<force-ip>
   ```
   (Or, from the repo root, `scripts/deploy.sh root@<force-ip> voice` to
   deploy just this module alongside Maze Sequencer — see the top-level
   README.) This is equivalent to, and replaces, manually running:
   ```
   ssh root@<force-ip> 'rm -rf /media/662522/AddOns/ForceMazeVoice'
   scp -r addon root@<force-ip>:/media/662522/AddOns/ForceMazeVoice
   ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/manage.sh ENABLE'
   ssh root@<force-ip> '/media/662522/AddOns/ForceMazeVoice/web/manage.sh ENABLE'   # web panel
   ```
   The engine (`addon/manage.sh`) and the web panel (`addon/web/manage.sh`)
   are two **separate** addons — enabling one does not enable the other. The
   web panel has no dependency on the audio tap and can stay always-on even
   while the engine is stopped (every control just answers 503 until the
   engine's control socket exists).
3. **Start the engine** from the nodeServer Modules page (`/moduler`) — see
   the hard rule below for why this should not be enabled at boot.
4. Route a MIDI track to `Mockba Maze:In` for notes, and set up an Audio-In
   track to monitor the injected audio (confirmed live at `hw:2` — may
   enumerate differently if your USB device order differs).

## Hard rule: do not restart `acvs` while a voice is attached

Once Maze Voice has been started via the nodeServer toggle, **do not
restart the `acvs` service** (which also fully re-runs MockbaMod's boot
sequence) while it's running — stop the voice from the same toggle first.
Every tested case of restarting `acvs` with a voice attached has left
pads/buttons or WiFi unresponsive; there is currently no confirmed-safe way
to do this. A plain reboot or `acvs` restart with **no** voice attached is
safe and has been verified repeatedly. See `DESIGN.md`'s "Known
limitations" for the investigation behind this rule.

The engine's autoload is deliberately disabled at boot for this reason —
only the audio tap arms at boot, with zero voices attached; start
`maze_host` itself afterwards from the nodeServer Modules page.

## Touchscreen page

`addon/shadow_page.conf` defines this module's on-device control page for
[force-shadow](https://github.com/sd88me/force-shadow) (`SHIFT+SCENE-3`),
with three tabs: **VOICE** (oscillators, envelopes, mixer/tone), **WAVEFOLDER
/ FILTER**, and **MOD / RANDOM** (randomise controls plus both LFOs). It's
discovered automatically at force-shadow startup — no rebuild needed to
change it.

## Building from source

```bash
./scripts/build.sh   # maze_host, native armhf-under-QEMU Docker build
```

Writes straight into `addon/`, ready to deploy. `forceAudioJack.so` (the
shared audio tap) is built from its own separate
[`force-audio-jack`](https://github.com/sd88me/force-audio-jack) repo, not from
here.

## Project layout

```
src/
  maze_voice.c            schwung-maze's DSP core, ported verbatim
  maze_host.cpp            RtMidi in, timer-driven render, writes to the audio-tap's ring
  forceAudioInject.h        shared-memory ring layout, vendored from force-audio-jack (keep byte-for-byte identical)
  include/plugin_api_v1.h   Schwung's plugin ABI
  rtmidi/                   vendored RtMidi 6 (ALSA backend)
addon/                     the MockbaMod addon (engine): manage.sh, prebuilt
                           maze_host, module.json, NSMODULE.json, Force
                           Maze Control.xtk, web/ (bundled copy of the web panel)
web/                       the web control panel, a separate addon
  index.html                control panel UI
  server.py                 stdlib-only HTTP bridge to maze_host's control socket
  manage.sh, run_maze_web.sh
scripts/                   Dockerfile/build.sh (armhf build), build_xtk.py (generates the .xtk)
docs/
  CC-MAP.md                 the 16 Q-Link knobs' CC assignments
  capture-xtk.md             .xtk format notes
  V2-PLAN.md                design/status notes for the LFO & External Voice work
nodeserver-integration/    patches for the separate nodeServer addon (home-page link)
```

## Related projects & credits

- [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) ([mockbatheb.org](http://mockbatheb.org/)) —
  the addon firmware framework this runs on.
- [`schwung-maze`](https://github.com/sd88me/schwung-maze) — the original
  Ableton Move module this ports.

## License

MIT — see the [top-level LICENSE](../LICENSE). Copyright © sd88me.
