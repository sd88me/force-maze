# Maze Voice — design notes

## Why this exists

`schwung-maze`'s Maze Voice is a real audio-rendering DSP synth, not a MIDI
generator, so porting it to the Force means getting its audio out of a
standalone process and onto an actual Audio-In track. The Force's main app
(`/usr/bin/MPC`, a JUCE binary) opens its ADA2 codec with raw `hw:` device
names and holds both playback and capture exclusively — there is no JACK, no
`snd-aloop`, and `MPC` calls `snd_pcm_writei`/`readi` directly rather than
the mmap path. So there is no host-level audio bus to inject synthesized
audio into; the only seam is the one MockbaMod's own addons already use:
**LD_PRELOAD symbol interposition** against libasound's stable public ABI,
inside the live `MPC` process.

## Architecture

```
Mockba Maze:In (virtual MIDI port, notes + CC)
        │
        ▼
maze_host  ──renders──▶  maze_voice.c (verbatim DSP core, Schwung's plugin API v2)
        │
        ▼  (float32 stereo, shared-memory ring)
forceAudioJack.so  (LD_PRELOAD'd into /usr/bin/MPC)
        │  interposes snd_pcm_readi — mixes the ring's audio into whatever
        │  MPC reads from its capture device (hw:2, confirmed live)
        ▼
Audio-In track on the Force
```

`maze_voice.c` is `schwung-maze`'s DSP core, byte-for-byte verbatim. It
exports Schwung's plugin API v2 (`create_instance`, `on_midi`,
`set_param`/`get_param`, `render_block`), stores its host pointer, and never
calls back into it — no `host_api_v1_t` needs to be provided at all.

`maze_host.cpp` plays the role Move's chain host plays for the DSP core:

| Schwung chain host did | `maze_host.cpp` does |
|---|---|
| `dlopen(dsp.so)`, `move_plugin_init_v2` | links `maze_voice.o`, calls it directly |
| `on_midi(msg,len,source)` per event | RtMidi input callback → `on_midi` |
| `render_block(out,frames)` per SPI block | timer thread, real elapsed time → `render_block` |
| `set_param(key,"42")` from a knob | a local Unix control socket → `set_param` |
| knob repaint via `get_param` | the web UI's `DESCRIBE` call → `get_param` |
| int16 stereo out via the mailbox | float32 into a shared-memory ring |

