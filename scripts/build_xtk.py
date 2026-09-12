#!/usr/bin/env python3
"""Build addon/Force Maze Control.xtk -- a Force track template that
pre-assigns 16 Q-Link knobs (Force's physical knob bank) to maze_host's CC
map (src/maze_host.cpp's PARAMS[] table), so loading it onto a MIDI track
gets you named, correctly-ranged knobs instead of hand MIDI-learning each
one. This is the Force-side half of "headless" control (this project's own
term - see README.md/NSMODULE.json - for an engine with no on-screen GUI of
its own, driven instead by CC from a Force MIDI track); the web panel
(web/index.html) covers every chain_param, this template covers the 16 of
them worth a physical knob.

Format background (reverse-engineered, not documented by Akai/InMusic - see
docs/capture-xtk.md and force-acid/scripts/build_xtk.py, which this is
adapted from): a .xtk is

    <5-line ASCII header>\n<gzip-compressed JSON>

    ACVS
    3.3.0.0
    SerialisableTrackData
    json
    Linux

scripts/xtk-seed.json is the JSON body of a real captured template
(Harpie4T's own control track, pulled from a live MockbaMod Force) -
generic Force/mixer/pad-bank boilerplate, not specific to Harpie4T's or
force-acid's own logic, so it's reused byte-for-byte here except for
`data.program.customQLinks` (rebuilt below) and self-referential name
fields.

CAVEAT, same as force-acid's: NOT YET CONFIRMED to load correctly in the
Force's UI (nobody has clicked through and looked at it on a real screen).
Load it once and check: knob names show up, ranges look right, Generate
feels like a trigger not a sticky value. See docs/capture-xtk.md.

Usage:
    python3 scripts/build_xtk.py [--track-name "MAZE CTRL"] [--out "addon/Force Maze Control.xtk"]
"""
import argparse
import gzip
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEED_PATH = HERE / "xtk-seed.json"
HEADER = "ACVS\n3.3.0.0\nSerialisableTrackData\njson\nLinux\n"

# (key, label, cc, momentary) - must match src/maze_host.cpp's PARAMS[] table
# exactly (key names, CC numbers) or the knob will move but nothing will
# happen on the device.
KNOBS = [
    ("vco_tune",    "VCO TUNE",   20, False),
    ("mod_freq",    "MOD FREQ",   21, False),
    ("fm_depth",    "FM DEPTH",   22, False),
    ("vco_eg1",     "VCO EG1",    23, False),
    ("mod_eg1",     "MOD EG1",    24, False),
    ("env1_decay",  "EG1 DECAY",  25, False),
    ("env2_decay",  "EG2 DECAY",  26, False),
    ("fold_drive",  "FOLD DRIVE", 27, False),
    ("fold_bias",   "FOLD BIAS",  28, False),
    ("blend",       "BLEND",      29, False),
    ("cutoff",      "CUTOFF",     30, False),
    ("reso",        "RESONANCE",  31, False),
    ("filter_mode", "FILT MODE",  32, False),
    ("vco_lvl",     "VCO LVL",    33, False),
    ("level",       "VOLUME",     34, False),
    ("rnd_go",      "GENERATE",   35, True),
]
assert len(KNOBS) == 16, "one Q-Link bank is 16 knobs -- trim/extend deliberately"

FULL_RANGE = {"min": 0.0, "max": 1.0, "stride": 0.0, "deadspot": 0.0, "skew": 1.0}
INPUT_RANGE = {"min": 0.0, "max": 1.0, "stride": 0.0, "deadspot": 2.0, "skew": 1.0}


def make_qlink(label, cc, track_name, momentary):
    return {
        "name": label,
        "controlType": 0,
        "targetData": [{
            "version": 1,
            "parameter": cc,
            "track": track_name,
            "insertParamIndex": {"initialized": False},
            "instrumentIndex": 257,
            "paramType": 1,
            "controlInputRange": dict(INPUT_RANGE),
            "parameterRange": dict(FULL_RANGE),
            "behaviour": 0,
        }],
        "momentary": 1 if momentary else 0,
        "controlValue": 0.0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track-name", default="MAZE CTRL",
                     help='must match the MIDI track name exactly once loaded on the Force, and '
                          'src/maze_host.cpp\'s --control-channel must match that track\'s output '
                          'MIDI channel (default: "MAZE CTRL")')
    ap.add_argument("--out", default=str(HERE.parent / "addon" / "Force Maze Control.xtk"))
    ap.add_argument("--template-name", default="Force Maze Control")
    args = ap.parse_args()

    if not SEED_PATH.exists():
        sys.exit(f"seed file missing: {SEED_PATH}")

    doc = json.loads(SEED_PATH.read_text())

    program = doc["data"]["program"]
    program["customQLinks"] = [
        make_qlink(label, cc, args.track_name, momentary)
        for (_key, label, cc, momentary) in KNOBS
    ]

    # Self-referential name fields -- everywhere the seed said "Harpie 4T
    # Control" (its own template name), swap in ours. Leave targetData's
    # "track" fields alone -- those were just set above and mean something
    # different (the *destination* MIDI track name).
    def rename(obj):
        if isinstance(obj, dict):
            for k, v in obj.items():
                if k == "name" and isinstance(v, str) and v.startswith("Harpie 4T Control"):
                    obj[k] = v.replace("Harpie 4T Control", args.template_name)
                else:
                    rename(v)
        elif isinstance(obj, list):
            for item in obj:
                rename(item)

    rename(doc)

    body = HEADER + json.dumps(doc, indent=4)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.GzipFile(out_path, "wb", mtime=0) as f:
        f.write(body.encode("utf-8"))

    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    print(f"Q-Link bank: {len(program['customQLinks'])} knobs, track name '{args.track_name}'")
    print("Load it on the Force onto a MIDI track literally named "
          f"'{args.track_name}' -- the CC targets are bound by track NAME, not by track index.")
    print(f"maze_host must be started with --control-channel matching that track's output channel "
          "(default 1) for any of this to actually reach maze_voice.c.")


if __name__ == "__main__":
    main()
