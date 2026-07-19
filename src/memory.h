/*
 * memory.h - Atari Jaguar system bus / memory map.
 *
 * The Jaguar has no separate I/O space: DRAM, cartridge, boot ROM and the
 * Tom/Jerry register files are all memory-mapped on one big-endian 24-bit bus
 * shared by the 68000 and the RISC cores. This module models that bus and
 * routes the coprocessor apertures (GPU/DSP control blocks, Blitter registers,
 * joypad matrix); every access can be logged for analysis.
 *
 * Documented addresses come from the public Tom & Jerry Technical Reference
 * Manual. No Virtual Jaguar / MAME source was consulted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_MEMORY_H
#define ONCA_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "m68k.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical sizes ---- */
#define ONCA_DRAM_SIZE  (2u * 1024 * 1024)   /* 2 MB main DRAM        */
#define ONCA_ROM_SIZE   (128u * 1024)        /* 128 KB boot ROM       */
#define ONCA_CART_MAX   (6u * 1024 * 1024)   /* up to 6 MB cartridge  */

/* ---- base addresses (24-bit bus) ---- */
#define ONCA_DRAM_BASE  0x000000u
#define ONCA_DRAM_END   0x200000u
#define ONCA_CART_BASE  0x800000u
#define ONCA_CART_END   0xE00000u
#define ONCA_ROM_BASE   0xE00000u
#define ONCA_ROM_END    0xE20000u
#define ONCA_TOM_BASE   0xF00000u   /* Tom: video/OP/blitter/GPU        */
#define ONCA_TOM_END    0xF10000u
#define ONCA_JERRY_BASE 0xF10000u   /* Jerry: DSP/timers/joystick/audio */
#define ONCA_JERRY_END  0xF20000u

/* A few named Tom/Jerry register offsets we seed or synthesise (byte offsets
 * from the respective base). */
#define TOM_MEMCON1  0x0000
#define TOM_MEMCON2  0x0002
#define TOM_HC       0x0004   /* horizontal count (free-running) */
#define TOM_VC       0x0006   /* vertical count   (free-running) */
#define TOM_OLP      0x0020   /* object list pointer (long)      */
#define TOM_VMODE    0x0028
#define TOM_VI       0x004E   /* vertical interrupt line         */
#define TOM_INT1     0x00E0   /* CPU interrupt control           */
#define TOM_INT2     0x00E2
#define JERRY_JOYSTICK 0x4000 /* $F14000 */
#define JERRY_JOYBUTS  0x4002 /* $F14002: buttons + config bits  */
#define JERRY_JINTCTRL 0x0020 /* $F10020 */

/* Jaguar pad-1 buttons: bit indices into onca_mem_t.joypad1 (active high = held).
 * The JOYSTICK/JOYBUTS registers decode these into the Jaguar's row-scanned,
 * active-low button matrix (see jaguar_joyst_word / jaguar_joybut_word). */
enum {
    TJ_UP = 0, TJ_DOWN, TJ_LEFT, TJ_RIGHT,
    TJ_A, TJ_B, TJ_C, TJ_PAUSE, TJ_OPTION,
    TJ_K0, TJ_K1, TJ_K2, TJ_K3, TJ_K4, TJ_K5, TJ_K6, TJ_K7, TJ_K8, TJ_K9,
    TJ_STAR, TJ_HASH
};

/* ---- access-log regions for the trace harness ---- */
typedef enum {
    ONCA_LOG_DRAM, ONCA_LOG_ROM, ONCA_LOG_CART,
    ONCA_LOG_TOM, ONCA_LOG_JERRY, ONCA_LOG_OPEN
} onca_region_t;

struct onca_mem;
typedef void (*onca_log_fn)(void *ctx, int is_write, int width,
                             onca_region_t region, uint32_t addr, uint32_t val);

