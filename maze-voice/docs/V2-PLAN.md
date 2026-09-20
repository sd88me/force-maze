# Maze Voice v2 plan (2026-09-20) — handoff document

Status legend: [ ] todo  [~] in progress  [x] done. Update as work lands.

Scope: `force-maze/maze-voice` (Force) and `schwung-maze/src/maze-voice` (Schwung).
Shared DSP lives in `maze_voice.c` (Force copy; Schwung copy under `dsp/`; keep in sync).

## 0. Findings from reading the code (do not re-derive)
- Signal chain (`voice_tick`): osc mix -> `core`; `route` 0=fold->filter, 1=parallel, 2=filter->fold;
  `folded` is the wavefolder output in ALL routes, then `blend`, `satOut`, amp (EG2*vel), Boss sat, `level`.
- Filter is ALREADY a TPT SVF with tanh on both integrator states and LP<->BP morph (`svf_tick`).
  So "more character" = nonlinearity in the FEEDBACK path + drive interaction, not just tanh on state.
- `OVERSAMPLE 2` already exists but wraps the WHOLE `voice_tick` via a biquad; the filter itself is not separately oversampled.
- Force audio path: maze_host -> shm ring -> `force-audioin` `forceAudioIn.so` hooks `snd_pcm_readi` (IN1/2 only).
- Force shadow pages are data-driven: `addon/shadow_page.conf` (`[tab X]`, `frame`, `knob`, ...).
  Web GUI: `web/index.html` + `server.py`. Schwung: `module.json` chain_params + `web_ui.html` + ui.js.
- Params: `set_param/get_param` string keys in `maze_voice.c` (~line 520-620), CC map in `docs/CC-MAP.md`.

## 1. Output mode (both platforms)
Param `out_mode`: 0 = `INTERNAL VOICE` (today), 1 = `EXTERNAL VOICE`.
- External: tap = `folded` (post-wavefolder, after `dcFold`), scaled by `level` only.
  No blend, no satOut/Boss, no EG2/velocity amp, not gated -> oscillators free-run, retrigger phase on note-on
  only for pitch tracking (note pitch/mod/FM still apply). In external mode the filter is bypassed entirely in ALL routes:
  `folded = wavefold(foldGain*core + bias)` straight from the raw mix (so route 2 filter->fold never leaks filter into the tap).
- Controls: Schwung display menu item + web_ui toggle; Force web GUI toggle (Output section); shadow GUI
  toggle on the voice page next to OUT LEVEL. CC: assign next free CC after the LFO block (TBD, see CC-MAP).
- Persist in presets; default 0 so old presets load unchanged.
- Destination routing (IN1/IN2/IN1&2/OUT3/OUT4/OUT3&4) is INDEPENDENT of out_mode — see section 6.

## 2. Filter upgrade — option B (nonlinear TPT SVF), keep LP<->BP morph
- Add nonlinear feedback: tanh (fast_tanh) on the band-pass/damping term `k*v1` inside the loop, so
  resonance saturates and blooms rather than ringing linearly; keep integrator-state tanh.
- Drive interaction: `filt_drive` gain feeds the nonlinear loop (more drive = more grit at same reso).
- Self-oscillation: extend reso so k reaches ~0.005 (from 0.012) with output-limited sine.
- Bass compensation: gentler than current `lpComp` (user wants some bass loss at high reso = character);
  add `RESO_BASS_COMP` constant (0..1) to tune by ear.
- Oversampling: already provided by the voice-level OVERSAMPLE 2 (see section 0). Nothing extra added.
- IMPLEMENTED: SVF_RESO_NL=0.7, RESO_BASS_COMP=0.7, SVF_K_MIN=0.006 (tune by ear).
- LP<->BP morph unchanged. HP tap NOT added (ask before).
- Must be identical in Force and Schwung copies. A/B compare with offline render before/after.

