/*
 * test_op.c - unit tests for the Object Processor compositor.
 *
 * Builds a synthetic object list + bitmap data in a fresh memory image, renders
 * it, and checks framebuffer pixels. Uses RGB16 mode (VMODE bit1 set) so CLUT
 * entries pass through to exact RGB565 values, making pixel checks deterministic
 * independent of the CRY approximation.
 *
 * Build: gcc -std=c11 src/m68k.c src/memory.c src/op.c tests/test_op.c -o test_op
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/op.h"
#include "../src/memory.h"
#include <stdio.h>
#include <string.h>

static onca_mem_t mem;
static m68k_bus_t  bus;

static void w8 (uint32_t a, uint8_t v)  { bus.write8 (bus.ctx, a, v); }
static void w16(uint32_t a, uint16_t v) { bus.write16(bus.ctx, a, v); }
static void w32(uint32_t a, uint32_t v) { bus.write32(bus.ctx, a, v); }
static void wphrase(uint32_t a, uint64_t p) { w32(a, (uint32_t)(p >> 32)); w32(a + 4, (uint32_t)p); }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define W 64
#define H 8
static uint16_t fb[W * H];

static void t_bitmap_clut(void) {
    onca_mem_init(&mem);
    onca_mem_bind(&mem, &bus);

    w16(0xF00028, 0x0003);   /* VMODE: VIDEN + bit1 (RGB16, CLUT passes through) */
    w16(0xF00046, 0x0000);   /* VDB = 0  -> top line = ypos/2 */
    w16(0xF00058, 0x001F);   /* BG = blue */

    /* CLUT entries 1..3 */
    w16(0xF00402, 0xF800);   /* red   */
    w16(0xF00404, 0x07E0);   /* green */
    w16(0xF00406, 0xFFFF);   /* white */

    /* object list @ 0x40000 -> OLP (lo @F00020, hi @F00022) */
    w16(0xF00020, 0x0000);
    w16(0xF00022, 0x0004);

    /* object 0: 8bpp bitmap, ypos=0, height=2, xpos=5, iwidth=dwidth=1 */
    uint32_t link19 = 0x40020 >> 3, data21 = 0x50000 >> 3;
    uint64_t p0 = (uint64_t)OP_OBJ_BITMAP
                | ((uint64_t)0 << 3)               /* ypos   */
                | ((uint64_t)2 << 14)              /* height */
                | ((uint64_t)link19 << 24)
                | ((uint64_t)data21 << 43);
    uint64_t p1 = (uint64_t)(5 + 14)               /* xpos (raster: +14 origin) */
                | ((uint64_t)3 << 12) | ((uint64_t)1 << 15)              /* depth 3 = 8bpp */
                | ((uint64_t)1 << 18)              /* dwidth */
                | ((uint64_t)1 << 28)              /* iwidth */
                | ((uint64_t)1 << 47);             /* TRANS: colour 0 transparent */
    wphrase(0x40000, p0);
    wphrase(0x40008, p1);
    wphrase(0x40020, (uint64_t)OP_OBJ_STOP);       /* STOP */

    /* bitmap data @ 0x50000: line0 = {1,2,3,0,...}, line1 = {3,3,...} */
    w8(0x50000, 1); w8(0x50001, 2); w8(0x50002, 3); w8(0x50003, 0);
    w8(0x50008, 3); w8(0x50009, 3);

    int drawn = onca_op_render(&mem, fb, W, H);

    /* CLUT/BG values are Jaguar RGB16 (red 15-11, blue 10-6, green 5-0); the
     * renderer converts to standard RGB565. $001F = pure green -> $03E0;
     * $07E0 = blue + half green -> $041F; $F800/$FFFF map to themselves. */
    CHECK(drawn == 1, "drawn=%d", drawn);
    CHECK(fb[0] == 0x03E0, "corner should be BG, got %04X", fb[0]);
    CHECK(fb[0 * W + 5] == 0xF800, "(5,0)=red got %04X", fb[5]);
    CHECK(fb[0 * W + 6] == 0x041F, "(6,0) converted got %04X", fb[6]);
    CHECK(fb[0 * W + 7] == 0xFFFF, "(7,0)=white got %04X", fb[7]);
    CHECK(fb[0 * W + 8] == 0x03E0, "(8,0) index0 transparent -> BG got %04X", fb[8]);
    CHECK(fb[1 * W + 5] == 0xFFFF, "(5,1)=white got %04X", fb[1 * W + 5]);
    CHECK(fb[1 * W + 7] == 0x03E0, "(7,1) untouched -> BG got %04X", fb[1 * W + 7]);
}

