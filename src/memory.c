/*
 * memory.c - Atari Jaguar bus implementation.
 *
 * All memory is modelled as big-endian byte arrays; 16/32-bit accesses
 * assemble MSB-first, matching the 68000. The Tom/Jerry register files carry
 * a few synthesised values (free-running HC/VC video counters, the joypad
 * matrix, Blitter status) and route the GPU/DSP control apertures to the
 * RISC cores; unknown reads return their last-written value and can be logged.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "memory.h"
#include "gpu.h"
#include "blitter.h"
#include <string.h>

/* GPU control-register aperture within Tom ($F02100-$F0211F). */
#define GPU_CTRL_LO 0xF02100u
#define GPU_CTRL_HI 0xF02120u

/* DSP control-register aperture within Jerry ($F1A100-$F1A11F). Same register
 * layout as the GPU block, so it reuses onca_gpu_read_ctrl/write_ctrl. */
#define DSP_CTRL_LO 0xF1A100u
#define DSP_CTRL_HI 0xF1A120u

/* NTSC field timing for the free-running video counters: the 68000 runs at
 * ~13.295 MHz and the display refreshes ~60 times/sec, so a field is ~221,583
 * CPU cycles. VC counts in half-lines (NTSC ~525 half-lines/field). */
#define ONCA_HALFLINES 525u
#define ONCA_CYC_PER_HALFLINE 422u   /* 221583 / 525 */

void onca_mem_init(onca_mem_t *m) {
    memset(m->dram, 0, sizeof(m->dram));
    memset(m->tom, 0, sizeof(m->tom));
    memset(m->jerry, 0, sizeof(m->jerry));
    m->cart = NULL;
    m->cart_size = 0;
    m->overlay = 1;
    m->video_irq = 0;
    m->cycles = NULL;
    m->gpu = NULL;
    m->dsp = NULL;
    m->joypad1 = 0;
    m->joy_row = 0xFF;   /* no row selected until the game writes one */
    m->log = NULL;
    m->log_ctx = NULL;

    /* Seed Tom's version/ID nibble. The Object Processor version lives in the
     * low bits of TOM_INT1's neighbourhood on real silicon; the exact POST
     * check is TBD from tracing, so this is a placeholder. */
    /* JOYBUTS ($F14002): the upper bits carry board configuration. Bit 4 is
     * commonly read as a hardware-present/eeprom indicator and the button
     * bits idle high (no button pressed = 1). Seed all-idle. */
    m->jerry[JERRY_JOYBUTS]     = 0x00;
    m->jerry[JERRY_JOYBUTS + 1] = 0x1F; /* buttons idle high, low 5 bits set */

    /* Blank cart EEPROM: an erased 93C46 reads all-ones, which makes a game's
     * saved-settings checksum fail so it falls back to sane defaults. */
    memset(m->ee_data, 0xFF, sizeof(m->ee_data));
    m->ee_do = 1;
}

void onca_mem_set_cart(onca_mem_t *m, uint8_t *data, size_t size) {
    m->cart = data;
    m->cart_size = size > ONCA_CART_MAX ? ONCA_CART_MAX : size;
}

static inline void do_log(onca_mem_t *m, int w, int width,
                          onca_region_t r, uint32_t a, uint32_t v) {
    if (m->log) m->log(m->log_ctx, w, width, r, a, v);
}

