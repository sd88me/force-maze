/* maze_host.cpp — Force/MockbaMod runtime host for the ported Maze Voice
 * DSP synth. Plays the role Move's chain host plays for maze_voice.c, but
 * for AUDIO rather than MIDI-FX (see force-acid/src/host_shim.cpp for the
 * MIDI-generator equivalent of this same porting pattern):
 *
 *   Schwung host                          this shim
 *   -------------------------------------- --------------------------------
 *   dlopen(dsp.so), move_plugin_init_v2    links maze_voice.o, calls it directly
 *   on_midi() per incoming note            RtMidi input callback -> on_midi()
 *   render_block() per 128-frame SPI block wall-clock timer thread -> render_block()
 *   set_param(key, "42") from a knob       a local control socket -> set_param()
 *   knob repaint via get_param             the web UI's /describe calls get_param
 *   int16 stereo out via the mailbox       float32 into ForceAudioIn's shared-
 *                                          memory ring (forceAudioInject.h) --
 *                                          forceAudioIn.so (LD_PRELOAD'd into
 *                                          /usr/bin/MPC) mixes it into what MPC
 *                                          reads from its capture device.
 *
 * maze_voice.c is verbatim from schwung-maze (its host pointer is stored but
 * never called, so no host_api_v1_t needs to be provided at all).
 *
 * THE RENDER CADENCE IS THE SYNTH'S CLOCK: unlike a byte-generator that can
 * just skip a tick when a downstream buffer is full, maze_voice's envelopes
 * and filters advance real time every render_block call. So the timer loop
 * ALWAYS calls render_block every ~2.9ms regardless of ring space, and only
 * DROPS the resulting samples (bumping an overflow counter) if the shared
 * ring has no room -- never skips the render itself.
 *
 * Build: see scripts/build.sh (native armhf under QEMU, links -lasound
 * -lpthread -lrt, same toolchain as force-acid).
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "rtmidi/RtMidi.h"
#include "include/plugin_api_v1.h"
#include "forceAudioInject.h"

extern "C" plugin_api_v2_t *move_plugin_init_v2(const void *host);

/* ---------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
static std::atomic<bool> g_run{true};
static std::mutex        g_lock;        /* serialises every call into the core */
static plugin_api_v2_t  *g_api  = nullptr;
static void             *g_inst = nullptr;

static ai_shm_t *g_shm_in = nullptr;   /* Audio-In 1/2 ring (AI_SHM_NAME_FMT) */
static ai_shm_t *g_shm_out = nullptr;  /* Out 3/4 ring (AI_SHM_NAME_FMT_OUT) */
static std::atomic<uint64_t> g_ring_drops{0};

static std::string g_chain_params_json;
static std::string g_ctrl_sock_path = "/tmp/maze_ctrl.sock";
static bool         g_verbose = false;
static unsigned     g_mix_slot = 0;   /* which /forceAudioInjectN this instance owns */

/* ---------------------------------------------------------------------------
 * CC -> set_param, for a Force Q-Link-mapped MIDI track ("headless" control -
 * this project's own term, see NSMODULE.json/README: no on-screen GUI of its
 * own, driven by a Force MIDI track). One Q-Link bank is 16 knobs, so this
 * picks the 16 most useful of maze_voice's ~30 chain_params (module.json) -
 * the rest stay reachable only from the web panel. Values/ranges/curve match
 * module.json exactly (mod_freq is the one param with a log curve - see
 * web/index.html's identical wireRaw() for the same formula at the UI layer).
 * kind: 'f' float linear, 'g' float log (mod_freq only), 'w' write-only
 * momentary trigger (rnd_go).
 * ------------------------------------------------------------------------- */
