/*
 * injectTone — proof-of-concept generator for forceAudioIn.so.
 *
 * Creates the /forceAudioInject shared-memory ring and continuously writes a
 * sine wave into it, paced to real time. forceAudioIn.so (LD_PRELOAD'd into
 * MPC) mixes this into whatever MPC reads from its capture device.
 *
 * This is deliberately dumb - a real MIDI-generator renderer would replace
 * this process, writing rendered note audio into the same ring instead of a
 * fixed tone. The point of this program is only to prove the injection path
 * end-to-end: if the tone shows up in forceAudioIn.so's logged peak/consumed
 * counters (or audibly on an Audio-In track), the mechanism works.
 *
 * Usage: injectTone [freqHz] [gain0to1] [channels(1|2)]
 *
 * BUILD:
 *   zig cc -target arm-linux-gnueabihf.2.39 -O2 \
 *       -o injectTone injectTone.c -lpthread -lrt -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "forceAudioInject.h"

#define GEN_RATE 44100
#define BLOCK_FRAMES 256

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    double freq = (argc > 1) ? atof(argv[1]) : 440.0;
    double gain = (argc > 2) ? atof(argv[2]) : 0.2;
    unsigned channels = (argc > 3) ? (unsigned)atoi(argv[3]) : 1;
    if (channels < 1 || channels > AI_MAX_CH) channels = 1;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    shm_unlink(AI_SHM_NAME); /* start clean - we are the sole producer */
    int fd = shm_open(AI_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, AI_SHM_BYTES) != 0) { perror("ftruncate"); return 1; }
    ai_shm_t *shm = mmap(NULL, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    memset(shm, 0, AI_SHM_BYTES);
    shm->rate = GEN_RATE;
    shm->channels = channels;
    __atomic_store_n(&shm->magic, AI_MAGIC, __ATOMIC_RELEASE);

    printf("[injectTone] writing %.1f Hz tone at gain %.2f, %u ch, %u Hz into %s\n",
           freq, gain, channels, (unsigned)GEN_RATE, AI_SHM_NAME);

    double phase = 0.0;
    const double phase_inc = 2.0 * M_PI * freq / GEN_RATE;
    struct timespec block_time = { 0, (long)(1e9 * BLOCK_FRAMES / GEN_RATE) };
    uint64_t reported = 0;
    struct timespec last_report; clock_gettime(CLOCK_MONOTONIC, &last_report);

    while (g_run) {
        uint32_t head = shm->head;                                   /* sole producer */
        uint32_t tail = __atomic_load_n(&shm->tail, __ATOMIC_ACQUIRE);
        uint32_t space = (AI_RING_FRAMES - 1) - ((head - tail) & (AI_RING_FRAMES - 1));

        if (space < BLOCK_FRAMES) {
            nanosleep(&block_time, NULL);
            continue;
        }

        unsigned i;
        for (i = 0; i < BLOCK_FRAMES; i++) {
            float v = (float)(gain * sin(phase));
            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            uint32_t fr = (head + i) & (AI_RING_FRAMES - 1);
            float *dst = &shm->ring[(size_t)fr * AI_MAX_CH];
            unsigned c;
            for (c = 0; c < channels; c++) dst[c] = v;
        }

        __atomic_store_n(&shm->head, (head + BLOCK_FRAMES) & (AI_RING_FRAMES - 1),
                         __ATOMIC_RELEASE);
        shm->frames_written += BLOCK_FRAMES;

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_report.tv_sec >= 5) {
            printf("[injectTone] produced %llu consumed %llu underruns %llu\n",
                   (unsigned long long)shm->frames_written,
                   (unsigned long long)shm->frames_consumed,
                   (unsigned long long)shm->underruns);
            last_report = now;
            (void)reported;
        }

        nanosleep(&block_time, NULL);
    }

    munmap(shm, AI_SHM_BYTES);
    shm_unlink(AI_SHM_NAME);
    printf("[injectTone] stopped\n");
    return 0;
}
