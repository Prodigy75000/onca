/*
 * makefakebios.c - emit a 128 KB synthetic big-endian Jaguar boot ROM for
 * pipeline testing (no real BIOS needed). Mimics a POST shape: reset vector
 * -> init at $E00008 -> configure Tom MEMCON1 -> read the VC video counter ->
 * countdown loop -> idle self-branch. Enough to exercise the 68000 core, the
 * boot overlay, and the Tom register stubs end-to-end.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ROM_SIZE (128 * 1024)

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "fakebios.rom";
    uint8_t *rom = calloc(1, ROM_SIZE);
    if (!rom) return 1;

    /* reset vector: SSP into DRAM, PC to the real high-ROM alias $E00008 */
    put_be32(&rom[0], 0x00040000u);   /* initial SSP */
    put_be32(&rom[4], 0x00E00008u);   /* initial PC  */

    /* init routine at ROM offset 8 (executes from $E00008) */
    static const uint16_t prog[] = {
        0x41F9, 0x00F0, 0x0000, /* lea   $00F00000,a0     ; Tom base       */
        0x30BC, 0x1861,         /* move.w #$1861,(a0)     ; MEMCON1        */
        0x3039, 0x00F0, 0x0006, /* move.w $00F00006,d0    ; read VC        */
        0x323C, 0x0003,         /* move.w #3,d1                            */
        0x5341,                 /* subq.w #1,d1           ; loop:          */
        0x66FC,                 /* bne.s  loop                             */
        0x60FE                  /* bra.s  .               ; idle           */
    };
    for (size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); i++)
        put_be16(&rom[8 + i * 2], prog[i]);

    FILE *f = fopen(out, "wb");
    if (!f) { free(rom); return 2; }
    fwrite(rom, 1, ROM_SIZE, f);
    fclose(f);
    free(rom);
    printf("wrote %s (%d bytes)\n", out, ROM_SIZE);
    return 0;
}
