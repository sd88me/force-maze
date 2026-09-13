# Force Maze Sequencer

Port of `schwung-maze`'s **Maze** sequencer — a dual 8-step generative
sequencer (Moog Labyrinth style), originally an "overtake tool" for Ableton
Move — to the **Akai Force** running
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod). Sibling module to
[`../maze-voice`](../maze-voice/README.md) in this repo; see the top-level
[`../README.md`](../README.md) for how the two relate.

Unlike Maze Voice (a real audio DSP synth) or `force-acid` (a MIDI-FX that
processes an incoming stream), this is the simplest of the three ports: a
pure **MIDI generator** that steps entirely off incoming MIDI transport
bytes (clock/start/stop) and writes notes to its own output port — no audio,
no `tick()`/`render_block()` timer abstraction to reimplement at all.

## How the port works

| Layer | Move | Force |
|---|---|---|
| Sequencer | `dsp.so` (`maze_seq.c`) | `src/maze_seq_core.c` — **same file**, only the header swapped + a runtime-set state path (see its own file header) |
| Host | Schwung's overtake-tool manager | `src/host_shim.cpp` — standalone process, RtMidi in/out, no timer thread needed |
| Params | knobs → `set_param("42")` | MIDI **CC** on a control channel → rescale → `set_param`, or the web panel |
| Clock | Move's own transport, fed as MIDI bytes | Force transport: MIDI clock + Start/Stop into the virtual port, unchanged |
| Persistence | `/data/UserData/schwung/tool_state/maze_seq.bin` | `<addon dir>/maze_seq.bin` — same binary format, FORCE-ONLY path |
| Output channel | independent per sequencer, **unmodified from upstream** | same — `maze_seq.c` was never subject to Move's chain-host one-channel-per-slot restriction (see `src/maze_seq_core.c`'s header) |
| UI | pads + step buttons + knobs on the Move display | CC map ([`docs/CC-MAP.md`](docs/CC-MAP.md)); a Force track template ([`docs/capture-xtk.md`](docs/capture-xtk.md)); a browser panel (`web/`, ported from `schwung-maze`'s own `web_ui.html`) |

## Layout

```
src/
  maze_seq_core.c    schwung-maze's maze_seq.c, verbatim but for the include
                      line and a small FORCE-ONLY state-path change (see its
                      own file header for exactly what and why)
  maze_seq_host.h     the 2 host/plugin ABI symbols the core needs, re-declared
  host_shim.cpp       RtMidi virtual ports, CC->set_param, control socket, main()
  rtmidi/             vendored RtMidi 6 (ALSA backend), same copy as
                      force-acid/../maze-voice
addon/                MockbaMod addon: NSMODULE.json, manage.sh, run_maze_seq.sh,
                      Force Maze Seq Control.xtk (Q-Link track template),
                      web/ (bundled copy of the web panel below)
web/                  a SEPARATE addon (the web panel), independently enabled
  index.html          control panel, ported from schwung-maze's own web_ui.html
                      (rack look, SVG knobs, step-LED bits strips all unchanged);
                      only the transport (Move's schwungRemote/postMessage API)
                      is swapped for fetch() calls + a GET /state poll loop
  server.py           stdlib-only HTTP server bridging the page to
                      host_shim's Unix control socket (SET/GET), with a
                      state-JSON fallback for the many knob params
                      get_param() doesn't answer directly - see its own header
  manage.sh, run_maze_seq_web.sh   its own ENABLE/DISABLE, PID-file based
scripts/
  Dockerfile, build.sh          armhf-native (QEMU) build, writes straight
                                into addon/, same toolchain as force-acid
  build_xtk.py, xtk-seed.json    generates addon/Force Maze Seq Control.xtk
docs/
  CC-MAP.md                      the CC assignments (2 pages + the
                                  independent-channel feature)
  capture-xtk.md                 the .xtk format's reverse-engineering notes
tests/
  run.sh, test_smoke.c           native (non-ARM) smoke test: sequencing
                                  logic, independent per-seq output channel,
                                  and the state-persistence round-trip
nodeserver-integration/  patches for the SEPARATE nodeServer addon (home-page
                        link) - see its own README.md
```

## Build & deploy

```bash
./tests/run.sh                                          # logic check, no Docker

./scripts/build.sh                                       # -> addon/maze_seq_host  (needs Docker)
ssh root@<force-ip> 'rm -rf /media/662522/AddOns/ForceMazeSeq'   # scp -r nests otherwise, see ../DESIGN.md
scp -r addon root@<force-ip>:/media/662522/AddOns/ForceMazeSeq
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeSeq/manage.sh ENABLE'
ssh root@<force-ip> '/media/662522/AddOns/ForceMazeSeq/web/manage.sh ENABLE'   # the web panel
```

Then on the Force: Preferences → MIDI, enable **Clock** (not just Track) on
`Mockba Maze Seq In` — this is the one that's easy to miss and the engine
has no other way to know the transport is running (`maze_seq_core.c` only
sets its internal `running` flag on an actual `0xFA`/`0xFB` Start/Continue
byte; with Clock off it never arrives, no notes are ever generated, and the
web panel correctly shows "Stopped" even while the Force is visibly
playing). Also enable Track on `Mockba Maze Seq Out`; a MIDI track named
`MAZE SEQ CTRL` → `Mockba Maze Seq In` ch 1 for CC control (load
`Force Maze Seq Control.xtk` onto it for pre-named knobs); one or two
instrument tracks ← `Mockba Maze Seq Out`, on whichever channel(s)
`s1_channel`/`s2_channel` are set to (both default to 1); press Play.
Or skip the hardware knobs entirely and use the web panel at
`http://<force-ip>:8305`.

## Status

**Built for armhf and hardware-verified (2026-09-13).** Native logic smoke
test passes (`tests/run.sh`): sequencing, note on/off balance, independent
per-sequencer output channel, and the state-persistence round-trip (save →
reload → same channels/lengths) all verified on the host architecture.
Deployed to a live Force: both addons enable cleanly, `maze_seq_host`'s
virtual ALSA ports register, and the web panel's SET/GET round-trips
against the real running engine (confirmed live: `s2_channel`, `s1_length`).
Real transport clock confirmed too: both sequencers' play-heads observed
advancing live via `GET /state` once **Clock** was enabled on
`Mockba Maze Seq In` (see the deploy steps above — Track alone isn't
enough). Not yet confirmed on hardware: the `.xtk` template on a real
touchscreen — see `DESIGN.md`'s TODO list.

## License

Inherits `schwung-maze`'s terms for `maze_seq_core.c`.
