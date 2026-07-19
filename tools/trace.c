/*
 * trace.c - standalone Atari Jaguar boot trace harness.
 *
 * Loads a boot ROM image, wires up the 68000 + memory bus, single-steps from
 * the reset vector and logs Tom/Jerry/open-bus accesses (and optionally every
 * instruction). Used to empirically discover what the boot POST requires
 * before it will advance to the startup logo / cartridge handoff.
 *
 * Usage: trace <jagboot.rom> [max_steps] [--insn] [--all]
 *                            [--vblank] [--histbefore PC N]
 *   --insn     print every executed instruction (class + PC + opcode)
 *   --all      also log DRAM/ROM/CART accesses (very noisy)
 *   --vblank   pulse a level-2 video interrupt every ~field so VBLANK waits
 *              don't hang (approximation until Tom timing is real)
 *   --histbefore PC N  dump the last N instructions before first reaching PC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/gpu.h"
#include "../src/bios.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_insn = 0, g_all = 0;

static const char *region_str(onca_region_t r) {
    switch (r) {
        case ONCA_LOG_DRAM:  return "DRAM";
        case ONCA_LOG_ROM:   return "ROM ";
        case ONCA_LOG_CART:  return "CART";
        case ONCA_LOG_TOM:   return "TOM ";
        case ONCA_LOG_JERRY: return "JERRY";
        default:              return "OPEN";
    }
}

static void logger(void *ctx, int is_write, int width, onca_region_t region,
                   uint32_t addr, uint32_t val) {
    (void)ctx;
    if (!g_all && (region == ONCA_LOG_DRAM || region == ONCA_LOG_ROM
                || region == ONCA_LOG_CART))
        return;
    printf("    [%s %-5s.%d @ %06X = %0*X]\n", is_write ? "WR" : "RD",
           region_str(region), width * 8, addr, width * 2, val);
}

/* very small instruction classifier for readable traces */
static const char *insn_class(uint16_t op) {
    switch ((op >> 12) & 0xF) {
    case 0x0: return (op & 0x0100) || ((op & 0x0F00) == 0x0800) ? "BITOP" : "IMM";
    case 0x1: return "MOVE.B";
    case 0x2: return "MOVE.L";
    case 0x3: return "MOVE.W";
    case 0x4:
        if ((op & 0xF1C0) == 0x41C0) return "LEA";
        if ((op & 0xFF80) == 0x4880) return "MOVEM/EXT";
        if ((op & 0xFF80) == 0x4E80) return "JSR/JMP";
        if (op == 0x4E75) return "RTS";
        if (op == 0x4E73) return "RTE";
        return "MISC";
    case 0x5: return ((op & 0x00C0) == 0x00C0) ? "Scc/DBcc" : "ADDQ/SUBQ";
    case 0x6: return ((op >> 8) & 0xF) == 0 ? "BRA" : ((op >> 8) & 0xF) == 1 ? "BSR" : "Bcc";
    case 0x7: return "MOVEQ";
    case 0x8: return "OR/DIV";
    case 0x9: return "SUB";
    case 0xB: return "CMP/EOR";
    case 0xC: return "AND/MUL";
    case 0xD: return "ADD";
    case 0xE: return "SHIFT";
    default:  return "?";
    }
}

#define HIST_MAX 512
static struct { long i; uint32_t pc; uint16_t op; uint32_t d0, a0, a7; } g_hist[HIST_MAX];
static long g_histcount = 0;
static uint32_t g_histpc = 0xFFFFFFFF;
static long g_histn = 64;

static m68k_t g_cpu;

