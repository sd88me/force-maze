# Force Maze Sequencer — CC map

Control channel: **1** by default (`--control-channel`, 1-16). All of
`maze_seq_core.c`'s params are plain integers (the core does `atoi()` on
every value), so every CC below rescales 0-127 linearly to `[lo,hi]`,
rounded — except `panic`, which is momentary (CC ≥ 64 fires, the release is
ignored).

Two contiguous blocks, matching `schwung-maze`'s own two on-device knob
pages (`help.json`: "Seq Ctrl T1" and "Global T2") — CC 28-29 left as a gap
between them for future growth, same convention as `force-acid`'s CC-MAP.md.

| Page | CC | Param | Wire range → value | Notes |
|---|---|---|---|---|
| **Seq Ctrl** | 20 | `s1_corrupt` | 0–127 → 0–100 | chance per step of a random bit/CV mutation |
| | 21 | `s1_cv_range` | 0–127 → 0–100 | how far a step's random CV can spread the pitch |
| | 22 | `s1_length` | 0–127 → 1–8 | takes effect immediately |
| | 23 | `trig_mix` | 0–127 → −63…+64 | velocity crossfade between Seq1/Seq2 triggers; shared, not per-seq |
| | 24 | `s2_corrupt` | 0–127 → 0–100 | |
| | 25 | `s2_cv_range` | 0–127 → 0–100 | |
| | 26 | `s2_length` | 0–127 → 1–8 | |
| | 27 | `g_reset` | 0–127 → 0–4 | Reset Both: snap both play-heads to step 1 every 1/2/4/8 bars, or Off |
| **Global** | 30 | `scale` | 0–127 → 0–11 | 12 options, see below |
| | 31 | `key` | 0–127 → 0–11 | C … B |
| | 32 | `note_rate` | 0–127 → 0–5 | 1/32 … 1 bar, 6 options |
| | 33 | `note_length` | 0–127 → 0–7 | gate length, 8 options |
| | 34 | `s1_channel` | 0–127 → 1–16 | **independent per-sequencer output channel** — see below |
| | 35 | `s2_channel` | 0–127 → 1–16 | **independent per-sequencer output channel** — see below |
| | 36 | `transpose` | 0–127 → −48…+48 | semitones, shared across both sequencers |
| | 37 | `pad_semis` | 0–127 → −60…+60 | semitone offset from the (nonexistent-on-Force) pad keyboard; web/MIDI-learn only, not in the `.xtk` template |
| | 38 | `panic` | ≥64 fires | momentary; all notes off |

## Independent per-sequencer output channel

`s1_channel`/`s2_channel` let each sequencer address a different Force
instrument track independently — this was **already present, unmodified, in
the upstream Move module**: `maze_seq.c` is a "tool" component that calls
`host->midi_send_internal` directly, never through Move's chain host, so it
was never subject to the one-channel-per-slot restriction that made
`schwung-acid`'s independent `a_channel`/`b_channel` a Force-only patch (see
`force-acid/docs/CC-MAP.md`'s equivalent section, and `../src/maze_seq_core.c`'s
file header). Nothing in the core needed changing for this port; CC
34/35 above and the web panel's Channel steppers just expose what was
already there end to end.

Defaults (before any CC arrives, and before any saved state loads) are
channel 1 for both sequencers (`s1_channel`/`s2_channel` = 0 internally).

## Enum landing points

For the enum params the wire value is quantised to the nearest option
index, so the useful CC values are:

- **scale** (12): 0, 12, 23, 35, 46, 58, 69, 81, 92, 104, 115, 127 —
  Chromatic, Major, Minor, Pent Maj, Pent Min, Mel Min, Harm Min, Whole,
  Hirajoshi, Major 7, Minor 7, Unquant
- **key** (12): 0, 12, 23, 35, 46, 58, 69, 81, 92, 104, 115, 127 — C … B
- **note_rate** (6): 0, 25, 51, 76, 102, 127 — 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar
- **note_length** (8): 0, 18, 36, 54, 73, 91, 109, 127 — 1/4 … 2 (gate steps)
- **g_reset** (5): 0, 32, 64, 95, 127 — 1b, 2b, 4b, 8b, Off (default)

## Not CC-mapped — control-socket/web panel only

Step-level operations are step-button-shaped, not knob-shaped, so they're
reachable only from `web/` (or a raw `SET`/`GET` on the control socket, see
`../src/host_shim.cpp`'s header):

- `s1_flip`/`s2_flip` (0–7) — flip one step's on/off bit
- `s1_adv`/`s2_adv` (±1) — nudge that sequencer's play-head
- `s1_len_dec`/`s2_len_dec` — decrement length by one (wraps 1→8)
- `suspend` — pause without resetting state

## Not mapped yet

- parameter feedback (surface ← generator): no CC-out yet, unlike
  `force-acid`'s v0.2 feedback channel. The web panel's `GET /state` poll
  is the only live readback today.
- preset save/recall beyond the single auto-persisted pattern
  (`maze_seq.bin` next to the addon binary): not in this port.
