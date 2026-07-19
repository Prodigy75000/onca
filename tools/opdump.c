/*
 * opdump.c - boot the ROM, run it to its main loop, then decode Tom's Object
 * Processor list, video window registers and CLUT straight out of memory.
 *
 * This is an analysis aid for building the Object Processor : it lets
 * us read the real object list the boot ROM assembles, rather than guessing the
 * phrase bitfields. Decoding follows the public Tom & Jerry Technical Reference
 * Manual object-list format.
 *
 * Usage: opdump <jagboot.rom> [run_cycles]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/bios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m68k_t cpu;
static onca_mem_t mem;

static uint32_t r32(uint32_t a) { return cpu.bus.read32(cpu.bus.ctx, a); }
static uint16_t r16(uint32_t a) { return cpu.bus.read16(cpu.bus.ctx, a); }

static const char *type_name(int t) {
    switch (t) {
    case 0: return "BITMAP";
    case 1: return "SCALED BITMAP";
    case 2: return "GPU";
    case 3: return "BRANCH";
    case 4: return "STOP";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <jagboot.rom> [cycles]\n", argv[0]); return 2; }
    long cycles = argc > 2 ? strtol(argv[2], NULL, 0) : 6000000;

    onca_mem_init(&mem);
    onca_bios_info_t info;
    if (onca_bios_load_file(argv[1], &mem, &info) != 0) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    m68k_reset(&cpu);

    /* run to the main loop, pulsing the video IRQ like the frontend does */
    uint64_t next_vbl = 8000;
    while (cpu.cycles < (uint64_t)cycles) {
        if (cpu.cycles >= next_vbl) { m68k_set_irq(&cpu, 2); next_vbl += 221583; }
        else m68k_set_irq(&cpu, 0);
        m68k_step(&cpu);
    }

    printf("=== video setup ===\n");
    printf("  VMODE  ($F00028) = %04X\n", r16(0xF00028));
    printf("  BG     ($F00058) = %04X\n", r16(0xF00058));
    printf("  BORD1/2($F0002A) = %04X %04X\n", r16(0xF0002A), r16(0xF0002C));
    printf("  HDB1/HDE ($38/3C)= %04X %04X\n", r16(0xF00038), r16(0xF0003C));
    printf("  VDB/VDE ($46/48) = %04X %04X\n", r16(0xF00046), r16(0xF00048));
    printf("  VP/HP  ($3E/$2E) = %04X %04X\n", r16(0xF0003E), r16(0xF0002E));

    /* OLP is two 16-bit halves: $F00020 = low word, $F00022 = high word. */
    uint32_t olp_lo = r16(0xF00020), olp_hi = r16(0xF00022);
    uint32_t olp = (olp_hi << 16) | olp_lo;
    printf("\n=== object list ===\n");
    printf("  OLP raw  $F00020=%04X $F00022=%04X -> list @ %06X\n", olp_lo, olp_hi, olp & 0xFFFFFF);

    uint32_t addr = olp & 0xFFFFFF;
    for (int i = 0; i < 48; i++) {
        uint32_t hi = r32(addr), lo = r32(addr + 4);
        uint64_t ph = ((uint64_t)hi << 32) | lo;
        int type = (int)(ph & 7);
        uint32_t ypos   = (uint32_t)((ph >> 3)  & 0x7FF);
        uint32_t height = (uint32_t)((ph >> 14) & 0x3FF);
        uint32_t link   = (uint32_t)(((ph >> 24) & 0x7FFFF) << 3);
        uint32_t data   = (uint32_t)(((ph >> 43) & 0x1FFFFF) << 3);

        printf("  @%06X  %016llX  %-13s", addr, (unsigned long long)ph, type_name(type));
        if (type == 0 || type == 1) {
            uint32_t hi2 = r32(addr + 8), lo2 = r32(addr + 12);
            uint64_t ph2 = ((uint64_t)hi2 << 32) | lo2;
            int xpos   = (int)(ph2 & 0xFFF);
            int depth  = (int)((ph2 >> 12) & 7);
            int iwidth = (int)((ph2 >> 28) & 0x3FF);
            int dwidth = (int)((ph2 >> 18) & 0x3FF);
            printf(" ypos=%u h=%u xpos=%d depth=%d(%dbpp) iw=%u dw=%u data=%06X link=%06X\n",
                   ypos, height, xpos, depth, 1 << depth, iwidth, dwidth, data, link);
            if (type == 1) {
                uint32_t s = r32(addr + 16); /* scale phrase: HSCALE[7:0] VSCALE[15:8] REMAINDER[23:16] */
                printf("       scale phrase = %08X  hscale=%02X vscale=%02X\n",
                       s, (s >> 24) & 0xFF, (s >> 16) & 0xFF);
            }
            /* peek at the pixel data - is it actually present, or did the
             * stubbed blitter leave it empty? */
            printf("       data[%06X]:", data);
            for (int k = 0; k < 16; k++) printf(" %02X", cpu.bus.read8(cpu.bus.ctx, data + k));
            printf("\n");
        } else if (type == 3) {
            int cc  = (int)((ph >> 14) & 7);
            uint32_t cmp = (uint32_t)((ph >> 3) & 0x7FF);
            printf(" cc=%d cmpYPOS=%u link=%06X\n", cc, cmp, link);
        } else {
            printf(" (raw)\n");
        }
        if (type == 4) break;                 /* STOP */
        if (link == 0 || link == addr) { printf("  (link terminates)\n"); break; }
        addr = link;
    }

    printf("\n=== CLUT ($F00400, first 16 of 256) ===\n  ");
    for (int i = 0; i < 16; i++) printf("%04X ", r16(0xF00400 + i * 2));
    printf("\n");

    printf("\n=== GPU ===\n");
    printf("  G_FLAGS=%08X G_PC=%08X G_CTRL=%08X G_END=%08X\n",
           r32(0xF02100), r32(0xF02110), r32(0xF02114), r32(0xF0210C));
    uint32_t gpc = r32(0xF02110) & 0xFFFF;   /* offset within Tom window */
    printf("  GPU program near G_PC ($F0%04X):\n", gpc);
    for (int row = 0; row < 8; row++) {
        uint32_t base = 0xF00000 + gpc - 8 + row * 8;
        printf("   %06X:", base & 0xFFFFFF);
        for (int i = 0; i < 4; i++) {
            uint16_t w = r16(base + i * 2);
            printf(" %04X(op=%2d s=%2d d=%2d)", w, (w >> 10) & 0x3F, (w >> 5) & 0x1F, w & 0x1F);
        }
        printf("\n");
    }
    return 0;
}
