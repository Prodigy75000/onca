/*
 * m68k.h - Motorola 68000 (68EC000) interpreting CPU core for the Trophy Hub
 * Atari Jaguar core.
 *
 * Clean-room implementation written against the public Motorola M68000
 * Programmer's Reference Manual (M68000PM) and the public 68000 User's Manual.
 * Not derived from Virtual Jaguar, MAME (Musashi), or any other emulator's
 * source.
 *
 * The Jaguar's "manager" CPU is a 68000 @ 13.295 MHz. It is a big-endian,
 * 16/32-bit device with a 24-bit external address bus. The core talks to the
 * outside world only through the bus callbacks in m68k_bus_t, so it can be
 * exercised against a flat memory array in unit tests with no Jaguar hardware
 * present.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_M68K_H
#define ONCA_M68K_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status register bit masks ---- */
#define M68K_SR_C 0x0001u
#define M68K_SR_V 0x0002u
#define M68K_SR_Z 0x0004u
#define M68K_SR_N 0x0008u
#define M68K_SR_X 0x0010u
#define M68K_SR_I 0x0700u   /* interrupt mask (3 bits) */
#define M68K_SR_S 0x2000u   /* supervisor         */
#define M68K_SR_T 0x8000u   /* trace              */

/* ---- Exception vector numbers (byte address = vector * 4) ---- */
enum {
    M68K_VEC_RESET_SSP   = 0,   /* initial SSP at 0x000000 */
    M68K_VEC_RESET_PC    = 1,   /* initial PC  at 0x000004 */
    M68K_VEC_BUS_ERROR   = 2,
    M68K_VEC_ADDR_ERROR  = 3,
    M68K_VEC_ILLEGAL     = 4,
    M68K_VEC_ZERO_DIVIDE = 5,
    M68K_VEC_CHK         = 6,
    M68K_VEC_TRAPV       = 7,
    M68K_VEC_PRIV        = 8,
    M68K_VEC_TRACE       = 9,
    M68K_VEC_LINE_A      = 10,
    M68K_VEC_LINE_F      = 11,
    M68K_VEC_SPURIOUS    = 24,
    M68K_VEC_AUTOVEC     = 24,  /* level n autovector = 24 + n */
    M68K_VEC_TRAP0       = 32   /* TRAP #n = 32 + n */
};

/* Bus interface. All addresses are byte addresses on the 68000's 24-bit bus
 * (callers should treat addresses as masked to 0xFFFFFF). 16/32-bit accesses
 * are big-endian. The 68000 raises an address error on odd word/long
 * accesses; that check lives in the core, not the bus. */
typedef struct {
    void    *ctx;
    uint8_t  (*read8)  (void *ctx, uint32_t addr);
    uint16_t (*read16) (void *ctx, uint32_t addr);
    uint32_t (*read32) (void *ctx, uint32_t addr);
    void     (*write8) (void *ctx, uint32_t addr, uint8_t  val);
    void     (*write16)(void *ctx, uint32_t addr, uint16_t val);
    void     (*write32)(void *ctx, uint32_t addr, uint32_t val);
} m68k_bus_t;

typedef struct m68k {
    uint32_t d[8];      /* data registers    */
    uint32_t a[8];      /* address registers; a[7] is the active stack pointer */
    uint32_t pc;

    /* Inactive stack pointers. The active SP lives in a[7]; whichever of these
     * is inactive is kept in sync on mode switches. */
    uint32_t usp;       /* user stack pointer   */
    uint32_t isp;       /* interrupt/supervisor stack pointer */

    /* Condition codes kept as discrete 0/1 flags for speed; assembled into the
     * real SR only on MOVE-from-SR / exception entry (see m68k_get_sr). */
    uint8_t  xf, nf, zf, vf, cf;

    uint8_t  s;         /* supervisor (1) / user (0) */
    uint8_t  t;         /* trace enable */
    uint8_t  imask;     /* interrupt priority mask 0..7 */

    uint8_t  stopped;   /* set by STOP, cleared by an interrupt or reset */
    uint8_t  halted;    /* set on double bus fault; diagnostics only */
    int      ipl;       /* current asserted interrupt level (0 = none) */
    int      int_vector;/* device-supplied IRQ vector number, or 0 for the
                         * 68000 autovector (vector 0 is the reset SSP, never a
                         * real IRQ vector, so 0 safely means "autovector"). The
                         * Jaguar's TOM is a vectored source (vector 64 = $100). */

    m68k_bus_t bus;
    uint64_t cycles;    /* approximate cycle counter */

    /* Optional trace hook, called before each instruction with the PC and the
     * opcode word. Used by the boot trace harness. */
    void (*trace)(void *ctx, uint32_t pc, uint16_t op);
    void  *trace_ctx;
} m68k_t;

/* Reset: read SSP from 0x000000 and PC from 0x000004 via the bus, enter
 * supervisor mode, mask all interrupts, clear trace. */
void m68k_reset(m68k_t *cpu);

/* Execute one instruction (or take a pending interrupt / trace). Returns the
 * approximate number of clock cycles consumed. */
int m68k_step(m68k_t *cpu);

/* Run until at least `cycles` clocks have elapsed; returns cycles run. */
uint64_t m68k_run(m68k_t *cpu, uint64_t cycles);

/* Assert an interrupt priority level (0 = none, 7 = NMI). Sampled between
 * instructions; a level strictly greater than the mask (or level 7) is taken. */
void m68k_set_irq(m68k_t *cpu, int level);

/* Assemble / parse the 16-bit status register from the discrete flags. */
uint16_t m68k_get_sr(const m68k_t *cpu);
void     m68k_set_sr(m68k_t *cpu, uint16_t sr);

#ifdef __cplusplus
}
#endif

#endif /* ONCA_M68K_H */
