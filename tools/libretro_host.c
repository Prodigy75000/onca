/*
 * libretro_host.c - minimal libretro frontend that drives the real core exactly
 * like RetroArch: set callbacks, retro_load_game(jagboot.rom), call retro_run in
 * a loop, and inspect the framebuffer the core hands to video_refresh. This is
 * the definitive check of what the shipped .so actually puts on screen.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/libretro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t *g_frame;
static unsigned g_w, g_h;

static bool env_cb(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
    if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        *(const char **)data = "bios";   /* repo bios/ holds jagboot.rom */
        return true;
    }
    return false;
}
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    (void)pitch; g_frame = (const uint16_t *)data; g_w = w; g_h = h;
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p; (void)d; (void)i; (void)id; return 0;
}
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }

static unsigned distinct_colors(uint16_t *seen, unsigned cap) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_w * g_h && n < cap; i++) {
        uint16_t p = g_frame[i];
        unsigned j; for (j = 0; j < n; j++) if (seen[j] == p) break;
        if (j == n) seen[n++] = p;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <jagboot.rom> [frames]\n", argv[0]); return 2; }
    int frames = argc > 2 ? atoi(argv[2]) : 300;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) { return 1; } fclose(f);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_init();

    struct retro_game_info game; memset(&game, 0, sizeof(game));
    game.data = buf; game.size = (size_t)sz; game.path = argv[1];
    if (!retro_load_game(&game)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }

    uint16_t last = 0xEEEE;
    for (int i = 0; i < frames; i++) {
        retro_run();
        if (!g_frame) continue;
        uint16_t seen[64]; unsigned nc = distinct_colors(seen, 64);
        uint16_t c0 = g_frame[g_w * g_h / 2];   /* centre pixel */
        if (c0 != last) {
            printf("frame %4d: centre=%04X  distinct_colors=%u  (%ux%u)\n", i, c0, nc, g_w, g_h);
            last = c0;
        }
    }
    printf("done (%d frames)\n", frames);
    retro_unload_game();
    retro_deinit();
    free(buf);
    return 0;
}
