# The `.xtk` track template

**Update:** the original version of this doc assumed `.xtk` was "an
undocumented Akai binary" that had to be captured by hand on real hardware.
That was wrong — it's inspectable and directly authorable. Kept below for
the real format, how it was reverse-engineered, and what's still unverified.

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
`python3 -m json.tool` — it decompresses cleanly to `{"data": {...}}`, a large
JSON tree (track/mixer/pad-bank state) whose relevant part for a control
template is `data.program.customQLinks`: a **flat 16-entry array**, one per
Force Q-Link knob (Force's physical knob bank — 16 knobs, not "3 pages of 8"
like Move's UI; the Move-era plan in this file's earlier draft assumed the
wrong shape). Each entry:

```json
{
  "name": "DENSITY A",
  "controlType": 0,
  "targetData": [{
    "version": 1,
    "parameter": 22,
    "track": "ACID CTRL",
    "insertParamIndex": {"initialized": false},
    "instrumentIndex": 257,
    "paramType": 1,
    "controlInputRange": {"min": 0.0, "max": 1.0, "stride": 0.0, "deadspot": 2.0, "skew": 1.0},
    "parameterRange":    {"min": 0.0, "max": 1.0, "stride": 0.0, "deadspot": 0.0, "skew": 1.0},
    "behaviour": 0
  }],
  "momentary": 0,
  "controlValue": 0.0
}
```

- `parameter` — the CC number.
- `track` — the **name** of the destination MIDI track. Binding is by
  string match against the track's name in the Force session, not by track
  index/ID — the loaded template only works if a track is named exactly
  this.
- `instrumentIndex: 257` — constant across every entry in the one real file
  inspected. Meaning unconfirmed; left untouched.
- `paramType` — `1` on every continuous-knob entry seen; `0` on one
  momentary-style entry in the reference file. Not confirmed whether that's
  meaningful or incidental (see below).
- `momentary` — `1` on some entries (not consistently on what looked like
  the obvious "trigger" CCs in the reference file). Set to `1` for our own
  Generate/Mutate knobs on the theory that's clearly the intent; unconfirmed
  against real hardware behavior.

## How our template was built

`scripts/build_xtk.py` + `scripts/xtk-seed.json` (the decompressed JSON body
of that real Harpie4T template, used purely as a structural skeleton —
mixer/pad-bank/sample boilerplate we don't understand or need to change is
left byte-identical, on the theory that a file structurally identical to one
a real Force is known to load is far more likely to also load than a
hand-built minimal skeleton). The script rewrites `customQLinks` to our own
16 knobs (Seq A + Seq B's `generate/mutate/density/accent/slide/octaves/
length/gate`, CC 20-27 and 40-47) and the self-referential template name
fields, and re-emits the gzip+header framing.

```bash
python3 scripts/build_xtk.py                        # -> addon/Force Acid Control.xtk
python3 scripts/build_xtk.py --track-name "MY TRACK" # if you don't use "ACID CTRL"
```

Regenerate it (same command) if `docs/CC-MAP.md`'s CC assignments ever
change — the two need to stay in sync by hand, same convention as
`host_shim.cpp`'s `PARAMS[]` table.

**Only Seq A + Seq B's 8 core knobs each are covered (16 total = one full
Q-Link bank).** Global (scale/root/blend/swing/...) and the newer Advanced
controls (channel/offset/dir/jitter/auto_gen) aren't in this template —
they're reachable via plain MIDI-learn on any Force track, or from the web
control panel (`web/`), which covers every CC. A second template for a
second Q-Link bank is possible later if that turns out to matter in
practice.

## Status: NOT YET visually confirmed on a real screen

The file is structurally valid (round-trips through gzip/JSON correctly,
matches the real reference file's shape) and has been copied onto the test
Force. What hasn't been confirmed: actually loading it onto a MIDI track
named `ACID CTRL` in the Force UI and looking at whether the 16 knobs show
up with the right names/ranges, and whether Generate/Mutate feel like
momentary triggers rather than sticky positions. That needs someone at the
touchscreen — SSH/text tooling can't drive it. Report back either way (works
as-is / knob names missing / momentary feels wrong / wouldn't load at all) so
this doc and the `momentary`/`paramType` open questions above can be
corrected with a real answer instead of a guess.

## Fallback: skip the template

MIDI-learn works with no template at all — assign any Force track's knobs to
the CCs directly. The template is a convenience, not a requirement.
