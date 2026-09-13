# The `.xtk` track template

## What it actually is

`.xtk` = a 5-line ASCII header, then a **gzip-compressed JSON document**:

```
ACVS
3.3.0.0
SerialisableTrackData
json
Linux
<gzip-compressed JSON follows>
```

Confirmed by pulling a real one off a live MockbaMod Force over SSH
(`Harpie4T/Harpie 4T Control.xtk`) and running it through `gunzip`/
`python3 -m json.tool` — see `force-acid/scripts/build_xtk.py`'s header
comment (or `../../maze-voice/docs/capture-xtk.md`) for the full
reverse-engineering story; the format and its unconfirmed details
(`momentary`, `paramType`, `instrumentIndex: 257`) are identical here.
`scripts/xtk-seed.json` in this module is the same seed file, copied
byte-for-byte — it's generic structural material (mixer defaults, empty pad
banks), not specific to any one module.

The relevant part for a control template is `data.program.customQLinks`: a
flat 16-entry array, one per Force Q-Link knob.

## How this module's template was built

`scripts/build_xtk.py` rewrites `customQLinks` to 16 of `docs/CC-MAP.md`'s
CCs (all of the "Seq Ctrl" and "Global" blocks except `pad_semis`, plus
`panic` — see that script's own `KNOBS` list) and the self-referential
template name fields, then re-emits the gzip+header framing.

```bash
python3 scripts/build_xtk.py                            # -> addon/Force Maze Seq Control.xtk
python3 scripts/build_xtk.py --track-name "MY TRACK"     # if you don't use "MAZE SEQ CTRL"
```

Regenerate it (same command) if `docs/CC-MAP.md`'s CC assignments ever
change — the two need to stay in sync by hand, same convention as
`host_shim.cpp`'s `PARAMS[]` table.

**`pad_semis` is deliberately left out** of the 16-knob template — it's the
least-used control of the set (a semitone offset that on Move came from the
pad keyboard, no direct Force equivalent) and something had to give to fit
one Q-Link bank. It's still reachable via plain MIDI-learn or the web panel.

## Status: NOT YET visually confirmed on a real screen

Structurally valid (round-trips through gzip/JSON, matches the real
reference file's shape) but not yet loaded onto a real Force MIDI track and
looked at — see `../../maze-voice/docs/capture-xtk.md`'s own status section,
same caveat applies here. Report back either way so this can be corrected
with a real answer instead of a guess.

## Fallback: skip the template

MIDI-learn works with no template at all — assign any Force track's knobs to
the CCs directly (`docs/CC-MAP.md`). The template is a convenience, not a
requirement.
