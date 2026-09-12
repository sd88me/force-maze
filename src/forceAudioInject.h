/*
 * Shared memory layout between the injector process(es) (e.g. injectTone,
 * later a real MIDI-generator renderer) and forceAudioIn.so, the LD_PRELOAD
 * shim that mixes injected audio into what /usr/bin/MPC reads from its
 * capture device.
 *
 * The producer (injector) always writes 32-bit float, mono or stereo, at a
 * fixed rate it declares in the header. forceAudioIn.so does the conversion
 * to whatever format/channel count MPC actually configured on the capture
 * handle - the producer never needs to know or care what MPC is doing.
 */
#ifndef FORCE_AUDIO_INJECT_H
#define FORCE_AUDIO_INJECT_H

#include <stdint.h>

#define AI_SHM_NAME    "/forceAudioInject"
#define AI_MAGIC       0x414e4a49u   /* 'AINJ' */
#define AI_RING_FRAMES (1u << 16)    /* 65536 frames of ring, per channel slot */
#define AI_MAX_CH      2

typedef struct {
    uint32_t magic;
    uint32_t rate;                 /* producer's sample rate, e.g. 44100      */
    uint32_t channels;             /* producer's channel count: 1 or 2        */
    uint32_t reserved;

    /* SPSC ring, in frames (not bytes) - one producer (injector), one
     * consumer (the tap, running on MPC's real-time capture thread). Samples
     * are interleaved float32, `channels` per frame. */
    volatile uint32_t head;        /* producer writes (frames produced)       */
    volatile uint32_t tail;        /* consumer writes (frames consumed)       */

    volatile uint64_t frames_written;   /* producer-side stats               */
    volatile uint64_t frames_consumed;  /* consumer-side stats               */
    volatile uint64_t underruns;        /* consumer had nothing to mix in    */

    float ring[AI_RING_FRAMES * AI_MAX_CH];
} ai_shm_t;

#define AI_SHM_BYTES (sizeof(ai_shm_t))

#endif
