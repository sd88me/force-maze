# force-maze — repo-level design notes

This repo holds two independent `schwung-maze` module ports, each with its
own full design writeup:

- [`maze-voice/DESIGN.md`](maze-voice/DESIGN.md) — the audio-DSP port (Maze
  Voice): the LD_PRELOAD/shared-memory audio injection problem, timing/
  clock-rate tuning, the boot-time race with MockbaMod's own addons.
- [`maze-sequencer/DESIGN.md`](maze-sequencer/DESIGN.md) — the MIDI-generator
  port (Maze Sequencer): host-shim design, state persistence, CC map, and
  the independent per-sequencer output channel.

Repo history: both modules used to be two separate top-level layouts split
across commits before 2026-09-13, when they were consolidated into
`maze-voice/` and `maze-sequencer/` subfolders so one repo can hold every
`schwung-maze` port. `git log --follow` on a file under either subfolder
still finds its pre-move history.
