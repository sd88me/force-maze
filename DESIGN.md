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

**Deployment split, then a full repo split (2026-09-13):** `forceAudioIn.c`/
`injectTone.c` and their zig build script used to live here (`src/`,
`scripts/build_audiotap.sh`), staged into this repo's own `addon/` and then
hand-copied into the MockbaMod fork's `SD/AddOns/ForceAudioIn`. They've now
moved entirely into their own repo,
[`ForceAudioIn`](https://github.com/sd88me/ForceAudioIn) - that repo owns
arming the shared `LD_PRELOAD` tap exclusively (see its own README.md for
why: one arming addon, many voice addons attaching to it, rather than every
voice addon bundling its own copy and racing on the same file), and is the
source for what's deployed in the MockbaMod fork at
[`SD/AddOns/ForceAudioIn`](https://github.com/sd88me/MockbaMod/tree/main/SD/AddOns/ForceAudioIn).
`maze_host.cpp` still needs `forceAudioInject.h`'s shared ring layout, so
this repo keeps a vendored copy of just that one header (`src/forceAudioInject.h`,
marked at its top as vendored - must stay byte-for-byte identical to the
canonical copy). This repo's own `addon/manage.sh`/`run_maze_host.sh` don't
touch `LD_PRELOAD` or `acvs` at all - see "Shipped baseline" below.

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

## Boot race (the ring one - see below for a second, unrelated one)

`forceAudioIn.so`'s constructor needs the `/forceAudioInject` shared-memory
ring to already exist the moment `MPC`'s process is `exec`'d (constructors
run at library load, before `main()`). `maze_host` must therefore start
producing into the ring *before* MPC exists, not after - the same boot-race
constraint `ForceLinkAudio`'s `run_ForceLinkAudio.sh` solves by arming
`LD_PRELOAD` before backgrounding its own startup. An earlier version of
`run_maze_host.sh` waited for `{MPC Main Thread}` to appear before
launching `maze_host` (copying `ForceLinkAudio`'s *network*-process startup
pattern, which has no such ordering requirement) - that always lost the
race, so the tap saw "passthrough-only" and MPC would need restarting again
to attach at all. Fixed by launching `maze_host` immediately, unconditionally.

This does mean every restart produces a one-time large backlog dump before
the consumer attaches (the "boot race" head start draining into the ring
with nothing reading it yet) - `forceAudioIn.so`'s hysteresis trim absorbs
this on its very first `mix_in()` call, but it's also the confound behind
the ruled-out adaptive-controller experiment above.

## Boot-time LD_PRELOAD race (confirmed 2026-09-13 - the serious one)

*(This section and the three below it are about `forceAudioIn.so` itself,
which has since moved into its own repo - see
[`ForceAudioIn/DESIGN.md`](https://github.com/sd88me/ForceAudioIn/blob/main/DESIGN.md)
for the canonical, kept-up-to-date version of this history. Left here
too since this project's own testing is what surfaced most of it.)*

A completely different, much more consequential race, discovered after real
overnight boot failures unrelated to any active development: pads/buttons
dead, or WiFi dead, alternating unpredictably across successive reboots,
sometimes fine. Traced to `/dev/shm/.LD_PRELOAD` (the file `apps.sh` reads
into `LD_PRELOAD` before exec'ing `MPC` - see the mockbamod-module-creator
skill's `architecture.md`).

**Confirmed by reading the actual scripts on a live device**:
`mockbaMagic`'s and `MidiLoop`'s own `run_*.sh` scripts both read this file,
check their library isn't already present, and write the whole file back -
with **no locking at all**. MockbaMod's own boot sequence backgrounds every
top-level addon script concurrently (`architecture.md`: "launches every
top-level `AddOns/*.sh` (`"$f" &`)"). Three or more unsynchronized
read-modify-write scripts racing on one shared file at boot is a textbook
lost-update: whichever write lands last wins, based on whatever it read,
silently dropping another script's entry. Lose `MidiLoop`'s
`tkgl_anyctrl_lt.so` and the control-surface remapper never loads -> dead
pads/buttons. Lose or corrupt `mockbaMagic.so`'s entry and you get its own
already-documented WiFi-breaking failure mode. Same race, two different
casualties depending purely on write-ordering luck on a given boot -
matching exactly what was observed.

This bug is pre-existing in MockbaMod itself (`mockbaMagic` and `MidiLoop`
already race each other); adding `ForceMazeVoice`'s `run_maze_host.sh` as a
**third** unlocked writer measurably worsened the collision rate in
practice.

**Mitigation applied (our side)**: `run_maze_host.sh` and `addon/manage.sh`
wrap every read-modify-write of `$mmLD_PRELOAD_VAR` in an `mkdir`-based
mutex (atomic even on busybox; bounded retry, fails OPEN rather than
risking a hung boot on a stale lock). This makes OUR participation safe,
but on its own doesn't fix the underlying two-way race between
`mockbaMagic` and `MidiLoop` - they don't check for or respect a lock they
don't have.

**Actually fixed at the source (2026-09-13)**: patched the same `mkdir`
lock directly into `mockbaMagic`'s and `MidiLoop`'s own `run_*.sh` scripts
(both the top-level copy and the copy inside each addon's own folder, since
`manage.sh ENABLE` re-copies from the latter). Small, behavior-preserving
change - same `LD_PRELOAD` content, same load order, just serialized.
Patches committed in the `sd88me/MockbaMod` fork
(`SD/AddOns/mockbaMagic/`, `SD/AddOns/MidiLoop/`); full writeup in the
`mockbamod-module-creator` skill's `references/gotchas.md`.

Also learned along the way: `systemctl restart acvs` doesn't just restart
the touchscreen app - its cgroup includes `boot.sh` itself, so restarting
`acvs` **re-runs the entire top-level `AddOns/*.sh` kill+relaunch
sequence**, hitting this exact race again every time. That's actually
useful: it means `acvs` restarts (fast, no power-cycle needed) are a valid
way to repeatedly re-test this, not just physical reboots. It also means
there's no clever staging trick on our own side that avoids the race -
arming `LD_PRELOAD` for anything always goes through this same door.

**Verified live**: 8 consecutive `acvs` restarts (a mix of general testing
and a run starting from a confirmed-good baseline, specifically to rule out
"was it already broken beforehand") all produced correct `LD_PRELOAD`
content, and pads/buttons were physically confirmed responsive afterward
each time.

**Status now**: the root cause is fixed, so re-enabling the engine's
autolaunch (`addon/manage.sh ENABLE`) should no longer carry the elevated
risk that kept it disabled - the two-way race it would have joined is
itself now locked. Re-enable and re-verify with the same "several `acvs`
restarts + physical pad check" method before trusting it unattended.

The web panel (`web/manage.sh`) never touched `LD_PRELOAD` at all and had
none of this risk either way - it stayed enabled independently throughout.

## `acvs`, not `inmusic-mpc`

The mockbamod-module-creator skill's own reference docs call the service to
restart for LD_PRELOAD-content changes `inmusic-mpc`. On this device,
`systemctl list-units` shows no such unit - the real service is **`acvs`**,
described by systemd itself as "InMusic MPC Application". Confirmed live;
`manage.sh` here calls `systemctl restart acvs`.

## Toolchain

`maze_host` (needs RtMidi + ALSA headers): native armhf-under-QEMU Docker
build, identical toolchain to `force-acid` (`scripts/Dockerfile`,
`scripts/build.sh`) - `arm32v7/debian:stretch`, not `buster` (EOL apt
issues), `GLIBC_2.4`/`GLIBCXX_3.4.22` required, comfortably under the
Force's actual `GLIBCXX_3.4.32` ceiling.

`forceAudioIn.so`/`injectTone` are no longer built from this repo - see the
separate [`ForceAudioIn`](https://github.com/sd88me/ForceAudioIn) repo's
own `scripts/build.sh` (still the same `zig cc -target
arm-linux-gnueabihf.2.39` cross-build, no Docker needed).

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

**The web panel is its own addon** (`web/manage.sh` + `web/run_maze_web.sh`,
directly copying `force-acid`'s `web/manage.sh` + `run_forceacidweb.sh`
split), independent of the engine (`addon/manage.sh` + `run_maze_host.sh`).
It never touches `LD_PRELOAD`, only needs `maze_host`'s control socket to
exist by the time a browser actually asks it for something (answers 503
until then), and uses PID-file tracking rather than a name-based
`killall`/`pgrep` - this device runs other `python3` processes (nodeServer's
tooling, `force-acid`'s own web panel) that a name match would also kill;
confirmed the cost of that class of mistake the hard way already this
project (see the reverted `SCHED_FIFO` incident above for the general
lesson on blast radius). This split is what lets the panel stay always-on
even while the engine is disabled (see "Boot-time LD_PRELOAD race" above).

## nodeServer integration

Patches the separate **nodeServer** addon (not this one) for a home-page
quick-link and a Modules-page (`/moduler`) entry - see
`nodeserver-integration/README.md` for exactly what's patched. Two things
worth knowing if extending this:

- The Modules page needs **no nodeServer code change at all** - it scans
  every `AddOns/*/NSMODULE.json` and renders whatever it finds
  (`api/endpoints/moduler/index.js`). Adding `addon/NSMODULE.json` was
  sufficient.
- **Gotcha, found by reading `moduler/index.js` before deploying**: its
  autolaunch-toggle spawn call does `JSN.ARGUMENTS.map(A => A.VALUE)` and
  passes the result straight to Node's `spawn()` - each `ARGUMENTS[].VALUE`
  becomes exactly one argv entry; `spawn()` does not shell-split a string
  containing a space. A value like `"--module-dir /path"` would arrive at
  `maze_host` as one unparseable argv token, not two. `NSMODULE.json` splits
  every flag and its value into separate array entries for this reason -
  `force-acid`'s own `NSMODULE.json` never hit this because its one
  argument (`-v`) has no value to split.
- The home-page link needs a real HTTP redirect (`forcemaze.js`, exactly
  `force-acid`'s `forceacid.js` pattern), not a plain link: `home.js`'s
  link renderer runs every URL through the legacy global `escape()`, which
  mangles the colon in an absolute `http://host:port/` URL.

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

## Multiple simultaneous voices (per-voice mix control, not a central mixer)

**Confirmed live on real hardware (2026-09-13).** Until this, every claim
in this section had only been unit-tested (`tests/test_mix.c`, synthetic
in-process structs) - never two real voices at once on the device. Ran
`maze_host` (slot 0, via the nodeServer Modules page) alongside
`ForceAudioIn`'s own `injectTone` (slot 1, `--channel R`, also via its own
Modules-page toggle, no `acvs` restart for either): both were audible
simultaneously, and `/proc/<MPC-pid>/maps` confirmed both
`/forceAudioInject0` and `/forceAudioInject1` live and current (alongside
several harmlessly-leaked `(deleted)` mappings from earlier stop/restart
cycles - see the segment-replacement fix above). This is the first real
confirmation the multi-voice mixer works end to end, not just in the unit
test.

`forceAudioIn.so` mixes up to `AI_MAX_VOICES` (4) independent voice hosts at
once, each in its own named shared-memory ring (`/forceAudioInject0`,
`/forceAudioInject1`, ...) - see `forceAudioInject.h`. Every ring stays
genuinely single-producer/single-consumer (one voice host writes its own
ring; `forceAudioIn.so` is the sole reader of all of them), so this scales
without adding any cross-process synchronization beyond what already existed
per ring.

**Deliberately no central "mixer" control panel.** Each ring's `enabled`/
`gain`/`channel_mask` fields are the on/off, volume, and L/R/L+R routing for
*that* voice, written by that voice's own control socket (`maze_host`'s
`mix.enabled`/`mix.gain`/`mix.channel` keys, intercepted in
`handle_ctrl_line` before reaching `maze_voice.c`'s `set_param`) and exposed
in that voice's own web panel ("Output Mix" section, `web/index.html`). A
separate coordinating mixer page would need its own IPC path into each
voice's socket for no real benefit - every voice already has one.

**Channel select is L / R / L+R, not an arbitrary voice-count.** The tapped
capture handle is confirmed 2-channel (see "The core problem" above) - that
is the actual hardware ceiling, not a software choice. Two voices routed to
the same channel simply sum there, same as two synths sharing one mixer
channel; you cannot get more than 2 fully-independent buses out of a
2-channel tap.

**Mute happens at the consumer, never by pausing the producer.** A voice's
render cadence is its own synth's clock (envelopes/filters advance every
`render_block` call - see "THE RENDER CADENCE IS THE SYNTH'S CLOCK" in
`maze_host.cpp`). `enabled=0` only skips adding that voice's samples into the
output in `mix_in_one`; the ring is still drained at the normal rate so a
re-enabled voice resumes from live backlog, not a stale one.

**LD_PRELOAD is armed once, not per voice.** `forceAudioIn.so` itself needs
loading into MPC exactly once - it already attaches to every slot that has a
ring present. Running a second/third simultaneous voice addon means giving
its `maze_host` a distinct `--mix-slot`, but its `run_*.sh` must skip the
"ARM THE TAP FIRST" `LD_PRELOAD` section entirely (see the comment in
`addon/run_maze_host.sh`) - two addons both arming the same-by-substring
`forceAudioIn` entry race exactly like the mockbaMagic/MidiLoop
boot-time-LD_PRELOAD bug documented above.

**Update 2026-09-13: cross-built and hardware-verified.** `mix.gain`/
`mix.channel`/`mix.enabled` round-tripped correctly through the real control
socket into `forceAudioIn.so`'s own diagnostic log on the real audio thread,
one voice at a time (slot 0 only tested live so far - no second voice addon
created yet; would need its own module.json/NSMODULE.json/ports, per the
mockbamod-module-creator skill's port-collision guidance, e.g. web panel 8305
following 8303/8304).

## Open incident: pads/buttons dead with forceAudioIn.so armed (2026-09-13)

Found while re-enabling the engine for the hardware test above: two live
`acvs` restarts with `/dev/shm/.LD_PRELOAD` content confirmed correct both
times (all three libs present - `cat`'d directly) still killed pads/buttons.
This is **not** the boot-time file race fixed elsewhere in this doc - that
race is about the file's *content* being wrong; here the content was right
and it still happened. Root cause not yet found. Ruled out so far (static
analysis, no device access needed):

- **Not a symbol collision with MidiLoop's `tkgl_anyctrl_lt.so`.**
  `forceAudioIn.so` interposes only `snd_pcm_readi`/`snd_pcm_readn`/
  `snd_pcm_hw_params` (PCM streaming). `tkgl_anyctrl_lt.so` interposes
  `snd_rawmidi_open`/`snd_rawmidi_read`/`snd_seq_create_simple_port`/
  `snd_midi_event_decode`/`aconnect` (sequencer/rawmidi) plus a
  `midiPortBlacklist.txt`-driven filter - confirmed directly from both
  `.so`'s dynamic symbol tables. Zero overlap. (This also means the
  mockbamod-module-creator skill's gotchas.md tier list was wrong to group
  `tkgl_anyctrl_lt.so` with `mockbaMagic`'s raw patching - it's the same
  interposition tier as `forceAudioIn.so` itself, just a disjoint symbol
  set. Corrected there.)
- **Probably not mockbaMagic's raw address-patching either.** Confirmed
  `mockbaMagic.din` genuinely is a firmware-version-keyed patch table
  (raw bytes show `FORCE` + version strings + address/offset/bytes records),
  but the script that actually invokes the ptrace-based patcher
  (`livePatcher.sh`) is commented out in this device's `run_mockbaMagic.sh`
  - the raw-patch mechanism looks dormant on this device right now, so an
  address-shift-from-a-third-library theory probably doesn't apply here.

**Live-tested elimination sequence, in order, each a real acvs-restart cycle
on the actual device:**

1. Diagnostics thread's mere existence - gated it off by default behind a
   marker file (`AI_DIAG_MARKER` = `/tmp/forceAudioIn.diag` in
   `forceAudioIn.c`; `touch` it before an `acvs` restart to re-enable for
   debugging). **Ruled out**: failed again with the thread confirmed not
   spawning (log showed "diagnostics thread disabled by default" on every
   load).
2. `forceAudioIn.so` merely being loaded, zero voices attached (`maze_host`
   never started). **Ruled out**: survived 3x rapid restarts cleanly.
3. The per-sample write loop in `mix_in_one` specifically - tested by
   attaching a voice but muting it (`mix.enabled = 0`) before the restarts,
   which still runs every other line of `mix_in_one` (the atomic head/tail
   loads, the backlog/trim math, the atomic tail store) but skips the
   `chan_allowed`/`sample_to_float`/`float_to_sample` inner loop entirely.
   **Ruled out**: failed again, same restart-2/3 pattern.

**Current standing**: the bug needs a voice actually attached (a real
`/forceAudioInjectN` ring existing), but does NOT need it to be actively
mixing samples into the output. That leaves two remaining, not yet
distinguished candidates: (a) the ring bookkeeping/atomics/backlog-trim path
in `mix_in_one` that runs on every ALSA read regardless of `enabled`, or
(b) something about the separate voice-host PROCESS itself (`maze_host`'s
RtMidi ALSA-sequencer client, its timer thread, its control socket thread -
none of which run inside MPC, but see below on a genuine correctness bug
in how the ring behaves across a multi-second consumer gap, found while
re-reading this code, not yet confirmed as related).

**A related but unconfirmed real bug**: `avail = (head - tail) & (AI_RING_FRAMES
- 1)` silently aliases if the true unconsumed gap ever exceeds
`AI_RING_FRAMES` (65536 frames, ~1.49s @ 44100Hz) - plausible for a full MPC
relaunch. `maze_host` never stops rendering across an `acvs` restart, so the
gap where literally no consumer exists could exceed one full ring lap. This
would cause audio-quality artifacts (stale/wrapped data being mixed in), not
an input-handling symptom like dead pads, so it's flagged as real and worth
fixing on its own merits but not currently believed to explain this
incident. Parked per user instruction until the incident itself is
resolved.

**Lazy re-attach, added 2026-09-13 for a different reason (see below), with
a side effect worth flagging for this investigation**: `forceAudioIn.so`
used to attach to each voice slot ONLY once, in the constructor - if
`maze_host` started after MPC (no intervening `acvs` restart), it was never
noticed. Fixed (`ai_try_attach`, called from both the constructor and a
background thread's ~2s wake loop) so a voice started post-boot gets picked
up live. **This makes the background thread unconditional again** - it no
longer only exists when `AI_DIAG_MARKER` is set or a voice happens to be
present at load time; it always runs now, because lazy re-attach needs it
to. The marker file still gates the diagnostics-LOGGING half of the thread's
job, not its existence. Net effect on the open incident above: item 1 in the
elimination list (thread's mere existence, tested with a voice already
attached at load) is not perfectly re-tested by this - worth re-confirming
that a bare "library loaded, background thread running, zero voices ever
attached" survives repeated restarts under this new always-on-thread
version, since that specific combination (thread present, but for a
different reason and always-on rather than gated) hasn't been tested in
exactly this shape. (Live-tested afterward: it doesn't - a bare loaded
library with the always-on thread and zero voices ever attaching survived
3x restarts cleanly, same as before.)

**Address-shift theory: measured, then ruled out by disassembly.** A voice-
attached restart's `/proc/<MPC-pid>/maps`, diffed against a zero-voice
baseline, showed `mockbaMagic.so`'s own load base shift by exactly 0x40000
(256KB) - mechanically expected (our extra mappings load earlier in the
sequence, pushing everything after them down), and the first genuinely new,
measurable structural fact distinguishing a failing run from a passing one.
Disassembled both `mockbaMagic.so`'s and `tkgl_anyctrl_lt.so`'s actual
`.init_array` entries (ARM/Thumb-2, via the QEMU-emulated armhf Docker
image's own `objdump` - a native reader, not guesswork) to check whether
either does anything address-dependent at load time that a base shift could
break. Both are provably inert: `mockbaMagic.so`'s three constructors are
`frame_dummy` (generic EH-frame registration) plus two `std::ios_base::Init`
calls (automatic iostream setup) - nothing touches `mockbaMagic.din` or does
address arithmetic; that happens in a *different*, uncalled-at-load-time
function, almost certainly only reached from the separate standalone
`mockbaMagic <pid>` executable's own `main()` (spawned by the currently-
disabled `livePatcher.sh`), not from anything that runs inside MPC right
now. `tkgl_anyctrl_lt.so`'s one constructor is also just `frame_dummy`; its
real logic (`match`/`GetSeqClientFromPortName`) is ordinary functions called
on demand, not at load. **Ruled out**: neither suspect library's own
startup code can be broken by where it lands in memory.

## Breakthrough: this is a race, not a fixed bug (2026-09-13)

A test running `systemctl restart acvs &` in the *background*, with a
concurrent `ps`-polling loop in the same shell (incidental - it was there to
catch the new MPC pid for an `strace` attach attempt that itself failed at
the tooling level and never actually traced anything) - the first "voice
already attached" restart that did NOT kill pads/wifi, after that exact
repro shape had failed with zero exceptions across every prior test (the
maps-diff test, the idle-voice test, the original repro, the muted test).
The one difference: extra CPU/scheduling activity during MPC's startup that
no prior test had. n=1, so not conclusive on its own, but a real,
previously-100%-reproducible failure flipping to a pass on a pure timing
perturbation is strong evidence this is a race condition, not a fixed
logical or address bug - consistent with the address-shift finding above
(real, measurable, but inert on its own) and with this project's own
SCHED_FIFO incident (a different mechanism, but the same theme: this
addon's own early activity interacting badly with something else's startup
timing, not a straightforward code defect).

**Controlled follow-up experiment, not yet live-tested**: rather than ask
for the same accident to be repeated (uncontrolled - unclear if it was the
CPU load, process creation, `/proc` access, or something else about that
shell pipeline), added a deliberate, opt-in, marker-gated delay at the very
start of `ai_ctor`, before any attach work happens (`ai_maybe_delay()` in
`forceAudioIn.c`, gated behind `/tmp/forceAudioIn.delay` - present but
empty defaults to 250ms, or reads a millisecond count from the file's
content; absent = no delay, unchanged default behavior). If a plain delay
alone reproduces the fix across *multiple* restarts (not the n=1 the
accident gave), that's clean, controlled confirmation of a race resolved by
not running this constructor's work "too fast" relative to something else's
own startup - and a far better workaround than keeping a polling loop
running forever. If it doesn't help, that rules out simple "we're just too
fast" and points back toward something more specific about what the
accidental CPU/scheduling perturbation actually did.

## Shipped baseline: zero-voices-at-boot + on-demand start (2026-09-13)

The controlled delay experiment above was run live: 300ms, 2 restarts with
a voice attached before each - 1 pass, 1 fail. Not enough to confirm or
rule out the race theory on its own (a genuinely racy ~50/50 mechanism
looks exactly like this by chance), but enough to rule out "300ms alone is
a reliable fix." Root cause is still unknown as of this writing.

Rather than block a usable setup on finding that root cause, shipped the
workaround this section's own earlier design was already built for:

- `addon/run_maze_host.sh` now **only arms `forceAudioIn.so`** at boot,
  with zero voices ever attached at that point - proven safe across every
  repeated-restart test run against it, including a real physical reboot.
  It no longer starts `maze_host` itself.
- `maze_host` is started **only** on demand, via the nodeServer Modules
  page's own start/stop toggle (`NSMODULE.json`'s `PROCESSNAME`/
  `FILENAME`/`ARGUMENTS`) - this spawns the process directly, with no
  `LD_PRELOAD`/`acvs` involvement at all, so it never re-triggers the race.
  `forceAudioIn.so`'s lazy re-attach (its always-on background thread)
  picks the new ring up within ~2s, no restart needed.
- The nodeServer "Autoload" checkbox on that page is a live
  `fs.existsSync` check against the same top-level script `manage.sh
  ENABLE` creates (confirmed by reading `moduler/index.js` directly) - not
  a separate mechanism, and self-healing if that file ever goes missing
  (recopies the same arm-only script).
- **The hard rule this depends on**: once a voice has been started this
  way, do not restart `acvs` again until it's been stopped first (same
  toggle). Every live test of "`acvs` restart while a voice is attached"
  has failed, with the single accidental exception above - there is no
  known-safe way to do it on purpose yet.

Verified end-to-end on real hardware the same day: enabled persistently,
survived a real physical reboot (zero voices, pads/wifi fine), started via
the nodeServer toggle (lazy-attach confirmed via `/proc/<MPC-pid>/maps`,
no restart, pads/wifi fine, audio audibly playing), stopped via the same
toggle (clean `killall`, no restart, pads/wifi fine). This is the current
shipped baseline - continuing to chase the actual root cause (the delay
experiment's ambiguous result, and whether a longer delay or a genuine
concurrent workload reproduces the accidental fix) is separate, ongoing
work, not a blocker for normal use under the hard rule above.

One unrelated thing surfaced during this testing, worth remembering: a
`run_<name>.sh` placed directly at the top level of `AddOns/` (as
`manage.sh ENABLE` does) can be wiped by an SD-card-level recovery action
(replacing MockbaMod's boot files / running its `emmc-repair` tool) even
though the addon's own subfolder is untouched - re-running `manage.sh
ENABLE` is enough to restore it, no data is actually lost, but it's worth
checking for after any such recovery.

## Not yet built

- A real (not adaptive-controller) fix for the clock-rate mismatch.
- Control-surface feedback (CC/value echoed back out, e.g. so the web panel
  or a controller with LED feedback shows the device's *true* current value
  rather than just "whatever was last sent") - see `force-acid`'s
  `send_feedback_cc`/`FEEDBACK_CHANNEL` for the pattern if picked up later.
- Visual confirmation that `Force Maze Control.xtk` actually loads and
  displays correctly on a real Force screen.
