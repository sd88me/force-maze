# force-maze

Ports of [`schwung-maze`](https://github.com/sd88me/schwung-maze)'s modules —
originally native DSP/sequencer modules for Ableton Move — to the **Akai
Force** running [MockbaMod](https://github.com/MockbaTheBorg/MockbaMod).
Two independent addon modules live in this repo:

| Module | Directory | What it is | Output |
|---|---|---|---|
| **Maze Voice** | [`maze-voice/`](maze-voice/README.md) | Monophonic Moog Labyrinth-style thru-zero oscillator / wavefolder / state-variable filter voice | real audio, mixed into a Force Audio-In track |
| **Maze Sequencer** | [`maze-sequencer/`](maze-sequencer/README.md) | Dual 8-step generative sequencer (Moog Labyrinth style), clock-synced to the Force transport | MIDI notes on a virtual port |

Both follow the same porting pattern (see
`~/.claude/skills/mockbamod-module-creator/references/porting-schwung-modules.md`
if you have that skill installed, or `force-acid` for the original writeup):
the DSP/sequencer core ships **verbatim** from `schwung-maze`, and a small
**host shim** stands in for the Schwung chain host Move used to provide
(RtMidi ports, a timer or MIDI-clock-driven "tick", CC/socket control in
place of on-screen knobs). Each module is a fully independent MockbaMod
addon — install, enable, and use them separately.

## Layout

```
maze-voice/         Maze Voice: audio DSP synth. See maze-voice/README.md.
maze-sequencer/      Maze Sequencer: MIDI generator. See maze-sequencer/README.md.
```

Each module directory is self-contained: its own `src/`, `addon/`, `web/`,
`scripts/`, `docs/`, `README.md` and `DESIGN.md`.

## License

Inherits `schwung-maze`'s terms for the ported DSP/sequencer cores.