struct ParamSpec {
    const char *key;
    char        kind;
    double      lo, hi;
    int         cc;
};
static const ParamSpec PARAMS[] = {
    { "vco_tune",    'f', -24,  24,   20 },
    { "mod_freq",    'g', 0.2, 1300,  21 },
    { "fm_depth",    'f', 0,   100,   22 },
    { "vco_eg1",     'f', -100, 100,  23 },
    { "mod_eg1",     'f', -100, 100,  24 },
    { "env1_decay",  'f', 0,   100,   25 },
    { "env2_decay",  'f', 0,   100,   26 },
    { "fold_drive",  'f', 0,   100,   27 },
    { "fold_bias",   'f', -100, 100,  28 },
    { "blend",       'f', -100, 100,  29 },
    { "cutoff",      'f', 0,   100,   30 },
    { "reso",        'f', 0,   100,   31 },
    { "filter_mode", 'f', 0,   100,   32 },
    { "vco_lvl",     'f', 0,   200,   33 },
    { "level",       'f', 0,   100,   34 },
    { "rnd_go",      'w', 0,   1,     35 },
};
static const int N_PARAMS = (int)(sizeof(PARAMS) / sizeof(PARAMS[0]));
static std::unordered_map<int, int> g_cc2param;  /* CC -> index into PARAMS, built at startup */
static int g_ctrl_ch = 0;                        /* 0-based; --control-channel is 1-16 */

static void apply_cc(int idx, int value /* 0..127 */) {
    const ParamSpec &p = PARAMS[idx];
    char buf[32];
    switch (p.kind) {
        case 'w':
            if (value < 64) return;   /* only the press, not the release */
            std::snprintf(buf, sizeof(buf), "go");
            break;
        case 'g': {
            double v = p.lo * std::pow(p.hi / p.lo, value / 127.0);
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            break;
        }
        default: {
            double v = p.lo + (p.hi - p.lo) * (value / 127.0);
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            break;
        }
    }
    { std::lock_guard<std::mutex> lk(g_lock); g_api->set_param(g_inst, p.key, buf); }
    if (g_verbose) fprintf(stderr, "[maze] cc %d -> %s = %s\n", p.cc, p.key, buf);
}

/* ---------------------------------------------------------------------------
 * Shared-memory ring setup (producer side -- mirrors injectTone.c, but with
 * a real DSP engine behind it instead of a fixed tone).
 * ------------------------------------------------------------------------- */
static char g_shm_name_in[24];
static char g_shm_name_out[24];

/* Opens/creates one ring at `name`, zeroed and stamped with the given
 * initial mix state, magic published last. Shared by the in-bus and
 * out-bus setup below -- same struct, different shm namespace. */
static ai_shm_t *shm_open_ring(const char *name, uint32_t enabled, uint32_t channel_mask) {
    shm_unlink(name);  /* we are the sole producer for this slot -- start clean */
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return nullptr; }
    if (ftruncate(fd, AI_SHM_BYTES) != 0) { perror("ftruncate"); close(fd); return nullptr; }
    void *m = mmap(nullptr, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return nullptr; }

    ai_shm_t *shm = (ai_shm_t *)m;
    memset(shm, 0, AI_SHM_BYTES);
    shm->rate = (uint32_t)MOVE_SAMPLE_RATE;
    shm->channels = 2;   /* maze_voice's render_block is stereo interleaved */
    shm->enabled = enabled;
    shm->gain = 1.0f;
    shm->channel_mask = channel_mask;
    __atomic_store_n(&shm->magic, AI_MAGIC, __ATOMIC_RELEASE);
    return shm;
}

/* Both rings are always created: the producer always renders into both (see
 * ring_push), and "mix.dest" just flips which ring is `enabled` (mixed by
 * forceAudioJack.so) -- same host-level mute contract as a single-ring
 * voice, just applied per-destination. Default destination is IN1,2. */
static bool shm_setup() {
    ai_shm_name(g_mix_slot, g_shm_name_in, sizeof(g_shm_name_in));
    ai_shm_name_out(g_mix_slot, g_shm_name_out, sizeof(g_shm_name_out));
    g_shm_in  = shm_open_ring(g_shm_name_in, 1, AI_CHAN_LR);
    g_shm_out = shm_open_ring(g_shm_name_out, 0, AI_CHAN_LR);
    return g_shm_in && g_shm_out;
}

/* Push `frames` stereo frames (already float32, [-1,1]) into the ring.
 * Drops from the OLDEST-writable position (i.e. simply doesn't advance head
 * past available space) rather than ever blocking -- this is the producer
 * side, so unlike forceAudioIn.so's consumer path there is no RT thread to
 * protect here, but we still never want to stall the render loop's cadence
 * waiting for forceAudioIn.so to catch up. */
