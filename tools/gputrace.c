/*
 * gputrace.c - boot with an optional cartridge, wait until the 68000 kicks the
 * GPU, then trace GPU instructions with a small disassembler. Used to see what
 * the boot's cartridge-check GPU program does and where it wedges.
 *
 * Usage: gputrace <jagboot.rom> [--cart file] [gpu_steps]
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
static long g_trace_left;

static const char *NAMES[64] = {
 "ADD","ADDC","ADDQ","ADDQT","SUB","SUBC","SUBQ","SUBQT","NEG","AND","OR","XOR",
 "NOT","BTST","BSET","BCLR","MULT","IMULT","IMULTN","RESMAC","IMACN","DIV","ABS",
 "SH","SHLQ","SHRQ","SHA","SHARQ","ROR","RORQ","CMP","CMPQ","SAT8","SAT16","MOVE",
 "MOVEQ","MOVETA","MOVEFA","MOVEI","LOADB","LOADW","LOAD","LOADP","LOAD(R14+n)",
 "LOAD(R15+n)","STOREB","STOREW","STORE","STOREP","STORE(R14+n)","STORE(R15+n)",
 "MOVEPC","JUMP","JR","MMULT","MTOI","NORMI","NOP","LOAD(R14+Rn)","LOAD(R15+Rn)",
 "STORE(R14+Rn)","STORE(R15+Rn)","SAT24","PACK" };

static int g_storesonly, g_loadsonly;
static void gtrace(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx;
    if (g_trace_left <= 0) return;
    int opc = (op >> 10) & 0x3F, s = (op >> 5) & 0x1F, d = op & 0x1F;
    uint32_t *R = &gpu.reg[gpu.bank * 32];
    /* --stores mode: only show STOREs to main DRAM (result writes the 68000 reads) */
    if (g_storesonly) {
        int isstore = (opc >= 45 && opc <= 50) || opc == 60 || opc == 61;
        if (!isstore || (R[s] & 0xFFFFFF) >= 0x200000) return;
        printf("  %06X %-11s  [%06X] <= %08X (R%d)\n", pc & 0xFFFFFF, NAMES[opc],
               R[s] & 0xFFFFFF, R[d], d);
        g_trace_left--;
        return;
    }
    if (g_loadsonly) {   /* LOADs from cart or boot ROM = the crypto inputs */
        int isload = (opc >= 39 && opc <= 44) || opc == 58 || opc == 59;
        uint32_t addr = R[s] & 0xFFFFFF;
        if (!isload || (addr < 0x800000)) return;   /* only cart/ROM sources */
        printf("  %06X %-11s  R%d <= [%06X]\n", pc & 0xFFFFFF, NAMES[opc], d, addr);
        g_trace_left--;
        return;
    }
    printf("  %06X %04X %-11s s=%-2d d=%-2d | Rs=%08X Rd=%08X  R22=%08X R23=%08X Z=%d C=%d\n",
           pc & 0xFFFFFF, op, NAMES[opc], s, d, R[s], R[d], R[22], R[23], gpu.zf, gpu.cf);
    g_trace_left--;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <jagboot.rom> [--cart f] [steps]\n", argv[0]); return 2; }
    const char *cartf = NULL; long steps = 80;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--cart") && i + 1 < argc) cartf = argv[++i];
        else if (!strcmp(argv[i], "--stores")) g_storesonly = 1;
        else if (!strcmp(argv[i], "--loads")) g_loadsonly = 1;
        else steps = strtol(argv[i], NULL, 0);
    }

    onca_mem_init(&mem);
    onca_bios_info_t info;
    if (onca_bios_load_file(argv[1], &mem, &info) != 0) { fprintf(stderr, "no bios\n"); return 1; }
    static uint8_t cart[ONCA_CART_MAX];
    if (cartf) {
        FILE *f = fopen(cartf, "rb"); if (!f) { fprintf(stderr, "no cart\n"); return 1; }
        size_t n = fread(cart, 1, ONCA_CART_MAX, f); fclose(f);
        onca_mem_set_cart(&mem, cart, n);
        printf("cart %zu bytes @ $800000\n", n);
    }
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    m68k_reset(&cpu);

    /* run the 68000 (with per-field IRQ) until it starts the GPU */
    uint64_t field = 221805, guard = 200000000;
    while (!gpu.running && cpu.cycles < guard) {
        mem.video_irq = 1;
        uint64_t t = cpu.cycles + field;
        while (cpu.cycles < t && !gpu.running) {
            m68k_set_irq(&cpu, mem.video_irq ? 2 : 0);
            m68k_step(&cpu);
        }
    }
    if (!gpu.running) { printf("GPU never started within %llu cycles\n", (unsigned long long)guard); return 0; }
    printf("GPU started: G_PC=%06X after %llu CPU cycles. Tracing %ld GPU instrs:\n",
           gpu.pc & 0xFFFFFF, (unsigned long long)cpu.cycles, steps);

    /* static disassembly of the wedge region so we can see delay slots */
    printf("--- static disasm F0358E..F035A2 ---\n");
    for (uint32_t a = 0xF0337C; a <= 0xF033D0; ) {
        uint16_t w = onca_peek16(&mem, a);
        int opc = (w >> 10) & 0x3F, s = (w >> 5) & 0x1F, d = w & 0x1F;
        printf("  %06X %04X %-11s s=%-2d d=%-2d\n", a, w, NAMES[opc], s, d);
        a += 2;
        if (opc == 38) { printf("  %06X %04X   (imm lo)\n", a, onca_peek16(&mem,a)); a+=2;
                         printf("  %06X %04X   (imm hi)\n", a, onca_peek16(&mem,a)); a+=2; }
    }
    printf("--- trace ---\n");

    g_trace_left = steps;
    gpu.trace = gtrace;
    for (long i = 0; i < steps && gpu.running; i++) onca_gpu_step(&gpu);

    printf("--- modulus region $F03080-$F030D0 (longwords) ---\n");
    for (uint32_t a = 0xF03000; a < 0xF032F0; a += 4) printf("  %06X: %08X\n", a, onca_peek32(&mem, a));
    printf("--- signature region $F032A0-$F032F0 (longwords) ---\n");
    for (uint32_t a = 0xF032A0; a < 0xF032F0; a += 4) printf("  %06X: %08X\n", a, onca_peek32(&mem, a));
    return 0;
}
