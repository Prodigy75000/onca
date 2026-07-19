/*
 * gpu.c - Atari Jaguar GPU (Tom RISC) interpreter (clean-room).
 *
 * 16-bit instructions, big-endian, opcode = bits[15:10], source = bits[9:5],
 * dest = bits[4:0]. Two banks of 32 registers (active bank = G_FLAGS bit14).
 * Written against the public Tom & Jerry Technical Reference Manual; the
 * encoding was cross-checked against the boot ROM's own GPU program.
 *
 * Some fine details of the RISC (exact quick-shift immediate scaling, jump
 * delay slots, the signed-divide 16.16 mode) are implemented best-effort from
 * the public docs and will be tuned as the boot program is validated. Where a
 * choice was made it is commented.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gpu.h"
#include <stdio.h>
#include <string.h>

void onca_gpu_init(onca_gpu_t *g, onca_mem_t *mem) {
    for (int i = 0; i < 64; i++) g->reg[i] = 0;
    g->pc = 0; g->ctrl = 0; g->hidata = 0; g->remain = 0; g->divctrl = 0;
    g->mtxc = g->mtxa = 0;
    g->zf = g->cf = g->nf = 0; g->bank = 0; g->flags_hi = 0;
    g->acc = 0; g->running = 0; g->cycles = 0;
    g->delay_target = 0; g->delay_pending = 0; g->mac_lock = 0;
    g->mod = 0; g->is_dsp = 0;
    g->mem = mem;
    memset(g->imm_off, 0, sizeof(g->imm_off));
    memset(g->imm_word, 0, sizeof(g->imm_word));
    g->last_jmp_pc = 0; g->last_jmp_op = 0; g->last_jmp_tgt = 0;
    g->misaligns = 0;
    g->misalign_pc = 0; g->misalign_jmp_pc = 0; g->misalign_jmp_op = 0; g->misalign_tgt = 0;
}

/* ---- G_FLAGS assemble / parse ---- */
static uint32_t flags_get(onca_gpu_t *g) {
    uint32_t f = g->flags_hi & ~(GF_ZERO | GF_CARRY | GF_NEG | GF_REGPAGE);
    if (g->zf) f |= GF_ZERO;
    if (g->cf) f |= GF_CARRY;
    if (g->nf) f |= GF_NEG;
    if (g->bank) f |= GF_REGPAGE;
    return f;
}
static void flags_set(onca_gpu_t *g, uint32_t f) {
    g->zf = (f & GF_ZERO) ? 1 : 0;
    g->cf = (f & GF_CARRY) ? 1 : 0;
    g->nf = (f & GF_NEG) ? 1 : 0;
    g->bank = (f & GF_REGPAGE) ? 1 : 0;
    g->flags_hi = f & ~(GF_ZERO | GF_CARRY | GF_NEG | GF_REGPAGE);
}

/* The Tom GPU control block ($F02100) and the Jerry DSP control block ($F1A100)
 * have identical register layouts, so switch on the offset within the block
 * (addr & 0x1C) - the same code drives both cores at their different bases. */
void onca_gpu_write_ctrl(onca_gpu_t *g, uint32_t addr, uint32_t v) {
    switch (addr & 0x1Cu) {
    case 0x00: flags_set(g, v); break;    /* G_FLAGS / D_FLAGS */
    case 0x04: g->mtxc = v; break;        /* MTXC (GPU) / D_MOD (DSP) */
    case 0x08: g->mtxa = v; break;        /* MTXA (GPU) */
    case 0x0C: break;                     /* END: endian latch, we fetch big-endian */
    case 0x10: g->pc = v & 0xFFFFFF; break;
    case 0x14:                            /* G_CTRL / D_CTRL */
        g->ctrl = v;
        g->running = (v & GC_GPUGO) ? 1 : 0;
        break;
    case 0x18:                            /* G_HIDATA (Tom) / D_MOD (Jerry) -
                                           * the one offset where the two
                                           * control blocks differ. Doom's DSP
                                           * kernel writes $FFFFE000 here for
                                           * its 8KB sample ring. */
        if (g->is_dsp) g->mod = v; else g->hidata = v;
        break;
    case 0x1C: g->divctrl = v; break;     /* write side = DIVCTRL */
    default: break;
    }
}

