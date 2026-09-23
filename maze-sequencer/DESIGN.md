# Maze Sequencer — design notes

## Why this exists

`schwung-maze`'s Maze sequencer is an "overtake tool" that generates two
independent 8-step MIDI sequences on Ableton Move — not a chain-slot module
and not an audio synth. Porting it to the Force means giving it a
standalone process that can register virtual MIDI ports, follow the Force's
own transport clock, and expose the same generative/mutation/persistence
behaviour it had on Move.

## Why this port is simpler than Maze Voice

`maze_seq.c` is a **"tool"** component (`plugin_api_v2`,
`move_plugin_init_v2`), not a chain MIDI-FX or a chain sound generator.
Concretely:

1. It calls **exactly one** host callback, ever: `host->midi_send_internal`.
   The trimmed ABI header (`src/maze_seq_host.h`) only needs that one field.
2. It steps entirely off **raw MIDI transport bytes fed to its own
   `on_midi`** (`0xF8`/`0xFA`/`0xFB`/`0xFC`) — there is no `tick()` or
   `render_block()` abstraction standing in for an audio callback at all, so
   `host_shim.cpp` needs no timer thread: an RtMidi input callback
   forwarding bytes both ways, plus a control-socket thread, is the entire
   runtime.
3. It already has **independent per-sequencer output channels**
   (`s1_channel`/`s2_channel`) — see below.

## What ports unchanged

`src/maze_seq_core.c` is `schwung-maze`'s `maze_seq.c` **verbatim** apart
from two small, clearly marked (`FORCE-ONLY`) changes: the header include
(swapped for `src/maze_seq_host.h`), and the state-file path (see
Persistence below). Everything else — both sequencers' step/CV generation
and corruption logic, scale quantisation, the Reset-Both/polymeter
accounting, gate-off bookkeeping, and the entire `set_param`/`get_param`
string interface — comes across with zero edits.

## The host gap, and how the shim fills it

| Schwung overtake-tool manager did | `host_shim.cpp` does |
|---|---|
| loads `dsp.so`, `move_plugin_init_v2(host)` | links `maze_seq_core.o`, calls it directly |
| `create_instance(dir, cfg)` at tool load | once, at startup, with `--module-dir` as `dir` |
| `on_midi(msg, len, source)` per incoming event | RtMidi input callback → `on_midi` (everything but a control-channel CC hit passes straight through) |
| `set_param(key, "42")` from a knob | CC on the control channel, or a control-socket `SET` from the web panel |
| `get_param("state")` polled while the Tool tab is open | the web panel's `GET /state`, polled client-side every ~200ms |
| `host->midi_send_internal(pkt, 4)` | writes to our own RtMidi `Out` port |
| everything on one audio thread | one mutex around every core call (input callback vs. control-socket thread) |

### Threading

One mutex, every core call under it. Contention is low: a handful of MIDI
bytes plus occasional CC/socket calls, no periodic tick at all.

## Independent per-sequencer output channel

`force-acid`'s equivalent feature needed a genuine core-logic addition,
because Move's chain host rewrites the MIDI channel nibble to the slot's one
receive channel on the way out — a chain module has no way to address two
sub-sequences on different channels no matter what it does internally.

`maze_seq.c` never had that problem: as a "tool", it calls
`host->midi_send_internal` directly, and Move's overtake-tool manager
doesn't rewrite the channel byte the way the chain host does. So
`s1_channel`/`s2_channel` were **already independent per sequencer in the
upstream module** — each sequencer stamps its own channel on every note it
emits (`send_midi(L, 0x90|(q->channel&0x0F), ...)` in `step_seq()`). Porting
this to the Force meant exposing it end to end (CC 34/35, the web panel's
Channel steppers, the `.xtk` template), not adding it to the core.

## LFOs

Ports the same LFO design used in `../maze-voice` (see its DESIGN.md for the
shared rationale) into `maze_seq_core.c`: two LFOs, each modulating up to
eight destinations — `corrupt1`, `range1`, `length1`, `trig_mix`,
`corrupt2`, `range2`, `length2`, and `note_length`. Since there is no
per-sample audio path here, LFOs advance once per incoming MIDI clock pulse
(24 ppqn) rather than per audio block; sync-mode tempo is estimated from the
wall-clock gap between clock pulses (clamped 20–300 BPM), defaulting to 120
BPM with no clock present. Offsets are applied at point of use in
`step_seq()`/`trig_velocities()` — not baked into the stored base
values — so LFO depth can be changed live without disturbing the underlying
pattern. Ranges: ±50 for corrupt/range/trig-mix-style destinations, ±3.5 for
length/note-length destinations (rounded to the nearest integer step).