static void ring_push_one(ai_shm_t *shm, const float *interleaved, uint32_t frames) {
    if (!shm) return;
    uint32_t head = shm->head;                                     /* sole producer */
    uint32_t tail = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
    uint32_t space = (AI_RING_FRAMES - 1) - ((head - tail) & (AI_RING_FRAMES - 1));

    uint32_t take = frames;
    if (take > space) {
        take = space;
        g_ring_drops++;
    }
    for (uint32_t i = 0; i < take; i++) {
        uint32_t fr = (head + i) & (AI_RING_FRAMES - 1);
        float *dst = &shm->ring[(size_t)fr * AI_MAX_CH];
        dst[0] = interleaved[2 * i];
        dst[1] = interleaved[2 * i + 1];
    }
    __atomic_store_n(&shm->head, (head + take) & (AI_RING_FRAMES - 1), __ATOMIC_RELEASE);
    shm->frames_written += take;
}

/* Renders into both rings every tick regardless of which one is currently
 * `enabled` -- same "still drains while muted" contract as a single-ring
 * voice (see forceAudioInject.h), just applied to whichever ring isn't the
 * active mix.dest right now, so switching destinations never resumes from a
 * stale backlog. */
static void ring_push(const float *interleaved, uint32_t frames) {
    ring_push_one(g_shm_in, interleaved, frames);
    ring_push_one(g_shm_out, interleaved, frames);
}

/* ---------------------------------------------------------------------------
 * RtMidi input -- notes (maze_voice ignores note-off; it's a decay-envelope
 * monosynth, see maze_voice.c's on_midi comment) plus Control Change on the
 * control channel, for a Q-Link-mapped MIDI track (see PARAMS[] above).
 * ------------------------------------------------------------------------- */
static void on_midi_cb(double /*dt*/, std::vector<unsigned char> *msg, void * /*ud*/) {
    if (!msg || msg->empty()) return;
    const uint8_t *b = msg->data();
    size_t len = msg->size();
    uint8_t status = b[0];
    if (status == 0xF8) {   /* MIDI clock: derive tempo for synced LFOs (24 ppqn), averaged over 24 ticks */
        static std::chrono::steady_clock::time_point t0; static int n = 0;
        auto now = std::chrono::steady_clock::now();
        if (n == 0) t0 = now;
        if (++n >= 24) {
            double sec = std::chrono::duration<double>(now - t0).count();
            if (sec > 0.05) {
                char bpm[16]; snprintf(bpm, sizeof(bpm), "%.1f", 60.0 / sec);
                std::lock_guard<std::mutex> lk(g_lock);
                g_api->set_param(g_inst, "lfo_bpm", bpm);
            }
            n = 0;
        }
        return;
    }
    uint8_t type = status & 0xF0;
    uint8_t chan = status & 0x0F;

    if (type == 0xB0 && len >= 3 && chan == (uint8_t)g_ctrl_ch) {
        auto it = g_cc2param.find(b[1]);
        if (it != g_cc2param.end()) apply_cc(it->second, b[2]);
        return;   /* CC never reaches maze_voice.c's on_midi - it only looks at notes anyway */
    }

    std::lock_guard<std::mutex> lk(g_lock);
    g_api->on_midi(g_inst, b, (int)len, 0 /* MOVE_MIDI_SOURCE_INTERNAL */);
}

/* ---------------------------------------------------------------------------
 * Timer thread -- the synth's real-time clock. render_block has no internal
 * 128-frame assumption (checked: FRAMES_PER_BLOCK only appears in
 * maze_voice.c's PC-only test harness, never inside render_block itself), so
 * this measures ACTUAL elapsed wall-clock time each wake and renders exactly
 * that many frames -- the same jitter-robust technique force-acid's
 * host_shim.cpp uses for its tick() cadence.
 *
 * A fixed "always render 128" cadence (the first version of this loop) falls
 * behind real time whenever sleep_for() or the mutex is delayed by scheduler
 * jitter -- ordinary and expected on a non-RT thread sharing the box with
 * MPC's own audio thread. The ring's CONSUMER (forceAudioIn.so, running on
 * MPC's real ALSA-clocked capture thread) keeps pulling at the true hardware
 * rate regardless, so any shortfall here shows up as a ring underrun --
 * audible as a click, more so on low notes where masking from high-frequency
 * content is weaker. Rendering the real elapsed frame count keeps production
 * caught up with consumption instead of silently drifting behind it.
 * ------------------------------------------------------------------------- */
