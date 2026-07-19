/*
 * blitter.c - Tom's Blitter. See blitter.h.
 *
 * Implements the two modes the boot ROM and games actually use: pattern fill
 * (clear the framebuffer with B_PATD) and copy (move a bitmap from the A2 source
 * window to the A1 destination window, through the logic-function unit). Runs the
 * whole blit synchronously the moment B_CMD is written.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "blitter.h"
#include "memory.h"
#include <stdint.h>

/* --- B_CMD bits (Tom & Jerry manual) --- */
#define BC_SRCEN    0x00000001u   /* read source data                */
#define BC_DSTEN    0x00000008u   /* read destination data           */
#define BC_UPDA1    0x00000200u   /* write A1 pointer back after     */
#define BC_UPDA2    0x00000400u
#define BC_DSTA2    0x00000800u   /* swap roles: dest = A2, src = A1 */
#define BC_PATDSEL  0x00010000u   /* source = pattern data (B_PATD) */
#define BC_LFU_SHIFT 21           /* 4-bit logic-function field      */

/* Blitter register byte addresses. */
enum {
    A1_BASE = 0xF02200, A1_FLAGS = 0xF02204, A1_PIXEL = 0xF0220C, A1_STEP = 0xF02210,
    A1_FSTEP = 0xF02214, A1_FPIXEL = 0xF02218, A1_INC = 0xF0221C, A1_FINC = 0xF02220,
    A2_BASE = 0xF02224, A2_FLAGS = 0xF02228, A2_PIXEL = 0xF02230, A2_STEP = 0xF02234,
    B_COUNT = 0xF0223C, B_PATD_HI = 0xF02268, B_PATD_LO = 0xF0226C
};

#define BC_UPDA1F   0x00000100u   /* add A1 fractional step per outer pass */
#define BC_SRCSHADE 0x40000000u   /* add the intensity iterator to source CRY */

enum { B_IINC = 0xF02270 };

/* Pixel depth (bits) from an A1/A2 flags word: field bits[5:3] select
 * 1/2/4/8/16/32 bpp. */
static int flags_bpp(uint32_t flags) {
    static const int bits[8] = { 1, 2, 4, 8, 16, 32, 32, 32 };
    return bits[(flags >> 3) & 7];
}

/* Window pixel width from a flags word. The width lives in bits[14:9] as a small
 * floating-point value; the encoding was recovered from real blits (flags $4220
 * -> 320, $3820 -> 128, $2818 -> 32, each matching the A1_STEP X reset):
 *   f = (flags>>9)&0x3F;  width = (4 + (f & 3)) << ((f>>2) - 2). */
static uint32_t flags_width(uint32_t flags) {
    uint32_t f = (flags >> 9) & 0x3F;
    int e = (int)(f >> 2) - 2;
    uint32_t m = 4 + (f & 3);
    return (e >= 0) ? (m << e) : (m >> (-e));
}

/* Inner-loop pointer update per the A1/A2 flags (Tech Ref A1_FLAGS bits 16-20):
 * X add control 00 = add phrase width and truncate to phrase boundary,
 *               01 = add one pixel (bit 19 sign makes it subtract),
 *               10 = add zero, 11 = add the increment register (approximated +1).
 * Y add control (bit 18) adds one (bit 20 sign subtracts); ignored when X is in
 * add-increment mode.
 *
 * Empirically calibrated update cadence (both shipped setups must work):
 *  - X updates fire after EVERY inner pixel including the last: the boot's
 *    320-wide fills program STEP.x = -320 (compensating 320 X-advances).
 *  - Y updates fire only BETWEEN pixels (n-1 times): Doom's wall columns run
 *    inner=1 with Y-add set AND STEP.y=+1; one Y-advance per pass total.
 * `last` = this was the pass's final pixel. */
static void inner_update(uint32_t flags, int *x, int *y, int last) {
    int xadd  = (int)((flags >> 16) & 3);
    int yadd  = (int)((flags >> 18) & 1);
    int xsign = (int)((flags >> 19) & 1);
    int ysign = (int)((flags >> 20) & 1);
    switch (xadd) {
    case 0: *x += 1; break;      /* phrase mode: contiguous advance (equivalent
                                  * to phrase-at-a-time for aligned runs) */
    case 1: *x += xsign ? -1 : 1; break;
    case 2: break;                                      /* add zero */
    default: *x += 1; break;                            /* add increment: approx */
    }
    if (xadd != 3 && yadd && !last) *y += ysign ? -1 : 1;
}

