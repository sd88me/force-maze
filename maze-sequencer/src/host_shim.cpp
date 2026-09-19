/* host_shim.cpp — Force/MockbaMod runtime host for the ported Maze Sequencer.
 *
 * Plays the role Move's chain host played for maze_seq.c, but this port
 * turns out simpler than both existing examples (force-acid/src/host_shim.cpp,
 * ../../maze-voice/src/maze_host.cpp): maze_seq.c is a "tool" component that
 * steps entirely off raw MIDI transport bytes fed to its own on_midi
 * (0xF8/0xFA/0xFB/0xFC) and never calls a tick()/render_block() abstraction,
 * so there is no timer thread standing in for an audio callback here at all —
 * just MIDI forwarding both ways, plus CC/socket control:
 *
 *   Schwung host                          this shim
 *   -------------------------------------- --------------------------------
 *   dlopen(dsp.so), move_plugin_init_v2    links maze_seq_core.o, calls it directly
 *   on_midi() per incoming MIDI byte       RtMidi input callback -> on_midi()
 *   host->midi_send_internal(pkt)          writes to our own RtMidi "Out" port
 *   set_param(key, "42") from a knob       CC on the control channel, or a
 *                                          control socket for the web panel
 *   one hard-wired output channel          s1_channel/s2_channel, ALREADY
 *                                          independent in the upstream core
 *                                          (see maze_seq_core.c's header) —
 *                                          nothing to patch here, just wire
 *                                          both through CC + web + .xtk
 *
 * maze_seq_core.c never marks its persisted state dirty on its own (only an
 * explicit set_param(inst,"save","1") does, which on Move presumably came
 * from ui.js — not ported here). So every mutating SET below (CC or control
 * socket) is immediately followed by set_param(inst,"save","1"): the core's
 * own background worker thread (already in maze_seq_core.c, unmodified)
 * picks up the dirty flag and persists within ~2s. No separate periodic-save
 * timer needed.
 *
 * Build: see ../scripts/build.sh (native armhf under QEMU, links -lasound
 * -lpthread, same toolchain as force-acid/../../maze-voice).
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
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "rtmidi/RtMidi.h"

extern "C" {
#include "maze_seq_host.h"
}

/* ---------------------------------------------------------------------------
 * CC -> set_param table — transcribed from schwung-maze's help.json (the two
 * on-device knob pages: "Seq Ctrl T1" and "Global T2"). All of maze_seq's
 * params are plain integers (the core does atoi() on every value), so there
 * is only one rescale kind here plus a momentary trigger:
 *   kind: 'i' integer linear (CC 0-127 -> [lo,hi], rounded), 'w' write-only
 *         momentary trigger (panic).
 * s1_channel/s2_channel (CC 34/35) are the per-sequencer output channels —
 * see maze_seq_core.c's file header for why these needed no core patch here,
 * unlike schwung-acid's a_channel/b_channel.
 * ------------------------------------------------------------------------- */
struct ParamSpec {
    const char *key;
    char        kind;
    long        lo, hi;
    int         cc;
};

static const ParamSpec PARAMS[] = {
    /* key            kind  lo    hi   CC   (Seq Ctrl page, help.json K1-K8) */
    { "s1_corrupt",   'i',  0,    100, 20 },
    { "s1_cv_range",  'i',  0,    100, 21 },
    { "s1_length",    'i',  1,    8,   22 },
    { "trig_mix",     'i', -63,   64,  23 },
    { "s2_corrupt",   'i',  0,    100, 24 },
    { "s2_cv_range",  'i',  0,    100, 25 },
    { "s2_length",    'i',  1,    8,   26 },
    { "g_reset",      'i',  0,    4,   27 },   /* enum: 1/2/4/8/off bars */

    /* Global page (help.json K1-K6) */
    { "scale",        'i',  0,    11,  30 },   /* enum: 12 scales */
    { "key",          'i',  0,    11,  31 },
    { "note_rate",    'i',  0,    5,   32 },   /* enum: 6 rates */
    { "note_length",  'i',  0,    7,   33 },   /* enum: 8 gate lengths */
    { "s1_channel",   'i',  0,    15,  34 },   /* independent per-seq channel */
    { "s2_channel",   'i',  0,    15,  35 },   /* independent per-seq channel */
    { "transpose",    'i', -48,   48,  36 },
    { "pad_semis",    'i', -60,   60,  37 },

    { "panic",        'w',  0,    1,   38 },
};
static const int N_PARAMS = (int)(sizeof(PARAMS) / sizeof(PARAMS[0]));

