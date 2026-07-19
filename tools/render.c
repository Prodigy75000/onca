/*
 * render.c - headless Object Processor preview.
 *
 * Boots the ROM to its main loop, renders one Object Processor frame, prints a
 * stats summary + an ASCII-art preview of the 320x240 output, and optionally
 * writes a PPM. Lets us "see" the compositor without a display.
 *
 * With the real boot ROM the frame is the background colour: the boot's logo
 * bitmaps are produced by the Blitter + GPU, which are not
 * yet implemented, so the object list points at empty buffers. Pass --demo to
 * inject a synthetic object list and confirm the OP end-to-end.
 *
 * Usage: render <jagboot.rom> [--demo] [--ppm out.ppm] [cycles]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include "../src/bios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 320
#define FB_H 240

static m68k_t cpu;
static onca_mem_t mem;
static uint16_t fb[FB_W * FB_H];
static long g_reached_cart;
static uint32_t g_first_cart_pc;
static int g_forcecart, g_forced;
static uint32_t g_entry;   /* explicit bypass entry; 0 = read from $800404 */
static uint8_t g_vecsnap[0x400];   /* intact exception vectors, pre-crypto */
static int g_snapped, g_prevgpu;
/* ring of recent instructions once forced, to see why the cart bails */
static struct { uint32_t pc; uint16_t op; } g_ring[64];
static int g_rh, g_dumped;
static void cart_tracer(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx;
    if (!g_forced) return;
    g_ring[g_rh & 63].pc = pc & 0xFFFFFF; g_ring[g_rh & 63].op = op; g_rh++;
    uint32_t p = pc & 0xFFFFFF;
    if (g_forced && g_rh > 40 && !g_dumped && p < 0x800000) {   /* left cart -> dump */
        fprintf(stderr, "--- last 40 instrs (cart exec -> exit to %06X) ---\n", p);
        for (int i = g_rh - 40; i < g_rh; i++)
            fprintf(stderr, "  %06X  %04X\n", g_ring[i & 63].pc, g_ring[i & 63].op);
        g_dumped = 1;
    }
}

static void w16(uint32_t a, uint16_t v) { cpu.bus.write16(cpu.bus.ctx, a, v); }
static void w32(uint32_t a, uint32_t v) { cpu.bus.write32(cpu.bus.ctx, a, v); }

/* Inject a synthetic object list: one 16bpp RGB object, a horizontal
 * brightness gradient, so the ASCII preview shows the OP placing pixels. */
static void inject_demo(void) {
    w16(0xF00028, 0x0003);   /* VMODE VIDEN + RGB16 */
    w16(0xF00046, 0x0000);   /* VDB = 0 */
    w16(0xF00058, 0x0000);   /* BG black */

    int ox = 60, oy = 70, bw = 200, bh = 100;
    uint32_t data = 0x120000;
    int phrases = (bw + 3) / 4;               /* 4 px per phrase at 16bpp */
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int lvl = x * 31 / (bw - 1);       /* 0..31 */
            uint16_t px = (uint16_t)((lvl << 11) | (lvl * 2 << 5) | lvl); /* grey-ish ramp */
            if (px == 0) px = 1;               /* avoid transparent */
            w16(data + (uint32_t)(y * phrases * 4 + x) * 2, px);
        }

    uint32_t list = 0x40000;
    w16(0xF00020, (uint16_t)(list & 0xFFFF));
    w16(0xF00022, (uint16_t)(list >> 16));
    uint64_t p0 = (uint64_t)OP_OBJ_BITMAP
                | ((uint64_t)(oy * 2) << 3)              /* ypos in half-lines */
                | ((uint64_t)bh << 14)                   /* height */
                | ((uint64_t)((list + 0x20) >> 3) << 24) /* link */
                | ((uint64_t)(data >> 3) << 43);         /* data */
    uint64_t p1 = (uint64_t)ox
                | ((uint64_t)4 << 12)                    /* depth 4 = 16bpp */
                | ((uint64_t)phrases << 18)              /* dwidth */
                | ((uint64_t)phrases << 28);             /* iwidth */
    w32(0x40000, (uint32_t)(p0 >> 32)); w32(0x40004, (uint32_t)p0);
    w32(0x40008, (uint32_t)(p1 >> 32)); w32(0x4000C, (uint32_t)p1);
    w32(0x40020, 0); w32(0x40024, OP_OBJ_STOP);
}