/* Read / write one pixel of the given depth at pixel index (y*width + x). */
static uint32_t read_px(onca_mem_t *m, uint32_t base, uint32_t width,
                        int bpp, int x, int y) {
    uint32_t idx = (uint32_t)(y * (int)width + x);
    switch (bpp) {
    case 32: return onca_peek32(m, base + idx * 4);
    case 16: return onca_peek16(m, base + idx * 2);
    case 8:  return onca_peek8(m, base + idx);
    case 4:  { uint8_t b = onca_peek8(m, base + idx / 2);
               return (idx & 1) ? (b & 0x0F) : (b >> 4); }
    case 2:  { uint8_t b = onca_peek8(m, base + idx / 4);
               return (b >> (2 * (3 - (idx & 3)))) & 0x3; }
    default: { uint8_t b = onca_peek8(m, base + idx / 8);
               return (b >> (7 - (idx & 7))) & 1; }
    }
}

static void write_px(onca_mem_t *m, uint32_t base, uint32_t width,
                     int bpp, int x, int y, uint32_t v) {
    uint32_t idx = (uint32_t)(y * (int)width + x);
    switch (bpp) {
    case 32: onca_poke32(m, base + idx * 4, v); break;
    case 16: onca_poke16(m, base + idx * 2, (uint16_t)v); break;
    case 8:  onca_poke8(m, base + idx, (uint8_t)v); break;
    case 4:  { uint32_t a = base + idx / 2; uint8_t b = onca_peek8(m, a);
               b = (idx & 1) ? (uint8_t)((b & 0xF0) | (v & 0x0F))
                             : (uint8_t)((b & 0x0F) | ((v & 0x0F) << 4));
               onca_poke8(m, a, b); break; }
    default: /* 1/2 bpp writes are rare for draws; handle the byte-aligned case */
             onca_poke8(m, base + idx / 8, (uint8_t)v); break;
    }
}

/* Logic-function unit: per-bit boolean of source S and destination D selected by
 * the 4-bit function (the four bits map to the S/D truth table). */
static uint32_t lfu(int func, uint32_t s, uint32_t d) {
    uint32_t r = 0;
    if (func & 0x1) r |= ~s & ~d;
    if (func & 0x2) r |= ~s &  d;
    if (func & 0x4) r |=  s & ~d;
    if (func & 0x8) r |=  s &  d;
    return r;
}

/* Blitter registers live in the Tom register file; read them raw so we see the
 * value last written to B_CMD (a plain read of $F02238 returns idle status, not
 * the command). */
static uint32_t breg(onca_mem_t *m, uint32_t addr) {
    uint32_t o = addr & 0xFFFF;
    return ((uint32_t)m->tom[o] << 24) | ((uint32_t)m->tom[o + 1] << 16)
         | ((uint32_t)m->tom[o + 2] << 8) | m->tom[o + 3];
}

static void onca_blitter_run_body(onca_mem_t *m);

