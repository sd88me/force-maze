/*
 * forceAudioIn.so — LD_PRELOAD audio injector for the Akai Force's MPC
 * application. The inverse of MockbaMod's forceStream.so (which taps
 * snd_pcm_writei to EXTRACT what MPC plays): this hooks snd_pcm_readi to
 * MIX synthesized audio INTO what MPC reads from its capture device, so a
 * generator process elsewhere on the box can appear as live signal on an
 * Audio-In track without any hardware loopback cable.
 *
 * WHY THIS EXISTS
 *   MPC opens the ADA2 codec with raw hw: device names and holds capture
 *   exclusively, same as playback (confirmed live: fd open on
 *   /dev/snd/pcmC2D0c continuously, independent of whether anything is
 *   record-armed). There is no ALSA loopback (snd-aloop is not built into
 *   this kernel) and no JACK running, so the only seam is the one
 *   forceStream.so already uses: LD_PRELOAD symbol interposition inside the
 *   live process, against libasound's stable public ABI (not raw addresses
 *   in the closed MPC binary, so this is NOT firmware-version-pinned the
 *   way mockbaMagic's binary patching is).
 *
 * WHAT IT DOES
 *   A separate process (e.g. injectTone, or eventually a real MIDI-generator
 *   renderer) writes interleaved float32 audio into a POSIX shared-memory
 *   ring (forceAudioInject.h). Every time MPC calls snd_pcm_readi on the
 *   capture handle, this shim lets the real hardware read happen first, then
 *   ADDS (mixes) samples popped from that ring into the buffer before
 *   returning it to MPC. Real hardware input keeps working unmodified.
 *
 * SAFETY CONTRACT (same as forceStream.so)
 *   - Never breaks capture: on ANY failure the real read result is returned
 *     untouched. Missing shared memory = pure passthrough, not an error.
 *   - Audio-thread path does bounds checks, format conversion and adds -
 *     no allocation, no syscalls, no file I/O, no logging. Shared-memory
 *     open, format detection and diagnostics-thread creation all happen in
 *     a library constructor at load time.
 *   - Ring underrun leaves the real captured audio untouched and bumps a
 *     counter. It never blocks MPC waiting for the injector.
 *   - Disabled by default: with no /forceAudioInject shared memory segment
 *     present this is a no-op tap that only writes a one-line log so you can
 *     confirm it loaded.
 *
 * BUILD (cross-compiled with zig, matching the Force's exact glibc):
 *   zig cc -target arm-linux-gnueabihf.2.39 -shared -fPIC -O2 \
 *       -o forceAudioIn.so forceAudioIn.c -lpthread -lrt
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "forceAudioInject.h"

/* Opaque ALSA types - we never dereference these, so no ALSA headers needed. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#define LOG_PATH "/tmp/forceAudioIn.log"

/* ---- originals ----------------------------------------------------------*/
static snd_pcm_sframes_t (*orig_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*orig_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static int (*orig_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);

static int (*q_get_format)(const snd_pcm_hw_params_t *, int *);
static int (*q_get_channels)(const snd_pcm_hw_params_t *, unsigned int *);
static int (*q_get_rate)(const snd_pcm_hw_params_t *, unsigned int *, int *);
static const char *(*q_pcm_name)(snd_pcm_t *);

/* ---- shared memory (the injection source) --------------------------------
 * Opened once, in the constructor, never touched again except mmap'd memory
 * reads/CAS on the hot path - no syscalls after startup. */
static ai_shm_t *g_shm = NULL;

/* Bounding latency needs HYSTERESIS, not a hard ceiling: a persistent tiny
 * clock-rate mismatch between the producer's wall-clock timer and this
 * consumer's real ALSA-clocked read rate means backlog is ALWAYS drifting
 * toward a hard trim threshold - trimming AT that threshold on every call
 * fires almost continuously once backlog reaches it, and each trim is a
 * small phase discontinuity, i.e. a click. A train of those, many times a
 * second, is exactly "fine at first, then a fast glitching artifact
 * appears" (observed live): clean while backlog is below the ceiling, then
 * constant micro-clicks once drift catches up to it.
 *
 * Trim only once backlog exceeds TRIGGER (~100ms) and drop it all the way
 * down to TARGET (~25ms) when it does - one bigger, much rarer correction
 * instead of continuous small ones. */
/* 2026-09-12: a 25/100ms target/trigger pair looked fine in short live tests
 * (backlog stable, few trims) but under longer play the relationship between
 * the producer's wall-clock render rate and the consumer's real ALSA-clocked
 * read rate turned out to wander in BOTH directions over time, not drift
 * one-way as first measured - backlog was later observed sitting at ~2ms
 * with underruns climbing (hundreds of them, i.e. frequent small dropouts,
 * heard as "glitching a bit"). 25ms of cushion isn't enough margin against
 * that wander. Bigger targets trade a bit more latency for headroom in both
 * directions; ~100/200ms still reads as responsive on a played note and
 * remains far below the original ~1.5s (full-ring) latency bug this
 * mechanism replaced. */
#define AI_LATENCY_TARGET_FRAMES  4410u   /* ~100ms @ 44100Hz - trim DOWN to this */
#define AI_LATENCY_TRIGGER_FRAMES 8820u   /* ~200ms @ 44100Hz - only trim past this */

static uint64_t g_trim_events = 0;   /* diagnostics only, read/logged from diag_main */
static uint64_t g_trim_frames = 0;

/* ---- per-handle capture shape, same pattern as forceStream.so's g_pcms[] */
#define MAX_PCMS 8
static struct {
    void *pcm;
    unsigned frame_bytes, channels, rate;
    int format;
    volatile uint64_t frames;
} g_caps[MAX_PCMS];
static volatile void *g_tap_pcm = NULL;   /* the one capture handle we mix into */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int g_announce_claim = 0;

static void ai_log(const char *fmt, ...)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static unsigned width_of(int fmt)
{
    switch (fmt) {
        case 0:  case 1:  return 1;   /* S8 / U8             */
        case 2:  case 3:  return 2;   /* S16_LE / S16_BE     */
        case 6:  case 7:  return 3;   /* S24_3LE / 3BE       */
        case 10: case 11: return 4;   /* S32_LE / S32_BE     */
        case 14: case 15: return 4;   /* FLOAT_LE / FLOAT_BE */
        default:          return 4;
    }
}

/* Read one interleaved sample at byte offset `off` in `buf`, format `fmt`,
 * as a float in [-1, 1]. */
static inline float sample_to_float(const unsigned char *buf, size_t off, int fmt)
{
    switch (fmt) {
        case 2: { int16_t v; memcpy(&v, buf + off, 2); return v / 32768.0f; }
        case 10: { int32_t v; memcpy(&v, buf + off, 4); return v / 2147483648.0f; }
        case 14: { float v; memcpy(&v, buf + off, 4); return v; }
        default: return 0.0f;
    }
}

/* Write a float in [-1, 1] back as one interleaved sample, clamped. */
static inline void float_to_sample(unsigned char *buf, size_t off, int fmt, float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    switch (fmt) {
        case 2: { int16_t s = (int16_t)(v * 32767.0f); memcpy(buf + off, &s, 2); break; }
        case 10: { int32_t s = (int32_t)(v * 2147483647.0f); memcpy(buf + off, &s, 4); break; }
        case 14: { memcpy(buf + off, &v, 4); break; }
        default: break;
    }
}

/* ---- init ------------------------------------------------------------- */
static void ai_resolve(void)
{
    orig_readi     = dlsym(RTLD_NEXT, "snd_pcm_readi");
    orig_readn     = dlsym(RTLD_NEXT, "snd_pcm_readn");
    orig_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    q_get_format   = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
    q_get_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
    q_get_rate     = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
    q_pcm_name     = dlsym(RTLD_NEXT, "snd_pcm_name");
}

static inline void ensure_init(void) { pthread_once(&g_once, ai_resolve); }

/* Diagnostics only - wakes every ~5s, never touches the hot path directly. */
static void *diag_main(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec ts = {5, 0};
        nanosleep(&ts, NULL);
        if (g_announce_claim) {
            g_announce_claim = 0;
            ai_log("[forceAudioIn] tapping capture handle");
        }
        if (!g_shm) continue;
        int i;
        for (i = 0; i < MAX_PCMS; i++) {
            if (!g_caps[i].pcm) continue;
            ai_log("[forceAudioIn] handle %d: %u ch %u Hz, %llu frames read (%s)",
                   i, g_caps[i].channels, g_caps[i].rate,
                   (unsigned long long)g_caps[i].frames,
                   (g_caps[i].pcm == g_tap_pcm) ? "TAPPED" : "idle");
        }
        {
            uint32_t h = __atomic_load_n(&g_shm->head, __ATOMIC_ACQUIRE);
            uint32_t t = g_shm->tail;
            uint32_t backlog = (h - t) & (AI_RING_FRAMES - 1);
            ai_log("[forceAudioIn] live backlog: %u frames (%.1fms)  trims: %llu events, %llu frames total",
                   backlog, backlog * 1000.0 / (g_shm->rate ? g_shm->rate : 44100),
                   (unsigned long long)g_trim_events, (unsigned long long)g_trim_frames);
        }
        ai_log("[forceAudioIn] injector: %u Hz %u ch, produced %llu consumed %llu underruns %llu",
               g_shm->rate, g_shm->channels,
               (unsigned long long)g_shm->frames_written,
               (unsigned long long)g_shm->frames_consumed,
               (unsigned long long)g_shm->underruns);
    }
    return NULL;
}

