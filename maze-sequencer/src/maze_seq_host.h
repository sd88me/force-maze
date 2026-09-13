/* maze_seq_host.h — minimal host-ABI shim for the ported Maze Sequencer.
 *
 * schwung-maze's maze_seq.c was written against one Schwung header
 * (plugin_api_v1.h), whose real host_api_v1_t is a big struct (audio
 * constants, mailbox pointers, ~10 callbacks) meant for Move's chain host.
 * maze_seq.c only ever dereferences ONE field of it —
 * `L->host->midi_send_internal` (grep the core to confirm) — so this header
 * re-declares just that, with the same field name, plus the plugin_api_v2_t
 * vtable shape maze_seq.c populates via designated initializers (field
 * names must match; declaration order doesn't).
 *
 * Both maze_seq_core.c and host_shim.cpp compile against THIS header, and
 * host_shim.cpp is the only thing that ever constructs a host_api_v1_t or
 * calls move_plugin_init_v2 — there is no real Move host anywhere in this
 * process, so the two sides only ever need to agree with each other, not
 * with Move's actual (much larger) struct layout.
 *
 * If you ever need a symbol Schwung's real headers have and this one
 * doesn't, add it here — do not pull in the full upstream plugin_api_v1.h,
 * it drags in the whole audio-plugin ABI this module never uses.
 */
#ifndef MAZE_SEQ_HOST_H
#define MAZE_SEQ_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- host -> core -------------------------------------------------------- */
typedef struct host_api_v1 {
    int (*midi_send_internal)(const uint8_t *msg, int len);
} host_api_v1_t;

/* --- core -> host (plugin_api_v2, from Schwung's plugin_api_v1.h) -------- */
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void  (*destroy_instance)(void *instance);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int   (*get_error)(void *instance, char *buf, int buf_len);
    void  (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* Exported by maze_seq_core.c; called once by the shim at startup. */
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

#ifdef __cplusplus
}
#endif

#endif /* MAZE_SEQ_HOST_H */
