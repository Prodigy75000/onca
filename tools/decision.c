/*
 * decision.c - find the Jaguar boot cart-security decision point.
 *
 * Boots with a cartridge, interleaves 68000 + GPU, and watches:
 *   - 68000 reads of the low-DRAM result region ($000000-$0000FF), with PC,
 *     since that is where the GPU writes the decrypted signature that the boot
 *     compares.
 *   - the branch the 68000 takes right after (the accept/reject decision).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/gpu.h"
#include "../src/bios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m68k_t cpu;
static onca_mem_t mem;
static onca_gpu_t gpu;
static long g_hits;
static uint32_t g_last_pc;

static void tracer(void *ctx, uint32_t pc, uint16_t op) { (void)ctx; (void)op; g_last_pc = pc; }

static void logger(void *c, int wr, int width, onca_region_t r, uint32_t a, uint32_t v) {
    (void)c;
    if (r != ONCA_LOG_DRAM) return;
    uint32_t addr = a & 0xFFFFFF;
    /* 68000 read of the low result region after the GPU has run */
    if (!wr && addr < 0x100 && gpu.cycles > 100000 && g_hits < 60) {
        printf("  CPU RD [%06X].%d = %0*X   at 68kPC=%06X  (gpu.running=%d gpu.cyc=%lluM)\n",
               addr, width * 8, width * 2, v, g_last_pc & 0xFFFFFF, gpu.running,
               (unsigned long long)(gpu.cycles / 1000000));
        g_hits++;
    }
}

int main(int argc, char **argv) {
    const char *cartf = argc > 2 ? argv[2] : NULL;
    onca_mem_init(&mem);
    onca_bios_info_t info;
    if (onca_bios_load_file(argv[1], &mem, &info) != 0) { fprintf(stderr, "no bios\n"); return 1; }
    static uint8_t cart[ONCA_CART_MAX];
    if (cartf) {
        FILE *f = fopen(cartf, "rb"); if (!f) { fprintf(stderr, "no cart\n"); return 1; }
        size_t n = fread(cart, 1, ONCA_CART_MAX, f); fclose(f);
        onca_mem_set_cart(&mem, cart, n);
    }
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    mem.log = logger;
    cpu.trace = tracer;
    m68k_reset(&cpu);

    uint64_t field = 221805;
    for (uint64_t run = 0; run < 400000000ull && g_hits < 60; run += field) {
        mem.video_irq = 1;
        uint64_t t = cpu.cycles + field;
        while (cpu.cycles < t && !cpu.halted) {
            m68k_set_irq(&cpu, mem.video_irq ? 2 : 0);
            m68k_step(&cpu);
            if (gpu.running) onca_gpu_run(&gpu, 200);
        }
    }
    printf("(%ld low-DRAM reads observed)\n", g_hits);
    return 0;
}