/* Step-level ops (s1_flip/s2_flip/s1_adv/s2_adv/s1_len_dec/s2_len_dec/
 * suspend) are control-socket/web-only, not CC-mapped — they're step-button-
 * shaped (need a 0-7 index or a direction), not knob-shaped. */

/* ---------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
static std::atomic<bool>  g_run{true};
static std::mutex         g_lock;          /* serialises every call into the core */
static plugin_api_v2_t   *g_api  = nullptr;
static void              *g_inst = nullptr;
static RtMidiOut          *g_out  = nullptr;

static int         g_ctrl_ch      = 0;      /* 0-based control channel (default 1) */
static bool        g_verbose      = false;
static std::string g_ctrl_sock_path = "/tmp/maze_seq_ctrl.sock";

static std::unordered_map<int, int> g_cc2param;  /* CC -> index into PARAMS */

/* ---------------------------------------------------------------------------
 * host_api_v1_t: the ONE callback maze_seq_core.c ever calls.
 * ------------------------------------------------------------------------- */
static int host_send_internal(const uint8_t *msg, int len) {
    if (!g_out || !msg || len <= 0) return 0;
    std::vector<unsigned char> m(msg, msg + len);
    try { g_out->sendMessage(&m); } catch (...) { return 0; }
    return len;
}

/* ---------------------------------------------------------------------------
 * CC -> set_param, then mark state dirty (see file header).
 * ------------------------------------------------------------------------- */
static void apply_cc(int idx, int value /* 0..127 */) {
    const ParamSpec &p = PARAMS[idx];
    char buf[32];

    if (p.kind == 'w') {
        if (value < 64) return;   /* only the press, not the release */
        std::snprintf(buf, sizeof(buf), "1");
    } else {
        long v = std::lround(p.lo + (p.hi - p.lo) * (value / 127.0));
        if (v < p.lo) v = p.lo;
        if (v > p.hi) v = p.hi;
        std::snprintf(buf, sizeof(buf), "%ld", v);
    }

    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->set_param(g_inst, p.key, buf);
        g_api->set_param(g_inst, "save", "1");
    }
    if (g_verbose) std::fprintf(stderr, "[maze-seq] cc %d -> %s = %s\n", p.cc, p.key, buf);
}

/* ---------------------------------------------------------------------------
 * RtMidi input — CC on the control channel is consumed here; everything else
 * (transport 0xF8/0xFA/0xFB/0xFC, and anything the core doesn't act on) goes
 * straight to on_midi, exactly as maze_seq_core.c expects to receive it.
 * ------------------------------------------------------------------------- */
static void on_midi_cb(double /*dt*/, std::vector<unsigned char> *msg, void * /*ud*/) {
    if (!msg || msg->empty()) return;
    const uint8_t *b = msg->data();
    size_t len = msg->size();
    uint8_t status = b[0];
    uint8_t type   = status & 0xF0;
    uint8_t chan   = status & 0x0F;

    if (type == 0xB0 && len >= 3 && chan == (uint8_t)g_ctrl_ch) {
        auto it = g_cc2param.find(b[1]);
        if (it != g_cc2param.end()) apply_cc(it->second, b[2]);
        return;   /* CC never reaches maze_seq_core.c - it has no CC handling of its own */
    }

    std::lock_guard<std::mutex> lk(g_lock);
    g_api->on_midi(g_inst, b, (int)len, 0 /* source: unused by the core */);
}