## Persistence

`maze_seq.c` persists its full pattern/parameter state to a binary file via
a background worker thread (unmodified upstream code; its audio-thread
realtime-safety comments don't bind here since there's no SPI callback, but
the design is harmless as-is). Two things needed care:

1. **The path.** Move hardcodes `/data/UserData/schwung/tool_state/
   maze_seq.bin`, which doesn't exist on the Force. `maze_create()` now
   builds the path as `"<module_dir>/maze_seq.bin"` at create time (the
   shim always passes its own addon directory as `module_dir`), falling
   back to the original Move path only if `module_dir` is `NULL`.
2. **Nothing marks state dirty except an explicit `set_param(inst, "save",
   "1")`** — on Move that came from `ui.js`, which isn't ported here (the
   browser panel replaces it). So `host_shim.cpp` calls `set_param(inst,
   "save", "1")` immediately after every mutating `SET`, whether it came
   from a CC or the control socket; the core's own worker thread
   (unmodified, wakes every ~2s) picks up the dirty flag from there.
   Verified end to end in `tests/test_smoke.c`: set parameters → destroy
   (final synchronous save) → create a fresh instance in the same
   directory → confirm the same values read back.

## Web GUI

`web/index.html` is `schwung-maze`'s own `web_ui.html` with only the
transport layer and the live-sync mechanism changed — every knob/stepper/
bits-strip/button function, the SVG knob generator, and the CSS are
untouched.

1. **Transport**: Move's `window.schwungRemote` (backed by a manager iframe
   that doesn't exist standalone) is replaced with `fetch()` calls to
   `web/server.py`, the same shape as Maze Voice's equivalent swap.
2. **`get_param` has fewer keys here than Move's manager exposed.**
   `maze_seq_core.c`'s own `get_param()` only implements `running`/
   `module_id`/`state`/`s1_state`/`s2_state` — every knob parameter is
   `set_param`-only, exactly as on Move. `server.py`'s `/param` GET
   reproduces Move's manager-side flattening: it tries a direct `GET <key>`
   first, and falls back to parsing the `state` JSON blob if the control
   socket answers `ERR`.
3. **Live sync has no server push on the Force.** `index.html` polls `GET
   /state` itself, every ~200ms, and fans the single JSON response out to
   every registry entry that owns a matching key — one HTTP request
   refreshes every knob, both bits strips, and the play-head/running
   indicator at once, rather than one HTTP round-trip per parameter.

The step-LED "bits strips" are genuinely live and clickable, unlike a
synth's static knobs — this is the part of the panel the poll loop matters
most for, since watching the play-head advance while the sequencer runs is
a real part of what makes this UI useful.

## Toolchain

Identical to Maze Voice: native armhf-under-QEMU Docker build,
`arm32v7/debian:stretch` base (not `buster`, which has EOL apt issues). See
`../maze-voice/DESIGN.md`'s Toolchain section for the full story.

One build-flag wrinkle specific to this core: the background save-worker
thread calls `usleep()` (verbatim upstream code). glibc only declares
`usleep()` under `-std=c11` when a feature-test macro enables XOPEN/BSD
extensions, so both `scripts/build.sh` and `tests/run.sh` compile the core
with `-D_DEFAULT_SOURCE` — a compiler-flag fix, not a source change.

## Dependencies

Vendored RtMidi 6 (`src/rtmidi/`, ALSA backend) + system ALSA
(`libasound.so.2`). No other addon dependency.

## Known limitations

- **The `.xtk` Q-Link track template has not been visually confirmed on a
  real touchscreen.** `addon/Force Maze Seq Control.xtk` is
  reverse-engineered from one sample file (see `docs/capture-xtk.md`) —
  structurally valid but not yet checked on a real screen.
- **No parameter feedback (surface ← generator).** There is no CC-out path
  mirroring changes back to a controller's LEDs; the web panel's `GET
  /state` poll is the only live readback today.
- **No preset save/recall beyond the single auto-persisted pattern**
  (`maze_seq.bin` next to the addon binary).
- **LFO sync tempo is only meaningful if MIDI clock actually reaches**
  `Mockba Maze Seq In`; otherwise LFOs free-run at a fixed 120 BPM assumption.

## Related projects

- Sibling module in this repo: [`../maze-voice`](../maze-voice/DESIGN.md) —
  shares the same host-shim porting pattern and LFO design, and is a
  natural pairing (this module's MIDI output driving that module's input).