`forceAudioJack.so` is the consumer: LD_PRELOAD'd into `MPC`, it mixes
(additive, not replace) whatever's in the ring into the real captured audio
on the capture handle `MPC` actually reads. It is a general-purpose
mechanism, not specific to this module, and lives in its own repo,
[`force-audio-jack`](https://github.com/sd88me/force-audio-jack) — this repo
depends on it rather than bundling it, so several voice addons can share one
tap instead of each arming its own copy. `maze_host.cpp` keeps a vendored,
byte-for-byte copy of just the shared ring-layout header
(`src/forceAudioInject.h`) since it needs to agree on the wire format.

`forceAudioJack.so` mixes up to 4 independent voice hosts at once, each in its
own named shared-memory ring — every ring stays single-producer/
single-consumer (one voice host writes its own ring; `forceAudioJack.so` is
the sole reader of all of them), so this scales without extra cross-process
synchronisation. Each ring's `enabled`/`gain`/`channel_mask` fields are that
voice's own on/off, volume, and L/R/L+R routing, set from that voice's own
control socket (`mix.enabled`/`mix.gain`/`mix.channel` keys) and exposed in
its own web panel's Output Mix section — deliberately no central mixer page,
since every voice already has its own IPC path. Mute happens at the
consumer (skipping a voice's samples in the mix), never by pausing the
producer, since a voice's envelopes/filters need to keep advancing on their
own clock regardless of whether it's currently audible.

## Signal chain

```
 MIDI note / velocity
        │
        ├─► EG1 (decay) ── modulation → VCO pitch · MOD pitch · FM depth
        │                              · Fold amount · Filter cutoff
        └─► EG2 (decay) ──────────────► VCA (amplitude)

  OSCILLATORS: VCO (sine) ◄─TZFM─ MOD VCO (triangle), RING (VCO × MOD), NOISE (var. tone)
        → per-channel warm saturation → mix
        │
  ORDER (VCW>VCF · Parallel · VCF>VCW):
    VCW = wavefolder (+ Bias)      VCF = 2-pole state-variable filter (LP ↔ BP, nonlinear resonance, Filt Drive)
        → BLEND crossfade between fold and filter paths
        │
  VCA × EG2 × velocity → Tone/Sat (drive → asymmetric clip → passive tone) → Out Level → + analogue noise floor → L/R out
```

Every LFO destination (see Features in the README) applies as a bipolar
offset computed per-block and summed into the corresponding parameter before
it reaches the signal chain above, so LFO modulation and manual knob values
share the same code path.

### External Voice mode

`out_mode` selects between Internal (the full chain above) and External: in
External mode the tap point moves to immediately after the wavefolder
(`folded = wavefold(foldGain*core + bias)`, computed straight from the raw
oscillator mix), scaled only by Out Level — no blend, no Tone/Sat, no EG2/
velocity amplitude shaping, and the voice is never gated (oscillators
free-run; note-on only resets phase for pitch tracking). In External mode
the filter is bypassed entirely in every `ORDER` setting, so a filter→fold
route never leaks filtered signal into the tap. Intended for feeding an
external analogue filter/amp chain instead of the built-in one.

### Nonlinear filter resonance

The state-variable filter's feedback path applies a nonlinear (tanh-based)
saturation to the damping term, so resonance saturates and blooms as it
approaches self-oscillation instead of ringing linearly. Tuned-by-ear
constants: `SVF_RESO_NL = 0.95`, `SVF_K_MIN = 0.004` (extends how close
resonance gets to self-oscillation), `RESO_BASS_COMP = 0.7` (a gentler bass
compensation than a purely linear filter would use, since some bass loss at
high resonance is treated as character rather than a defect to fully
correct). The filter's existing 2x oversampling (already present at the
voice level) covers this nonlinear stage too — no separate oversampling was
added for it.

### LFOs

Two LFOs, each with shape (saw/triangle/sine/square/sample-and-hold), rate
(free-running, 0.02–30 Hz log curve, or MIDI-clock-synced to a division from
1/16 to 8 bars), and optional retrigger on note-on. Each LFO has an
independent bipolar depth (−100..100) to each of nine destinations: VCO
pitch, Mod pitch, FM depth, filter cutoff, Env1 decay, Env2 decay, filter
drive, fold amount, and fold bias. `effective = base + Σ(lfo_n × depth_n,j ×
range_j)`, clamped to the destination's normal range; full-depth ranges are
±1 octave for VCO/Mod pitch, ±3 octaves for cutoff, and ±1.0 normalised
(±0.5 of the knob range for the two decay destinations, which are
block-rate) for the rest. Sync tempo comes from MIDI clock reaching
`maze_host`'s input port (24 ppqn, averaged); with no clock present it
free-runs at 120 BPM. Every depth is an ordinary parameter, so it's
preset/automation/web-sync-able like any other knob — deliberately a fixed
matrix rather than a slot list, at the cost of a larger parameter count (28
LFO-related parameters).

Compiled under `MAZE_LFO`, defined only by this repo's own
`scripts/build.sh` — the shared `maze_voice.c` file also lives in
`schwung-maze`, where the LFO block is not compiled in, so behaviour there
is unaffected; keep both copies otherwise identical.

## Clock-rate correction

The Force's actual ALSA-clocked capture rate runs very slightly faster than
`maze_host`'s software timer's notion of elapsed time — measured
consistently at roughly 1000ppm (~44–45 frames/second) of drift. Left
uncompensated, ring backlog (and therefore playback latency relative to a
played note) grows without bound. Current mitigation, in order of what's
deployed:

1. A **fixed multiplicative correction** (`RATE_CORRECTION = 44100.0 /
   (44100.0 - 45.0)` in `maze_host.cpp`) renders very slightly ahead of raw
   wall-clock time, removing the bulk of the drift but not exactly.
2. A **hysteresis-based trim** in `forceAudioJack.so`'s `mix_in()`: only trims
   backlog down (to a target of ~100ms) once it exceeds a trigger of
   ~200ms, rather than clipping at a single hard ceiling — a hard ceiling
   with no hysteresis band causes near-continuous small trims once backlog
   reaches it, each one a phase discontinuity (audible as fast, repeated
   glitching). The 100/200ms figures trade latency for headroom against
   residual drift and short-term rate noise.

This combination held up for several minutes of real playing in testing but
is not proven "locked" over long sessions — see Known limitations.

## Known limitations

**Audio timing is "much better," not fully solved.** The fixed
`RATE_CORRECTION` plus hysteresis trim above removes the bulk of the
clock-drift problem, but hasn't been proven stable over long sessions. A
proper fix needs an isolated, logged (not live-tuned) measurement of the
true clock-rate drift, correlating `forceAudioJack.so`'s own hardware-clocked
read timestamps against wall-clock time directly, rather than inferring
drift from ring backlog trend, before attempting any adaptive correction
again.

**An adaptive integral rate-correction controller was tried and reverted.**
Two live tests with near-identical starting conditions produced opposite
backlog trends under nearly the same correction value — most likely because
the ring's one-time startup backlog (see below) resolves at different
speeds run to run, and the short observation windows used for live tuning
were too close to that transient to separate real steady-state drift from
startup noise. Do not re-attempt adaptive correction without a properly
isolated measurement session first.

**`SCHED_FIFO` on the render thread was tried and reverted.** Giving
`maze_host`'s timer thread real-time scheduling priority looked clean by
every metric that thread itself could see, but shortly after enabling it on
real hardware, pads/knobs went completely unresponsive and WiFi dropped —
consistent with a runaway real-time thread starving other system threads on
the Force's shared PREEMPT_RT kernel. A clean wake-gap log on your own
thread does not prove you aren't starving something else. Reverted to plain
`SCHED_OTHER`.

**Boot-time `LD_PRELOAD` race: confirmed and fixed at the source.** Multiple
MockbaMod addon scripts (including this project's own) independently
read-modify-write the shared `/dev/shm/.LD_PRELOAD` file at boot with no
locking, causing an intermittent lost-update race — whichever script's
write landed last silently dropped another's entry, which could leave
pads/buttons or WiFi non-functional depending purely on write-ordering luck
on a given boot. Fixed by adding an `mkdir`-based mutex around every
read-modify-write of that file, both in this project's own scripts and
patched directly into the other affected MockbaMod addons' scripts. A
related read-vs-write race (`boot.sh` reading `LD_PRELOAD` into the
environment before every addon's write had necessarily landed) was found
and fixed separately. Verified with repeated `acvs` restarts (which
re-triggers the entire boot sequence) producing correct `LD_PRELOAD` content
and responsive pads/buttons every time — this fix is considered solid.

**Still open: pads/buttons occasionally going unresponsive when a voice is
attached during an `acvs` restart.** This is a *different* failure from the
boot-time file race above — confirmed present even with `LD_PRELOAD`
content verified correct (via `/proc/<pid>/environ`, not just the file) —
and root cause is not yet identified. What's been ruled out: a symbol
collision with MockbaMod's `MidiLoop` addon (disjoint interposed symbol
sets, confirmed from both libraries' dynamic symbol tables); `mockbaMagic`'s
own raw-patching mechanism (its live-patcher invocation is commented out on
the test device, so it looks dormant); the diagnostics thread's mere
existence; the audio-tap library being loaded with zero voices attached; and
the per-sample mixing loop specifically (failed even with a voice attached
but muted, which skips that loop but not the surrounding ring bookkeeping).
What's still suspected but not confirmed: something in the ring
bookkeeping/backlog-trim path that runs on every audio read regardless of
mute state, or something about the separate voice-host process itself (its
MIDI client, timer thread, or control-socket thread). One test run — not
reproduced since — flipped from a reliable failure to a pass under a timing
perturbation (concurrent unrelated CPU/scheduling activity during `MPC`
startup), suggesting this may be a race rather than a fixed logical bug, but
this has not been confirmed across multiple controlled runs.

**Current operational mitigation**: rather than block a usable setup on
finding the root cause, the engine's autoload is disabled at boot — only
the audio tap (`forceAudioJack.so`) arms at boot with zero voices attached,
which has been verified safe across many repeated restarts and a real
physical reboot. `maze_host` itself starts only on demand via the
nodeServer Modules page, with no `LD_PRELOAD`/`acvs` involvement, and the
tap's lazy re-attach picks up the new ring within a couple of seconds with
no restart needed. **The hard rule this depends on: once a voice has been
started this way, do not restart `acvs` again until it has been stopped
first.** Every tested case of restarting `acvs` while a voice was attached
has failed, with the single unreproduced timing-perturbation exception
above — there is no known-safe way to do this on purpose yet. A plain
reboot or `acvs` restart with **no** voice attached is safe and has been
verified repeatedly. The web panel never touches `LD_PRELOAD` and has no
such risk either way; it stays enabled independently.

A related but unconfirmed correctness issue was found while investigating
this: the ring's backlog calculation can alias if the true unconsumed gap
between producer and consumer ever exceeds the ring's capacity (~1.49s at
44.1kHz) — plausible across a full `MPC` relaunch, since `maze_host` never
stops rendering across an `acvs` restart. This would cause audio-quality
artefacts (stale or wrapped data being mixed in), not an input-handling
symptom, so it's tracked separately and not currently believed to explain
the pads-dead issue above.

**Q-Link `.xtk` template not yet visually confirmed on a real screen.**
`addon/Force Maze Control.xtk` is reverse-engineered from one sample file
(see `docs/capture-xtk.md`) — structurally valid (round-trips through
gzip/JSON, matches the real file's shape) but not yet checked on a real
touchscreen.

**Not yet built**: control-surface feedback (CC/value echoed back out so a
controller with LED feedback shows the device's true current value, rather
than just whatever was last sent).

## Toolchain

`maze_host` (needs RtMidi + ALSA headers): native armhf-under-QEMU Docker
build (`scripts/Dockerfile`, `scripts/build.sh`), `arm32v7/debian:stretch`
base (not `buster`, which has EOL apt issues) — `GLIBC_2.4`/
`GLIBCXX_3.4.22` required, comfortably under the Force's actual
`GLIBCXX_3.4.32` ceiling. `forceAudioJack.so` builds from its own separate
[`force-audio-jack`](https://github.com/sd88me/force-audio-jack) repo (`zig cc
-target arm-linux-gnueabihf.2.39`, no Docker needed).

## Web GUI

Reuses `schwung-maze`'s own `web_ui.html` almost verbatim — CSS,
SVG-generated knobs, drag/wheel/dblclick interaction, and the section
layout are all unchanged, since they're already hand-matched to this
module's `chain_params`. Only the transport layer changed: Move's
`schwungRemote`/`postMessage` API is replaced by plain `fetch()` calls to
`web/server.py`, a stdlib-only Python HTTP server bridging the page to
`maze_host`'s Unix control socket (`SET`/`GET`/`DESCRIBE`/`NOTE`). The
wire-value math, knob registry, and every parameter's min/max/curve are
untouched, since they only ever depended on `set_param`/`get_param`
semantics, which `maze_host` forwards verbatim to `maze_voice.c`. One
addition not in the Move original: an "Audition" strip of note buttons,
since there's no hardware pad feeding notes into this page.

Served on port 8304. The web panel is its own addon, independent of the
engine, tracked by PID file rather than a name-based process match (this
device runs other `python3` processes that a name match would also kill).

## Related projects

- Sibling module in this repo: [`../maze-sequencer`](../maze-sequencer/DESIGN.md),
  which can drive Maze Voice's MIDI input for a full generative-sequencer +
  voice rig.
- [`force-audio-jack`](https://github.com/sd88me/force-audio-jack) — the shared
  audio-injection tap this module depends on; see its own README/DESIGN for
  the injection mechanism's own design detail.