uint32_t onca_gpu_read_ctrl(onca_gpu_t *g, uint32_t addr) {
    switch (addr & 0x1Cu) {
    case 0x00: return flags_get(g);
    case 0x04: return g->mtxc;
    case 0x08: return g->mtxa;
    case 0x10: return g->pc;
    case 0x14: return g->ctrl | (g->running ? GC_GPUGO : 0);
    case 0x18: return g->is_dsp ? g->mod : g->hidata;
    case 0x1C: return g->remain;
    default: return 0;
    }
}

/* ---- flag helpers ---- */
static inline void set_zn(onca_gpu_t *g, uint32_t r) {
    g->zf = (r == 0); g->nf = (r >> 31) & 1;
}

/* Take a RISC interrupt (Tom manual): interrupt `num` vectors to RAM_base +
 * num*0x10. The master mask (FLAGS bit 3) is set, the PRIMARY (bank 0) register
 * bank is forced, and the resume address is pushed onto the primary R31 stack
 * (R31 -= 4; mem[R31] = pc-2, since the service routine adds 2 before returning).
 * Caller must have checked the source is enabled and not masked. */
int onca_gpu_interrupt(onca_gpu_t *g, int num) {
    /* Refuse at unclean boundaries (mid-branch, inside a MAC group, already in
     * an ISR). Returns 0 so the caller keeps the interrupt OWED and retries -
     * dropping it instead skews every consumer of the ISR's sample counter
     * (Doom's music ran at ~70% speed because deliveries landing in the ISR's
     * own return-jump delay slot were being discarded). */
    if (!g->running || g->delay_pending || g->mac_lock) return 0;
    if (g->flags_hi & 0x08) return 0;               /* IMASK already set */
    uint32_t base = g->is_dsp ? 0xF1B000u : 0xF03000u;
    uint32_t r31 = (g->reg[31] - 4) & 0xFFFFFF;     /* primary bank stack */
    onca_poke32(g->mem, r31, (g->pc - 2) & 0xFFFFFF);
    g->reg[31] = r31;
    g->flags_hi |= 0x08;                            /* set master interrupt mask */
    /* Setting IMASK forces the PRIMARY bank for register access (see the active-
     * bank calc in onca_gpu_step), but g->bank keeps the underlying code's
     * REGPAGE value so the ISR's save/restore of D_FLAGS returns to it. */
    g->pc = base + (uint32_t)(num & 7) * 0x10;
    return 1;
}

/* ---- jump condition ---- */
/* Jaguar RISC condition code (5 bits): bits[1:0] are the Z condition
 * (bit0 = require Z clear, bit1 = require Z set); bit4 selects whether bits[3:2]
 * refer to C (bit4=0) or N (bit4=1): bit2 = require flag clear, bit3 = set.
 * cc == 0 is "always". Verified against the boot cart-check (uses NN = 0x14). */
static int cond(onca_gpu_t *g, int cc) {
    int take = 1;
    if (cc & 0x01) take &= (g->zf == 0);
    if (cc & 0x02) take &= (g->zf == 1);
    if (cc & 0x10) {                       /* N-flag conditions */
        if (cc & 0x04) take &= (g->nf == 0);
        if (cc & 0x08) take &= (g->nf == 1);
    } else {                               /* C-flag conditions */
        if (cc & 0x04) take &= (g->cf == 0);
        if (cc & 0x08) take &= (g->cf == 1);
    }
    return take;
}