/* ---- synthesised Tom register reads (16-bit) ---- */
static uint16_t tom_reg16(onca_mem_t *m, uint32_t off) {
    if (m->cycles) {
        uint64_t cyc = *m->cycles;
        if (off == TOM_VC)
            return (uint16_t)((cyc / ONCA_CYC_PER_HALFLINE) % ONCA_HALFLINES);
        if (off == TOM_HC)
            return (uint16_t)((cyc % ONCA_CYC_PER_HALFLINE) & 0x3FF);
    }
    /* Blitter command/status B_CMD ($F02238): bit0 = "blitter idle". Blits
     * run to completion synchronously the moment B_CMD is written (see
     * blitter.c), so status always reads idle/done - the busy-wait loops the
     * boot ROM and games run after kicking a blit fall straight through. */
    if (off == 0x2238) return 0x0000;   /* high word of the 32-bit B_CMD */
    if (off == 0x223A) return 0x0001;   /* low word: idle bit set        */
    /* INT1 ($F000E0): reading returns the pending interrupt-source latches.
     * Bit0 = video interrupt. The boot's handler reads this to identify the
     * source (expects 1 for video) before acking with a write. */
    if (off == 0xE0) return m->video_irq ? 0x0001 : 0x0000;
    return ((uint16_t)m->tom[off] << 8) | m->tom[off + 1];
}
/* Pad-1 button matrix, indexed [row 0..3][JOYSTICK bit 8..11] = TJ_* button, and
 * jb1_row[row] = the button on JOYBUTS bit1. Row `r` is scanned when the game
 * writes a JOYSTICK low byte with bit r low (e.g. $FE=row0, $FD=row1, ...). */
static const int joy_matrix[4][4] = {
    { TJ_UP,   TJ_DOWN, TJ_LEFT, TJ_RIGHT },  /* row0 ($FE) */
    { TJ_STAR, TJ_K1,   TJ_K4,   TJ_K7    },  /* row1 ($FD) */
    { TJ_K0,   TJ_K8,   TJ_K5,   TJ_K2    },  /* row2 ($FB) */
    { TJ_HASH, TJ_K9,   TJ_K6,   TJ_K3    },  /* row3 ($F7) */
};
static const int joy_jb1_row[4] = { TJ_A, TJ_B, TJ_C, TJ_OPTION };

/* JOYSTICK ($F14000) read: bits 11..8 report the selected row's four buttons,
 * active low (0 = held). All other bits idle high. */
static uint16_t jaguar_joyst_word(onca_mem_t *m) {
    uint16_t v = 0xFFFF;
    if (!m->ee_do) v &= ~1u;   /* cart EEPROM DO line, active transaction */
    for (int row = 0; row < 4; row++) {
        if (m->joy_row & (1u << row)) continue;          /* row not selected */
        for (int b = 0; b < 4; b++)
            if (m->joypad1 & (1u << joy_matrix[row][b])) v &= ~(1u << (8 + b));
    }
    return v;
}

/* JOYBUTS ($F14002) read: bit1 = selected row's fire button (A/B/C/Option),
 * bit0 = Pause (row0 only), active low; upper bits keep the seeded config. */
static uint16_t jaguar_joybut_word(onca_mem_t *m) {
    uint16_t v = (((uint16_t)m->jerry[JERRY_JOYBUTS] << 8) | m->jerry[JERRY_JOYBUTS + 1]) | 0x0003;
    for (int row = 0; row < 4; row++) {
        if (m->joy_row & (1u << row)) continue;
        if (m->joypad1 & (1u << joy_jb1_row[row])) v &= ~0x0002u;
        if (row == 0 && (m->joypad1 & (1u << TJ_PAUSE))) v &= ~0x0001u;
    }
    return v;
}

/* ---- cart EEPROM (93C46) bit-bang over Jerry GPIO ---- */

/* $F15000 access = chip-select toggle: abort any transaction in progress. */
static void ee_reset(onca_mem_t *m) {
    m->ee_state = 0; m->ee_bits = 0; m->ee_shift = 0; m->ee_wbits = 0;
    m->ee_do = 1;    /* DO idles high (also reads as "ready") */
}