/* ---------------------------------------------------------------------------
 * Control socket — same shape as ../../maze-voice/src/maze_host.cpp's:
 * one connection per request, newline-terminated text protocol.
 *
 *   SET <key> <value>\n   -> "OK\n" or "ERR\n"  (also marks state dirty)
 *   GET <key>\n           -> "<value>\n" or "ERR\n"
 *
 * The web panel uses GET/SET for every knob AND for step-level ops
 * (s1_flip/s1_adv/s1_len_dec/... - see maze_seq_core.c's set_param) and
 * GET state for the step-grid/play-head poller (maze_seq_core.c's
 * build_state_json). No NOTE/DESCRIBE command: nothing to audition, no
 * chain_params to introspect (this is a "tool", not a chainable module).
 * ------------------------------------------------------------------------- */
static void handle_ctrl_line(int fd, const std::string &line) {
    char cmd[16] = {0}, key[64] = {0}, val[256] = {0};
    if (sscanf(line.c_str(), "%15s", cmd) != 1) { send(fd, "ERR\n", 4, 0); return; }

    if (!strcmp(cmd, "SET") && sscanf(line.c_str(), "%*s %63s %255[^\n]", key, val) == 2) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->set_param(g_inst, key, val);
        if (strcmp(key, "save") != 0) g_api->set_param(g_inst, "save", "1");
        send(fd, "OK\n", 3, 0);
        if (g_verbose) fprintf(stderr, "[maze-seq] ctrl SET %s = %s\n", key, val);
        return;
    }
    if (!strcmp(cmd, "GET") && sscanf(line.c_str(), "%*s %63s", key) == 1) {
        char buf[512];
        int n;
        /* Name lists for the shadow page's list widgets (scale/key pickers). */
        static const char SCALE_NAMES[] = "[{\"name\":\"Chromatic\"},{\"name\":\"Major\"},{\"name\":\"Minor\"},{\"name\":\"Pent Maj\"},{\"name\":\"Pent Min\"},{\"name\":\"Mel Min\"},{\"name\":\"Harm Min\"},{\"name\":\"Whole\"},{\"name\":\"Hirajoshi\"},{\"name\":\"Major 7\"},{\"name\":\"Minor 7\"},{\"name\":\"Unquant\"}]\n";
        static const char KEY_NAMES[] = "[{\"name\":\"C\"},{\"name\":\"C Sharp\"},{\"name\":\"D\"},{\"name\":\"D Sharp\"},{\"name\":\"E\"},{\"name\":\"F\"},{\"name\":\"F Sharp\"},{\"name\":\"G\"},{\"name\":\"G Sharp\"},{\"name\":\"A\"},{\"name\":\"A Sharp\"},{\"name\":\"B\"}]\n";
        if (!strcmp(key, "scale_names")) { send(fd, SCALE_NAMES, sizeof(SCALE_NAMES) - 1, 0); return; }
        if (!strcmp(key, "key_names"))   { send(fd, KEY_NAMES, sizeof(KEY_NAMES) - 1, 0); return; }
        { std::lock_guard<std::mutex> lk(g_lock);
          n = g_api->get_param(g_inst, key, buf, sizeof(buf));
          /* Knob params are set-only in the core: fall back to the flat
           * "state" JSON (same as web/server.py) so every key is readable. */
          if (n <= 0) {
              char sb[512];
              int sn = g_api->get_param(g_inst, "state", sb, sizeof(sb));
              if (sn > 0) {
                  sb[sn < (int)sizeof(sb) ? sn : (int)sizeof(sb) - 1] = 0;
                  std::string pat = std::string("\"") + key + "\":\"";
                  const char *p = strstr(sb, pat.c_str());
                  if (p) {
                      p += pat.size();
                      const char *e = strchr(p, '"');
                      if (e && (size_t)(e - p) < sizeof(buf)) { memcpy(buf, p, (size_t)(e - p)); n = (int)(e - p); }
                  }
              }
          } }
        if (n <= 0) { send(fd, "ERR\n", 4, 0); return; }
        std::string reply(buf, n); reply += "\n";
        send(fd, reply.c_str(), reply.size(), 0);
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
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  -v                    verbose (log every param change)\n"
        "  --client NAME         ALSA client name       (default: Maze Seq)\n"
        "  --module-dir PATH     dir to persist maze_seq.bin in (default: .)\n"
        "  --ctrl-sock PATH      control socket path     (default: /tmp/maze_seq_ctrl.sock)\n"
        "  --control-channel N   1-16, CC-in             (default: 1)\n",
        me);
}

