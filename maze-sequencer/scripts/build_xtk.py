#!/usr/bin/env python3
"""Build addon/Force Maze Seq Control.xtk -- a Force track template that
pre-assigns 16 Q-Link knobs (Force's physical knob bank) to the CCs from
docs/CC-MAP.md, so loading it onto a MIDI track gets you named,
correctly-ranged knobs instead of hand MIDI-learning each one.

Same reverse-engineered .xtk format as force-acid/scripts/build_xtk.py (see
that script's own header comment for the full story and caveats) --
xtk-seed.json here is the identical seed file, copied verbatim: it's
structural/schema material (mixer defaults, empty pad banks), not anything
specific to any one module's generation logic, so it's shared byte-for-byte
across every module in this org that ships a track template.

CAVEAT: reverse-engineered from one sample file, NOT YET CONFIRMED to load
correctly in the Force's UI on a real screen -- see docs/capture-xtk.md.

Usage:
    python3 scripts/build_xtk.py [--track-name "MAZE SEQ CTRL"] [--out "addon/Force Maze Seq Control.xtk"]
"""
import argparse
import gzip
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEED_PATH = HERE / "xtk-seed.json"
HEADER = "ACVS\n3.3.0.0\nSerialisableTrackData\njson\nLinux\n"

# (key, label, cc, momentary) -- 16 of the ~17 CC-MAP.md entries for one
# Q-Link bank; pad_semis is left web/CC-only (see docs/CC-MAP.md).
KNOBS = [
    ("s1_corrupt",   "S1 CORRUPT",  20, False),
    ("s1_cv_range",  "S1 RANGE",    21, False),
    ("s1_length",    "S1 LENGTH",   22, False),
    ("trig_mix",     "TRIG MIX",    23, False),
    ("s2_corrupt",   "S2 CORRUPT",  24, False),
    ("s2_cv_range",  "S2 RANGE",    25, False),
    ("s2_length",    "S2 LENGTH",   26, False),
    ("g_reset",      "RESET BOTH",  27, False),
    ("scale",        "SCALE",       30, False),
    ("key",          "KEY",         31, False),
    ("note_rate",    "NOTE RATE",   32, False),
    ("note_length",  "NOTE LEN",    33, False),
    ("s1_channel",   "S1 CHANNEL",  34, False),
    ("s2_channel",   "S2 CHANNEL",  35, False),
    ("transpose",    "TRANSPOSE",   36, False),
    ("panic",        "PANIC",       38, True),
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
    ap.add_argument("--track-name", default="MAZE SEQ CTRL",
                     help='must match the MIDI track name exactly once loaded on the Force (default: "MAZE SEQ CTRL", matching README.md)')
    ap.add_argument("--out", default=str(HERE.parent / "addon" / "Force Maze Seq Control.xtk"))
    ap.add_argument("--template-name", default="Force Maze Seq Control")
    args = ap.parse_args()

    if not SEED_PATH.exists():
        sys.exit(f"seed file missing: {SEED_PATH}")

    doc = json.loads(SEED_PATH.read_text())

    program = doc["data"]["program"]
    program["customQLinks"] = [
        make_qlink(label, cc, args.track_name, momentary)
        for (_key, label, cc, momentary) in KNOBS
    ]

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


if __name__ == "__main__":
    main()
