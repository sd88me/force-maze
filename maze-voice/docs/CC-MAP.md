# Force Maze Voice — CC map

Control channel: **1** by default (`--control-channel` on `maze_host`, must
match the Q-Link control track's own output channel). CC numbers sit inside
the same safe block `force-acid`'s map uses (clear of 0, 1, 32, 64, 121+,
which MockbaMod's MidiLoop docs warn about).

One Q-Link bank is 16 knobs; `maze_voice.c` has ~30 `chain_params`
(`module.json`), so this covers the 16 most commonly-tweaked ones. Every
param, including the other ~14, is reachable from the web panel
(`http://<force-ip>:8304`) regardless.

| CC | Param | Wire range → value | Notes |
|---|---|---|---|
| 20 | `vco_tune` | 0–127 → −24..24 st | |
| 21 | `mod_freq` | 0–127 → 0.2..1300 Hz | **log** curve, matches `module.json`'s `"curve":"log"` |
| 22 | `fm_depth` | 0–127 → 0..100 | Mod→VCO FM amount |
| 23 | `vco_eg1` | 0–127 → −100..100 | EG1→VCO pitch amount |
| 24 | `mod_eg1` | 0–127 → −100..100 | EG1→Mod-VCO amount |
| 25 | `env1_decay` | 0–127 → 0..100 | EG1 (filter/mod env) decay |
| 26 | `env2_decay` | 0–127 → 0..100 | EG2 (VCA) decay |
| 27 | `fold_drive` | 0–127 → 0..100 | Wavefolder drive |
| 28 | `fold_bias` | 0–127 → −100..100 | Wavefolder bias |
| 29 | `blend` | 0–127 → −100..100 | Fold/Filter blend (`route` sets the order, web-only) |
| 30 | `cutoff` | 0–127 → 0..100 | DSP applies its own internal Hz curve — wire value is plain linear |
| 31 | `reso` | 0–127 → 0..100 | Filter resonance |
| 32 | `filter_mode` | 0–127 → 0..100 | LP↔BP morph |
| 33 | `vco_lvl` | 0–127 → 0..200 | |
| 34 | `level` | 0–127 → 0..100 | Output volume |
| 35 | `rnd_go` | ≥64 fires | momentary; Generate (randomises whatever pages are armed — arm them from the web panel's Randomise strip) |

Not on a knob (web panel only): `mod_key`, `vco_key`, `fold_eg1`, `fold_key`,
`cutoff_eg1`, `cutoff_key`, `filt_drive`, `noise_lvl`, `noise_tone`,
`ring_lvl`, `mod_lvl`, `sat`, `rnd_voice`/`rnd_wavefolder`/`rnd_filter`/
`rnd_tone` (the Randomise page-arm toggles).

## Setup on the Force

1. Enable Sync + Track on `Mockba Maze In` in Preferences → MIDI (needed for
   notes; CC works over any routed MIDI track regardless).
2. Create a MIDI track named exactly `MAZE CTRL` (must match
   `scripts/build_xtk.py`'s `--track-name`, default `MAZE CTRL`), output to
   `Mockba Maze:In`, channel matching `maze_host`'s `--control-channel`
   (default 1).
3. Load `addon/Force Maze Control.xtk` onto that track for pre-named,
   pre-ranged Q-Link knobs (see `docs/capture-xtk.md` for the format's
   caveats — not yet visually confirmed on a real screen).
4. A separate instrument/Audio-In track monitors `hw:2`'s capture input
   (see `DESIGN.md`) to actually hear the voice.