static void ascii_preview(void) {
    const char *ramp = " .:-=+*#%@";
    int cols = 80, rows = 24;
    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x = rx * FB_W / cols, y = ry * FB_H / rows;
            uint16_t p = fb[y * FB_W + x];
            int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            int lum = (r * 2 + g + b * 2) * 255 / (31 * 2 + 63 + 31 * 2);
            putchar(ramp[lum * 9 / 255]);
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <jagboot.rom> [--demo] [--ppm out.ppm] [cycles]\n", argv[0]); return 2; }
    int demo = 0; const char *ppm = NULL, *cartfile = NULL; long cycles = 6000000;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc) cartfile = argv[++i];
        else if (!strcmp(argv[i], "--forcecart")) g_forcecart = 1;
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) { g_forcecart = 1; g_entry = strtoul(argv[++i], NULL, 0); }
        else cycles = strtol(argv[i], NULL, 0);
    }

    onca_mem_init(&mem);
    onca_bios_info_t info;
    if (onca_bios_load_file(argv[1], &mem, &info) != 0) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    static uint8_t cartbuf[ONCA_CART_MAX];
    if (cartfile) {
        FILE *cf = fopen(cartfile, "rb");
        if (!cf) { fprintf(stderr, "cannot read cart %s\n", cartfile); return 1; }
        size_t cn = fread(cartbuf, 1, ONCA_CART_MAX, cf);
        fclose(cf);
        onca_mem_set_cart(&mem, cartbuf, cn);
        fprintf(stderr, "cart: %zu bytes @ $800000\n", cn);
    }
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    static onca_gpu_t gpu;
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    cpu.trace = cart_tracer;
    m68k_reset(&cpu);

    /* Match the libretro core's per-field loop exactly (assert the video IRQ
     * for the whole field, then clear), so headless behaviour tracks the
     * device. Report the background/border registers each field so we can see
     * the boot's colour sequence. */
    uint64_t field = 221583;
    uint16_t last_bg = 0xEEEE;
    for (uint64_t run = 0; run < (uint64_t)cycles; run += field) {
        /* BYPASS EXPERIMENT: once the boot has initialised hardware, force the
         * 68000 to the cart entry ($800404 holds it) instead of waiting for the
         * security check to pass. */
        /* snapshot the exception vectors the instant the boot kicks the GPU,
         * before the crypto scribbles over low DRAM */
        if (gpu.running && !g_prevgpu && !g_snapped) {
            memcpy(g_vecsnap, mem.dram, sizeof(g_vecsnap));
            g_snapped = 1;
        }
        g_prevgpu = gpu.running;

        if (g_forcecart && !g_forced && !gpu.running && cpu.cycles > 15000000) {
            uint32_t entry = g_entry ? g_entry : onca_peek32(&mem, 0x800404);
            if (g_snapped) memcpy(mem.dram, g_vecsnap, sizeof(g_vecsnap)); /* restore vectors */
            /* fresh high stack + a halt loop as the return address, so the
             * entry can be "called" (RTS lands somewhere safe) */
            onca_poke16(&mem, 0x1F0000, 0x60FE);   /* BRA.S self (safe return) */
            cpu.a[7] = 0x1FFF00;
            cpu.a[7] -= 4; onca_poke32(&mem, cpu.a[7], 0x1F0000);
            fprintf(stderr, "FORCE: call cart entry %06X (SP=%06X, ret=1F0000)\n",
                    entry & 0xFFFFFF, cpu.a[7] & 0xFFFFFF);
            cpu.pc = entry & 0xFFFFFF;
            cpu.imask = 7;
            cpu.s = 1;
            g_forced = 1;
        }
        /* In bypass mode, hold interrupts off until the game has installed its
         * own level-2 vector at $68 (the GPU crypto clobbered the boot's). */
        mem.video_irq = (g_forcecart && !g_forced) ? 0 : 1;
        uint64_t target = cpu.cycles + field;
        while (cpu.cycles < target && !cpu.halted) {
            /* bypass: only fire the vblank IRQ while the game is STOP-waiting */
            int lvl = (g_forcecart && g_forced) ? (cpu.stopped ? 2 : 0)
                                                : (mem.video_irq ? 2 : 0);
            m68k_set_irq(&cpu, lvl);
            m68k_step(&cpu);
            uint32_t p = cpu.pc & 0xFFFFFF;
            if (p >= 0x800000 && p < 0xE00000) { g_reached_cart++; if (!g_first_cart_pc) g_first_cart_pc = p; }
        }
        /* GPU runs ~2x the 68000 clock; give it a field's worth after the CPU. */
        if (gpu.running) onca_gpu_run(&gpu, field * 2);
        uint16_t bg = onca_peek16(&mem, 0xF00058);
        if (bg != last_bg) {
            printf("  field @%llum cyc: BG=%04X BORD=%04X%04X VMODE=%04X\n",
                   (unsigned long long)(cpu.cycles / 1000000), bg,
                   onca_peek16(&mem, 0xF0002A), onca_peek16(&mem, 0xF0002C),
                   onca_peek16(&mem, 0xF00028));
            last_bg = bg;
        }
    }

    if (g_forcecart) {   /* dump the last instructions (the stuck loop) */
        fprintf(stderr, "--- last 24 68k instrs (the loop it's stuck in) ---\n");
        for (int i = g_rh - 24; i < g_rh; i++)
            fprintf(stderr, "  %06X  %04X\n", g_ring[i & 63].pc, g_ring[i & 63].op);
    }
    fprintf(stderr, "GPU: running=%d pc=%06X cycles=%llu R0=%08X R1=%08X R13=%08X R14=%08X R15=%08X R29=%08X\n",
            gpu.running, gpu.pc & 0xFFFFFF, (unsigned long long)gpu.cycles,
            gpu.reg[0], gpu.reg[1], gpu.reg[13], gpu.reg[14], gpu.reg[15], gpu.reg[29]);
    fprintf(stderr, "CPU: pc=%06X  reached_cart=%ld (first cart PC=%06X)\n",
            cpu.pc & 0xFFFFFF, g_reached_cart, g_first_cart_pc);

    if (demo) inject_demo();
    int drawn = onca_op_render(&mem, fb, FB_W, FB_H);

    uint16_t bg = fb[0];
    int nonbg = 0;
    for (int i = 0; i < FB_W * FB_H; i++) if (fb[i] != bg) nonbg++;
    printf("objects drawn : %d\n", drawn);
    printf("frame         : %dx%d, BG=%04X, %d non-background pixels\n", FB_W, FB_H, bg, nonbg);
    if (!demo && nonbg == 0)
        printf("note          : empty because the boot logo bitmaps are Blitter+GPU-\n"
               "                produced (not yet implemented). Use --demo to exercise the OP.\n");
    printf("----- ASCII preview (80x24) -----\n");
    ascii_preview();

    if (ppm) {
        FILE *f = fopen(ppm, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
            for (int i = 0; i < FB_W * FB_H; i++) {
                uint16_t p = fb[i];
                unsigned char rgb[3] = {
                    (unsigned char)(((p >> 11) & 0x1F) << 3),
                    (unsigned char)(((p >> 5) & 0x3F) << 2),
                    (unsigned char)((p & 0x1F) << 3) };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            printf("wrote %s\n", ppm);
        }
    }
    return 0;
}