/* Write to $F14800: clock one bit in (DI = bit 0 of the written value). */
static void ee_clock_in(onca_mem_t *m, uint32_t v) {
    int di = (int)(v & 1);
    if (m->ee_state == 2) {                      /* collecting write data */
        m->ee_wdata = (uint16_t)((m->ee_wdata << 1) | di);
        if (++m->ee_wbits == 16) {
            if (m->ee_wen) { m->ee_data[m->ee_addr] = m->ee_wdata; m->ee_dirty = 1; }
            ee_reset(m);
        }
        return;
    }
    m->ee_shift = (uint16_t)((m->ee_shift << 1) | di);
    if (++m->ee_bits < 9) return;
    /* 9-bit command: start(1) | 2-bit opcode | 6-bit address */
    uint16_t cmd = m->ee_shift & 0x1FF;
    int op = (cmd >> 6) & 3, addr = cmd & 0x3F;
    m->ee_bits = 0; m->ee_shift = 0;
    if (!(cmd & 0x100)) return;                  /* no start bit: ignore */
    switch (op) {
    case 2: m->ee_state = 1; m->ee_addr = (uint8_t)addr; m->ee_bit = 15; break;   /* READ  */
    case 1: m->ee_state = 2; m->ee_addr = (uint8_t)addr; m->ee_wbits = 0; break;  /* WRITE */
    case 3: if (m->ee_wen) { m->ee_data[addr] = 0xFFFF; m->ee_dirty = 1; } break; /* ERASE */
    case 0:
        switch ((addr >> 4) & 3) {
        case 3: m->ee_wen = 1; break;                                             /* EWEN  */
        case 0: m->ee_wen = 0; break;                                             /* EWDS  */
        case 2: if (m->ee_wen) { memset(m->ee_data, 0xFF, sizeof(m->ee_data));    /* ERAL  */
                                 m->ee_dirty = 1; } break;
        }
        break;
    }
}

/* Read of $F14800 = one clock pulse: shift the next data bit onto the DO
 * line. The game then samples DO from JOYSTICK ($F14000) bit 0 - that is how
 * the part is wired on the cart connector (Doom's reader: TST $F14800 to
 * clock, then MOVE.W $F14000 / LSR #1 / ADDX to collect the bit). Sequential
 * reads roll into the next word, as the part does. */
static uint16_t ee_clock_out(onca_mem_t *m) {
    if (m->ee_state != 1) return 1;
    m->ee_do = (uint8_t)((m->ee_data[m->ee_addr] >> m->ee_bit) & 1);
    if (--m->ee_bit < 0) { m->ee_bit = 15; m->ee_addr = (m->ee_addr + 1) & 63; }
    return m->ee_do;
}

static uint16_t jerry_reg16(onca_mem_t *m, uint32_t off) {
    if (off == JERRY_JOYSTICK) return jaguar_joyst_word(m);
    if (off == JERRY_JOYBUTS)  return jaguar_joybut_word(m);
    if (off == 0x4800) return ee_clock_out(m);
    if (off == 0x5000) { ee_reset(m); return 0; }
    return ((uint16_t)m->jerry[off] << 8) | m->jerry[off + 1];
}