## 3. LFOs (Force only): 2 LFOs x 9 destinations, fixed matrix
Per LFO n in {1,2}: `lfoN_shape` (0 saw,1 tri,2 sine,3 square,4 S&H), `lfoN_rate` (0.02..30 Hz, log),
`lfoN_sync` (0 free, 1 sync), `lfoN_div` (1/16 .. 8 bars; from transport clock), `lfoN_retrig` (0/1 on note-on).
Depth params `lfoN_<dest>` bipolar -100..100, dest in:
`vco_pitch, mod_pitch, fm_depth, cutoff, env1_decay, env2_decay, filt_drive, fold_amt, fold_bias`.
Per-block: lfo value in [-1,1] computed once per block (per-sample for pitch/cutoff if zipper is audible).
`effective = base + sum_n(lfo_n * depth_nj * range_j)`, clamp. Ranges: pitch +-12 st (x depth), cutoff +-3 oct,
decay: +-1 in log-time, others +-1.0 normalised. Sync: clock from maze_host's MIDI clock (24ppq) if present,
else free-run at tempo 120.
Rationale for fixed matrix (vs slot list): every depth is an ordinary param -> presets/automation/web sync free.
Param count +5x2 +9x2 = 28; need a second Q-Link bank page or web/shadow-only (shadow+web only for v2).

## 4. Voice page re-layout (Force shadow GUI + web GUI)
```
 top bar: [ MAZE VOICE ]  [ENGINE on/off]  [CH assign v]  [OUT MODE: INT|EXT]  [tabs..]
 ┌ OSCILLATOR ──────────┐ ┌ ENVELOPES ─┐ ┌ MIXER / TONE ────────────┐
 │ VCO TUNE   VCO EG1   │ │ ENV1 DEC   │ │ VCO LVL     MOD LVL      │
 │ [VCO KEY]            │ │ CUT EG1    │ │ NOISE LVL   NOISE TONE   │
 │ MOD FREQ   MOD EG1   │ │ FOLD EG1   │ │ RING LVL    TONE/SAT     │
 │ [MOD KEY]            │ │ (stacked   │ │        OUT LEVEL (last)  │
 │ FM DEPTH   FM EG1    │ │  vertical) │ │  (voice on/off REMOVED)  │
 │                      │ │ VCA DECAY  │ │                          │
 └──────────────────────┘ └────────────┘ └──────────────────────────┘
```
- VCO KEY / MOD KEY move into the Oscillator area next to their pitch knobs.
- Envelope knobs stacked in ONE vertical column -> narrower Envelopes frame; give width to Oscillator/Mixer.
- Voice on/off removed (engine switch remains). Keep the param readable so old presets/CC still load; ignore it.
- OUT LEVEL is the last knob on the page (bottom-right).
- Channel assign moves to the top bar (check shadow hit-test/`top bar` code in force_shadow.c).

## 5. Mod page (LFO matrix)
```
 ┌ LFO 1 ─────────────────────────────┐ ┌ LFO 2 ─────────────────────────────┐
 │ SHAPE [saw tri sin sqr S&H] RATE    │ │ SHAPE [saw tri sin sqr S&H] RATE    │
 │ SYNC [free|sync] DIV [1/4 v] RETRIG │ │ SYNC [free|sync] DIV [1/4 v] RETRIG │
 └─────────────────────────────────────┘ └─────────────────────────────────────┘
 ┌ MOD MATRIX (depth, bipolar) ────────────────────────────────────────────────┐
 │           VCO   MOD   FM    CUT   ENV1  ENV2  FILT  FOLD  FOLD               │
 │           PITCH PITCH DEPTH FREQ  DEC   DEC   DRIVE AMT   BIAS               │
 │ LFO 1      (o)   (o)   (o)   (o)   (o)   (o)   (o)   (o)   (o)               │
 │ LFO 2      (o)   (o)   (o)   (o)   (o)   (o)   (o)   (o)   (o)               │
 └─────────────────────────────────────────────────────────────────────────────┘
```
Implemented as a new `[tab MOD]` in `shadow_page.conf` + a MOD section in `web/index.html`.

