# force-maze — repo-level design notes

This repo holds two independent `schwung-maze` module ports for the Akai
Force, each with its own full design writeup:

- [`maze-voice/DESIGN.md`](maze-voice/DESIGN.md) — the audio-DSP port (Maze
  Voice): the LD_PRELOAD/shared-memory audio injection mechanism, clock-rate
  correction, and its operational hard rules.
- [`maze-sequencer/DESIGN.md`](maze-sequencer/DESIGN.md) — the MIDI-generator
  port (Maze Sequencer): host-shim design, state persistence, CC map, and
  the independent per-sequencer output channel.

## Shared porting pattern

Both modules follow the same approach: the DSP/sequencer core ships
**verbatim** from `schwung-maze` (Schwung's `plugin_api_v1`/`v2` core files,
unmodified apart from clearly marked `FORCE-ONLY` changes), and a small
**host shim** process stands in for the Schwung chain host that Move
provides — virtual MIDI ports (via a vendored copy of RtMidi 6, ALSA
backend), a timer- or MIDI-clock-driven "tick", and CC/socket control in
place of on-screen knobs. Each module is a fully independent MockbaMod
addon: its own engine binary, control socket, web control panel
(`web/index.html` + `server.py`, both ported from the module's original
Move `web_ui.html`), and on-device touchscreen page
(`addon/shadow_page.conf`, for [force-shadow](https://github.com/sd88me/force-shadow)).

## Repo history

Both modules used to live as two separate top-level layouts before being
consolidated into `maze-voice/` and `maze-sequencer/` subfolders, so one repo
can hold every `schwung-maze` port. `git log --follow` on a file under
either subfolder still finds its pre-move history.