/* ---- classify + read ---- */
static uint32_t read_impl(onca_mem_t *m, uint32_t a, int width) {
    a &= 0x00FFFFFFu;

    /* Boot overlay: at reset the boot ROM is mirrored across low memory so the
     * 68000 can fetch its reset vector. The first access to the real high-ROM
     * alias ($E00000) means the CPU is now running from ROM proper, so drop the
     * mirror and let low memory read as DRAM thereafter. */
    if (m->overlay) {
        if (a >= ONCA_ROM_BASE && a < ONCA_ROM_END) {
            m->overlay = 0;
        } else if (a < ONCA_CART_BASE) {
            uint32_t o = a & (ONCA_ROM_SIZE - 1);
            uint32_t v = (width == 1) ? m->rom[o]
                       : (width == 2) ? (((uint32_t)m->rom[o] << 8) | m->rom[o + 1])
                       : (((uint32_t)m->rom[o] << 24) | ((uint32_t)m->rom[o + 1] << 16)
                        | ((uint32_t)m->rom[o + 2] << 8) | m->rom[o + 3]);
            do_log(m, 0, width, ONCA_LOG_ROM, a, v);
            return v;
        }
    }

    if (a < ONCA_DRAM_END) {
        const uint8_t *p = &m->dram[a & (ONCA_DRAM_SIZE - 1)];
        uint32_t v = (width == 1) ? p[0]
                   : (width == 2) ? (((uint32_t)p[0] << 8) | p[1])
                   : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                    | ((uint32_t)p[2] << 8) | p[3]);
        do_log(m, 0, width, ONCA_LOG_DRAM, a, v);
        return v;
    }
    if (a >= ONCA_CART_BASE && a < ONCA_CART_END) {
        uint32_t v = 0;
        uint32_t off = a - ONCA_CART_BASE;
        if (m->cart && off < m->cart_size) {
            const uint8_t *p = &m->cart[off];
            size_t rem = m->cart_size - off;
            v = (width == 1) ? p[0]
              : (width == 2) ? (((uint32_t)p[0] << 8) | (rem > 1 ? p[1] : 0))
              : (((uint32_t)p[0] << 24) | ((uint32_t)(rem > 1 ? p[1] : 0) << 16)
               | ((uint32_t)(rem > 2 ? p[2] : 0) << 8) | (rem > 3 ? p[3] : 0));
        }
        do_log(m, 0, width, ONCA_LOG_CART, a, v);
        return v;
    }
    if (a >= ONCA_ROM_BASE && a < ONCA_ROM_END) {
        const uint8_t *p = &m->rom[(a - ONCA_ROM_BASE) & (ONCA_ROM_SIZE - 1)];
        uint32_t v = (width == 1) ? p[0]
                   : (width == 2) ? (((uint32_t)p[0] << 8) | p[1])
                   : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                    | ((uint32_t)p[2] << 8) | p[3]);
        do_log(m, 0, width, ONCA_LOG_ROM, a, v);
        return v;
    }
    if (a >= ONCA_TOM_BASE && a < ONCA_TOM_END) {
        if (m->gpu && a >= GPU_CTRL_LO && a < GPU_CTRL_HI) {
            uint32_t cur = onca_gpu_read_ctrl(m->gpu, a & ~3u);
            uint8_t b[4] = { (uint8_t)(cur >> 24), (uint8_t)(cur >> 16), (uint8_t)(cur >> 8), (uint8_t)cur };
            int o = a & 3; uint32_t v = 0;
            for (int i = 0; i < width; i++) v = (v << 8) | b[o + i];
            do_log(m, 0, width, ONCA_LOG_TOM, a, v);
            return v;
        }
        uint32_t off = a & 0xFFFF;
        uint32_t v = (width == 4)
                   ? (((uint32_t)tom_reg16(m, off) << 16) | tom_reg16(m, (off + 2) & 0xFFFF))
                   : (width == 2) ? tom_reg16(m, off & ~1u)
                   : ((tom_reg16(m, off & ~1u) >> ((off & 1) ? 0 : 8)) & 0xFF);
        /* Cartridge-security bypass: report the pass magic $03D0DEAD for the OS's
         * verdict read of $F03000 (see security_bypass). Scoped to the code that
         * performs the OS-internal check (movea.l #$F03000,a0; move.l (a0),d0;
         * cmp.l #$03D0DEAD,d0 at ~$5E40) so ONLY that gate is affected - not the
         * early boot check at $422 (which must fail for universal-header carts),
         * and not the GPU's own use of $F03000 as scratch. Keying on the check's
         * PC rather than the computed value keeps it robust to whatever the GPU
         * verify actually leaves there. */
        if (m->security_bypass && width == 4 && off == 0x3000 &&
            m->cpu_pc && *m->cpu_pc >= 0x5E00 && *m->cpu_pc < 0x5F00)
            v = 0x03D0DEADu;
        do_log(m, 0, width, ONCA_LOG_TOM, a, v);
        return v;
    }
    if (a >= ONCA_JERRY_BASE && a < ONCA_JERRY_END) {
        if (m->dsp && a >= DSP_CTRL_LO && a < DSP_CTRL_HI) {
            uint32_t cur = onca_gpu_read_ctrl(m->dsp, a & ~3u);
            uint8_t b[4] = { (uint8_t)(cur >> 24), (uint8_t)(cur >> 16), (uint8_t)(cur >> 8), (uint8_t)cur };
            int o = a & 3; uint32_t v = 0;
            for (int i = 0; i < width; i++) v = (v << 8) | b[o + i];
            do_log(m, 0, width, ONCA_LOG_JERRY, a, v);
            return v;
        }
        uint32_t off = a & 0xFFFF;
        uint32_t v = (width == 4)
                   ? (((uint32_t)jerry_reg16(m, off) << 16) | jerry_reg16(m, (off + 2) & 0xFFFF))
                   : (width == 2) ? jerry_reg16(m, off & ~1u)
                   : ((jerry_reg16(m, off & ~1u) >> ((off & 1) ? 0 : 8)) & 0xFF);
        do_log(m, 0, width, ONCA_LOG_JERRY, a, v);
        return v;
    }
    do_log(m, 0, width, ONCA_LOG_OPEN, a, 0);
    return 0;
}