void onca_blitter_run(onca_mem_t *m) {
    /* Re-entrancy guard: a blit that writes near B_CMD ($F0223B) would otherwise
     * re-trigger the blitter and recurse without bound. */
    static int in_blit = 0;
    if (in_blit) return;
    in_blit = 1;
    onca_blitter_run_body(m);
    in_blit = 0;
}
static void onca_blitter_run_body(onca_mem_t *m) {
    uint32_t cmd   = breg(m, 0xF02238);
    uint32_t count = breg(m, B_COUNT);
    int inner = (int)(count & 0xFFFF);
    int outer = (int)(count >> 16);
    if (inner <= 0 || outer < 0) return;
    if (inner > 4096) inner = 4096;
    if (outer > 4096) outer = 4096;
    if (outer == 0) outer = 1;

    uint32_t a1base = breg(m, A1_BASE) & 0xFFFFFF;
    uint32_t a1flags = breg(m, A1_FLAGS);
    uint32_t a1pix = breg(m, A1_PIXEL);
    uint32_t a1step = breg(m, A1_STEP);
    int a1bpp = flags_bpp(a1flags);
    uint32_t a1w = flags_width(a1flags);
    int a1x = (int16_t)(a1pix & 0xFFFF), a1y = (int16_t)(a1pix >> 16);
    int a1sx = (int16_t)(a1step & 0xFFFF), a1sy = (int16_t)(a1step >> 16);

    uint32_t a2base = breg(m, A2_BASE) & 0xFFFFFF;
    uint32_t a2flags = breg(m, A2_FLAGS);
    uint32_t a2pix = breg(m, A2_PIXEL);
    uint32_t a2step = breg(m, A2_STEP);
    int a2bpp = flags_bpp(a2flags);
    uint32_t a2w = flags_width(a2flags);
    int a2x = (int16_t)(a2pix & 0xFFFF), a2y = (int16_t)(a2pix >> 16);
    int a2sx = (int16_t)(a2step & 0xFFFF), a2sy = (int16_t)(a2step >> 16);

    int srcen   = (cmd & BC_SRCEN) != 0;
    int dsten   = (cmd & BC_DSTEN) != 0;
    int dsta2   = (cmd & BC_DSTA2) != 0;
    int patdsel = (cmd & BC_PATDSEL) != 0;
    int upda1   = (cmd & BC_UPDA1) != 0;
    int upda1f  = (cmd & BC_UPDA1F) != 0;
    int upda2   = (cmd & BC_UPDA2) != 0;
    int func    = (cmd >> BC_LFU_SHIFT) & 0xF;

    if (m->blit_trace) m->blit_trace(m->blit_trace_ctx, cmd, a1base, a2base, count);

    /* A1's pointer is a 16.16 fixed-point DDA: integer parts in A1_PIXEL,
     * fractions in A1_FPIXEL, stepped per outer pass by A1_STEP (UPDA1) and
     * A1_FSTEP (UPDA1F) - this is how wall columns walk a scaled texture. */
    uint32_t a1fpix  = breg(m, A1_FPIXEL);
    uint32_t a1fstep = breg(m, A1_FSTEP);
    int32_t a1fx = ((int32_t)a1x << 16) | (int32_t)(a1fpix & 0xFFFF);
    int32_t a1fy = ((int32_t)a1y << 16) | (int32_t)(a1fpix >> 16);
    int32_t a1fsx = (upda1 ? ((int32_t)a1sx << 16) : 0) | (upda1f ? (int32_t)(a1fstep & 0xFFFF) : 0);
    int32_t a1fsy = (upda1 ? ((int32_t)a1sy << 16) : 0) | (upda1f ? (int32_t)(a1fstep >> 16) : 0);

    /* A1 X-add mode 3 = "add the increment": a per-pixel 2D fixed-point DDA
     * through A1_INC (integers) / A1_FINC (fractions). Doom's floor/ceiling
     * spans walk their 64x64 flat texture with it - the source u,v arcs across
     * the tile as the span crosses the screen. */
    uint32_t a1incr  = breg(m, A1_INC);
    uint32_t a1fincr = breg(m, A1_FINC);
    int32_t a1incx = (int32_t)((uint32_t)(int16_t)(a1incr & 0xFFFF) << 16) | (int32_t)(a1fincr & 0xFFFF);
    int32_t a1incy = (int32_t)((uint32_t)(int16_t)(a1incr >> 16) << 16)    | (int32_t)(a1fincr >> 16);
    int a1_incmode = (((a1flags >> 16) & 3) == 3);

    /* Channel state, indexed 0=A1 1=A2; role mapping per DSTA2 (default:
     * A1 = destination, A2 = source; DSTA2 swaps - Doom's wall draws copy
     * texture (A1) into the framebuffer (A2) with DSTA2 set). */
    uint32_t basev[2] = { a1base, a2base }, wv[2] = { a1w, a2w };
    uint32_t flg[2]   = { a1flags, a2flags };
    int bppv[2] = { a1bpp, a2bpp };
    int xi[2] = { a1x, a2x }, yi[2] = { a1y, a2y };

    /* Phrase-mode copy: the data path moves source-sized pixels regardless of
     * the destination window's declared pixel size (Doom's wall blits declare
     * the 16bpp framebuffer as a 4bpp window for count bookkeeping; the writes
     * are source-sized). Empirically calibrated - see docs. */
    {
        int Dq = dsta2 ? 1 : 0, Sq = dsta2 ? 0 : 1;
        if (((flg[Dq] >> 16) & 3) == 0 && srcen && !patdsel && bppv[Sq] > bppv[Dq])
            bppv[Dq] = bppv[Sq];
    }
    int D = dsta2 ? 1 : 0, S = dsta2 ? 0 : 1;

    /* Pattern data: 64 bits = one phrase, indexed by the destination X within
     * the phrase (number of pixels per phrase depends on depth). */
    uint32_t pat_hi = breg(m, B_PATD_HI), pat_lo = breg(m, B_PATD_LO);
    int px_per_phrase = bppv[D] ? 64 / bppv[D] : 4;

    /* SRCSHADE intensity iterator (Tech Ref: PATD lanes hold the 8-bit integer
     * intensity, SRCDATA the fractions, IINC the saturating per-pass step).
     * Doom shades its floor/ceiling columns with a distance-lighting ramp:
     * iterator starts at PATD lane 0 and brightens per row. */
    int srcshade = (cmd & BC_SRCSHADE) != 0;
    int64_t ii = ((int64_t)(pat_lo & 0xFF) << 24) | 0;   /* 8.24 accumulator */
    int32_t iinc = (int32_t)breg(m, B_IINC);             /* per-pass step    */

    for (int o = 0; o < outer; o++) {
        for (int i = 0; i < inner; i++) {
            uint32_t s;
            if (patdsel) {
                int slot = px_per_phrase ? (xi[D] & (px_per_phrase - 1)) : 0;
                int shift = (px_per_phrase - 1 - slot) * bppv[D];
                uint64_t phrase = ((uint64_t)pat_hi << 32) | pat_lo;
                s = (uint32_t)(phrase >> shift);
                if (bppv[D] < 32) s &= (1u << bppv[D]) - 1;
            } else if (srcen) {
                s = read_px(m, basev[S], wv[S], bppv[S], xi[S], yi[S]);
            } else {
                s = 0;
            }
            /* Shade the source CRY pixel's intensity byte (saturating). */
            if (srcshade && !patdsel) {
                int shade = (int)(ii >> 24);
                if (shade < 0) shade = 0; else if (shade > 255) shade = 255;
                int y8 = (int)(s & 0xFF) + shade;
                if (y8 > 255) y8 = 255;
                s = (s & 0xFFFFFF00u) | (uint32_t)y8;
            }
            uint32_t d = dsten ? read_px(m, basev[D], wv[D], bppv[D], xi[D], yi[D]) : 0;
            uint32_t res = patdsel ? s : lfu(func, s, d);
            write_px(m, basev[D], wv[D], bppv[D], xi[D], yi[D], res);
            /* A1 in increment mode advances by the 16.16 DDA per pixel;
             * everything else uses the flag-selected simple updates. */
            if (a1_incmode) {
                a1fx += a1incx; a1fy += a1incy;
                xi[0] = a1fx >> 16; yi[0] = a1fy >> 16;
                inner_update(flg[1], &xi[1], &yi[1], i == inner - 1);
            } else {
                inner_update(flg[D], &xi[D], &yi[D], i == inner - 1);
                inner_update(flg[S], &xi[S], &yi[S], i == inner - 1);
            }
        }
        /* Outer-pass pointer updates. A1: fixed-point DDA step (integer via
         * UPDA1, fraction via UPDA1F, carry propagating into the integer).
         * The inner loop advanced the integer part; fold that in first -
         * except in increment mode, where a1fx/a1fy are already exact. */
        if (!a1_incmode) {
            a1fx = ((a1fx & 0xFFFF) | ((int32_t)xi[0] << 16)) + a1fsx;
            a1fy = ((a1fy & 0xFFFF) | ((int32_t)yi[0] << 16)) + a1fsy;
        } else {
            a1fx += a1fsx; a1fy += a1fsy;
        }
        xi[0] = a1fx >> 16; yi[0] = a1fy >> 16;
        if (upda2) { xi[1] += a2sx; yi[1] += a2sy; }
        if (srcshade) ii += iinc;   /* intensity ramp advances per pass */
    }

    /* The pointer registers are LIVE state on real hardware: the blit walks
     * them, and their post-blit values persist. Doom's ceiling/floor spans
     * depend on this - it writes A2_PIXEL once per row, then kicks several
     * span blits that each continue where the previous one ended. Write the
     * walked pointers back. */
    {
        uint32_t o;
        o = A1_PIXEL & 0xFFFF;
        m->tom[o]   = (uint8_t)(yi[0] >> 8); m->tom[o+1] = (uint8_t)yi[0];
        m->tom[o+2] = (uint8_t)(xi[0] >> 8); m->tom[o+3] = (uint8_t)xi[0];
        o = A1_FPIXEL & 0xFFFF;
        m->tom[o]   = (uint8_t)(a1fy >> 8);  m->tom[o+1] = (uint8_t)a1fy;
        m->tom[o+2] = (uint8_t)(a1fx >> 8);  m->tom[o+3] = (uint8_t)a1fx;
        o = A2_PIXEL & 0xFFFF;
        m->tom[o]   = (uint8_t)(yi[1] >> 8); m->tom[o+1] = (uint8_t)yi[1];
        m->tom[o+2] = (uint8_t)(xi[1] >> 8); m->tom[o+3] = (uint8_t)xi[1];
    }
}