/* Diagnostics: exposes whether the render thread is actually being starved
 * by the scheduler (a real gap in DSP time, distinct from a ring-side
 * latency issue) -- max elapsed-per-wake seen, and how many wakes needed a
 * frame count above what a clean 1451us cadence should ever produce. */
static std::atomic<double>   g_max_wake_ms{0.0};
static std::atomic<uint64_t> g_late_wakes{0};   /* wakes needing > 400 frames (~9ms) */
static std::atomic<uint64_t> g_total_wakes{0};

/* Clock-rate compensation: the Force's real ALSA-clocked capture consumes
 * samples very slightly faster than our software timer's notion of elapsed
 * time. RATE_CORRECTION renders very slightly ahead of raw wall-clock time
 * to match the hardware's true rate instead of chasing it with an
 * ever-bigger buffer.
 *
 * REVERTED 2026-09-12: this briefly became an adaptive integral controller
 * that nudged the correction toward whatever kept ring backlog at target.
 * Two live tests with nearly identical starting conditions produced
 * opposite backlog trends (slow decay in one, steady growth in the other),
 * most likely because each test's ~20s observation window was too close to
 * the ring's own startup transient to tell real steady-state drift from
 * startup noise - tuning a feedback loop against a signal that noisy, live,
 * is how you get exactly that kind of contradictory result. Back to the
 * fixed measurement (~44-45 frames/sec of drift, ~1000ppm, stable and
 * repeatable across sessions) plus a generous ~100/200ms ring buffer
 * (forceAudioIn.so's AI_LATENCY_TARGET_FRAMES/TRIGGER_FRAMES) to absorb the
 * residual. Less clever, but its failure mode is a slow, rare, well-
 * understood buffer trim rather than an unpredictable feedback loop -
 * don't reintroduce adaptive correction without a properly isolated
 * measurement session first (not live trial-and-error). */
constexpr double RATE_CORRECTION = 44100.0 / (44100.0 - 45.0);   /* ~1.00102 */