static void write_impl(onca_mem_t *m, uint32_t a, int width, uint32_t v) {
    a &= 0x00FFFFFFu;

    if (a < ONCA_DRAM_END) {
        uint8_t *p = &m->dram[a & (ONCA_DRAM_SIZE - 1)];
        if (width == 1) p[0] = (uint8_t)v;
        else if (width == 2) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        else { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
               p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v; }
        do_log(m, 1, width, ONCA_LOG_DRAM, a, v);
        return;
    }
    if (a >= ONCA_CART_BASE && a < ONCA_CART_END) {
        do_log(m, 1, width, ONCA_LOG_CART, a, v); /* cartridge is read-only */
        return;
    }
    if (a >= ONCA_ROM_BASE && a < ONCA_ROM_END) {
        do_log(m, 1, width, ONCA_LOG_ROM, a, v);  /* boot ROM is read-only */
        return;
    }
    if (a >= ONCA_TOM_BASE && a < ONCA_TOM_END) {
        if (m->gpu && a >= GPU_CTRL_LO && a < GPU_CTRL_HI) {
            uint32_t cur = onca_gpu_read_ctrl(m->gpu, a & ~3u);
            uint8_t b[4] = { (uint8_t)(cur >> 24), (uint8_t)(cur >> 16), (uint8_t)(cur >> 8), (uint8_t)cur };
            int o = a & 3;
            for (int i = 0; i < width; i++) b[o + i] = (uint8_t)(v >> (8 * (width - 1 - i)));
            onca_gpu_write_ctrl(m->gpu, a & ~3u,
                ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
            do_log(m, 1, width, ONCA_LOG_TOM, a, v);
            return;
        }
        uint32_t off = a & 0xFFFF;
        if (width == 1) m->tom[off] = (uint8_t)v;
        else if (width == 2) { m->tom[off] = (uint8_t)(v >> 8); m->tom[off + 1] = (uint8_t)v; }
        else { m->tom[off] = (uint8_t)(v >> 24); m->tom[off + 1] = (uint8_t)(v >> 16);
               m->tom[(off + 2) & 0xFFFF] = (uint8_t)(v >> 8); m->tom[(off + 3) & 0xFFFF] = (uint8_t)v; }
        /* A write to INT1 ($F000E0) acknowledges/clears the video interrupt
         * latch (the handler's ack write). Clearing here is what breaks the
         * interrupt storm: the line drops until VBLANK re-raises it. */
        if (off <= 0xE0 && off + width > 0xE0) m->video_irq = 0;
        /* A write that touches B_CMD ($F02238) kicks the Blitter; run it to
         * completion now so the following status read sees it idle/done. */
        if (off <= 0x2238 && off + width > 0x2238 && !m->blitter_off) onca_blitter_run(m);
        do_log(m, 1, width, ONCA_LOG_TOM, a, v);
        return;
    }
    if (a >= ONCA_JERRY_BASE && a < ONCA_JERRY_END) {
        if (m->dsp && a >= DSP_CTRL_LO && a < DSP_CTRL_HI) {
            uint32_t cur = onca_gpu_read_ctrl(m->dsp, a & ~3u);
            uint8_t b[4] = { (uint8_t)(cur >> 24), (uint8_t)(cur >> 16), (uint8_t)(cur >> 8), (uint8_t)cur };
            int o = a & 3;
            for (int i = 0; i < width; i++) b[o + i] = (uint8_t)(v >> (8 * (width - 1 - i)));
            onca_gpu_write_ctrl(m->dsp, a & ~3u,
                ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
            do_log(m, 1, width, ONCA_LOG_JERRY, a, v);
            return;
        }
        uint32_t off = a & 0xFFFF;
        if (width == 1) m->jerry[off] = (uint8_t)v;
        else if (width == 2) { m->jerry[off] = (uint8_t)(v >> 8); m->jerry[off + 1] = (uint8_t)v; }
        else { m->jerry[off] = (uint8_t)(v >> 24); m->jerry[off + 1] = (uint8_t)(v >> 16);
               m->jerry[(off + 2) & 0xFFFF] = (uint8_t)(v >> 8); m->jerry[(off + 3) & 0xFFFF] = (uint8_t)v; }
        /* A write to JOYSTICK ($F14000) latches its low byte ($F14001) as the
         * controller row-select for the next JOYSTICK/JOYBUTS read. */
        if (off <= (JERRY_JOYSTICK + 1) && off + width > (JERRY_JOYSTICK + 1))
            m->joy_row = m->jerry[JERRY_JOYSTICK + 1];
        /* Cart EEPROM GPIO: a write to $F14800 clocks a bit in; touching
         * $F15000 resets the transaction. */
        if (off <= 0x4800 && off + width > 0x4800) ee_clock_in(m, v);
        if (off <= 0x5000 && off + width > 0x5000) ee_reset(m);
        do_log(m, 1, width, ONCA_LOG_JERRY, a, v);
        return;
    }
    do_log(m, 1, width, ONCA_LOG_OPEN, a, v);
}

/* ---- side-effect-free peeks (for the Object Processor + tools) ---- */
uint8_t onca_peek8(onca_mem_t *m, uint32_t a) {
    a &= 0x00FFFFFFu;
    if (m->gpu && a >= GPU_CTRL_LO && a < GPU_CTRL_HI) {
        uint32_t cur = onca_gpu_read_ctrl(m->gpu, a & ~3u);
        return (uint8_t)(cur >> (8 * (3 - (a & 3))));
    }
    if (m->dsp && a >= DSP_CTRL_LO && a < DSP_CTRL_HI) {
        uint32_t cur = onca_gpu_read_ctrl(m->dsp, a & ~3u);
        return (uint8_t)(cur >> (8 * (3 - (a & 3))));
    }
    if (a < ONCA_DRAM_END) return m->dram[a & (ONCA_DRAM_SIZE - 1)];
    if (a >= ONCA_CART_BASE && a < ONCA_CART_END) {
        uint32_t off = a - ONCA_CART_BASE;
        return (m->cart && off < m->cart_size) ? m->cart[off] : 0;
    }
    if (a >= ONCA_ROM_BASE && a < ONCA_ROM_END) return m->rom[(a - ONCA_ROM_BASE) & (ONCA_ROM_SIZE - 1)];
    if (a >= ONCA_TOM_BASE && a < ONCA_TOM_END) {
        uint32_t off = a & 0xFFFF;
        /* B_CMD ($F02238) reads back Blitter status, not the last command. Blits
         * run synchronously, so it is always idle: the low word reads 0x0001 (idle
         * bit). Mirror what the CPU sees via tom_reg16 so the GPU's own
         * wait-for-blitter poll (LOAD B_CMD; BTST #0) also completes. */
        if (off >= 0x2238 && off <= 0x223B) return (off == 0x223B) ? 0x01 : 0x00;
        return m->tom[off];
    }
    if (a >= ONCA_JERRY_BASE && a < ONCA_JERRY_END) return m->jerry[a & 0xFFFF];
    return 0;
}
uint16_t onca_peek16(onca_mem_t *m, uint32_t a) {
    return ((uint16_t)onca_peek8(m, a) << 8) | onca_peek8(m, a + 1);
}
uint32_t onca_peek32(onca_mem_t *m, uint32_t a) {
    return ((uint32_t)onca_peek16(m, a) << 16) | onca_peek16(m, a + 2);
}

void onca_poke8(onca_mem_t *m, uint32_t a, uint8_t v) {
    a &= 0x00FFFFFFu;
    /* Coprocessor write watch (diagnostics): pokes are the GPU/DSP/Blitter
     * write funnel, invisible to the CPU-bus log, so corruption hunts need
     * this hook to catch a RISC store landing where it shouldn't. */
    if (m->watch_cb && a >= m->watch_lo && a < m->watch_hi)
        m->watch_cb(m->watch_ctx, a, v);
    if (m->gpu && a >= GPU_CTRL_LO && a < GPU_CTRL_HI) {
        uint32_t reg = a & ~3u, cur = onca_gpu_read_ctrl(m->gpu, reg);
        int sh = 8 * (3 - (a & 3));
        cur = (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        onca_gpu_write_ctrl(m->gpu, reg, cur);
        return;
    }
    if (m->dsp && a >= DSP_CTRL_LO && a < DSP_CTRL_HI) {
        uint32_t reg = a & ~3u, cur = onca_gpu_read_ctrl(m->dsp, reg);
        int sh = 8 * (3 - (a & 3));
        cur = (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        onca_gpu_write_ctrl(m->dsp, reg, cur);
        return;
    }
    if (a < ONCA_DRAM_END) m->dram[a & (ONCA_DRAM_SIZE - 1)] = v;
    else if (a >= ONCA_TOM_BASE && a < ONCA_TOM_END) {
        uint32_t off = a & 0xFFFF;
        m->tom[off] = v;
        /* The GPU drives the Blitter too (STORE to B_CMD). A poke completing the
         * low byte of B_CMD ($F0223B) kicks the blit, mirroring the CPU path in
         * write_impl. */
        if (off == 0x223B && !m->blitter_off) onca_blitter_run(m);
    }
    else if (a >= ONCA_JERRY_BASE && a < ONCA_JERRY_END) m->jerry[a & 0xFFFF] = v;
    /* cart/boot ROM are read-only; ignore */
}
void onca_poke16(onca_mem_t *m, uint32_t a, uint16_t v) {
    onca_poke8(m, a, (uint8_t)(v >> 8)); onca_poke8(m, a + 1, (uint8_t)v);
}
void onca_poke32(onca_mem_t *m, uint32_t a, uint32_t v) {
    onca_poke16(m, a, (uint16_t)(v >> 16)); onca_poke16(m, a + 2, (uint16_t)v);
}

/* ---- m68k bus glue ---- */
static uint8_t  bus_read8 (void *c, uint32_t a) { return (uint8_t) read_impl((onca_mem_t *)c, a, 1); }
static uint16_t bus_read16(void *c, uint32_t a) { return (uint16_t)read_impl((onca_mem_t *)c, a, 2); }
static uint32_t bus_read32(void *c, uint32_t a) { return          read_impl((onca_mem_t *)c, a, 4); }
static void bus_write8 (void *c, uint32_t a, uint8_t  v) { write_impl((onca_mem_t *)c, a, 1, v); }
static void bus_write16(void *c, uint32_t a, uint16_t v) { write_impl((onca_mem_t *)c, a, 2, v); }
static void bus_write32(void *c, uint32_t a, uint32_t v) { write_impl((onca_mem_t *)c, a, 4, v); }

void onca_mem_bind(onca_mem_t *m, m68k_bus_t *bus) {
    bus->ctx = m;
    bus->read8  = bus_read8;
    bus->read16 = bus_read16;
    bus->read32 = bus_read32;
    bus->write8  = bus_write8;
    bus->write16 = bus_write16;
    bus->write32 = bus_write32;
}
