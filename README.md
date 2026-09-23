# force-maze

Moog Labyrinth-inspired modules for the **Akai Force**, running
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod). These are ports of
[`schwung-maze`](https://github.com/sd88me/schwung-maze), the original set of
Maze modules built for the Schwung framework on Ableton Move.

This repo holds two independent MockbaMod addons:

| Module | Directory | What it is | Output |
|---|---|---|---|
| **Maze Voice** | [`maze-voice/`](maze-voice/README.md) | Monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder / state-variable filter synth voice | real audio, mixed into a Force Audio-In track |
| **Maze Sequencer** | [`maze-sequencer/`](maze-sequencer/README.md) | Dual 8-step generative sequencer (Moog Labyrinth style), clock-synced to the Force transport | MIDI notes on a virtual port |

Pair them for the full Labyrinth-style rig: Maze Sequencer generating notes
into Maze Voice, both driven from the Force's own transport.

## How it works

Each module ships as a fully independent MockbaMod addon (its own engine
process, control socket, web control panel, and on-device touchscreen page)
— install, enable, and use them separately, or together. The DSP/sequencer
core in each is ported **verbatim** from `schwung-maze`; a small **host
shim** process stands in for the Schwung chain host that Move provides
(virtual MIDI ports, a timer- or clock-driven "tick", and CC/socket control
in place of on-screen knobs).

## Layout

```
maze-voice/       Maze Voice: audio DSP synth. See maze-voice/README.md.
maze-sequencer/   Maze Sequencer: MIDI generator. See maze-sequencer/README.md.
```

Each module directory is self-contained: its own `src/`, `addon/`, `web/`,
`scripts/`, `docs/`, `README.md` and `DESIGN.md`.

## Requirements

- An Akai Force running [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).
- Maze Voice additionally requires the [`force-audio-jack`](https://github.com/sd88me/force-audio-jack)
  addon (a shared audio-injection tap) — see `maze-voice/README.md` for
  details. Maze Sequencer has no such dependency; it only needs virtual MIDI
  ports, which MockbaMod provides natively.

## Installation

```bash
scripts/deploy.sh root@<force-ip>            # deploys + enables both modules
scripts/deploy.sh root@<force-ip> voice      # ...or just one
scripts/deploy.sh root@<force-ip> sequencer
```

One command copies each module's built `addon/` onto the SD card and runs
its `manage.sh ENABLE` (engine and web panel both). Requires `./scripts/build.sh`
to have been run first in each module you're deploying (see each module's own
README for the build step) — see `maze-voice/README.md` / `maze-sequencer/README.md`
for what still needs doing by hand afterwards (enabling the separate
`force-audio-jack` dependency for Maze Voice, starting the engine from the
nodeServer Modules page, hard safety rules).

## Related projects & credits

- [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) ([mockbatheb.org](http://mockbatheb.org/)) —
  the addon firmware framework both modules run on.
- [`schwung-maze`](https://github.com/sd88me/schwung-maze) — the original
  Ableton Move modules this repo ports.

## License

MIT — see [LICENSE](LICENSE). Copyright © sd88me.
