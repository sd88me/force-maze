# Maze Sequencer

A dual 8-step generative sequencer (Moog Labyrinth style), ported from
[`schwung-maze`](https://github.com/sd88me/schwung-maze) (originally an
"overtake tool" for Ableton Move) to the **Akai Force** running
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod). Sibling module to
[`../maze-voice`](../maze-voice/README.md) in this repo — pair them for a
full generative-sequencer + synth-voice rig, or drive any other instrument
track with Maze Sequencer's MIDI output.

Maze Sequencer is a pure **MIDI generator**: it steps entirely off incoming
MIDI transport bytes (clock/start/stop) from the Force and writes notes to
its own virtual output port — no audio synthesis, just sequencing.

## Features

- **Dual 8-step generative sequencers**, each with per-step random CV,
  quantised to a scale.
- **Corrupt (0–100)** — mutates stored voltages; past the midpoint also
  flips bits, producing evolving patterns.
- **CV Range (0–100)** — bipolar pitch spread around the root.
- **Trig Mix** — velocity crossfade between Seq 1 and Seq 2.
- **Length**, **bit flip**, and **advance** per sequencer.
- **Sequence Reset** — snap a sequencer's play-head back to step 1 every
  1/2/4/8 bars (or off), bar-aligned at any note rate; a **Reset Both**
  control resets both together.
- **12 scales**, selectable key, note rate (1/32 … 1 bar), and gate length.
- **Independent output channel per sequencer** — route Seq 1 and Seq 2 to
  different instrument tracks.
- **Two LFOs**, each with a choice of saw/triangle/sine/square/
  sample-and-hold shapes and a free-running or MIDI-clock-synced rate, with
  bipolar depth to eight destinations: Seq 1 corrupt, range, and length,
  Seq 2 corrupt, range, and length, trig mix, and note length. Since there's
  no per-sample audio path here, LFOs advance once per incoming MIDI clock
  pulse (24 ppqn); sync-mode tempo is estimated from the wall-clock gap
  between pulses.
- **Clock-synced** to the Force transport (24 ppqn, start/stop/continue).
- **Pattern and parameter persistence** — survives a restart.
- Full web control panel with a live-tracking step display, a Q-Link track
  template for physical-knob control, and an on-device touchscreen page.

## Using Maze Sequencer

In Preferences → MIDI, enable **Clock** (not just Track) on `Mockba Maze Seq
In` — see the hard rule below, this is easy to miss. Also enable Track on
`Mockba Maze Seq Out`. Route:

- A MIDI track named `MAZE SEQ CTRL` → `Mockba Maze Seq In` channel 1, with
  `Force Maze Seq Control.xtk` loaded onto it, for physical-knob control.
- One or two instrument tracks ← `Mockba Maze Seq Out`, on whichever
  channel(s) `s1_channel`/`s2_channel` are set to (both default to channel
  1 — change one of them if you want the two sequencers driving different
  instruments).

Press Play. Or skip the hardware knobs entirely and use the web panel at
`http://<force-ip>:8305`, which mirrors every control and shows a
genuinely live-updating step display and play-head.

## Hard rule: enable Clock, not just Track, on the input port

The engine only starts sequencing on a real MIDI Start/Continue byte
(`0xFA`/`0xFB`). With only **Track** enabled on `Mockba Maze Seq In` (not
**Clock**), that byte never arrives — no notes are ever generated, and the
web panel correctly shows "Stopped" even while the Force is visibly
playing. This is the single most common setup mistake with this module.

## Requirements

- An Akai Force running [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).
- No other addon dependency — Maze Sequencer only needs virtual MIDI ports,
  which MockbaMod provides natively.
- For the touchscreen page: [force-shadow](https://github.com/sd88me/force-shadow).

## Installation

```bash
./scripts/build.sh                # -> addon/maze_seq_host
scripts/deploy.sh root@<force-ip> # scp's addon/, enables engine + web panel
```

(Or, from the repo root, `scripts/deploy.sh root@<force-ip> sequencer` to
deploy just this module alongside Maze Voice — see the top-level README.)
This replaces manually running:

```bash
ssh root@<force-ip> 'rm -rf /media/662522/AddOns/ForceMazeSeq'    # scp -r nests otherwise
scp -r addon root@<force-ip>:/media/662522/AddOns/ForceMazeSeq
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeSeq/manage.sh ENABLE'
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeSeq/web/manage.sh ENABLE'   # web panel
```

The engine and the web panel are two separate addons — enabling one does
not enable the other. Unlike Maze Voice, the engine here auto-launches at
boot (no `LD_PRELOAD` involvement), so `deploy.sh` enabling it is safe to
run unattended.

## Building from source / testing

```bash
./tests/run.sh          # native logic smoke test, no Docker: sequencing,
                         # note on/off balance, independent per-sequencer
                         # output channel, state-persistence round-trip
./scripts/build.sh       # armhf-native (QEMU) build -> addon/maze_seq_host
```

## Touchscreen GUI (shadow mode)

A full editor page for the Force's own touchscreen, rendered by
[`force-shadow`](https://github.com/sd88me/force-shadow): open it with
`SHIFT+SCENE-4`, in the same palette as the web panel.

| Tab | Contents |
|-----|----------|
| SEQUENCERS | Sequencer A / B stacked on the left two-thirds (8 tappable step LEDs per sequencer: tap to flip, a white halo marks the play head; plus corrupt, CV range, length, channel, and an Advance button); TIMING / MIX on the right third (note rate, note length, trig mix, reset both) |
| GLOBAL | Scale and key pickers, transpose, pad transpose, panic |
| LFO | Two side-by-side panels, one per LFO, matching Maze Voice's own LFO page layout |

## Project layout

```
src/
  maze_seq_core.c     schwung-maze's maze_seq.c, verbatim but for the
                      include line and a small FORCE-ONLY state-path change
  maze_seq_host.h     the host/plugin ABI symbols the core needs, re-declared
  host_shim.cpp       RtMidi virtual ports, CC->set_param, control socket, main()
  rtmidi/             vendored RtMidi 6 (ALSA backend)
addon/                the MockbaMod addon: manage.sh, run_maze_seq.sh,
                      Force Maze Seq Control.xtk, shadow_page.conf,
                      web/ (bundled copy of the web panel)
web/                  the web control panel, a separate addon
  index.html           control panel UI, live step/play-head display
  server.py            stdlib-only HTTP bridge to host_shim's control socket
  manage.sh, run_maze_seq_web.sh
scripts/              Dockerfile/build.sh (armhf build), build_xtk.py (generates the .xtk)
docs/
  CC-MAP.md            the CC assignments
  capture-xtk.md        .xtk format notes
tests/
  run.sh, test_smoke.c  native (non-ARM) smoke test
nodeserver-integration/  patches for the separate nodeServer addon (home-page link)
```

## Related projects & credits

- [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) ([mockbatheb.org](http://mockbatheb.org/)) —
  the addon firmware framework this runs on.
- [`schwung-maze`](https://github.com/sd88me/schwung-maze) — the original
  Ableton Move module this ports.
- Sibling module: [`../maze-voice`](../maze-voice/README.md) — a matching
  monosynth voice this sequencer can drive.

## License

MIT — see the [top-level LICENSE](../LICENSE). Copyright © sd88me.