static void timer_loop() {
    constexpr int MAX_FRAMES = 2048;   /* generous headroom over a ~128-frame nominal block */
    int16_t  pcm[MAX_FRAMES * 2];
    float    flt[MAX_FRAMES * 2];
    const auto period = std::chrono::microseconds(1451); /* half a 128-frame block: render more often than strictly needed so a single slow wake doesn't need a huge catch-up render */

    /* REVERTED 2026-09-12: this used to request SCHED_FIFO priority 10 here,
     * on the theory that MPC's own real-time audio threads on this
     * PREEMPT_RT kernel could otherwise starve this thread of scheduling.
     * The request itself succeeded (logged "SCHED_FIFO priority 10") and
     * live diagnostics showed clean 1.6-1.7ms wake gaps with zero late
     * wakes - but shortly after, on real hardware, the Force's pads/knobs
     * went unresponsive and WiFi dropped, the same symptom this project's
     * own gotchas.md documents for a runaway real-time thread starving
     * OTHER system threads (networking included) on a shared RT kernel.
     * A clean-looking wake-gap log does not rule that out: it only shows
     * THIS thread was scheduled promptly, not what it may have displaced.
     * Do not re-add SCHED_FIFO here without a much more conservative,
     * separately-verified approach - stay on plain SCHED_OTHER. */

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    auto last_stat = prev;

    while (g_run.load()) {
        std::this_thread::sleep_for(period);
        auto now = clock::now();
        double secs = std::chrono::duration<double>(now - prev).count();
        prev = now;

        double ms = secs * 1000.0;
        double seen_max = g_max_wake_ms.load();
        if (ms > seen_max) g_max_wake_ms.store(ms);
        g_total_wakes++;

        int frames = (int)std::lround(secs * MOVE_SAMPLE_RATE * RATE_CORRECTION);
        if (frames < 1) frames = 1;
        if (frames > 400) g_late_wakes++;               /* > ~9ms since last wake */
        if (frames > MAX_FRAMES) frames = MAX_FRAMES;   /* clamp a huge stall */

        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_api->render_block(g_inst, pcm, frames);
        }
        for (int i = 0; i < frames * 2; i++) flt[i] = pcm[i] / 32768.0f;
        ring_push(flt, (uint32_t)frames);

        if (now - last_stat >= std::chrono::seconds(5)) {
            last_stat = now;
            uint32_t backlog = g_shm_in ? (uint32_t)((g_shm_in->head - __atomic_load_n(&g_shm_in->tail, __ATOMIC_ACQUIRE))
                                                   & (AI_RING_FRAMES - 1))
                                      : 0;
            fprintf(stderr, "[maze] render thread: max wake gap %.1fms, %llu/%llu wakes > 9ms, ring drops %llu, "
                            "backlog %u frames\n",
                    g_max_wake_ms.load(),
                    (unsigned long long)g_late_wakes.load(), (unsigned long long)g_total_wakes.load(),
                    (unsigned long long)g_ring_drops.load(), backlog);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Control socket -- a UNIX domain socket instead of MIDI CC (force-acid's
 * approach): the web UI is the primary control surface here, and a plain
 * local socket is simpler than round-tripping through ALSA CC for something
 * that never needs to be a hardware knob. One connection per request, plain
 * newline-terminated text protocol:
 *
 *   SET <key> <value>\n   -> "OK\n" or "ERR\n"
 *   GET <key>\n           -> "<value>\n" or "ERR\n"
 *   DESCRIBE\n            -> the module's chain_params JSON, one line
 *   NOTE <note> <vel>\n   -> trigger a note (web UI "audition" button)
 *
 * "mix.*" keys are host-level output-mix controls (voice on/off, volume,
 * L/R/L+R routing) that live in the shared-memory struct forceAudioIn.so
 * reads directly - see forceAudioInject.h. They are intercepted here rather
 * than forwarded to g_api->set_param/get_param, which only knows maze_voice
 * .c's own chain_params (module.json) and would just error on an unknown
 * key. There is deliberately no separate "mixer" service: each voice's own
 * control socket / web panel owns its own mix state.
 * ------------------------------------------------------------------------- */
/* mix.dest indices, in DEST_LABELS order below. */
enum { DEST_IN1 = 0, DEST_IN2 = 1, DEST_OUT3 = 2, DEST_OUT4 = 3, DEST_IN12 = 4, DEST_OUT34 = 5 };
static const char *DEST_LABELS[] = { "IN1", "IN2", "OUT3", "OUT4", "IN1,2", "OUT3,4" };
static int g_dest_idx = DEST_IN12;   /* default: stereo into Audio-In 1,2, matches prior AI_CHAN_LR default */

/* Applies a destination: exactly one of g_shm_in/g_shm_out is `enabled`
 * (mixed by forceAudioJack.so), the other stays attached-but-muted so
 * switching destinations never resumes from a stale backlog (see
 * ring_push). channel_mask picks L/R/both within whichever bus is live. */
static void apply_dest(int idx) {
    g_dest_idx = idx;
    bool is_out = (idx == DEST_OUT3 || idx == DEST_OUT4 || idx == DEST_OUT34);
    g_shm_in->enabled  = is_out ? 0u : 1u;
    g_shm_out->enabled = is_out ? 1u : 0u;
    uint32_t mask = (idx == DEST_IN1 || idx == DEST_OUT3) ? AI_CHAN_L
                  : (idx == DEST_IN2 || idx == DEST_OUT4) ? AI_CHAN_R
                  : AI_CHAN_LR;
    (is_out ? g_shm_out : g_shm_in)->channel_mask = mask;
}

static bool handle_mix_set(const std::string &key, const std::string &val) {
    if (key == "mix.enabled") {
        /* Kept for backward-compat with older control-socket callers; no
         * current GUI surfaces a separate voice on/off (the engine on/off
         * switch already covers that) - 0 mutes both rings, 1 restores
         * whichever destination was last selected. */
        if (val == "1" || val == "true") apply_dest(g_dest_idx);
        else { g_shm_in->enabled = 0u; g_shm_out->enabled = 0u; }
        return true;
    }
    if (key == "mix.gain") {
        /* wire value is percent (0..~150, matching every other level knob in
         * this UI) - forceAudioIn.so wants a plain linear multiplier. */
        float g = std::strtof(val.c_str(), nullptr) / 100.0f;
        g_shm_in->gain = g;
        g_shm_out->gain = g;
        return true;
    }
    if (key == "mix.dest_idx") {   /* shadow GUI: enum index */
        int idx = std::atoi(val.c_str());
        if (idx < 0 || idx > DEST_OUT34) return false;
        apply_dest(idx);
        return true;
    }
    if (key == "mix.dest") {   /* web GUI: label */
        for (int i = 0; i <= DEST_OUT34; i++) {
            if (val == DEST_LABELS[i]) { apply_dest(i); return true; }
        }
        return false;
    }
    return false;
}
static bool handle_mix_get(const std::string &key, std::string &out) {
    if (key == "mix.enabled") { out = (g_shm_in->enabled || g_shm_out->enabled) ? "1" : "0"; return true; }
    if (key == "mix.gain") {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", g_shm_in->gain * 100.0f);
        out = b; return true;
    }
    if (key == "mix.dest_idx") { out = std::to_string(g_dest_idx); return true; }
    if (key == "mix.dest") { out = DEST_LABELS[g_dest_idx]; return true; }
    return false;
}

static void handle_ctrl_line(int fd, const std::string &line) {
    char cmd[16] = {0}, key[64] = {0}, val[256] = {0};
    if (sscanf(line.c_str(), "%15s", cmd) != 1) { send(fd, "ERR\n", 4, 0); return; }

    if (!strcmp(cmd, "DESCRIBE")) {
        std::string reply = g_chain_params_json + "\n";
        send(fd, reply.c_str(), reply.size(), 0);
        return;
    }
    if (!strcmp(cmd, "SET") && sscanf(line.c_str(), "%*s %63s %255[^\n]", key, val) == 2) {
        if (handle_mix_set(key, val)) { send(fd, "OK\n", 3, 0); return; }
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->set_param(g_inst, key, val);
        send(fd, "OK\n", 3, 0);
        return;
    }
    if (!strcmp(cmd, "GET") && sscanf(line.c_str(), "%*s %63s", key) == 1) {
        std::string mix_val;
        if (handle_mix_get(key, mix_val)) {
            std::string reply = mix_val + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        char buf[256];
        int n;
        { std::lock_guard<std::mutex> lk(g_lock);
          n = g_api->get_param(g_inst, key, buf, sizeof(buf)); }
        if (n <= 0) { send(fd, "ERR\n", 4, 0); return; }
        std::string reply(buf, n); reply += "\n";
        send(fd, reply.c_str(), reply.size(), 0);
        return;
    }
    if (!strcmp(cmd, "NOTE")) {
        int note = 60, vel = 100;
        sscanf(line.c_str(), "%*s %d %d", &note, &vel);
        uint8_t on[3]  = { 0x90, (uint8_t)note, (uint8_t)vel };
        uint8_t off[3] = { 0x80, (uint8_t)note, 0 };
        { std::lock_guard<std::mutex> lk(g_lock);
          g_api->on_midi(g_inst, on, 3, 0); }
        std::thread([off]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::lock_guard<std::mutex> lk(g_lock);
            g_api->on_midi(g_inst, off, 3, 0);
        }).detach();
        send(fd, "OK\n", 3, 0);
        return;
    }
    send(fd, "ERR\n", 4, 0);
}

static void ctrl_server_loop(int lfd) {
    while (g_run.load()) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        char buf[512];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string line(buf);
            size_t nl = line.find('\n');
            if (nl != std::string::npos) line.resize(nl);
            if (g_verbose) fprintf(stderr, "[maze] ctrl: %s\n", line.c_str());
            handle_ctrl_line(cfd, line);
        }
        close(cfd);
    }
}

