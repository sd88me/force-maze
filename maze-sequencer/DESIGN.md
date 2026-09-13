# Force Maze Sequencer — porting `schwung-maze`'s Maze to the Akai Force

Status: **native logic smoke test passing, armhf build and hardware
untested.** Follows the same host-shim porting pattern as `force-acid`
(MIDI-FX) and `../maze-voice` (audio DSP), documented once at
`~/.claude/skills/mockbamod-module-creator/references/porting-schwung-modules.md`
if that skill is installed.

## Goal

Run `schwung-maze`'s Maze sequencer — an "overtake tool" that generates two
independent 8-step MIDI sequences, not a chain-slot module or an audio
synth — standalone on a MockbaMod Force, with the same generative/
mutation/persistence behavior it had on Move.

## Why this port is simpler than the other two

`maze_seq.c`'s own file header spells out its threading/timing model
clearly, and the key fact that made this port simpler than expected: it is
a **"tool"** component (`plugin_api_v2`, `move_plugin_init_v2`), not a chain
`midi_fx` (like `schwung-acid`) or a chain sound-generator (like Maze
Voice). Concretely that means:

1. It calls **exactly one** host callback, ever: `host->midi_send_internal`
   (grep confirms it — nothing else in `host_api_v1_t` is touched). So the
   trimmed ABI header (`src/maze_seq_host.h`) only needs that one field,
   simpler than `force-acid/src/acid_core.h`'s `get_bpm`/`get_clock_status`
   pair.
2. It steps entirely off **raw MIDI transport bytes fed to its own
   `on_midi`** (`0xF8`/`0xFA`/`0xFB`/`0xFC`) — there is no `tick(frames,
   sample_rate, ...)` or `render_block(...)` abstraction standing in for an
   audio callback at all. So `host_shim.cpp` needs no timer thread: an
   RtMidi input callback forwarding bytes both ways, plus a control-socket
   thread, is the entire runtime.
3. It **already has independent per-sequencer output channels**
   (`s1_channel`/`s2_channel`) — see "The seq-B-channel feature" below.

## What ports unchanged

`src/maze_seq_core.c` is `schwung-maze/src/maze_seq/dsp/maze_seq.c`
**verbatim** apart from two small, clearly marked (`FORCE-ONLY`) changes:

1. The header include, swapped for `src/maze_seq_host.h`.
2. The state-file path (see "Persistence" below).

Everything else comes across with zero edits: both sequencers' step/CV
generation and corruption logic, scale quantization, the Reset-Both/
polymeter accounting, gate-off bookkeeping, and the entire `set_param`/
`get_param` string interface (including the remote-UI `state` JSON contract
- see below).

## The host gap, and how the shim fills it

| Schwung overtake-tool manager did | `host_shim.cpp` does |
|---|---|
| loads `dsp.so`, `move_plugin_init_v2(host)` | links `maze_seq_core.o`, calls it directly |
| `create_instance(dir, cfg)` at tool load | once, at startup, with `--module-dir` as `dir` |
| `on_midi(msg, len, source)` per incoming event | RtMidi input callback → `on_midi` (everything but a control-channel CC hit passes straight through — the core already knows what to do with `0xF8`/`0xFA`/`0xFB`/`0xFC` and ignores the rest) |
| `set_param(key, "42")` from a knob | CC on the control channel (`PARAMS[]` table) or a control-socket `SET` from the web panel |
| `get_param("state")` polled ~100ms while the Tool tab is open | the web panel's `GET /state`, polled client-side every ~200ms (no server push exists here — see "Web GUI" below) |
| `host->midi_send_internal(pkt, 4)` | writes to our own RtMidi `Out` port |
| everything on one audio thread | one `std::mutex` around every core call (input callback vs. control-socket thread) |

### Threading

Same spirit as the other two ports: **one mutex, every core call under it.**
Contention here is even lower than `force-acid`'s — a handful of MIDI bytes
plus occasional CC/socket calls, no periodic tick at all.

## The "seq B channel" feature — already there, nothing to patch

