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

LEFTOVER-DATA FIX (ported from force-acid's own build_xtk.py, found there by
on-device inspection): the shared seed's `customQLinks` was always fully
overwritten, but `data.program.customisable.mapping` (127 entries) was not
-- it's a leftover automation-parameter-name catalog from a *different*
addon's own generator engine (its `name` fields read like "1 Mode 0-25 (1
Cluster 0-59)"), and `midiInputRoute`/`midiOutputRoute` pointed at
"Mockba Harpie 4T" / referenced that same donor addon's own ALSA client,
not this project's ("Mockba Maze", see src/maze_host.cpp's --client
default). `blank_mapping()`/`fix_midi_routes()` below scrub both; `audit()`
refuses to build if any leftover donor-addon string survives.

Usage:
    python3 scripts/build_xtk.py                            # -> addon/Force Maze Control.xtk (+ .xtk.json)
    python3 scripts/build_xtk.py --track-name "MY TRACK"     # if you don't use "MAZE CTRL"
    python3 scripts/build_xtk.py --pack path/to/edited.json --out "addon/Force Maze Control.xtk"
        # skip seed/KNOBS generation entirely -- just re-pack an already-assembled
        # (and possibly hand-edited) JSON dump back into .xtk framing.
"""
import argparse
import gzip
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SEED_PATH = HERE / "xtk-seed.json"
HEADER = "ACVS\n3.3.0.0\nSerialisableTrackData\njson\nLinux\n"

# Sentinel automationIndex the file's own schema already uses for "this slot
# has no automation target" -- see LEFTOVER-DATA FIX above.
UNUSED_AUTOMATION_INDEX = 2147483647

# Strings that must never survive into the built .xtk -- anything from a
# donor addon's own captured state that isn't ours. Checked by audit() after
# every build so a future seed swap/re-capture can't silently reintroduce
# this same leak.
FORBIDDEN_SUBSTRINGS = ["RiffMaker", "Harpie"]

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


def blank_mapping(doc):
    """Zero out data.program.customisable.mapping -- see LEFTOVER-DATA FIX.

    Leaves parameterIndex (positional/structural) alone; resets
    automationIndex/value/name to the file's own "unused slot" shape so no
    donor-addon parameter names survive.
    """
    mapping = doc["data"]["program"]["customisable"]["mapping"]
    for entry in mapping:
        entry["automationIndex"] = UNUSED_AUTOMATION_INDEX
        entry["value"] = 0.0
        entry["name"] = ""


def fix_midi_routes(doc, control_channel):
    """Point data.midiInputRoute/midiOutputRoute at this project's own ALSA
    port instead of the leftover donor addon's own. See LEFTOVER-DATA FIX.

    CORRECTED: the client name maze_host.cpp passes to RtMidi ("Mockba
    Maze") is NOT what actually shows up in ALSA/mido's port list on real
    hardware -- confirmed live (mido.get_output_names()/get_input_names(),
    same investigation that fixed web/server.py's identical bug --
    force-acid's web/server.py has the full story in its own
    IN_PORT_MATCH/OUT_PORT_MATCH comment): MockbaMod reorders it into
    "Maze:In (Mockba)" (an ephemeral numeric client:port suffix follows,
    e.g. "... 140:0", not included here since it's assigned fresh every
    boot). The previous version of this function used "Mockba Maze"
    verbatim, which was never actually correct on this device.

    UNLIKE force-acid/DX7/JV880/Maze Seq, maze_host opens only ONE port --
    confirmed live, its own startup banner says "up. port 'Maze:In
    (Mockba)'" (singular). It's a pure audio synth mixed in via
    ForceAudioIn's shared-memory tap, not a MIDI note generator, so there
    is no "Maze:Out (Mockba)" to reference at all; both routes below point
    at the one real port that exists.

    STILL UNCONFIRMED: whether Force's own route-matching needs the literal
    "(Mockba)" suffix, does prefix matching, or something else. `deviceId`
    (e.g. "137-0") looks like a cached, per-boot-ephemeral ALSA client:port
    number -- reset to "0-0" here as a clearly-unresolved placeholder on
    the assumption Force re-resolves routes by deviceName when the cached
    id doesn't match anything live -- not verified against real firmware
    behavior.
    """
    ch0 = control_channel - 1  # maze_host.cpp/--control-channel are 1-based; the numeric
                                # outputChannel field itself is 0-based.
    port_name = "Maze:In (Mockba)"   # the only port maze_host actually opens

    in_route = doc["data"]["midiInputRoute"]
    in_route["inputPort"]["deviceName"] = port_name
    in_route["inputPort"]["deviceId"] = "0-0"
    in_route["inputChannel"] = ch0

    out_route = doc["data"]["midiOutputRoute"]
    out_route["outputPort"]["deviceName"] = port_name
    out_route["outputPort"]["deviceId"] = "0-0"
    out_route["outputChannel"] = ch0


def rename(obj, template_name):
    """Self-referential name fields -- everywhere the seed said "Harpie 4T
    Control" (its own template name), swap in ours. Leave targetData's
    "track" fields alone -- those were just set above and mean something
    different (the *destination* MIDI track name)."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "name" and isinstance(v, str) and v.startswith("Harpie 4T Control"):
                obj[k] = v.replace("Harpie 4T Control", template_name)
            else:
                rename(v, template_name)
    elif isinstance(obj, list):
        for item in obj:
            rename(item, template_name)


def audit(doc):
    """Scan the final doc for leftover donor-addon strings. Returns a list of
    JSON-path strings where something forbidden was found (empty = clean)."""
    hits = []

    def walk(o, path):
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, f"{path}/{k}")
        elif isinstance(o, list):
            for i, item in enumerate(o):
                walk(item, f"{path}[{i}]")
        elif isinstance(o, str):
            for bad in FORBIDDEN_SUBSTRINGS:
                if bad in o:
                    hits.append(f"{path} = {o!r}")
    walk(doc, "")
    return hits