static int ctrl_socket_listen(const std::string &path) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); close(fd); return -1; }
    chmod(path.c_str(), 0666);
    if (listen(fd, 8) != 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
static void on_signal(int) { g_run.store(false); }

static void usage(const char *me) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -v                    verbose\n"
        "  --client NAME         ALSA client name       (default: Maze)\n"
        "  --module-dir PATH     dir containing module.json (default: .)\n"
        "  --ctrl-sock PATH      control socket path     (default: /tmp/maze_ctrl.sock)\n"
        "  --control-channel N   1-16, CC-in for the Q-Link track (default: 1)\n"
        "  --mix-slot N          voice slot 0..%d for forceAudioIn.so (default: 0) -\n"
        "                        each simultaneous voice needs a distinct slot\n",
        me, AI_MAX_VOICES - 1);
}

int main(int argc, char **argv) {
    std::string client = "Maze";
    std::string module_dir = ".";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-v")                        g_verbose = true;
        else if (a == "--client"     && i+1 < argc) client = argv[++i];
        else if (a == "--module-dir" && i+1 < argc) module_dir = argv[++i];
        else if (a == "--ctrl-sock"  && i+1 < argc) g_ctrl_sock_path = argv[++i];
        else if (a == "--control-channel" && i+1 < argc) g_ctrl_ch = (std::atoi(argv[++i]) - 1) & 0x0F;
        else if (a == "--mix-slot" && i+1 < argc) {
            int s = std::atoi(argv[++i]);
            if (s < 0 || s >= AI_MAX_VOICES) { usage(argv[0]); return 2; }
            g_mix_slot = (unsigned)s;
        }
        else { usage(argv[0]); return (a == "-h" || a == "--help") ? 0 : 2; }
    }

    for (int i = 0; i < N_PARAMS; i++) g_cc2param[PARAMS[i].cc] = i;

    if (!shm_setup()) { fprintf(stderr, "[maze] shared memory setup failed\n"); return 1; }

    g_api = move_plugin_init_v2(nullptr);
    if (!g_api || g_api->api_version != 2) {
        fprintf(stderr, "[maze] core init failed\n"); return 1;
    }
    g_inst = g_api->create_instance(module_dir.c_str(), nullptr);
    if (!g_inst) { fprintf(stderr, "[maze] create_instance failed\n"); return 1; }

    {
        static char buf[65536];
        int n = g_api->get_param(g_inst, "chain_params", buf, sizeof(buf));
        g_chain_params_json = (n > 0 && n < (int)sizeof(buf)) ? std::string(buf, n) : std::string("{}");
        if (n <= 0)
            fprintf(stderr, "[maze] warning: chain_params not found (module.json missing from %s?)\n",
                    module_dir.c_str());
    }

    RtMidiIn *in = nullptr;
    try {
        in = new RtMidiIn(RtMidi::UNSPECIFIED, client, 256);
        in->openVirtualPort("In (Mockba)");
        in->ignoreTypes(true, false, true);   /* sysex off, TIMING ON (LFO tempo sync), active-sense off */
        in->setCallback(&on_midi_cb, nullptr);
    } catch (RtMidiError &e) {
        fprintf(stderr, "[maze] MIDI setup failed: %s\n", e.getMessage().c_str());
        return 1;
    }

    int lfd = ctrl_socket_listen(g_ctrl_sock_path);
    if (lfd < 0) { fprintf(stderr, "[maze] control socket setup failed\n"); return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    fprintf(stderr,
        "[maze] up. port '%s:In (Mockba)'  ctrl socket %s  shm %s/%s  ctrl ch %d\n"
        "[maze] route a MIDI track to '%s:In (Mockba)' for notes and CC (Q-Link); audio\n"
        "[maze] is mixed into the Force's capture input via ForceAudioIn (must be enabled).\n",
        client.c_str(), g_ctrl_sock_path.c_str(), g_shm_name_in, g_shm_name_out, g_ctrl_ch + 1, client.c_str());

    std::thread timer(timer_loop);
    std::thread ctrl(ctrl_server_loop, lfd);

    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    timer.join();
    close(lfd);
    unlink(g_ctrl_sock_path.c_str());
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->destroy_instance(g_inst);
    }
    delete in;
    if (g_shm_in)  { munmap(g_shm_in, AI_SHM_BYTES); shm_unlink(g_shm_name_in); }
    if (g_shm_out) { munmap(g_shm_out, AI_SHM_BYTES); shm_unlink(g_shm_name_out); }
    fprintf(stderr, "[maze] bye\n");
    return 0;
}
