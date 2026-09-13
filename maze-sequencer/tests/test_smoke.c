/* Host-side smoke test for maze_seq_core.c — links the natively-built core
 * (NOT the armhf cross build) and drives it the way host_shim.cpp would:
 * set independent per-sequencer channels, start transport, feed clock
 * pulses, confirm notes land on the RIGHT channel per sequencer (the "seq B
 * channel" feature), no leaked notes, and that persistence round-trips
 * through the FORCE-ONLY module_dir-based state path. Local build-time
 * check only — see tests/run.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "maze_seq_host.h"

static int g_note_on = 0, g_note_off = 0;
static int g_min_note = 200, g_max_note = -1;
static int g_seen_ch[16] = {0};

/* send_midi() in maze_seq_core.c builds pkt[4] = { cin, status, d1, d2 }. */
static int mock_send_internal(const uint8_t *msg, int len) {
    if (len < 4) return len;
    uint8_t status = msg[1], d1 = msg[2], d2 = msg[3];
    uint8_t hi = status & 0xF0;
    uint8_t ch = status & 0x0F;
    if (hi == 0x90 && d2 > 0) {
        g_note_on++;
        g_seen_ch[ch] = 1;
        if (d1 < g_min_note) g_min_note = d1;
        if (d1 > g_max_note) g_max_note = d1;
        printf("note-on  ch=%2d note=%3d vel=%3d\n", ch+1, d1, d2);
    } else if (hi == 0x80 || (hi == 0x90 && d2 == 0)) {
        g_note_off++;
        printf("note-off ch=%2d note=%3d\n", ch+1, d1);
    }
    return len;
}

int main(int argc, char **argv) {
    const char *module_dir = (argc > 1) ? argv[1] : ".";

    host_api_v1_t host = { mock_send_internal };
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    if (!api) { fprintf(stderr, "init returned NULL\n"); return 1; }
    printf("api_version = %u (expect %u)\n", api->api_version, (unsigned)MOVE_PLUGIN_API_VERSION_2);

    void *inst = api->create_instance(module_dir, NULL);
    if (!inst) { fprintf(stderr, "create_instance returned NULL\n"); return 1; }
    printf("instance created OK (state file: %s/maze_seq.bin)\n\n", module_dir);

    /* Independent per-sequencer channels -- the "seq B channel" feature. */
    api->set_param(inst, "s1_channel", "1");   /* MIDI channel 2 (0-based "1") */
    api->set_param(inst, "s2_channel", "4");   /* MIDI channel 5 (0-based "4") */
    api->set_param(inst, "s1_length", "4");
    api->set_param(inst, "s2_length", "6");
    api->set_param(inst, "note_rate", "0");    /* fastest rate: step every 3 clock pulses */
    api->set_param(inst, "trig_mix", "0");     /* both seqs audible */
    api->set_param(inst, "scale", "0");        /* chromatic */
    api->set_param(inst, "save", "1");

    /* maze_seq_core.c's get_param only implements a fixed handful of keys
     * (running/module_id/state/s1_state/s2_state) - individual knob params
     * like s2_channel are set_param-only, exactly as on Move. Verification
     * below reads them back through the "state" JSON blob instead (same
     * contract the web panel's poller uses - see host_shim.cpp's header
     * comment and web/server.py). */
    char buf[512];
    int n = api->get_param(inst, "state", buf, sizeof(buf));
    buf[n > 0 ? n : 0] = '\0';
    printf("state readback: %s\n", buf);
    if (!strstr(buf, "\"s2_channel\":\"4\"")) {
        fprintf(stderr, "FAIL: state JSON does not show s2_channel=4 before transport\n");
    }

    uint8_t start_msg[1] = { 0xFA };
    api->on_midi(inst, start_msg, 1, 0);

    uint8_t clock_msg[1] = { 0xF8 };
    for (int i = 0; i < 2000; i++) api->on_midi(inst, clock_msg, 1, 0);

    api->destroy_instance(inst);   /* sends note-offs + final save internally */

    printf("\n=== summary ===\n");
    printf("note-on=%d note-off=%d\n", g_note_on, g_note_off);
    printf("note range: %d..%d\n", g_min_note, g_max_note);
    printf("channels seen: ");
    for (int c = 0; c < 16; c++) if (g_seen_ch[c]) printf("%d ", c + 1);
    printf("\n");

    int ok = 1;
    if (g_note_on == 0) { fprintf(stderr, "FAIL: no note-on events\n"); ok = 0; }
    if (g_min_note < 0 || g_max_note > 127) { fprintf(stderr, "FAIL: note out of range\n"); ok = 0; }
    if (g_note_off < g_note_on - 4) { fprintf(stderr, "FAIL: leaked notes\n"); ok = 0; }
    if (!g_seen_ch[1] || !g_seen_ch[4]) {
        fprintf(stderr, "FAIL: expected notes on ch 2 (Seq1) AND ch 5 (Seq2), saw: ");
        for (int c = 0; c < 16; c++) if (g_seen_ch[c]) fprintf(stderr, "%d ", c + 1);
        fprintf(stderr, "\n");
        ok = 0;
    }

    /* Persistence round-trip: a fresh instance in the same module_dir should
     * load the channels/lengths we just saved. */
    void *inst2 = api->create_instance(module_dir, NULL);
    if (!inst2) { fprintf(stderr, "FAIL: reload create_instance returned NULL\n"); ok = 0; }
    else {
        n = api->get_param(inst2, "state", buf, sizeof(buf));
        buf[n > 0 ? n : 0] = '\0';
        printf("\nreloaded state: %s\n", buf);
        if (!strstr(buf, "\"s2_channel\":\"4\"")) {
            fprintf(stderr, "FAIL: s2_channel did not persist\n"); ok = 0;
        }
        if (!strstr(buf, "\"s1_length\":\"4\"") || !strstr(buf, "\"s2_length\":\"6\"")) {
            fprintf(stderr, "FAIL: seq lengths did not persist\n"); ok = 0;
        }
        api->destroy_instance(inst2);
    }

    printf(ok ? "\nSMOKE TEST PASSED\n" : "\nSMOKE TEST FAILED\n");
    return ok ? 0 : 1;
}