`force-acid`'s `a_channel`/`b_channel` needed a genuine core-logic addition
(`acid_core.c`'s `out_ch` field, marked `FORCE-ONLY`) because Move's chain
host rewrites the MIDI channel nibble to the slot's one receive channel on
the way out — `schwung-acid` had no way to address Seq A and Seq B on
different channels no matter what it did internally.

`maze_seq.c` never had that problem: as a "tool", it calls
`host->midi_send_internal` directly, and Move's overtake-tool manager
doesn't rewrite the channel byte the way the chain host does. So
`s1_channel`/`s2_channel` (`seq_t.channel`, `set_param` keys `s1_channel`/
`s2_channel`) were **already independent per sequencer in the upstream
module**, confirmed by reading `send_midi(L, 0x90|(q->channel&0x0F), ...)`
in `step_seq()` — each sequencer stamps its own channel on every note it
emits. Porting this feature to the Force meant making sure it's exposed
end-to-end (CC 34/35 in `docs/CC-MAP.md`, the web panel's Channel steppers,
the `.xtk` template), not adding it to the core.

## Persistence

`maze_seq.c` persists its full pattern/param state to a binary file via a
background `SCHED_OTHER` worker thread (the audio-thread realtime-safety
rules this comment block cites don't bind on the Force at all, since there
is no SPI callback here — but the design is harmless as-is, so it ports
unchanged). Two things needed care:

1. **The path.** Move hardcodes `/data/UserData/schwung/tool_state/
   maze_seq.bin`, which doesn't exist on the Force. `maze_create()` now
   builds the path as `"<module_dir>/maze_seq.bin"` at create-time (the
   shim always passes its own addon directory as `module_dir`), falling
   back to the original Move path only if `module_dir` is `NULL`. See
   `src/maze_seq_core.c`'s file header and the `FORCE-ONLY` comments around
   `ensure_state_dir()`/`maze_save_state_locked()`/`maze_load_state()`.
2. **Nothing marks state dirty except an explicit `set_param(inst, "save",
   "1")`** — on Move that presumably came from `ui.js`, which isn't ported
   here (see "Web GUI" below — the browser panel replaces it, not the
   on-device screen). So `host_shim.cpp` calls `set_param(inst, "save",
   "1")` immediately after every mutating `SET`, whether it came from a CC
   or the control socket. The core's own worker thread (unmodified, wakes
   every ~2s) picks up the dirty flag from there. Verified end-to-end in
   `tests/test_smoke.c`: set params → destroy (final synchronous save) →
   create a fresh instance in the same directory → confirm the same values
   read back out of `get_param("state")`.

## Web GUI

Per the porting guide's guidance ("reuse the module's own `web_ui.html`
almost verbatim"), `web/index.html` is `schwung-maze/src/maze_seq/web_ui.html`
with only the transport layer and the live-sync mechanism changed — every
knob/stepper/bits-strip/button function, the SVG knob generator, and the
CSS are untouched:

1. **Transport**: Move's `window.schwungRemote` (backed by a manager iframe
   and `postMessage` that don't exist standalone) is replaced with `fetch()`
   calls to `web/server.py`, same shape as `../maze-voice/web/index.html`'s
   equivalent swap.
2. **`get_param` has fewer keys here than Move's manager exposed.**
   `maze_seq_core.c`'s own `get_param()` only implements `running`/
   `module_id`/`state`/`s1_state`/`s2_state` — every knob param
   (`s1_corrupt`, `s2_channel`, `scale`, ...) is `set_param`-only, exactly
   as on Move. Move's manager apparently flattened `get_param("state")`'s
   JSON into individually-addressable params server-side (the page's own
   original comment about "overtake tools get a real adaptive server-side
   poll of `get_param("state")`" is the tell). `server.py`'s `/param` GET
   reproduces that: it tries a direct `GET <key>` first, and falls back to
   parsing the `state` JSON blob if the control socket answers `ERR`.
3. **Live sync has no server push on the Force** (Move's `onParamChange`
   was driven by the manager's own adaptive poll, pushed down to the page).
   `index.html` polls `GET /state` itself, every ~200ms, and fans the
   single JSON response out to every registry entry that owns a matching
   key — one HTTP request refreshes every knob, both bits strips, and the
   play-head/running indicator at once, rather than porting Move's
   per-key-`getParam`-in-a-loop `seed()` verbatim (which would have meant
   ~20 separate HTTP round-trips every poll tick for no benefit — the whole
   state fits in one small JSON object already, see
   `maze_seq_core.c`'s `build_state_json()`).

The step-LED "bits strips" are genuinely live and clickable, unlike a
synth's static knobs — this is the one part of the panel that actually
needed the poll loop above to be useful in practice (watching the play-head
advance while the sequencer runs is a real part of what makes this UI worth
having, more so than for Maze Voice's panel).

## Toolchain

Identical to `force-acid`/`../maze-voice`: native armhf-under-QEMU Docker
build, `arm32v7/debian:stretch` base (not `buster` — EOL apt repos). See
either sibling's `DESIGN.md` "Toolchain" section for the full story;
`scripts/Dockerfile` here is a byte-for-byte copy.

One build-flag wrinkle specific to this core: `maze_seq_core.c`'s
background save-worker thread calls `usleep()` (verbatim upstream code).
glibc only declares `usleep()` under `-std=c11` when a feature-test macro
enables XOPEN/BSD extensions, so both `scripts/build.sh` and `tests/run.sh`
compile the core with `-D_DEFAULT_SOURCE`. This is a compiler-flag fix, not
a source change — the core file itself is untouched for this issue.

## Dependencies

Same as the other two ports: vendored RtMidi 6 (`src/rtmidi/`, ALSA
backend) + system ALSA (`libasound.so.2`).

## TODO before calling it v1

- [ ] real armhf cross build (`scripts/build.sh` needs Docker, unavailable
      in the environment this port was written in — same limitation
      `force-acid`'s own `DESIGN.md` recorded initially)
- [ ] hardware smoke test: virtual ports register, Force clock drives
      stepping, notes land on the right channel per sequencer, web panel
      talks to a real `maze_seq_host`
- [ ] visually confirm `addon/Force Maze Seq Control.xtk` on a real screen
      (see `docs/capture-xtk.md`)
- [ ] parameter feedback (CC out, mirroring `force-acid`'s v0.2 channel-16
      approach) — not attempted here; the web panel's poll loop is the only
      live readback today