static void tracer(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx;
    if (g_insn) {
        printf("%6llu  PC=%06X  %04X  %-9s  d0=%08X d1=%08X a0=%06X a7=%06X\n",
               (unsigned long long)g_cpu.cycles, pc, op, insn_class(op),
               g_cpu.d[0], g_cpu.d[1], g_cpu.a[0] & 0xFFFFFF, g_cpu.a[7] & 0xFFFFFF);
    }
    long h = g_histcount % HIST_MAX;
    g_hist[h].i = g_histcount; g_hist[h].pc = pc; g_hist[h].op = op;
    g_hist[h].d0 = g_cpu.d[0]; g_hist[h].a0 = g_cpu.a[0]; g_hist[h].a7 = g_cpu.a[7];
    g_histcount++;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <jagboot.rom> [max_steps] [--insn] [--all] [--vblank] [--histbefore PC N]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    long max_steps = 500000;
    int do_vblank = 0;
    const char *g_cartfile = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--insn")) g_insn = 1;
        else if (!strcmp(argv[i], "--all")) g_all = 1;
        else if (!strcmp(argv[i], "--vblank")) do_vblank = 1;
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc) g_cartfile = argv[++i];
        else if (!strcmp(argv[i], "--histbefore") && i + 2 < argc) {
            g_histpc = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_histn = strtol(argv[i + 2], NULL, 0);
            if (g_histn > HIST_MAX) g_histn = HIST_MAX;
            i += 2;
        }
        else max_steps = strtol(argv[i], NULL, 0);
    }

    static onca_mem_t mem;
    onca_mem_init(&mem);

    onca_bios_info_t info;
    if (onca_bios_load_file(path, &mem, &info) != 0) {
        fprintf(stderr, "error: cannot read boot ROM '%s'\n", path);
        return 1;
    }
    printf("Boot ROM: %s\n", path);
    printf("  size   : %zu bytes (%s)\n", info.size,
           info.size == ONCA_ROM_SIZE ? "128 KB OK" : "unexpected size");
    printf("  crc32  : %08X\n", info.crc32);
    printf("  status : %s%s\n", info.known ? "KNOWN " : "UNKNOWN (add CRC to table)",
           info.known ? info.desc->name : "");
    printf("  region : %s\n", onca_region_name(info.region));
    printf("  reset  : SSP=%02X%02X%02X%02X  PC=%02X%02X%02X%02X\n",
           mem.rom[0], mem.rom[1], mem.rom[2], mem.rom[3],
           mem.rom[4], mem.rom[5], mem.rom[6], mem.rom[7]);
    printf("----- trace (max %ld steps) -----\n", max_steps);

    mem.log = logger;

    static uint8_t cartbuf[ONCA_CART_MAX];
    if (g_cartfile) {
        FILE *cf = fopen(g_cartfile, "rb");
        if (cf) { size_t cn = fread(cartbuf, 1, ONCA_CART_MAX, cf); fclose(cf);
                  onca_mem_set_cart(&mem, cartbuf, cn);
                  printf("  cart   : %s (%zu bytes @ $800000)\n", g_cartfile, cn); }
    }

    memset(&g_cpu, 0, sizeof(g_cpu));
    onca_mem_bind(&mem, &g_cpu.bus);
    mem.cycles = &g_cpu.cycles;
    static onca_gpu_t gpu;
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    g_cpu.trace = tracer;
    m68k_reset(&g_cpu);
    printf("  reset  : SSP=%06X  PC=%06X\n", g_cpu.a[7] & 0xFFFFFF, g_cpu.pc & 0xFFFFFF);

    uint32_t prev_pc = 0xFFFFFFFF;
    int same_count = 0;
    uint64_t next_vblank = 8000;

    for (long i = 0; i < max_steps; i++) {
        uint32_t pc = g_cpu.pc;

        if (g_histpc != 0xFFFFFFFF && pc == g_histpc) {
            printf("=== last %ld insns before first reaching PC=%06X ===\n", g_histn, pc);
            long start = g_histcount < g_histn ? 0 : g_histcount - g_histn;
            for (long k = start; k < g_histcount; k++) {
                long idx = k % HIST_MAX;
                printf("%8ld  PC=%06X  %04X  %-9s d0=%08X a0=%06X a7=%06X\n",
                       g_hist[idx].i, g_hist[idx].pc, g_hist[idx].op,
                       insn_class(g_hist[idx].op), g_hist[idx].d0,
                       g_hist[idx].a0 & 0xFFFFFF, g_hist[idx].a7 & 0xFFFFFF);
            }
            printf("=== (reached trigger) ===\n");
            break;
        }

        /* tight self-branch / STOP = the classic idle-or-hang signature */
        if (pc == prev_pc && !g_cpu.stopped) {
            if (++same_count > 6) {
                printf("... PC stuck at %06X (idle/hang loop) after %ld steps\n", pc, i);
                break;
            }
        } else same_count = 0;
        prev_pc = pc;

        if (do_vblank && g_cpu.cycles >= next_vblank) {
            m68k_set_irq(&g_cpu, 2);   /* pulse video interrupt */
            next_vblank += 221583;     /* ~one NTSC field */
        } else if (do_vblank) {
            m68k_set_irq(&g_cpu, 0);
        }

        if (g_cpu.stopped && !do_vblank) {
            printf("... CPU STOPped at PC=%06X after %ld steps (no IRQ source; try --vblank)\n", pc, i);
            break;
        }
        m68k_step(&g_cpu);
        if (gpu.running) onca_gpu_run(&gpu, 128);   /* interleave the GPU */
    }

    printf("----- final CPU state -----\n");
    for (int i = 0; i < 8; i++)
        printf("  d%d=%08X   a%d=%08X\n", i, g_cpu.d[i], i, g_cpu.a[i]);
    printf("  pc=%06X  sr=%04X  %s  imask=%d  cycles=%llu\n",
           g_cpu.pc & 0xFFFFFF, m68k_get_sr(&g_cpu), g_cpu.s ? "SVC" : "USR",
           g_cpu.imask, (unsigned long long)g_cpu.cycles);
    return 0;
}