static void t_offscreen_clip(void) {
    onca_mem_init(&mem);
    onca_mem_bind(&mem, &bus);
    w16(0xF00028, 0x0003);
    w16(0xF00046, 0x0000);
    w16(0xF00058, 0x0000);
    w16(0xF00020, 0x0000); w16(0xF00022, 0x0004);

    /* xpos = -4 (12-bit signed 0xFFC): left half clipped, no crash/OOB */
    uint32_t data21 = 0x50000 >> 3;
    uint64_t p0 = (uint64_t)OP_OBJ_BITMAP | ((uint64_t)2 << 14)
                | ((uint64_t)(0x40020 >> 3) << 24) | ((uint64_t)data21 << 43);
 uint64_t p1 = (uint64_t)(14 - 4) | ((uint64_t)3 << 12) | ((uint64_t)1 << 15) | ((uint64_t)1 << 18) | ((uint64_t)1 << 28);
    wphrase(0x40000, p0);
    wphrase(0x40008, p1);
    wphrase(0x40020, (uint64_t)OP_OBJ_STOP);
    for (int i = 0; i < 8; i++) w8(0x50000 + i, 5);
    w16(0xF0040A, 0x1234);   /* CLUT[5] */

    int drawn = onca_op_render(&mem, fb, W, H);
    CHECK(drawn == 1, "clipped object still drawn=%d", drawn);
    /* pixels 0..3 visible (xpos -4 + col 4..7), pixel at x=0 = col4 = index5 */
    CHECK(fb[0] == 0x1688, "(0,0) = CLUT[5] ($1234 Jaguar RGB16 -> $1688) got %04X", fb[0]);
}

/* Canonical license-screen list: BRANCH(past display) / BRANCH(prior) / BITMAP /
 * STOP. Verifies non-taken branches fall through to the bitmap and taken branches
 * skip it, so the bitmap only appears within [vdb, vde]. */
static void t_branch_guard(void) {
    onca_mem_init(&mem);
    onca_mem_bind(&mem, &bus);
    w16(0xF00028, 0x0003);                 /* VMODE RGB16 */
    w16(0xF00046, 0x0004);                 /* VDB = 4 half-lines (display top = line 0) */
    w16(0xF00058, 0x0000);                 /* BG black */
    w16(0xF00020, 0x0000); w16(0xF00022, 0x0004);   /* OLP = 0x40000 */
    w16(0xF00402, 0xF800);                 /* CLUT[1] red */

    /* list: BRANCH(0) / BRANCH(8) / BITMAP(0x10, two phrases) / STOP(0x20) */
    uint32_t stop_addr = 0x40000 + 4 * 8;
    uint32_t link_stop = (stop_addr >> 3) & 0x7FFFF;

    /* BRANCH 0: bottom guard - CC=2 (YPOS < VC), YPOS = vde = 12 half-lines: taken
     * (skip to STOP) when VC > 12, i.e. below the display bottom. */
    uint64_t br0 = (uint64_t)OP_OBJ_BRANCH | ((uint64_t)12 << 3)
                 | ((uint64_t)2 << 14) | ((uint64_t)link_stop << 24);
    /* BRANCH 1: top guard - CC=1 (YPOS > VC), YPOS = vdb = 4 half-lines: taken
     * when VC < 4, i.e. above the display top. */
    uint64_t br1 = (uint64_t)OP_OBJ_BRANCH | ((uint64_t)4 << 3)
                 | ((uint64_t)1 << 14) | ((uint64_t)link_stop << 24);
    /* BITMAP: ypos=4, height=4, xpos=0, 8bpp, iwidth=dwidth=1, link STOP */
    uint64_t bp0 = (uint64_t)OP_OBJ_BITMAP | ((uint64_t)4 << 3) | ((uint64_t)4 << 14)
                 | ((uint64_t)link_stop << 24) | ((uint64_t)(0x50000 >> 3) << 43);
    uint64_t bp1 = (uint64_t)14 | ((uint64_t)3 << 12) | ((uint64_t)1 << 15) | ((uint64_t)1 << 18) | ((uint64_t)1 << 28);

    wphrase(0x40000, br0);
    wphrase(0x40008, br1);
    wphrase(0x40010, bp0);
    wphrase(0x40018, bp1);
    wphrase(stop_addr, (uint64_t)OP_OBJ_STOP);
    for (int i = 0; i < 8; i++) w8(0x50000 + i, 1);   /* line 0 all colour 1 */

    int drawn = onca_op_render(&mem, fb, W, H);
    CHECK(drawn == 1, "branch-guarded bitmap drawn=%d", drawn);
    CHECK(fb[0] == 0xF800, "(0,0) inside display = red got %04X", fb[0]);
    /* rows >= 4 are below vde (bitmap height 4): branch-past guards them to BG */
    CHECK(fb[5 * W] == 0x0000, "(0,5) below display = BG got %04X", fb[5 * W]);
}

static void t_decode16(void) {
    /* Jaguar RGB16: red 15-11, blue 10-6, green 5-0 (Tech Ref video mode 3),
     * converted to standard RGB565 for the framebuffer. */
    CHECK(onca_op_decode16(0x003F, 0) == 0x07E0, "Jaguar pure green -> 565 green");
    CHECK(onca_op_decode16(0x07C0, 0) == 0x001F, "Jaguar pure blue -> 565 blue");
    CHECK(onca_op_decode16(0xF800, 0) == 0xF800, "red maps to itself");
    CHECK(onca_op_decode16(0xFFFF, 0) == 0xFFFF, "white maps to itself");
    CHECK(onca_op_decode16(0xFEFF, 1) != 0, "CRY bright colour is non-black");
    CHECK(onca_op_decode16(0x0000, 1) == 0x0000, "CRY zero is black");
}

int main(void) {
    printf("== Onca Object Processor unit tests ==\n");
    t_bitmap_clut();
    t_offscreen_clip();
    t_branch_guard();
    t_decode16();
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
