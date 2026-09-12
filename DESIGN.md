# force-maze — porting `schwung-maze`'s Maze Voice to the Akai Force

Status: **working, hardware-verified, audio timing not yet fully solid.**
DSP core ported verbatim, notes trigger it via a virtual MIDI port, rendered
audio is audible on a real Audio-In track. Getting there took several real
hardware incidents worth recording so they aren't repeated.

## Goal

Run `schwung-maze`'s Maze Voice - a real audio-rendering DSP synth module,
not a MIDI generator - standalone on a MockbaMod-modded Force, with its
audio actually reaching an Audio-In track.

## The core problem this project solves

The Force's main app (`/usr/bin/MPC`, a JUCE binary) opens its ADA2 codec
with raw `hw:` device names and holds both playback and capture exclusively.
Confirmed live: no JACK running, no `snd-aloop` (not built into this
kernel), and `MPC` calls `snd_pcm_writei`/`readi` directly (not the mmap
path). So there is no host-level audio bus to inject synthesized audio into
- the only seam is the one MockbaMod's `customBufferSize` addon and
`forceStream.so` (bundled with `ForceLinkAudio`) already use: **LD_PRELOAD
symbol interposition** against libasound's stable public ABI, inside the
live `MPC` process.

`forceStream.so` taps `snd_pcm_writei` to *extract* what MPC plays.
`forceAudioIn.so` (this project) is the inverse: it taps `snd_pcm_readi` to
*inject* audio into what MPC reads from its capture device. Both are
symbol-name interposition against a stable public library ABI, not raw
address patching against the closed `MPC` binary the way `mockbaMagic` works
- that distinction matters (see "What we ruled out" below).

## Architecture

`maze_voice.c` is `schwung-maze/src/maze-voice/dsp/maze_voice.c` byte-for-byte
verbatim. It exports Schwung's plugin API v2 (`create_instance`, `on_midi`,
`set_param`/`get_param`, `render_block`) and stores its host pointer but
never calls back into it - so no `host_api_v1_t` needs to be provided at all,
unlike `force-acid`'s MIDI-FX port which does use host callbacks (BPM, clock
status).

`maze_host.cpp` plays the role Move's chain host plays for the DSP core,
the same porting pattern as `force-acid/src/host_shim.cpp`, but the output
side is different:

| Schwung chain host did | `maze_host.cpp` does |
|---|---|
| `dlopen(dsp.so)`, `move_plugin_init_v2` | links `maze_voice.o`, calls it directly |
| `on_midi(msg,len,source)` per event | RtMidi input callback -> `on_midi` |
| `render_block(out,frames)` per 128-frame SPI block | timer thread, real elapsed time -> `render_block` |
| `set_param(key,"42")` from a knob | a local Unix control socket -> `set_param` |
| knob repaint via `get_param` | the (not yet built) web UI's `DESCRIBE` calls `get_param` |
| int16 stereo out via the mailbox | float32 into a shared-memory ring (`forceAudioInject.h`) |

`forceAudioIn.so` is the consumer: LD_PRELOAD'd into `MPC`, it mixes
(additive, not replace) whatever's in the ring into the real captured audio
on the one capture handle MPC actually reads (confirmed live: `hw:2`, two
shapes get `hw_params`'d - 4ch and 2ch - only the 2ch one is ever actually
read).

## What we ruled out (read before re-adding either)

