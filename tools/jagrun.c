/*
 * jagrun.c - headless runner + tracer for .jag dev-kit executables.
 *
 * Loads a .jag at its header load address, runs frames like the core does
 * (pulse video IRQ, step CPU + GPU), and reports where the CPU is spending
 * time, whether the GPU runs, and the resulting OLP / framebuffer coverage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 320
#define FB_H 240
#define CPU_HZ 13295000u
#define FPS 60

static m68k_t cpu;
static onca_mem_t mem;
static onca_gpu_t gpu;
static uint16_t fb[FB_W * FB_H];

/* PC-bucket histogram (by 0x100 page within DRAM low 64K around load). */
static unsigned long g_pchist[64];   /* pages 0x5000..0x8FFF => (pc-0x5000)>>8 */
static unsigned long g_gpu_steps;

static long g_itrace = 0;   /* remaining instructions to print */
static uint32_t g_lastpc = 0;

static void tracer(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx;
    pc &= 0xFFFFFF;
    if (g_itrace > 0) {
        /* flag control-flow jumps (non-sequential) to spot where it derails */
        int jump = (g_lastpc && pc != g_lastpc + 2 && pc != g_lastpc + 4 &&
                    pc != g_lastpc + 6 && pc != g_lastpc + 8 && pc != g_lastpc + 10);
        printf("  %06X: %04X%s\n", pc, op, jump ? "   <-- jump" : "");
        g_itrace--;
    }
    g_lastpc = pc;
    if (pc >= 0x5000 && pc < 0x9000) g_pchist[(pc - 0x5000) >> 8]++;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.jag [frames]\n", argv[0]); return 2; }
    int frames = argc > 2 ? atoi(argv[2]) : 120;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    if (memcmp(buf + 0x1C, "JAGR", 4)) { fprintf(stderr, "not a .jag\n"); return 1; }
    uint32_t load = (buf[0x22]<<24)|(buf[0x23]<<16)|(buf[0x24]<<8)|buf[0x25];
    uint32_t size = (buf[0x26]<<24)|(buf[0x27]<<16)|(buf[0x28]<<8)|buf[0x29];
    uint32_t entry= (buf[0x2A]<<24)|(buf[0x2B]<<16)|(buf[0x2C]<<8)|buf[0x2D];
    if (size > sz - 0x2E) size = sz - 0x2E;
    printf("load=$%06X entry=$%06X size=%u\n", load, entry, size);

    onca_mem_init(&mem);
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    m68k_reset(&cpu);
    cpu.int_vector = 64;
    mem.cpu_pc = &cpu.pc;
    mem.overlay = 0;   /* no boot ROM: DRAM is directly visible at low addresses */
    memcpy(mem.dram + (load & 0x1FFFFF), buf + 0x2E, size);
    cpu.pc = entry;
    cpu.a[7] = cpu.isp = 0x1FFFF8;
    cpu.trace = tracer;
    if (getenv("ITRACE")) g_itrace = atol(getenv("ITRACE"));

    uint64_t budget = CPU_HZ / FPS;
    for (int fr = 0; fr < frames; fr++) {
        mem.video_irq = 1;
        uint64_t target = cpu.cycles + budget;
        while (cpu.cycles < target && !cpu.halted) {
            m68k_set_irq(&cpu, mem.video_irq ? 2 : 0);
            m68k_step(&cpu);
            if (gpu.running)
                for (int k = 0; k < 16 && gpu.running; k++) { onca_gpu_step(&gpu); g_gpu_steps++; }
        }
    }

    uint32_t olp = ((uint32_t)onca_peek16(&mem, 0xF00022) << 16) | onca_peek16(&mem, 0xF00020);
    uint16_t vmode = onca_peek16(&mem, 0xF00028);
    uint16_t vdb   = onca_peek16(&mem, 0xF00046);
    int drawn = onca_op_render(&mem, fb, FB_W, FB_H);
    int nz = 0; for (int i = 0; i < FB_W*FB_H; i++) if (fb[i]) nz++;

    printf("final: pc=$%06X halted=%d cycles=%llu gpu_steps=%lu\n",
           cpu.pc, cpu.halted, (unsigned long long)cpu.cycles, g_gpu_steps);
    printf("OLP=$%06X VMODE=%04X VDB=%04X op_drawn=%d fb_nonzero=%d\n",
           olp & 0xFFFFFF, vmode, vdb, drawn, nz);
    printf("PC histogram (hot pages in $5000..$8FFF):\n");
    for (int i = 0; i < 64; i++)
        if (g_pchist[i] > 0)
            printf("  $%06X: %lu\n", 0x5000 + (i << 8), g_pchist[i]);

    const char *ppm = getenv("PPM");
    if (ppm) {
        FILE *o = fopen(ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", FB_W, FB_H);
            for (int i = 0; i < FB_W*FB_H; i++) {
                uint16_t p = fb[i];
                unsigned char r = ((p >> 11) & 0x1F) << 3;
                unsigned char g = ((p >> 5)  & 0x3F) << 2;
                unsigned char b = (p & 0x1F) << 3;
                fputc(r, o); fputc(g, o); fputc(b, o);
            }
            fclose(o);
            printf("wrote %s\n", ppm);
        }
    }
    return 0;
}