__attribute__((constructor))
static void ai_ctor(void)
{
    ai_log("[forceAudioIn] loaded into pid %d", (int)getpid());

    int fd = shm_open(AI_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        ai_log("[forceAudioIn] no %s shared memory - tap is passthrough-only", AI_SHM_NAME);
        return;
    }
    void *m = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        ai_log("[forceAudioIn] mmap failed - tap is passthrough-only");
        return;
    }
    ai_shm_t *shm = (ai_shm_t *)m;
    if (shm->magic != AI_MAGIC) {
        ai_log("[forceAudioIn] bad magic in shared memory - ignoring");
        munmap(m, AI_SHM_BYTES);
        return;
    }
    g_shm = shm;
    ai_log("[forceAudioIn] attached to injector: %u Hz, %u ch", shm->rate, shm->channels);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, diag_main, NULL);
    pthread_attr_destroy(&attr);
}

/* ---- the audio-thread hot path ------------------------------------------
 * Pops up to `frames` frames from the injector ring (converting its fixed
 * float32/channels shape to the capture handle's real format/channel count)
 * and ADDS them into `buffer`, which already holds real captured audio. */
static inline void mix_in(unsigned char *buffer, size_t frames,
                          unsigned dst_channels, unsigned dst_frame_bytes, int dst_format)
{
    if (!g_shm || !frames) return;

    uint32_t head = __atomic_load_n(&g_shm->head, __ATOMIC_ACQUIRE);
    uint32_t tail = g_shm->tail;                       /* we are the only consumer */
    uint32_t avail = (head - tail) & (AI_RING_FRAMES - 1);

    /* Bound latency: a live-audio producer (e.g. maze_host) necessarily
     * starts filling this ring before MPC's process - and therefore this
     * constructor - even exists (it has to win that race), and any tiny
     * clock-rate mismatch between the producer's wall-clock timer and this
     * consumer's real ALSA-clocked read rate accumulates over time either
     * way. Left unchecked, backlog grows toward the full ring (~1.5s) and
     * every note plays back that far behind wall-clock time. We are the
     * ring's sole tail-writer, so it's safe to skip forward past stale
     * backlog here rather than ever letting playback run behind live. */
    if (avail > AI_LATENCY_TRIGGER_FRAMES) {
        uint32_t skip = avail - AI_LATENCY_TARGET_FRAMES;
        tail = (tail + skip) & (AI_RING_FRAMES - 1);
        avail = AI_LATENCY_TARGET_FRAMES;
        g_trim_events++;
        g_trim_frames += skip;
    }

    uint32_t take = (uint32_t)frames;
    if (take > avail) {
        g_shm->underruns++;
        take = avail;                                  /* mix what we have, then stop */
    }
    if (!take) return;

    unsigned src_ch = g_shm->channels ? g_shm->channels : 1;
    unsigned dw = width_of(dst_format);
    uint32_t i;

    for (i = 0; i < take; i++) {
        uint32_t ring_frame = (tail + i) & (AI_RING_FRAMES - 1);
        const float *src = &g_shm->ring[(size_t)ring_frame * AI_MAX_CH];

        unsigned c;
        for (c = 0; c < dst_channels; c++) {
            /* Mono injector -> duplicate to every destination channel.
             * Stereo injector -> map channel c mod src_ch (covers dst mono
             * by folding L+R would need averaging; duplicating ch0 is the
             * simple, good-enough default for a mono/stereo generator). */
            float sv = (src_ch >= 2) ? src[c % src_ch] : src[0];
            size_t off = (size_t)i * dst_frame_bytes + (size_t)c * dw;
            float real = sample_to_float(buffer, off, dst_format);
            float_to_sample(buffer, off, dst_format, real + sv);
        }
    }

    __atomic_store_n(&g_shm->tail, (tail + take) & (AI_RING_FRAMES - 1), __ATOMIC_RELEASE);
    g_shm->frames_consumed += take;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    ensure_init();
    if (!orig_hw_params) return -1;
    int r = orig_hw_params(pcm, params);
    if (r == 0) {
        int fmt = 14; unsigned ch = 2, rate = 44100, dir = 0;
        if (q_get_format)   q_get_format(params, &fmt);
        if (q_get_channels) q_get_channels(params, &ch);
        if (q_get_rate)     q_get_rate(params, &rate, (int *)&dir);
        if (ch >= 1 && ch <= 32 && rate >= 8000 && rate <= 192000) {
            int slot = -1, i;
            for (i = 0; i < MAX_PCMS; i++) {
                if (g_caps[i].pcm == (void *)pcm) { slot = i; break; }
                if (!g_caps[i].pcm && slot < 0) slot = i;
            }
            if (slot >= 0) {
                g_caps[slot].pcm = (void *)pcm;
                g_caps[slot].channels = ch;
                g_caps[slot].rate = rate;
                g_caps[slot].format = fmt;
                g_caps[slot].frame_bytes = ch * width_of(fmt);
            }
            ai_log("[forceAudioIn] configured capture %s: %u Hz, %u ch, fmt %d (%u B/frame)",
                   (q_pcm_name && pcm) ? q_pcm_name(pcm) : "?", rate, ch, fmt, ch * width_of(fmt));
        }
    }
    return r;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
    ensure_init();
    if (!orig_readi) return -1;
    snd_pcm_sframes_t r = orig_readi(pcm, buffer, size);
    if (r > 0) {
        int i, slot = -1;
        for (i = 0; i < MAX_PCMS; i++)
            if (g_caps[i].pcm == (void *)pcm) { slot = i; break; }
        if (slot < 0) return r;                         /* shape unknown - never guess */

        g_caps[slot].frames += (uint64_t)r;

        if (!g_tap_pcm) {
            g_tap_pcm = (void *)pcm;
            g_announce_claim = 1;
        }
        if ((void *)pcm != g_tap_pcm) return r;          /* a second handle - ignore it */

        mix_in((unsigned char *)buffer, (size_t)r,
               g_caps[slot].channels, g_caps[slot].frame_bytes, g_caps[slot].format);
    }
    return r;
}

snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
    ensure_init();
    if (!orig_readn) return -1;
    /* Non-interleaved: pass through untouched, same call this project made
     * for snd_pcm_writen in forceStream.so - MPC uses the interleaved path
     * in practice, and mixing per-plane here would cost more than it's
     * worth on the audio thread until proven otherwise. */
    return orig_readn(pcm, bufs, size);
}