/* ---- instruction fetch ---- */
static inline uint16_t fetch(onca_gpu_t *g) {
    uint16_t w = onca_peek16(g->mem, g->pc);
    g->pc += 2;
    return w;
}

int onca_gpu_step(onca_gpu_t *g) {
    if (!g->running) return 0;

    /* Register bank: while the master interrupt mask is set, the PRIMARY bank
     * (0) is forced for register access regardless of REGPAGE (g->bank). */
    int active_bank = (g->flags_hi & 0x08) ? 0 : g->bank;
    uint32_t *R   = &g->reg[active_bank * 32];
    uint32_t *ALT = &g->reg[(active_bank ^ 1) * 32];

    /* A branch taken on the previous instruction applies after this one (the
     * delay slot) executes. Capture it before we run the delay-slot op. */
    uint8_t  apply = g->delay_pending;
    uint32_t apply_target = g->delay_target;
    g->delay_pending = 0;

    uint32_t ins_pc = g->pc;
    uint16_t op = fetch(g);
    if (g->trace) g->trace(g->trace_ctx, ins_pc, op);

    /* PC-alignment assert: are we executing a word previously marked as a
     * MOVEI immediate? Validate the claim against live RAM first, so overlay
     * reloads (the loadloop rewrites this RAM constantly) self-invalidate. */
    {
        uint32_t ram_base  = g->is_dsp ? 0xF1B000u : G_RAM_BASE;
        uint32_t ram_words = g->is_dsp ? 4096u : 2048u;
        uint32_t widx = (ins_pc - ram_base) >> 1;
        if (ins_pc >= ram_base && widx < ram_words && g->imm_off[widx]) {
            uint32_t owner = ins_pc - 2u * g->imm_off[widx];
            uint16_t ow = onca_peek16(g->mem, owner);
            if (ow == g->imm_word[widx] && ((ow >> 10) & 0x3F) == 38) {
                if (g->misaligns == 0) {
                    g->misalign_pc     = ins_pc;
                    g->misalign_jmp_pc = g->last_jmp_pc;
                    g->misalign_jmp_op = g->last_jmp_op;
                    g->misalign_tgt    = g->last_jmp_tgt;
                }
                if (g->misaligns < 4)
                    fprintf(stderr, "[ONCA-ALIGN] %s PC=%06X inside MOVEI@%06X imm%d;"
                            " guilty branch: pc=%06X op=%04X tgt=%06X\n",
                            g->is_dsp ? "DSP" : "GPU", ins_pc, owner, g->imm_off[widx],
                            g->last_jmp_pc, g->last_jmp_op, g->last_jmp_tgt);
                g->misaligns++;
            } else {
                g->imm_off[widx] = 0;   /* stale claim: RAM was rewritten */
            }
        }
    }

    int opcode = (op >> 10) & 0x3F;
    int sfield = (op >> 5) & 0x1F;   /* source reg / immediate */
    int dfield = op & 0x1F;          /* dest reg / condition    */

    uint32_t rs = R[sfield];
    uint32_t rd = R[dfield];
    uint32_t res;

    switch (opcode) {
    case 0: /* ADD */
        res = rd + rs; g->cf = (res < rd); R[dfield] = res; set_zn(g, res); break;
    case 1: /* ADDC */ {
        uint64_t f = (uint64_t)rd + rs + g->cf; res = (uint32_t)f;
        g->cf = (f >> 32) & 1; R[dfield] = res; set_zn(g, res); break; }
    case 2: /* ADDQ  n (n:1..32, 0->32) */ {
        uint32_t n = sfield ? sfield : 32; uint64_t f = (uint64_t)rd + n;
        res = (uint32_t)f; g->cf = (f >> 32) & 1; R[dfield] = res; set_zn(g, res); break; }
    case 3: /* ADDQT n (no flags) */
        R[dfield] = rd + (sfield ? sfield : 32); break;
    case 4: /* SUB */
        res = rd - rs; g->cf = (rd < rs); R[dfield] = res; set_zn(g, res); break;
    case 5: /* SUBC */ {
        uint64_t f = (uint64_t)rd - rs - g->cf; res = (uint32_t)f;
        g->cf = (f >> 32) & 1; R[dfield] = res; set_zn(g, res); break; }
    case 6: /* SUBQ n */ {
        uint32_t n = sfield ? sfield : 32; res = rd - n; g->cf = (rd < n);
        R[dfield] = res; set_zn(g, res); break; }
    case 7: /* SUBQT n (no flags) */
        R[dfield] = rd - (sfield ? sfield : 32); break;
    case 8: /* NEG */
        res = (uint32_t)(0 - (int32_t)rd); g->cf = (rd != 0); R[dfield] = res; set_zn(g, res); break;
    case 9: /* AND */
        res = rd & rs; R[dfield] = res; set_zn(g, res); break;
    case 10: /* OR */
        res = rd | rs; R[dfield] = res; set_zn(g, res); break;
    case 11: /* XOR */
        res = rd ^ rs; R[dfield] = res; set_zn(g, res); break;
    case 12: /* NOT */
        res = ~rd; R[dfield] = res; set_zn(g, res); break;
    case 13: /* BTST n */
        g->zf = ((rd >> (sfield & 31)) & 1) ? 0 : 1; break;
    case 14: /* BSET n */
        res = rd | (1u << (sfield & 31)); R[dfield] = res; set_zn(g, res); break;
    case 15: /* BCLR n */
        res = rd & ~(1u << (sfield & 31)); R[dfield] = res; set_zn(g, res); break;
    case 16: /* MULT: unsigned 16x16 (bottom halves) -> 32 (Tom manual). The
              * boot's bignum code RORQ #16s limbs DOWN into the low half and
              * masks operands itself - both tells that the hardware multiplies
              * low 16s. (An earlier 32x32 reading here broke Doom's fixed-
              * point physics: FixedMul composes 16-bit half-products.) */
        res = (rd & 0xFFFF) * (rs & 0xFFFF); R[dfield] = res; set_zn(g, res); break;
    case 17: /* IMULT: signed 16x16 (bottom halves) -> 32 */
        res = (uint32_t)((int32_t)(int16_t)rd * (int32_t)(int16_t)rs);
        R[dfield] = res; set_zn(g, res); break;
    case 18: /* IMULTN - signed multiply into accumulator, no writeback.
              * MAC groups are atomic (the result register is not preserved by
              * interrupts, Tom manual): mac_lock holds off interrupt delivery
              * until the group ends. */
        g->acc = (int64_t)((int32_t)(int16_t)rd * (int32_t)(int16_t)rs); set_zn(g, (uint32_t)g->acc);
        g->mac_lock = 1; break;
    case 19: /* RESMAC */
        R[dfield] = (uint32_t)g->acc; g->mac_lock = 0; break;
    case 20: /* IMACN - multiply-accumulate */
        g->acc += (int64_t)((int32_t)(int16_t)rd * (int32_t)(int16_t)rs);
        g->mac_lock = 1; break;
    case 21: /* DIV (unsigned; 16.16 if DIVCTRL bit0) */
        if (rs == 0) { R[dfield] = 0xFFFFFFFFu; g->remain = 0; }
        else if (g->divctrl & 1) {
            uint64_t num = (uint64_t)rd << 16; R[dfield] = (uint32_t)(num / rs);
            g->remain = (uint32_t)(num % rs);
        } else { R[dfield] = rd / rs; g->remain = rd % rs; }
        break;
    case 22: /* ABS */ {
        int32_t s = (int32_t)rd; g->cf = (rd >> 31) & 1;
        res = (s < 0) ? (uint32_t)(-s) : rd; R[dfield] = res; set_zn(g, res); break; }
    case 23: /* SH  (Rs signed: >=0 shift right, <0 shift left) */ {
        int32_t n = (int32_t)rs; uint32_t v = rd;
        if (n >= 0) { int c = n & 63; g->cf = (c && (v >> (c ? c - 1 : 0)) & 1); res = c >= 32 ? 0 : v >> c; }
        else { int c = (-n) & 63; g->cf = (c && (v >> (32 - c)) & 1); res = c >= 32 ? 0 : v << c; }
        R[dfield] = res; set_zn(g, res); break; }
    case 24: /* SHLQ #n : shift left by (32 - n) */ {
        int c = (32 - sfield) & 63; g->cf = (c && (rd >> (32 - c)) & 1);
        res = c >= 32 ? 0 : rd << c; R[dfield] = res; set_zn(g, res); break; }
    case 25: /* SHRQ #n */ {
        int c = sfield ? sfield : 32; g->cf = (rd >> (c - 1)) & 1;
        res = c >= 32 ? 0 : rd >> c; R[dfield] = res; set_zn(g, res); break; }
    case 26: /* SHA (arithmetic, Rs signed direction) */ {
        int32_t n = (int32_t)rs; int32_t v = (int32_t)rd;
        if (n >= 0) { int c = n & 63; g->cf = (c && (rd >> (c ? c - 1 : 0)) & 1); res = (uint32_t)(c >= 32 ? (v >> 31) : (v >> c)); }
        else { int c = (-n) & 63; g->cf = (c && (rd >> (32 - c)) & 1); res = c >= 32 ? 0 : rd << c; }
        R[dfield] = res; set_zn(g, res); break; }
    case 27: /* SHARQ #n */ {
        int c = sfield ? sfield : 32; g->cf = (rd >> (c - 1)) & 1;
        res = (uint32_t)(c >= 32 ? ((int32_t)rd >> 31) : ((int32_t)rd >> c));
        R[dfield] = res; set_zn(g, res); break; }
    case 28: /* ROR (rotate right by Rs); C = MSB of result (bit rotated in) */ {
        int c = rs & 31; res = c ? ((rd >> c) | (rd << (32 - c))) : rd;
        g->cf = (res >> 31) & 1; R[dfield] = res; set_zn(g, res); break; }
    case 29: /* RORQ #n */ {
        int c = (sfield ? sfield : 32) & 31; res = c ? ((rd >> c) | (rd << (32 - c))) : rd;
        g->cf = (res >> 31) & 1; R[dfield] = res; set_zn(g, res); break; }
    case 30: /* CMP */
        res = rd - rs; g->cf = (rd < rs); set_zn(g, res); break;
    case 31: /* CMPQ (n signed 5-bit) */ {
        int32_t n = (sfield & 0x10) ? (sfield | ~0x1F) : sfield;
        res = rd - (uint32_t)n; g->cf = (rd < (uint32_t)n); set_zn(g, res); break; }
    case 32: /* SAT8 */ {
        int32_t s = (int32_t)rd; res = s < 0 ? 0 : (s > 255 ? 255 : (uint32_t)s);
        R[dfield] = res; set_zn(g, res); break; }
    case 33: /* SAT16 (Tom: unsigned, pixel math) / SAT16S (Jerry: signed -
              * the DSP variant saturates to [-32768,32767]; Doom's mixer runs
              * its accumulated audio through it, so an unsigned clamp here
              * half-wave-rectifies every sound). */ {
        int32_t s = (int32_t)rd;
        if (g->is_dsp) res = (uint32_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
        else           res = s < 0 ? 0 : (s > 65535 ? 65535 : (uint32_t)s);
        R[dfield] = res; set_zn(g, res); break; }
    case 34: /* MOVE Rs,Rd */
        R[dfield] = rs; break;
    case 35: /* MOVEQ #n */
        R[dfield] = (uint32_t)sfield; break;
    case 36: /* MOVETA Rs,Rd  (to alternate bank) */
        ALT[dfield] = rs; break;
    case 37: /* MOVEFA Rs,Rd  (from alternate bank) */
        R[dfield] = ALT[sfield]; break;
    case 38: /* MOVEI #imm32 (low word first) */ {
        uint32_t lo = fetch(g), hi = fetch(g); R[dfield] = lo | (hi << 16);
        /* Mark the two immediate words for the PC-alignment assert. */
        {
            uint32_t ram_base  = g->is_dsp ? 0xF1B000u : G_RAM_BASE;
            uint32_t ram_words = g->is_dsp ? 4096u : 2048u;
            uint32_t widx = (ins_pc - ram_base) >> 1;
            if (ins_pc >= ram_base && widx + 2 < ram_words) {
                g->imm_off[widx + 1] = 1; g->imm_word[widx + 1] = op;
                g->imm_off[widx + 2] = 2; g->imm_word[widx + 2] = op;
            }
        }
        break; }
    case 39: /* LOADB */
        R[dfield] = onca_peek8(g->mem, rs); break;
    case 40: /* LOADW */
        R[dfield] = onca_peek16(g->mem, rs); break;
    case 41: /* LOAD */
        R[dfield] = onca_peek32(g->mem, rs); break;
    case 42: /* LOADP (phrase) */
        g->hidata = onca_peek32(g->mem, rs); R[dfield] = onca_peek32(g->mem, rs + 4); break;
    case 43: /* LOAD (R14+n) */
        R[dfield] = onca_peek32(g->mem, R[14] + (uint32_t)sfield * 4); break;
    case 44: /* LOAD (R15+n) */
        R[dfield] = onca_peek32(g->mem, R[15] + (uint32_t)sfield * 4); break;
    /* STORE: address is the SOURCE-field register, data is the DEST-field
     * register - i.e. mem[Rs] = Rd (symmetric with LOAD Rd = mem[Rs]). */
    case 45: /* STOREB */
        onca_poke8(g->mem, rs, (uint8_t)rd); break;
    case 46: /* STOREW */
        onca_poke16(g->mem, rs, (uint16_t)rd); break;
    case 47: /* STORE */
        onca_poke32(g->mem, rs, rd); break;
    case 48: /* STOREP (phrase) */
        onca_poke32(g->mem, rs, g->hidata); onca_poke32(g->mem, rs + 4, rd); break;
    case 49: /* STORE (R14+n) */
        onca_poke32(g->mem, R[14] + (uint32_t)sfield * 4, rd); break;
    case 50: /* STORE (R15+n) */
        onca_poke32(g->mem, R[15] + (uint32_t)sfield * 4, rd); break;
    case 51: /* MOVE PC,Rd */
        R[dfield] = ins_pc; break;
    case 52: /* JUMP cc,(Rs) - takes effect after the delay slot */
        if (cond(g, dfield)) { g->delay_target = rs & 0xFFFFFF; g->delay_pending = 1;
            g->last_jmp_pc = ins_pc; g->last_jmp_op = op; g->last_jmp_tgt = g->delay_target; }
        break;
    case 53: /* JR cc,n (signed 5-bit word offset) - delayed */ {
        int32_t off = (sfield & 0x10) ? (sfield | ~0x1F) : sfield;
        if (cond(g, dfield)) { g->delay_target = (ins_pc + 2 + off * 2) & 0xFFFFFF; g->delay_pending = 1;
            g->last_jmp_pc = ins_pc; g->last_jmp_op = op; g->last_jmp_tgt = g->delay_target; }
        break; }
    case 54: if (g->is_dsp) { /* SUBQMOD #n,Rn (DSP-only): subtract, wrapping the
              * offset within the D_MOD modulo mask. Bits SET in D_MOD are
              * preserved from the original register (the ring's base address);
              * CLEAR bits take the subtraction result (the wrapping offset).
              * Doom's sample ring: D_MOD=$FFFFE000, 8KB ring at $1F0000. */
        uint32_t n = sfield ? sfield : 32; uint32_t raw = rd - n;
        res = (rd & g->mod) | (raw & ~g->mod);
        R[dfield] = res; g->cf = (rd < n); set_zn(g, res); break;
    } else { /* MMULT - systolic matrix multiply (Tom manual). One source
                * matrix is in the SECONDARY register bank, packed two signed
                * 16-bit elements per 32-bit register, starting at the source
                * register (sfield); the other is N signed 16-bit words in local
                * RAM at MTXA. Result = signed sum of the N products -> R[dfield]
                * (current bank). N = MTXC MWIDTH (3..15); MTXC bit4 (MADDW) picks
                * matrix-in-RAM stride: down a column (N apart) vs along a row (1
                * apart). Used for the boot logo's glyph transform. */
        int N = (int)(g->mtxc & 0x0F);
        if (N < 3) N = 3;
        int memstride = ((g->mtxc & 0x10) ? N : 1) * 2;   /* bytes per RAM element */
        uint32_t *SEC = &g->reg[32];                       /* secondary bank (bank 1) */
        int64_t sum = 0;
        uint32_t addr = g->mtxa;
        for (int i = 0; i < N; i++) {
            uint32_t packed = SEC[(sfield + i / 2) & 0x1F];
            int32_t rv = (int32_t)(int16_t)((i & 1) ? (packed >> 16) : (packed & 0xFFFF));
            int32_t mv = (int32_t)(int16_t)onca_peek16(g->mem, addr);
            sum += (int64_t)rv * mv;
            addr += memstride;
        }
        R[dfield] = (uint32_t)sum;
        break; }
    case 55: /* MTOI */
        R[dfield] = rs; break;      /* number-format convert: stubbed as move */
    case 56: /* NORMI */
        R[dfield] = rs; break;      /* stubbed */
    case 57: /* NOP */
        break;
    case 58: /* LOAD (R14+Rn) */
        R[dfield] = onca_peek32(g->mem, R[14] + rs); break;
    case 59: /* LOAD (R15+Rn) */
        R[dfield] = onca_peek32(g->mem, R[15] + rs); break;
    case 60: /* STORE (R14+Rn) */
        onca_poke32(g->mem, R[14] + rs, rd); break;
    case 61: /* STORE (R15+Rn) */
        onca_poke32(g->mem, R[15] + rs, rd); break;
    case 62: /* SAT24 */ {
        int32_t s = (int32_t)rd; res = s < 0 ? 0 : (s > 0xFFFFFF ? 0xFFFFFF : (uint32_t)s);
        R[dfield] = res; set_zn(g, res); break; }
    case 63:
        if (g->is_dsp) { /* ADDQMOD #n,Rn (DSP-only): add, wrapping within D_MOD.
                          * Set mask bits preserve the original (ring base);
                          * clear bits take the sum (offset wraps). */
            uint32_t n = sfield ? sfield : 32; uint32_t raw = rd + n;
            res = (rd & g->mod) | (raw & ~g->mod);
            g->cf = (raw < rd); R[dfield] = res; set_zn(g, res);
        } else {         /* PACK / UNPACK (Tom) - pixel (un)pack, stubbed as move */
            R[dfield] = rd;
        }
        break;
    }

    /* This instruction was the delay slot of a prior taken branch: transfer
     * control now. If the delay slot was itself a branch, its target pends. */
    if (apply) {
        uint8_t  new_pending = g->delay_pending;
        uint32_t new_target  = g->delay_target;
        g->pc = apply_target;
        g->delay_pending = new_pending;
        g->delay_target = new_target;
    }

    g->cycles += 1;
    return 1;
}

uint64_t onca_gpu_run(onca_gpu_t *g, uint64_t budget) {
    uint64_t start = g->cycles;
    while (g->running && g->cycles - start < budget) {
        if (!onca_gpu_step(g)) break;
    }
    return g->cycles - start;
}