int main(int argc, char **argv) {
    std::string client = "Maze Seq";
    std::string module_dir = ".";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-v")                              g_verbose = true;
        else if (a == "--client"          && i+1 < argc) client = argv[++i];
        else if (a == "--module-dir"      && i+1 < argc) module_dir = argv[++i];
        else if (a == "--ctrl-sock"       && i+1 < argc) g_ctrl_sock_path = argv[++i];
        else if (a == "--control-channel" && i+1 < argc) g_ctrl_ch = (std::atoi(argv[++i]) - 1) & 0x0F;
        else { usage(argv[0]); return (a == "-h" || a == "--help") ? 0 : 2; }
    }

    for (int i = 0; i < N_PARAMS; i++) g_cc2param[PARAMS[i].cc] = i;

    static host_api_v1_t host = { host_send_internal };
    g_api = move_plugin_init_v2(&host);
    if (!g_api || g_api->api_version != MOVE_PLUGIN_API_VERSION_2) {
        std::fprintf(stderr, "[maze-seq] core init failed\n"); return 1;
    }
    g_inst = g_api->create_instance(module_dir.c_str(), nullptr);
    if (!g_inst) { std::fprintf(stderr, "[maze-seq] create_instance failed\n"); return 1; }

    RtMidiIn *in = nullptr;
    try {
        in = new RtMidiIn(RtMidi::UNSPECIFIED, client, 256);
        g_out = new RtMidiOut(RtMidi::UNSPECIFIED, client);
        in->openVirtualPort("In (Mockba)");
        g_out->openVirtualPort("Out (Mockba)");
        in->ignoreTypes(true, false, true);   /* sysex off, TIMING ON (0xF8 clock), sensing off */
        in->setCallback(&on_midi_cb, nullptr);
    } catch (RtMidiError &e) {
        std::fprintf(stderr, "[maze-seq] MIDI setup failed: %s\n", e.getMessage().c_str());
        return 1;
    }

    int lfd = ctrl_socket_listen(g_ctrl_sock_path);
    if (lfd < 0) { std::fprintf(stderr, "[maze-seq] control socket setup failed\n"); return 1; }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::fprintf(stderr,
        "[maze-seq] up. port '%s:In (Mockba)' / '%s:Out (Mockba)'  ctrl ch %d  ctrl sock %s  state file %s/maze_seq.bin\n"
        "[maze-seq] connect Force transport SYNC+CLOCK to '%s:In (Mockba)', route a MIDI track\n"
        "[maze-seq] to it on ch %d for CC control, and instrument track(s) FROM '%s:Out (Mockba)'\n"
        "[maze-seq] on whichever channel(s) s1_channel/s2_channel are set to.\n",
        client.c_str(), client.c_str(), g_ctrl_ch + 1, g_ctrl_sock_path.c_str(), module_dir.c_str(),
        client.c_str(), g_ctrl_ch + 1, client.c_str());

    std::thread ctrl(ctrl_server_loop, lfd);

    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close(lfd);
    unlink(g_ctrl_sock_path.c_str());
    ctrl.detach();   /* blocked in accept() on a now-closed fd; process is exiting anyway */
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->destroy_instance(g_inst);   /* sends note-offs + final save internally */
    }
    delete in;
    delete g_out;
    std::fprintf(stderr, "[maze-seq] bye\n");
    return 0;
}