typedef struct onca_mem {
    uint8_t  dram[ONCA_DRAM_SIZE];
    uint8_t  rom[ONCA_ROM_SIZE];
    uint8_t *cart;          /* NULL if no cartridge inserted */
    size_t   cart_size;
    size_t   rom_loaded;

    /* Tom / Jerry register files as byte arrays (64 KB windows each). Only a
     * handful of offsets carry meaningful stub values; the rest read back what
     * was written, and unmapped reads are logged. */
    uint8_t  tom[0x10000];
    uint8_t  jerry[0x10000];

    int overlay;   /* 1 = boot ROM mirrored across low memory at reset */

    /* Cartridge-security bypass. The boot ROM validates a cart by running a
     * 574-bit RSA check on the GPU and gating on the decrypted magic $03D0DEAD
     * appearing at the verdict word $F03000 (checked at boot pc $422 and inside
     * the OS at $5E42). That check is pure DRM - it exists to reject unlicensed
     * carts, not for correctness - and bit-exact RSA emulation is not required
     * for compatibility, so like other Jaguar emulators we let legitimately
     * loaded carts through. When set, a CPU read of the verdict word that holds
     * the GPU's "reject" marker is reported as the pass magic. Only the specific
     * reject markers at $F03000 are translated; the GPU computation is untouched. */
    uint8_t security_bypass;
    uint8_t blitter_off;   /* debug: disable the Blitter */

    /* Controller (pad 1). joypad1 = currently-held buttons (active high, bit =
     * TJ_* index), set each frame by the frontend. joy_row = the last row-select
     * byte the game wrote to JOYSTICK ($F14001). Reads of JOYSTICK/JOYBUTS decode
     * these into the hardware's active-low, row-scanned button matrix. */
    uint32_t joypad1;
    uint8_t  joy_row;

    /* Optional Blitter trace hook: called once per blit with the decoded command,
     * destination (A1) base and source (A2) base, and the count word. Catches
     * both CPU- and GPU-kicked blits for analysis. */
    void (*blit_trace)(void *ctx, uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t count);
    void *blit_trace_ctx;

    /* Optional coprocessor write watch: fires per byte for any poke (GPU/DSP
     * STORE or Blitter output) landing in [watch_lo, watch_hi). CPU-bus writes
     * are visible via `log`; pokes are not, hence this separate hook. */
    uint32_t watch_lo, watch_hi;
    void (*watch_cb)(void *ctx, uint32_t addr, uint8_t val);
    void *watch_ctx;

    /* Cartridge EEPROM (93C46 serial, 64 x 16-bit), bit-banged over Jerry GPIO:
     * writes to $F14800 clock a command/data bit in (DI = bit 0), reads of
     * $F14800 clock a data bit out (DO = bit 0), and touching $F15000 resets
     * the transaction (chip-select toggle). Blank chip = all $FFFF, so a
     * game's settings checksum fails and it falls back to defaults (Doom
     * stores its sound volumes here - an all-zero EEPROM read "validates" a
     * zeroed settings block and silences the game). */
    uint16_t ee_data[64];
    uint16_t ee_shift;      /* command shift register (9 bits)              */
    uint8_t  ee_bits;       /* command bits received                        */
    uint8_t  ee_state;      /* 0 = command, 1 = reading out, 2 = write data */
    uint8_t  ee_wen;        /* write/erase enabled (EWEN latch)             */
    uint8_t  ee_addr;       /* active word address                          */
    int8_t   ee_bit;        /* next data bit index (15 down to 0)           */
    uint16_t ee_wdata;      /* write-data shift register                    */
    uint8_t  ee_wbits;      /* write-data bits received                     */
    uint8_t  ee_do;         /* DO line level, presented on JOYSTICK bit 0   */
    uint8_t  ee_dirty;      /* contents modified (frontend may persist)     */

    /* Video-interrupt latch. TOM raises it at VBLANK; the 68000 handler clears
     * it by writing INT1 ($F000E0). Modelling the clear (rather than holding
     * the IRQ line) is what makes exactly one interrupt fire per field instead
     * of storming the moment the boot unmasks interrupts. */
    uint8_t video_irq;

    /* Free-running video timing source. HC/VC are derived from this so the
     * boot ROM's wait-for-scanline / VBLANK loops make progress. */
    const uint64_t *cycles;

    /* Live 68000 PC (points at g_cpu.pc). Used to scope the cart-security
     * bypass to the OS-internal verdict check ($5E42) and NOT the early boot
     * check at $422 - the latter must fail so a universal-header cart falls
     * into the OS launcher (JSR $5000) instead of jumping to a bogus header
     * pointer. See security_bypass. */
    const uint32_t *cpu_pc;

    /* GPU (Tom RISC). When set, accesses to the GPU control registers
     * ($F02100-$F0211F) route to it so G_PC/G_CTRL/G_FLAGS take effect. */
    struct onca_gpu *gpu;

    /* DSP (Jerry RISC). Same RISC core as the GPU; its control registers live at
     * $F1A100-$F1A11F. When set, those accesses route to it (D_PC/D_CTRL/D_FLAGS),
     * so games that gate startup on a 68000<->DSP handshake can progress. */
    struct onca_gpu *dsp;

    onca_log_fn log;
    void        *log_ctx;
} onca_mem_t;

/* Initialise: zero RAM, enable boot overlay, seed Tom/Jerry identity bits. */
void onca_mem_init(onca_mem_t *m);

/* Attach an optional cartridge image (kept by pointer, not copied). */
void onca_mem_set_cart(onca_mem_t *m, uint8_t *data, size_t size);

/* Populate the m68k bus struct to point at this memory. */
void onca_mem_bind(onca_mem_t *m, m68k_bus_t *bus);

/* Side-effect-free big-endian reads of backing memory (no logging, no boot
 * overlay change, no HC/VC synthesis). Used by the Object Processor and by
 * analysis tools that inspect memory without perturbing CPU state. */
uint8_t  onca_peek8 (onca_mem_t *m, uint32_t a);
uint16_t onca_peek16(onca_mem_t *m, uint32_t a);
uint32_t onca_peek32(onca_mem_t *m, uint32_t a);

/* Side-effect-free big-endian writes to backing memory (DRAM / Tom / Jerry).
 * Used by the GPU RISC core's STORE instructions. */
void onca_poke8 (onca_mem_t *m, uint32_t a, uint8_t  v);
void onca_poke16(onca_mem_t *m, uint32_t a, uint16_t v);
void onca_poke32(onca_mem_t *m, uint32_t a, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* ONCA_MEMORY_H */