## 6. force-audioin destination routing (DEFERRED, later work)
Add per-ring destination field to `forceAudioInject.h` header: IN1, IN2, IN1&2, OUT3, OUT4, OUT3&4.
OUT3/4 requires a second hook on `snd_pcm_writei` summing into playback ch 3/4 — VERIFY the Force ALSA
playback device is multichannel first. Applies to ALL addons using force-audioin, each exposes the choice.
maze-voice: independent of out_mode. Needs the restart-acvs LD_PRELOAD workflow; test separately.

## 7. Work order
1. [x] Docs (this file)
2. [x] DSP: out_mode + filter B done in maze_voice.c (both copies identical, compiles, ext mode tested stable). Filter is already 2x oversampled via OVERSAMPLE, no extra stage. Still TODO: by-ear tuning of SVF_RESO_NL / RESO_BASS_COMP, offline A/B render.
3. [x] Schwung UI: out_mode in module.json (Tone menu level) + web_ui.html toggle (not yet device-tested)
4. [x] (web LFO section done; web re-layout of Voice page NOT done, deferred) Force UI: DONE = shadow_page.conf voice page re-layout + OUT MODE enum (offline-rendered OK), web toggle, Voice Out removed. TODO = web GUI re-layout (keys into osc area, stacked env), CHANNEL to TOP BAR (needs force_shadow.c change; interim: in Mixer frame), MOD tab now only Randomise (LFO frames go here)
5. [x] LFO DSP + params + Mod page (shadow) + web GUI sections: BUILT + STAGED on Force (not yet live-tested). See 'LFO implementation notes'.
6. [ ] force-audioin OUT3/4 (later)
Open questions: none blocking. Ask user before adding HP tap or changing CC map.

## Deploy log
- 2026-09-20 Move (.193): maze-voice installed as root to modules/sound_generators/maze-voice (dsp.so md5 verified); backup at /data/UserData/maze-voice.bak.20260920. Move NOT rebooted yet.
- 2026-09-20 Force (.44): maze_host, module.json, shadow_page.conf, web/index.html staged+mv'd, md5 verified; acvs restarted, force_shadow.so loaded, MAZE VOICE 3 tabs. Backups *.bak-v1 in AddOns/ForceMazeVoice. Not yet listened to / tested live (start engine, check OUT MODE + filter).
- Voice page: RING LVL/NOISE LVL swapped; Mod tab Randomise condensed to a top band, rest free for LFOs.

## LFO implementation notes (2026-09-20)
- Code is under `#ifdef MAZE_LFO` in maze_voice.c; only `force-maze/maze-voice/scripts/build.sh` defines it, so the Schwung copy stays unchanged (sync the file to schwung-maze on its next release; behaviour there is identical).
- Params: `lfoN_{shape,rate,sync,div,retrig}` and `lfoN_{vco_pitch,mod_pitch,fm_depth,cutoff,env1_decay,env2_decay,filt_drive,fold_amt,fold_bias}` (N=1,2; depths -100..100), plus `lfo_bpm`. set_param accepts labels and indices (shape SAW/TRI/SIN/SQR/S/H, sync FREE/SYNC, div 16TH..8BAR).
- Modulation: per-block LFO advance with per-sample linear interpolation. Scaling at full depth: VCO/Mod pitch +-1 octave, cutoff +-3 octaves, FM/drive/fold amt/bias +-1.0 (normalised, clamped), env decays +-0.5 of the knob range (block-rate).
- Sync tempo: maze_host now receives MIDI clock (ignoreTypes timing enabled) and sets `lfo_bpm` from 24 ppqn averaging; default 120 BPM when no clock reaches the port ("Mockba Maze In" must be given clock in MIDI prefs).
- Shadow page: shadow parser caps enums at 6 options (MAX_OPTIONS), so SYNC DIV shows 16TH..2BAR only; 4BAR/8BAR are reachable from the web GUI. Raise MAX_OPTIONS in force_shadow.c to expose all 8.
- module.json (Force) now compact, with the 28 LFO params + hierarchy levels; maze_host's chain_params buffer raised 8K -> 64K (it would have overflowed).
- Tests done: zero depth is bit-identical to the non-LFO build; each of the 9 destinations audibly changes output offline; label parsing OK.
- Open: no live test yet; LFO block-rate env decay stepping; sync only meaningful if MIDI clock arrives.