def build_doc(track_name, template_name, control_channel):
    if not SEED_PATH.exists():
        sys.exit(f"seed file missing: {SEED_PATH}")
    doc = json.loads(SEED_PATH.read_text())

    program = doc["data"]["program"]
    program["customQLinks"] = [
        make_qlink(label, cc, track_name, momentary)
        for (_key, label, cc, momentary) in KNOBS
    ]
    blank_mapping(doc)
    fix_midi_routes(doc, control_channel)
    rename(doc, template_name)
    return doc


def write_xtk(doc, out_path):
    body = HEADER + json.dumps(doc, indent=4)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.GzipFile(out_path, "wb", mtime=0) as f:
        f.write(body.encode("utf-8"))


def write_json(doc, out_path):
    """Always emitted alongside the .xtk -- the human-reviewable form. Edit
    this file and re-run with --pack to rebuild without touching this
    script's generation logic."""
    out_path.write_text(json.dumps(doc, indent=2) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track-name", default="MAZE CTRL",
                     help='must match the MIDI track name exactly once loaded on the Force, and '
                          'src/maze_host.cpp\'s --control-channel must match that track\'s output '
                          'MIDI channel (default: "MAZE CTRL")')
    ap.add_argument("--out", default=str(HERE.parent / "addon" / "Force Maze Control.xtk"))
    ap.add_argument("--template-name", default="Force Maze Control")
    ap.add_argument("--control-channel", type=int, default=1,
                     help="1-16, must match maze_host's --control-channel (default 1) -- "
                          "used for this track's MIDI I/O route, not the Q-Link CC targets")
    ap.add_argument("--pack", metavar="JSON_PATH",
                     help="skip seed/KNOBS generation -- wrap this already-assembled "
                          "JSON file (e.g. a previous build's --out .json, hand-edited) "
                          "into .xtk framing instead")
    args = ap.parse_args()

    out_path = Path(args.out)
    json_path = out_path.with_suffix(out_path.suffix + ".json")

    if args.pack:
        pack_path = Path(args.pack)
        if not pack_path.exists():
            sys.exit(f"--pack file missing: {pack_path}")
        doc = json.loads(pack_path.read_text())
        print(f"packing {pack_path} (skipping seed/KNOBS generation)")
    else:
        doc = build_doc(args.track_name, args.template_name, args.control_channel)

    hits = audit(doc)
    if hits:
        print("WARNING: leftover donor-addon strings survived into the build:", file=sys.stderr)
        for h in hits:
            print(f"  {h}", file=sys.stderr)
        sys.exit(1)

    write_xtk(doc, out_path)
    write_json(doc, json_path)

    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    print(f"wrote {json_path} (reviewable JSON -- edit + --pack it to rebuild)")
    if not args.pack:
        program = doc["data"]["program"]
        print(f"Q-Link bank: {len(program['customQLinks'])} knobs, track name '{args.track_name}'")
        print("Load it on the Force onto a MIDI track literally named "
              f"'{args.track_name}' -- the CC targets are bound by track NAME, not by track index.")
        print(f"maze_host must be started with --control-channel matching that track's output channel "
              "(default 1) for any of this to actually reach maze_voice.c.")


if __name__ == "__main__":
    main()