**LD_PRELOAD is not monolithically risky.** `mockbaMagic`'s raw in-memory
binary patching (keyed to exact firmware version, the documented cause of a
past WiFi-toggle-breaks incident in MockbaMod's own history) is a
fundamentally different, much riskier technique than symbol interposition
against a stable public ABI. Don't conflate them when reasoning about risk.

**`SCHED_FIFO` on the render thread: tried, reverted, do not re-add casually.**
The Force runs a PREEMPT_RT kernel. Giving `maze_host`'s timer thread
`SCHED_FIFO` priority 10 (on the theory that it could otherwise be starved by
MPC's own real-time audio threads) looked clean by every metric *this*
thread could see - 1.6-1.7ms max wake gap, zero late wakes, thousands of
samples. Shortly after enabling it on real hardware, the Force's pads/knobs
went completely unresponsive and WiFi dropped - the same "pads dead" and
"WiFi broken" symptoms this project's own MockbaMod `gotchas.md` documents
for a runaway real-time thread starving *other* system threads on a shared
RT kernel. **A clean wake-gap log on your own thread does not prove you
aren't starving something else** - it only shows your thread got scheduled
promptly, not what it displaced. Reverted to plain `SCHED_OTHER`; the
"glitching" audio symptom that prompted trying `SCHED_FIFO` in the first
place turned out to have a different, unrelated cause (see below) and got
*worse*, not better, while `SCHED_FIFO` was active - plausibly the same
contention mechanism disrupting MPC's own audio thread, not just system
responsiveness.

**An adaptive integral rate-correction controller: tried, reverted.** See
"Clock-rate mismatch" below for the problem it was trying to solve. Two
live tests with nearly identical starting conditions produced *opposite*
backlog trends (slow decay in one, steady growth in the other) under
nearly the same correction value. Most likely cause: the ring's startup
transient (a large one-time backlog dump before MPC's process, and
therefore the consumer, even exists - unavoidable, see "Boot race" below)
resolves at slightly different speeds run to run, and the ~20-second
observation windows used for live tuning were too close to that transient
to tell real steady-state drift from startup noise. Tuning a feedback loop
against a signal that noisy, live, on hardware that had already had two
real incidents that session, produced exactly the kind of contradictory
result you'd expect. **Do not re-attempt adaptive correction without a
properly isolated measurement session first** (long observation windows,
well clear of startup, ideally logged rather than live-tuned) - not another
round of live trial-and-error.

## Clock-rate mismatch (the real, still-open problem)

The Force's actual ALSA-clocked capture rate runs very slightly faster than
`maze_host`'s software timer's notion of elapsed time - measured
consistently at **~44-45 frames/second of drift (~1000ppm)** across
multiple live sessions. Left uncompensated, ring backlog (and therefore
playback latency relative to a played note) grows without bound - up to
the full ~1.5s ring in a couple of minutes, which is audible as significant
lag, not just growing latency.

Current fix, in order of what's actually deployed:
1. A **fixed multiplicative correction** (`RATE_CORRECTION = 44100.0 /
   (44100.0 - 45.0)` in `maze_host.cpp`) renders very slightly ahead of raw
   wall-clock time. Removes the bulk of the drift but isn't exact.
2. A **hysteresis-based trim** in `forceAudioIn.so`'s `mix_in()`: only trims
   backlog down (to `AI_LATENCY_TARGET_FRAMES`) once it exceeds
   `AI_LATENCY_TRIGGER_FRAMES`, rather than clipping at a single hard
   ceiling. A hard ceiling with no hysteresis band was tried first and
   causes near-continuous small trims once backlog reaches it (each trim is
   a phase discontinuity, i.e. a click) - sounds like fast, repeated
   glitching rather than one occasional correction. Currently ~100ms
   target / ~200ms trigger - a real, if imperfect, trade of latency for
   headroom against the residual drift and ordinary short-term rate noise
   (which, per the ruled-out adaptive-controller test above, may not even
   be perfectly one-directional over short windows).

This combination held up for a couple of minutes of real playing in testing
but is **not proven "locked" over long sessions** - the honest status is
"much better, not fully solved." A real fix needs an actual isolated
clock-rate measurement (long window, logged not live-tuned, ideally
correlating `forceAudioIn.so`'s own hardware-clocked read timestamps against
wall-clock time directly, rather than inferring drift from ring backlog
trend) before attempting adaptive correction again.

## Boot race

`forceAudioIn.so`'s constructor needs the `/forceAudioInject` shared-memory
ring to already exist the moment `MPC`'s process is `exec`'d (constructors
run at library load, before `main()`). `maze_host` must therefore start
producing into the ring *before* MPC exists, not after - the same boot-race
constraint `ForceLinkAudio`'s `run_ForceLinkAudio.sh` solves by arming
`LD_PRELOAD` before backgrounding its own startup. An earlier version of
`run_ForceMazeVoice.sh` waited for `{MPC Main Thread}` to appear before
launching `maze_host` (copying `ForceLinkAudio`'s *network*-process startup
pattern, which has no such ordering requirement) - that always lost the
race, so the tap saw "passthrough-only" and MPC would need restarting again
to attach at all. Fixed by launching `maze_host` immediately, unconditionally.

This does mean every restart produces a one-time large backlog dump before
the consumer attaches (the "boot race" head start draining into the ring
with nothing reading it yet) - `forceAudioIn.so`'s hysteresis trim absorbs
this on its very first `mix_in()` call, but it's also the confound behind
the ruled-out adaptive-controller experiment above.

## `acvs`, not `inmusic-mpc`

The mockbamod-module-creator skill's own reference docs call the service to
restart for LD_PRELOAD-content changes `inmusic-mpc`. On this device,
`systemctl list-units` shows no such unit - the real service is **`acvs`**,
described by systemd itself as "InMusic MPC Application". Confirmed live;
`manage.sh` here calls `systemctl restart acvs`.

## Toolchain

Two separate build paths:
- `maze_host` (needs RtMidi + ALSA headers): native armhf-under-QEMU
  Docker build, identical toolchain to `force-acid` (`scripts/Dockerfile`,
  `scripts/build.sh`) - `arm32v7/debian:stretch`, not `buster` (EOL apt
  issues), `GLIBC_2.4`/`GLIBCXX_3.4.22` required, comfortably under the
  Force's actual `GLIBCXX_3.4.32` ceiling.
- `forceAudioIn.so`/`injectTone` (just libc/libpthread/librt, no ALSA
  symbols needed directly): cross-compiled with `zig cc -target
  arm-linux-gnueabihf.2.39` (matches the Force's exact glibc, confirmed
  live) - no Docker needed at all. See `scripts/build_audiotap.sh`.

## Web GUI

Built by reusing `schwung-maze`'s own `web_ui.html` almost verbatim -
CSS, SVG-generated knobs, drag/wheel/dblclick interaction, and the exact
section layout (Oscillators / Mixer / Wavefolder→Filter / Env-Output /
Randomise) are all unchanged, since they're already hand-matched to this
module's real `chain_params`. Only the transport layer changed: Move's
`schwungRemote`/`postMessage` API (which needs a manager iframe host that
doesn't exist standalone on the Force) is replaced by plain `fetch()` calls
to `web/server.py`, a stdlib-only Python HTTP server that bridges to
`maze_host`'s Unix control socket. The wire-value math, knob registry, and
every param's min/max/curve are untouched from the original - they only
ever depended on `set_param`/`get_param` semantics, which `maze_host`
forwards verbatim to the same `maze_voice.c` functions Move would call.

One addition not in the Move original: an "Audition" strip of note buttons,
since there's no hardware pad feeding notes into this page - it POSTs
directly to `maze_host`'s `NOTE` control-socket command, so the voice can be
played and heard from the browser alone before ever wiring up a MIDI track.

`maze_host`'s `DESCRIBE` command (full `chain_params`/`ui_hierarchy` JSON,
served at `/describe`) is wired up server-side but not yet consumed by
`index.html`, which still hand-declares every knob like the original - a
generic renderer over that JSON would be the natural next step if/when a
second module needs the same treatment, rather than hand-porting a new
`web_ui.html` each time.

Served on port **8304** (`force-acid`'s web panel already owns 8303 -
see `~/.claude/skills/mockbamod-module-creator/references/web-gui.md` on
picking a port and checking for collisions).

## "Headless" control: CC map + `.xtk` track template

`maze_host` originally only parsed Note-On (matching `maze_voice.c`'s own
`on_midi`, which ignores everything else). Added a `PARAMS[]` CC dispatch
table (`src/maze_host.cpp`, modeled directly on `force-acid`'s
`host_shim.cpp` equivalent) so a Force MIDI track can drive it by CC on a
configurable control channel (`--control-channel`, default 1) - this
project's own established term for that pattern is "headless": no
on-screen GUI of its own, controlled from a Force MIDI track (see
`force-acid`'s `NSMODULE.json`).

One Q-Link bank is 16 knobs; `maze_voice.c` has ~30 `chain_params`, so
`docs/CC-MAP.md` picks the 16 most commonly-tweaked ones (spread across
Oscillator/Wavefolder/Filter/Output, plus the Generate trigger) - the rest
stay reachable only from the web panel. `scripts/build_xtk.py` (adapted
from `force-acid`'s script of the same name; `scripts/xtk-seed.json` is
identical generic Force boilerplate, not acid-specific) generates
`addon/Force Maze Control.xtk` from that same table, so the CC numbers
can't drift out of sync between the track template and what `maze_host`
actually listens for.

**Same caveat as force-acid's own `.xtk`**: reverse-engineered from one
sample file (see `docs/capture-xtk.md`), structurally valid (round-trips
through gzip/JSON, matches the real file's shape) but **not yet visually
confirmed on a real screen** - load it once and check that knob names/
ranges look right and Generate behaves as a momentary trigger, not a
sticky value.

## Not yet built

- A real (not adaptive-controller) fix for the clock-rate mismatch.
- Control-surface feedback (CC/value echoed back out, e.g. so the web panel
  or a controller with LED feedback shows the device's *true* current value
  rather than just "whatever was last sent") - see `force-acid`'s
  `send_feedback_cc`/`FEEDBACK_CHANNEL` for the pattern if picked up later.
- Visual confirmation that `Force Maze Control.xtk` actually loads and
  displays correctly on a real Force screen.
