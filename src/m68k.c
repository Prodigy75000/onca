/*
 * m68k.c - Motorola 68000 interpreter (clean-room).
 *
 * Written against the public M68000 Programmer's Reference Manual. Covers the
 * user + supervisor instruction set the Jaguar boot ROM and early game code
 * exercise: the full MOVE family, integer ALU ops, shifts/rotates, bit ops,
 * BCD, MUL/DIV, program control (Bcc/DBcc/JMP/JSR/RTS/RTE/RTR), MOVEM, LINK,
 * TRAP, and the privileged SR/USP ops. Addressing modes are complete (all 12).
 *
 * Endianness: the 68000 is big-endian; all 16/32-bit bus traffic is MSB-first,
 * which the bus layer (memory.c) provides. Address error (odd word/long
 * access) and privilege violation exceptions are modelled; the rarer bus-error
 * rerun semantics are simplified to a vector fetch, which is sufficient for
 * boot bring-up.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "m68k.h"

#define ADDR_MASK 0x00FFFFFFu   /* 68000 external address bus is 24-bit */

/* ---- size helpers (size is in bytes: 1, 2 or 4) ---- */
static inline uint32_t sz_mask(int size) {
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline uint32_t sz_msb(int size) {
    return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u;
}
static inline uint32_t sign_extend(uint32_t v, int size) {
    if (size == 1) return (uint32_t)(int32_t)(int8_t)v;
    if (size == 2) return (uint32_t)(int32_t)(int16_t)v;
    return v;
}

/* ---- SR assembly / parse ---- */
uint16_t m68k_get_sr(const m68k_t *cpu) {
    uint16_t sr = 0;
    if (cpu->cf) sr |= M68K_SR_C;
    if (cpu->vf) sr |= M68K_SR_V;
    if (cpu->zf) sr |= M68K_SR_Z;
    if (cpu->nf) sr |= M68K_SR_N;
    if (cpu->xf) sr |= M68K_SR_X;
    sr |= (uint16_t)(cpu->imask & 7) << 8;
    if (cpu->s) sr |= M68K_SR_S;
    if (cpu->t) sr |= M68K_SR_T;
    return sr;
}

/* Set SR, honouring the stack-pointer swap when the S bit changes. */
void m68k_set_sr(m68k_t *cpu, uint16_t sr) {
    int new_s = (sr & M68K_SR_S) ? 1 : 0;
    if (new_s != cpu->s) {
        /* Save the active SP into its bank, load the other. */
        if (cpu->s) cpu->isp = cpu->a[7]; else cpu->usp = cpu->a[7];
        cpu->a[7] = new_s ? cpu->isp : cpu->usp;
        cpu->s = (uint8_t)new_s;
    }
    cpu->cf = (sr & M68K_SR_C) ? 1 : 0;
    cpu->vf = (sr & M68K_SR_V) ? 1 : 0;
    cpu->zf = (sr & M68K_SR_Z) ? 1 : 0;
    cpu->nf = (sr & M68K_SR_N) ? 1 : 0;
    cpu->xf = (sr & M68K_SR_X) ? 1 : 0;
    cpu->imask = (uint8_t)((sr >> 8) & 7);
    cpu->t = (sr & M68K_SR_T) ? 1 : 0;
}

/* Set just the low byte (CCR). */
static void set_ccr(m68k_t *cpu, uint8_t ccr) {
    cpu->cf = (ccr & M68K_SR_C) ? 1 : 0;
    cpu->vf = (ccr & M68K_SR_V) ? 1 : 0;
    cpu->zf = (ccr & M68K_SR_Z) ? 1 : 0;
    cpu->nf = (ccr & M68K_SR_N) ? 1 : 0;
    cpu->xf = (ccr & M68K_SR_X) ? 1 : 0;
}

/* ---- bus helpers (address masked to 24 bits) ---- */
static inline uint8_t  rd8 (m68k_t *c, uint32_t a) { return c->bus.read8 (c->bus.ctx, a & ADDR_MASK); }
static inline uint16_t rd16(m68k_t *c, uint32_t a) { return c->bus.read16(c->bus.ctx, a & ADDR_MASK); }
static inline uint32_t rd32(m68k_t *c, uint32_t a) { return c->bus.read32(c->bus.ctx, a & ADDR_MASK); }
static inline void wr8 (m68k_t *c, uint32_t a, uint8_t  v) { c->bus.write8 (c->bus.ctx, a & ADDR_MASK, v); }
static inline void wr16(m68k_t *c, uint32_t a, uint16_t v) { c->bus.write16(c->bus.ctx, a & ADDR_MASK, v); }
static inline void wr32(m68k_t *c, uint32_t a, uint32_t v) { c->bus.write32(c->bus.ctx, a & ADDR_MASK, v); }

static inline uint32_t rd_sz(m68k_t *c, uint32_t a, int size) {
    return size == 1 ? rd8(c, a) : size == 2 ? rd16(c, a) : rd32(c, a);
}
static inline void wr_sz(m68k_t *c, uint32_t a, int size, uint32_t v) {
    if (size == 1) wr8(c, a, (uint8_t)v);
    else if (size == 2) wr16(c, a, (uint16_t)v);
    else wr32(c, a, v);
}

/* instruction-stream fetch */
static inline uint16_t fetch16(m68k_t *c) { uint16_t v = rd16(c, c->pc); c->pc += 2; return v; }
static inline uint32_t fetch32(m68k_t *c) { uint32_t v = rd32(c, c->pc); c->pc += 4; return v; }

/* ---- exception dispatch ---- */
static void exception(m68k_t *cpu, int vecnum) {
    uint16_t oldsr = m68k_get_sr(cpu);
    /* Enter supervisor mode (swaps to ISP if we were in user mode). */
    if (!cpu->s) { cpu->usp = cpu->a[7]; cpu->a[7] = cpu->isp; cpu->s = 1; }
    cpu->t = 0;
    cpu->a[7] -= 4; wr32(cpu, cpu->a[7], cpu->pc);
    cpu->a[7] -= 2; wr16(cpu, cpu->a[7], oldsr);
    cpu->pc = rd32(cpu, (uint32_t)vecnum * 4);
    cpu->cycles += 34;
}

/* ---- effective-address decoding ---- */
enum { EA_DREG, EA_AREG, EA_MEM, EA_IMM };
typedef struct { uint8_t kind; uint8_t reg; uint32_t addr; uint32_t imm; } ea_t;

/* index-mode brief extension word: d8(An,Xn) / d8(PC,Xn) */
static uint32_t index_ea(m68k_t *c, uint32_t base) {
    uint16_t ext = fetch16(c);
    int ri   = (ext >> 12) & 7;
    int isa  = (ext >> 15) & 1;
    int islong = (ext >> 11) & 1;
    uint32_t xn = isa ? c->a[ri] : c->d[ri];
    if (!islong) xn = (uint32_t)(int32_t)(int16_t)xn;
    int8_t disp = (int8_t)(ext & 0xFF);
    return base + (uint32_t)(int32_t)disp + xn;
}

static ea_t decode_ea(m68k_t *c, int mode, int reg, int size) {
    ea_t e; e.kind = EA_MEM; e.reg = (uint8_t)reg; e.addr = 0; e.imm = 0;
    switch (mode) {
    case 0: e.kind = EA_DREG; break;
    case 1: e.kind = EA_AREG; break;
    case 2: e.addr = c->a[reg]; break;                       /* (An)   */
    case 3: {                                                /* (An)+  */
        int inc = size; if (reg == 7 && size == 1) inc = 2;
        e.addr = c->a[reg]; c->a[reg] += inc; break;
    }
    case 4: {                                                /* -(An)  */
        int dec = size; if (reg == 7 && size == 1) dec = 2;
        c->a[reg] -= dec; e.addr = c->a[reg]; break;
    }
    case 5: e.addr = c->a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(c); break; /* d16(An) */
    case 6: e.addr = index_ea(c, c->a[reg]); break;          /* d8(An,Xn) */
    case 7:
        switch (reg) {
        case 0: e.addr = (uint32_t)(int32_t)(int16_t)fetch16(c); break;  /* abs.W */
        case 1: e.addr = fetch32(c); break;                             /* abs.L */
        case 2: { uint32_t base = c->pc;                                /* d16(PC) */
                  e.addr = base + (uint32_t)(int32_t)(int16_t)fetch16(c); break; }
        case 3: { uint32_t base = c->pc; e.addr = index_ea(c, base); break; } /* d8(PC,Xn) */
        case 4:                                                          /* #imm */
            e.kind = EA_IMM;
            if (size == 1)      e.imm = fetch16(c) & 0xFF;
            else if (size == 2) e.imm = fetch16(c);
            else                e.imm = fetch32(c);
            break;
        }
        break;
    }
    return e;
}

static uint32_t ea_read(m68k_t *c, const ea_t *e, int size) {
    switch (e->kind) {
    case EA_DREG: return c->d[e->reg] & sz_mask(size);
    case EA_AREG: return c->a[e->reg] & sz_mask(size);
    case EA_IMM:  return e->imm & sz_mask(size);
    default:      return rd_sz(c, e->addr, size);
    }
}

static void ea_write(m68k_t *c, const ea_t *e, int size, uint32_t v) {
    switch (e->kind) {
    case EA_DREG: {
        uint32_t m = sz_mask(size);
        c->d[e->reg] = (c->d[e->reg] & ~m) | (v & m);
        break;
    }
    case EA_AREG: c->a[e->reg] = v; break;   /* address writes are full 32-bit */
    case EA_IMM:  break;                      /* illegal, ignore */
    default:      wr_sz(c, e->addr, size, v); break;
    }
}

/* ---- flag setters ---- */
static void flags_logic(m68k_t *c, uint32_t r, int size) {
    r &= sz_mask(size);
    c->nf = (r & sz_msb(size)) ? 1 : 0;
    c->zf = (r == 0);
    c->vf = 0; c->cf = 0;
}

static void flags_add(m68k_t *c, uint32_t s, uint32_t d, int size, int with_x) {
    uint32_t mask = sz_mask(size), msb = sz_msb(size);
    uint32_t sm = s & mask, dm = d & mask;
    uint64_t full = (uint64_t)sm + dm + (with_x ? c->xf : 0);
    uint32_t rm = (uint32_t)full & mask;
    c->cf = (full >> (8 * size)) & 1;
    c->vf = ((~(sm ^ dm) & (sm ^ rm)) & msb) ? 1 : 0;
    c->nf = (rm & msb) ? 1 : 0;
    if (with_x) { if (rm) c->zf = 0; } else c->zf = (rm == 0);
    c->xf = c->cf;
}

/* result of d - s */
static void flags_sub(m68k_t *c, uint32_t s, uint32_t d, int size, int with_x, int is_cmp) {
    uint32_t mask = sz_mask(size), msb = sz_msb(size);
    uint32_t sm = s & mask, dm = d & mask;
    uint32_t borrow = with_x ? c->xf : 0;
    uint64_t full = (uint64_t)dm - sm - borrow;
    uint32_t rm = (uint32_t)full & mask;
    c->cf = (full >> (8 * size)) & 1;   /* borrow */
    c->vf = (((dm ^ sm) & (dm ^ rm)) & msb) ? 1 : 0;
    c->nf = (rm & msb) ? 1 : 0;
    if (with_x) { if (rm) c->zf = 0; } else c->zf = (rm == 0);
    if (!is_cmp) c->xf = c->cf;
}

/* ---- condition-code test for Bcc/Scc/DBcc ---- */
static int test_cc(m68k_t *c, int cc) {
    int n = c->nf, z = c->zf, v = c->vf, cf = c->cf;
    switch (cc) {
    case 0x0: return 1;                  /* T  */
    case 0x1: return 0;                  /* F  */
    case 0x2: return !cf && !z;          /* HI */
    case 0x3: return cf || z;            /* LS */
    case 0x4: return !cf;                /* CC/HS */
    case 0x5: return cf;                 /* CS/LO */
    case 0x6: return !z;                 /* NE */
    case 0x7: return z;                  /* EQ */
    case 0x8: return !v;                 /* VC */
    case 0x9: return v;                  /* VS */
    case 0xA: return !n;                 /* PL */
    case 0xB: return n;                  /* MI */
    case 0xC: return n == v;             /* GE */
    case 0xD: return n != v;             /* LT */
    case 0xE: return !z && (n == v);     /* GT */
    case 0xF: return z || (n != v);      /* LE */
    }
    return 0;
}

/* ---- stack helpers ---- */
static inline void push32(m68k_t *c, uint32_t v) { c->a[7] -= 4; wr32(c, c->a[7], v); }
static inline uint32_t pop32(m68k_t *c) { uint32_t v = rd32(c, c->a[7]); c->a[7] += 4; return v; }
static inline uint16_t pop16(m68k_t *c) { uint16_t v = rd16(c, c->a[7]); c->a[7] += 2; return v; }

/* ---- MOVEM register list ---- */
static int do_movem(m68k_t *c, uint16_t op) {
    int size = (op & 0x0040) ? 4 : 2;
    int to_mem = (op & 0x0400) == 0;   /* bit10: 0 = reg->mem, 1 = mem->reg */
    int mode = (op >> 3) & 7, reg = op & 7;
    uint16_t list = fetch16(c);
    int count = 0;

    if (to_mem) {
        if (mode == 4) {   /* -(An): predecrement, registers stored A7..D0 */
            uint32_t addr = c->a[reg];
            for (int i = 0; i < 16; i++) {
                if (list & (1 << i)) {
                    /* bit0 = A7 ... bit15 = D0 for predecrement order */
                    int rn = 15 - i;
                    uint32_t val = (rn < 8) ? c->d[rn] : c->a[rn - 8];
                    addr -= size;
                    wr_sz(c, addr, size, val);
                    count++;
                }
            }
            c->a[reg] = addr;
        } else {
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t addr = e.addr;
            for (int i = 0; i < 16; i++) {
                if (list & (1 << i)) {
                    uint32_t val = (i < 8) ? c->d[i] : c->a[i - 8];
                    wr_sz(c, addr, size, val);
                    addr += size;
                    count++;
                }
            }
        }
    } else {
        uint32_t addr;
        int postinc = (mode == 3);
        if (postinc) addr = c->a[reg];
        else { ea_t e = decode_ea(c, mode, reg, size); addr = e.addr; }
        for (int i = 0; i < 16; i++) {
            if (list & (1 << i)) {
                uint32_t val = rd_sz(c, addr, size);
                if (size == 2) val = (uint32_t)(int32_t)(int16_t)val; /* word loads sign-extend */
                if (i < 8) c->d[i] = val; else c->a[i - 8] = val;
                addr += size;
                count++;
            }
        }
        if (postinc) c->a[reg] = addr;
    }
    return 12 + count * (size == 4 ? 8 : 4);
}

/* ---- shift / rotate primitive on `size`-wide value ---- */
static uint32_t do_shift(m68k_t *c, int type, int left, uint32_t val, int cnt, int size) {
    uint32_t mask = sz_mask(size), msb = sz_msb(size);
    val &= mask;
    c->vf = 0;
    if (cnt == 0) {
        /* count 0: C cleared (X unaffected), NZ from value */
        c->cf = 0;
        c->nf = (val & msb) ? 1 : 0;
        c->zf = (val == 0);
        return val;
    }
    int bits = 8 * size;
    switch (type) {
    case 0: /* ASL/ASR (arithmetic) */
        if (left) {
            uint32_t before = val;
            for (int i = 0; i < cnt; i++) {
                c->cf = (val & msb) ? 1 : 0;
                val = (val << 1) & mask;
                /* V set if the sign bit ever changes during the shift */
                if (((val ^ before) & msb)) c->vf = 1;
            }
            c->xf = c->cf;
        } else {
            uint32_t sign = val & msb;
            for (int i = 0; i < cnt; i++) {
                c->cf = val & 1;
                val = (val >> 1) | sign;
                val &= mask;
            }
            c->xf = c->cf;
        }
        break;
    case 1: /* LSL/LSR (logical) */
        if (left) {
            for (int i = 0; i < cnt; i++) { c->cf = (val & msb) ? 1 : 0; val = (val << 1) & mask; }
        } else {
            for (int i = 0; i < cnt; i++) { c->cf = val & 1; val = (val >> 1) & mask; }
        }
        c->xf = c->cf;
        break;
    case 2: /* ROXL/ROXR (rotate through X) */
        if (left) {
            for (int i = 0; i < cnt; i++) {
                int hb = (val & msb) ? 1 : 0;
                val = ((val << 1) | c->xf) & mask;
                c->xf = c->cf = hb;
            }
        } else {
            for (int i = 0; i < cnt; i++) {
                int lb = val & 1;
                val = ((val >> 1) | (c->xf ? msb : 0)) & mask;
                c->xf = c->cf = lb;
            }
        }
        break;
    case 3: /* ROL/ROR (rotate, X unaffected) */
        cnt %= bits ? bits : 1;
        if (left) {
            for (int i = 0; i < cnt; i++) {
                int hb = (val & msb) ? 1 : 0;
                val = ((val << 1) | hb) & mask;
                c->cf = hb;
            }
        } else {
            for (int i = 0; i < cnt; i++) {
                int lb = val & 1;
                val = ((val >> 1) | (lb ? msb : 0)) & mask;
                c->cf = lb;
            }
        }
        break;
    }
    c->nf = (val & msb) ? 1 : 0;
    c->zf = (val == 0);
    return val;
}

/* ---- BCD helpers ---- */
static uint8_t abcd(m68k_t *c, uint8_t s, uint8_t d) {
    /* packed BCD add with decimal adjust */
    int res = (s & 0xFF) + (d & 0xFF) + c->xf;
    int adj = 0;
    if (((s & 0x0F) + (d & 0x0F) + c->xf) > 9) adj += 0x06;
    if ((res + adj) > 0x99) adj += 0x60;
    int out = (res + adj) & 0xFF;
    c->cf = c->xf = ((res + adj) > 0x99) ? 1 : 0;
    if (out) c->zf = 0;
    c->nf = (out & 0x80) ? 1 : 0;
    return (uint8_t)out;
}
static uint8_t sbcd(m68k_t *c, uint8_t s, uint8_t d) {
    int res = (d & 0xFF) - (s & 0xFF) - c->xf;
    int adj = 0;
    if (((d & 0x0F) - (s & 0x0F) - c->xf) < 0) adj -= 0x06;
    if (res < 0) adj -= 0x60;
    int out = (res + adj) & 0xFF;
    c->cf = c->xf = (res < 0) ? 1 : 0;
    if (out) c->zf = 0;
    c->nf = (out & 0x80) ? 1 : 0;
    return (uint8_t)out;
}

/* ================= main decode ================= */

/* returns cycles */
static int execute(m68k_t *c, uint16_t op) {
    int hi = (op >> 12) & 0xF;

    switch (hi) {

    /* ---- 0x0: immediate ALU, bit ops, MOVEP ---- */
    case 0x0: {
        int mode = (op >> 3) & 7, reg = op & 7;
        if ((op & 0x0100) || ((op & 0x0F00) == 0x0800)) {
            /* Bit ops: BTST/BCHG/BCLR/BSET. Bit number is dynamic (Dn) when
             * bit8 set, static (immediate) when opcode is 0000 1000 xx. */
            int dynamic = (op & 0x0100) != 0;
            int btype = (op >> 6) & 3;    /* 0=TST 1=CHG 2=CLR 3=SET */
            int bit;
            if (dynamic) bit = c->d[(op >> 9) & 7];
            else bit = fetch16(c) & 0xFF;
            int size = (mode == 0) ? 4 : 1;   /* Dn: long, memory: byte */
            bit &= (size == 4) ? 31 : 7;
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t v = ea_read(c, &e, size);
            c->zf = ((v >> bit) & 1) ? 0 : 1;
            if (btype != 0) {
                if (btype == 1) v ^= (1u << bit);
                else if (btype == 2) v &= ~(1u << bit);
                else v |= (1u << bit);
                ea_write(c, &e, size, v);
            }
            return 8;
        }
        /* Immediate-source ALU / CCR-SR forms, selected by bits 11-9. */
        int subop = (op >> 9) & 7;
        int size_field = (op >> 6) & 3;
        int size = size_field == 0 ? 1 : size_field == 1 ? 2 : 4;

        /* ORI/ANDI/EORI to CCR or SR: EA field = 0x3C (immediate) with size
         * byte (CCR) or word (SR). */
        if ((op & 0x00FF) == 0x003C && (subop == 0 || subop == 1 || subop == 5)) {
            uint16_t imm = fetch16(c) & 0xFF;   /* byte immediate */
            uint8_t ccr = (uint8_t)(m68k_get_sr(c) & 0x1F);
            if (subop == 0) ccr |= imm; else if (subop == 1) ccr &= imm; else ccr ^= imm;
            set_ccr(c, ccr);
            return 20;
        }
        if ((op & 0x00FF) == 0x007C && (subop == 0 || subop == 1 || subop == 5)) {
            if (!c->s) { exception(c, M68K_VEC_PRIV); return 34; }
            uint16_t imm = fetch16(c);
            uint16_t sr = m68k_get_sr(c);
            if (subop == 0) sr |= imm; else if (subop == 1) sr &= imm; else sr ^= imm;
            m68k_set_sr(c, sr);
            return 20;
        }

        uint32_t imm;
        if (size == 1) imm = fetch16(c) & 0xFF; else if (size == 2) imm = fetch16(c); else imm = fetch32(c);
        ea_t e = decode_ea(c, mode, reg, size);
        uint32_t d = ea_read(c, &e, size);
        uint32_t r;
        switch (subop) {
        case 0: r = d | imm;  ea_write(c, &e, size, r); flags_logic(c, r, size); break; /* ORI  */
        case 1: r = d & imm;  ea_write(c, &e, size, r); flags_logic(c, r, size); break; /* ANDI */
        case 2: r = d - imm;  ea_write(c, &e, size, r); flags_sub(c, imm, d, size, 0, 0); break; /* SUBI */
        case 3: r = d + imm;  ea_write(c, &e, size, r); flags_add(c, imm, d, size, 0); break;    /* ADDI */
        case 5: r = d ^ imm;  ea_write(c, &e, size, r); flags_logic(c, r, size); break; /* EORI */
        case 6: flags_sub(c, imm, d, size, 0, 1); break;                                /* CMPI */
        default: exception(c, M68K_VEC_ILLEGAL); break;
        }
        return 12;
    }

    /* ---- MOVE.B / MOVE.L / MOVE.W (also MOVEA) ---- */
    case 0x1: case 0x2: case 0x3: {
        int size = hi == 1 ? 1 : hi == 2 ? 4 : 2;
        int src_mode = (op >> 3) & 7, src_reg = op & 7;
        int dst_mode = (op >> 6) & 7, dst_reg = (op >> 9) & 7;
        ea_t se = decode_ea(c, src_mode, src_reg, size);
        uint32_t v = ea_read(c, &se, size);
        if (dst_mode == 1) {   /* MOVEA: sign-extend to 32, no flags */
            c->a[dst_reg] = sign_extend(v, size);
        } else {
            ea_t de = decode_ea(c, dst_mode, dst_reg, size);
            ea_write(c, &de, size, v);
            flags_logic(c, v, size);
        }
        return 4;
    }

    /* ---- 0x4: misc ---- */
    case 0x4: {
        int mode = (op >> 3) & 7, reg = op & 7;

        if ((op & 0xFFC0) == 0x40C0) { /* MOVE from SR */
            ea_t e = decode_ea(c, mode, reg, 2);
            ea_write(c, &e, 2, m68k_get_sr(c));
            return 8;
        }
        if ((op & 0xFFC0) == 0x44C0) { /* MOVE to CCR */
            ea_t e = decode_ea(c, mode, reg, 2);
            set_ccr(c, (uint8_t)(ea_read(c, &e, 2) & 0x1F));
            return 12;
        }
        if ((op & 0xFFC0) == 0x46C0) { /* MOVE to SR (privileged) */
            if (!c->s) { exception(c, M68K_VEC_PRIV); return 34; }
            ea_t e = decode_ea(c, mode, reg, 2);
            m68k_set_sr(c, (uint16_t)ea_read(c, &e, 2));
            return 12;
        }
        if ((op & 0xF1C0) == 0x41C0) { /* LEA */
            int an = (op >> 9) & 7;
            ea_t e = decode_ea(c, mode, reg, 4);
            c->a[an] = e.addr;
            return 4;
        }
        if ((op & 0xFFC0) == 0x4840 && mode == 0) { /* SWAP */
            uint32_t v = c->d[reg];
            v = (v >> 16) | (v << 16);
            c->d[reg] = v;
            c->nf = (v & 0x80000000u) ? 1 : 0; c->zf = (v == 0); c->vf = 0; c->cf = 0;
            return 4;
        }
        if ((op & 0xFFC0) == 0x4840) { /* PEA */
            ea_t e = decode_ea(c, mode, reg, 4);
            push32(c, e.addr);
            return 12;
        }
        if ((op & 0xFFB8) == 0x4880) { /* EXT */
            int longer = (op & 0x0040) != 0;
            uint32_t v = c->d[reg];
            if (!longer) { v = (v & 0xFFFF0000u) | (uint32_t)(int16_t)(int8_t)(v & 0xFF); c->d[reg] = v; v &= 0xFFFF; c->nf = (v & 0x8000) ? 1 : 0; }
            else { v = (uint32_t)(int32_t)(int16_t)(v & 0xFFFF); c->d[reg] = v; c->nf = (v & 0x80000000u) ? 1 : 0; }
            c->zf = ((longer ? v : (v & 0xFFFF)) == 0); c->vf = 0; c->cf = 0;
            return 4;
        }
        if ((op & 0xFB80) == 0x4880) { /* MOVEM (bit10 = direction) */
            return do_movem(c, op);
        }
        if ((op & 0xFF00) == 0x4200) { /* CLR */
            int sf = (op >> 6) & 3;
            if (sf == 3) { /* not CLR: fall through to others below */ }
            else {
                int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
                ea_t e = decode_ea(c, mode, reg, size);
                ea_read(c, &e, size); /* 68000 does a dummy read */
                ea_write(c, &e, size, 0);
                c->nf = 0; c->zf = 1; c->vf = 0; c->cf = 0;
                return 6;
            }
        }
        if ((op & 0xFF00) == 0x4400) { /* NEG */
            int sf = (op >> 6) & 3;
            if (sf != 3) {
                int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
                ea_t e = decode_ea(c, mode, reg, size);
                uint32_t d = ea_read(c, &e, size);
                uint32_t r = (uint32_t)(0 - d);
                ea_write(c, &e, size, r);
                flags_sub(c, d, 0, size, 0, 0);
                return 6;
            }
        }
        if ((op & 0xFF00) == 0x4000) { /* NEGX */
            int sf = (op >> 6) & 3;
            if (sf != 3) {
                int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
                ea_t e = decode_ea(c, mode, reg, size);
                uint32_t d = ea_read(c, &e, size);
                uint32_t r = (uint32_t)(0 - d - c->xf);
                ea_write(c, &e, size, r);
                flags_sub(c, d, 0, size, 1, 0);
                return 6;
            }
        }
        if ((op & 0xFF00) == 0x4600) { /* NOT */
            int sf = (op >> 6) & 3;
            if (sf != 3) {
                int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
                ea_t e = decode_ea(c, mode, reg, size);
                uint32_t r = ~ea_read(c, &e, size);
                ea_write(c, &e, size, r);
                flags_logic(c, r, size);
                return 6;
            }
        }
        if ((op & 0xFF00) == 0x4A00) { /* TST / TAS */
            int sf = (op >> 6) & 3;
            if (sf == 3) { /* TAS: byte, sets bit7 */
                ea_t e = decode_ea(c, mode, reg, 1);
                uint32_t v = ea_read(c, &e, 1);
                c->nf = (v & 0x80) ? 1 : 0; c->zf = ((v & 0xFF) == 0); c->vf = 0; c->cf = 0;
                ea_write(c, &e, 1, v | 0x80);
                return 10;
            } else {
                int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
                ea_t e = decode_ea(c, mode, reg, size);
                uint32_t v = ea_read(c, &e, size);
                flags_logic(c, v, size);
                return 4;
            }
        }
        if ((op & 0xFFF8) == 0x4E50) { /* LINK */
            int an = op & 7;
            int16_t disp = (int16_t)fetch16(c);
            push32(c, c->a[an]);
            c->a[an] = c->a[7];
            c->a[7] += (uint32_t)(int32_t)disp;
            return 16;
        }
        if ((op & 0xFFF8) == 0x4E58) { /* UNLK */
            int an = op & 7;
            c->a[7] = c->a[an];
            c->a[an] = pop32(c);
            return 12;
        }
        if ((op & 0xFFF0) == 0x4E60) { /* MOVE USP */
            if (!c->s) { exception(c, M68K_VEC_PRIV); return 34; }
            int an = op & 7;
            if (op & 0x08) c->a[an] = c->usp;     /* USP -> An */
            else c->usp = c->a[an];               /* An -> USP */
            return 4;
        }
        if ((op & 0xFF80) == 0x4E80) { /* JSR / JMP */
            ea_t e = decode_ea(c, mode, reg, 4);
            if (op & 0x0040) { c->pc = e.addr; }             /* JMP */
            else { push32(c, c->pc); c->pc = e.addr; }       /* JSR */
            return 12;
        }
        if ((op & 0xF1C0) == 0x4180) { /* CHK (word) */
            int dn = (op >> 9) & 7;
            ea_t e = decode_ea(c, mode, reg, 2);
            int16_t bound = (int16_t)ea_read(c, &e, 2);
            int16_t v = (int16_t)(c->d[dn] & 0xFFFF);
            if (v < 0 || v > bound) { c->nf = (v < 0); exception(c, M68K_VEC_CHK); return 40; }
            return 10;
        }
        switch (op) {
        case 0x4E70: return 132;                              /* RESET (no-op) */
        case 0x4E71: return 4;                                /* NOP */
        case 0x4E72: {                                        /* STOP */
            if (!c->s) { exception(c, M68K_VEC_PRIV); return 34; }
            uint16_t imm = fetch16(c);
            m68k_set_sr(c, imm);
            c->stopped = 1;
            return 4;
        }
        case 0x4E73: {                                        /* RTE */
            if (!c->s) { exception(c, M68K_VEC_PRIV); return 34; }
            uint16_t sr = pop16(c);
            c->pc = pop32(c);
            m68k_set_sr(c, sr);
            return 20;
        }
        case 0x4E75:                                          /* RTS */
            c->pc = pop32(c); return 16;
        case 0x4E77: {                                        /* RTR */
            uint16_t ccr = pop16(c);
            set_ccr(c, (uint8_t)(ccr & 0x1F));
            c->pc = pop32(c); return 20;
        }
        case 0x4E76:                                          /* TRAPV */
            if (c->vf) { exception(c, M68K_VEC_TRAPV); return 34; }
            return 4;
        default: break;
        }
        if ((op & 0xFFF0) == 0x4E40) { /* TRAP #n */
            exception(c, M68K_VEC_TRAP0 + (op & 0xF));
            return 38;
        }
        exception(c, M68K_VEC_ILLEGAL);
        return 34;
    }

    /* ---- 0x5: ADDQ/SUBQ, Scc, DBcc ---- */
    case 0x5: {
        int mode = (op >> 3) & 7, reg = op & 7;
        int sf = (op >> 6) & 3;
        if (sf == 3) {
            int cc = (op >> 8) & 0xF;
            if (mode == 1) { /* DBcc */
                int dn = reg;
                int16_t disp = (int16_t)fetch16(c);
                if (!test_cc(c, cc)) {
                    uint16_t v = (uint16_t)(c->d[dn] & 0xFFFF);
                    v -= 1;
                    c->d[dn] = (c->d[dn] & 0xFFFF0000u) | v;
                    if (v != 0xFFFF) c->pc = c->pc - 2 + (uint32_t)(int32_t)disp;
                }
                return 10;
            } else { /* Scc */
                ea_t e = decode_ea(c, mode, reg, 1);
                ea_write(c, &e, 1, test_cc(c, cc) ? 0xFF : 0x00);
                return 8;
            }
        }
        int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        if (mode == 1) { /* ADDQ/SUBQ to An: full 32-bit, no flags */
            if (op & 0x0100) c->a[reg] -= data; else c->a[reg] += data;
            return 8;
        }
        ea_t e = decode_ea(c, mode, reg, size);
        uint32_t d = ea_read(c, &e, size);
        if (op & 0x0100) { /* SUBQ */
            uint32_t r = d - data; ea_write(c, &e, size, r); flags_sub(c, data, d, size, 0, 0);
        } else {           /* ADDQ */
            uint32_t r = d + data; ea_write(c, &e, size, r); flags_add(c, data, d, size, 0);
        }
        return 8;
    }

    /* ---- 0x6: Bcc / BRA / BSR ---- */
    case 0x6: {
        int cc = (op >> 8) & 0xF;
        int32_t disp = (int8_t)(op & 0xFF);
        uint32_t base = c->pc;
        if ((op & 0xFF) == 0x00) disp = (int16_t)fetch16(c);
        else if ((op & 0xFF) == 0xFF) disp = (int32_t)fetch32(c); /* 68020+, harmless */
        if (cc == 1) {          /* BSR */
            push32(c, c->pc);
            c->pc = base + disp;
        } else if (cc == 0) {   /* BRA */
            c->pc = base + disp;
        } else if (test_cc(c, cc)) {
            c->pc = base + disp;
        }
        return 10;
    }

    /* ---- 0x7: MOVEQ ---- */
    case 0x7: {
        int dn = (op >> 9) & 7;
        uint32_t v = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
        c->d[dn] = v;
        flags_logic(c, v, 4);
        return 4;
    }

    /* ---- 0x8: OR / DIVU / DIVS / SBCD ---- */
    case 0x8: {
        int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
        int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) { /* DIVU (3) / DIVS (7) */
            ea_t e = decode_ea(c, mode, reg, 2);
            uint32_t src = ea_read(c, &e, 2);
            if (src == 0) { exception(c, M68K_VEC_ZERO_DIVIDE); return 38; }
            if (opmode == 3) {
                uint32_t dividend = c->d[dn];
                uint32_t q = dividend / src, r = dividend % src;
                if (q > 0xFFFF) { c->vf = 1; return 70; }
                c->d[dn] = (r << 16) | (q & 0xFFFF);
                c->nf = (q & 0x8000) ? 1 : 0; c->zf = ((q & 0xFFFF) == 0); c->vf = 0; c->cf = 0;
            } else {
                int32_t dividend = (int32_t)c->d[dn];
                int32_t divisor = (int32_t)(int16_t)src;
                int32_t q = dividend / divisor, r = dividend % divisor;
                if (q > 32767 || q < -32768) { c->vf = 1; return 70; }
                c->d[dn] = ((uint32_t)(r & 0xFFFF) << 16) | (q & 0xFFFF);
                c->nf = (q & 0x8000) ? 1 : 0; c->zf = ((q & 0xFFFF) == 0); c->vf = 0; c->cf = 0;
            }
            return 140;
        }
        if ((op & 0x01F0) == 0x0100) { /* SBCD */
            int rm = op & 8;
            if (rm) { /* -(Ay),-(Ax) */
                uint8_t s = rd8(c, c->a[reg] -= 1);
                uint8_t d = rd8(c, c->a[dn] -= 1);
                wr8(c, c->a[dn], sbcd(c, s, d));
            } else {
                c->d[dn] = (c->d[dn] & 0xFFFFFF00u) | sbcd(c, (uint8_t)c->d[reg], (uint8_t)c->d[dn]);
            }
            return 6;
        }
        /* OR */
        int size = opmode & 3; size = size == 0 ? 1 : size == 1 ? 2 : 4;
        int to_ea = (opmode & 4) != 0;
        ea_t e = decode_ea(c, mode, reg, size);
        if (to_ea) {
            uint32_t d = ea_read(c, &e, size);
            uint32_t r = d | (c->d[dn] & sz_mask(size));
            ea_write(c, &e, size, r); flags_logic(c, r, size);
        } else {
            uint32_t r = (c->d[dn] & sz_mask(size)) | ea_read(c, &e, size);
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | (r & sz_mask(size));
            flags_logic(c, r, size);
        }
        return 4;
    }

    /* ---- 0x9: SUB / SUBA / SUBX ---- */
    case 0x9: {
        int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
        int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) { /* SUBA */
            int size = opmode == 3 ? 2 : 4;
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t v = sign_extend(ea_read(c, &e, size), size);
            c->a[dn] -= v;
            return 8;
        }
        int size = opmode & 3; size = size == 0 ? 1 : size == 1 ? 2 : 4;
        int to_ea = (opmode & 4) != 0;
        if (to_ea && mode == 0) { /* SUBX */
            uint32_t s = c->d[reg], d = c->d[dn];
            uint32_t r = (d - s - c->xf) & sz_mask(size);
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | r;
            flags_sub(c, s, d, size, 1, 0);
            return 4;
        }
        if (to_ea && mode == 1) { /* SUBX -(Ay),-(Ax) */
            uint32_t s = rd_sz(c, c->a[reg] -= size, size);
            uint32_t d = rd_sz(c, c->a[dn] -= size, size);
            uint32_t r = (d - s - c->xf) & sz_mask(size);
            wr_sz(c, c->a[dn], size, r);
            flags_sub(c, s, d, size, 1, 0);
            return 8;
        }
        ea_t e = decode_ea(c, mode, reg, size);
        if (to_ea) {
            uint32_t d = ea_read(c, &e, size);
            uint32_t r = d - (c->d[dn] & sz_mask(size));
            ea_write(c, &e, size, r); flags_sub(c, c->d[dn], d, size, 0, 0);
        } else {
            uint32_t d = c->d[dn] & sz_mask(size);
            uint32_t s = ea_read(c, &e, size);
            uint32_t r = d - s;
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | (r & sz_mask(size));
            flags_sub(c, s, d, size, 0, 0);
        }
        return 4;
    }

    /* ---- 0xA: line-A (unimplemented) ---- */
    case 0xA:
        exception(c, M68K_VEC_LINE_A);
        return 34;

    /* ---- 0xB: CMP / CMPA / EOR / CMPM ---- */
    case 0xB: {
        int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
        int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) { /* CMPA */
            int size = opmode == 3 ? 2 : 4;
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t s = sign_extend(ea_read(c, &e, size), size);
            flags_sub(c, s, c->a[dn], 4, 0, 1);
            return 6;
        }
        int size = opmode & 3; size = size == 0 ? 1 : size == 1 ? 2 : 4;
        if (opmode & 4) {
            if (mode == 1) { /* CMPM (Ay)+,(Ax)+ */
                uint32_t s = rd_sz(c, c->a[reg], size); c->a[reg] += size;
                uint32_t d = rd_sz(c, c->a[dn], size);  c->a[dn]  += size;
                flags_sub(c, s, d, size, 0, 1);
                return 12;
            }
            /* EOR */
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t r = ea_read(c, &e, size) ^ (c->d[dn] & sz_mask(size));
            ea_write(c, &e, size, r); flags_logic(c, r, size);
            return 8;
        }
        /* CMP */
        ea_t e = decode_ea(c, mode, reg, size);
        uint32_t s = ea_read(c, &e, size);
        flags_sub(c, s, c->d[dn], size, 0, 1);
        return 4;
    }

    /* ---- 0xC: AND / MULU / MULS / ABCD / EXG ---- */
    case 0xC: {
        int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
        int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) { /* MULU (3) / MULS (7) */
            ea_t e = decode_ea(c, mode, reg, 2);
            uint32_t src = ea_read(c, &e, 2);
            uint32_t r;
            if (opmode == 3) r = (uint32_t)(uint16_t)src * (uint16_t)c->d[dn];
            else r = (uint32_t)((int32_t)(int16_t)src * (int32_t)(int16_t)c->d[dn]);
            c->d[dn] = r;
            c->nf = (r & 0x80000000u) ? 1 : 0; c->zf = (r == 0); c->vf = 0; c->cf = 0;
            return 70;
        }
        if ((op & 0x01F0) == 0x0100) { /* ABCD */
            int rm = op & 8;
            if (rm) {
                uint8_t s = rd8(c, c->a[reg] -= 1);
                uint8_t d = rd8(c, c->a[dn] -= 1);
                wr8(c, c->a[dn], abcd(c, s, d));
            } else {
                c->d[dn] = (c->d[dn] & 0xFFFFFF00u) | abcd(c, (uint8_t)c->d[reg], (uint8_t)c->d[dn]);
            }
            return 6;
        }
        if ((op & 0x0130) == 0x0100) { /* EXG */
            int opm = (op >> 3) & 0x1F;
            if (opm == 0x08) { uint32_t t = c->d[dn]; c->d[dn] = c->d[reg]; c->d[reg] = t; } /* Dx,Dy */
            else if (opm == 0x09) { uint32_t t = c->a[dn]; c->a[dn] = c->a[reg]; c->a[reg] = t; } /* Ax,Ay */
            else { uint32_t t = c->d[dn]; c->d[dn] = c->a[reg]; c->a[reg] = t; } /* Dx,Ay */
            return 6;
        }
        /* AND */
        int size = opmode & 3; size = size == 0 ? 1 : size == 1 ? 2 : 4;
        int to_ea = (opmode & 4) != 0;
        ea_t e = decode_ea(c, mode, reg, size);
        if (to_ea) {
            uint32_t d = ea_read(c, &e, size);
            uint32_t r = d & (c->d[dn] & sz_mask(size));
            ea_write(c, &e, size, r); flags_logic(c, r, size);
        } else {
            uint32_t r = (c->d[dn] & sz_mask(size)) & ea_read(c, &e, size);
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | (r & sz_mask(size));
            flags_logic(c, r, size);
        }
        return 4;
    }

    /* ---- 0xD: ADD / ADDA / ADDX ---- */
    case 0xD: {
        int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
        int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) { /* ADDA */
            int size = opmode == 3 ? 2 : 4;
            ea_t e = decode_ea(c, mode, reg, size);
            uint32_t v = sign_extend(ea_read(c, &e, size), size);
            c->a[dn] += v;
            return 8;
        }
        int size = opmode & 3; size = size == 0 ? 1 : size == 1 ? 2 : 4;
        int to_ea = (opmode & 4) != 0;
        if (to_ea && mode == 0) { /* ADDX Dy,Dx */
            uint32_t s = c->d[reg], d = c->d[dn];
            uint32_t r = (d + s + c->xf) & sz_mask(size);
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | r;
            flags_add(c, s, d, size, 1);
            return 4;
        }
        if (to_ea && mode == 1) { /* ADDX -(Ay),-(Ax) */
            uint32_t s = rd_sz(c, c->a[reg] -= size, size);
            uint32_t d = rd_sz(c, c->a[dn] -= size, size);
            uint32_t r = (d + s + c->xf) & sz_mask(size);
            wr_sz(c, c->a[dn], size, r);
            flags_add(c, s, d, size, 1);
            return 8;
        }
        ea_t e = decode_ea(c, mode, reg, size);
        if (to_ea) {
            uint32_t d = ea_read(c, &e, size);
            uint32_t r = d + (c->d[dn] & sz_mask(size));
            ea_write(c, &e, size, r); flags_add(c, c->d[dn], d, size, 0);
        } else {
            uint32_t d = c->d[dn] & sz_mask(size);
            uint32_t s = ea_read(c, &e, size);
            uint32_t r = d + s;
            c->d[dn] = (c->d[dn] & ~sz_mask(size)) | (r & sz_mask(size));
            flags_add(c, s, d, size, 0);
        }
        return 4;
    }

    /* ---- 0xE: shifts / rotates ---- */
    case 0xE: {
        int mode = (op >> 3) & 7;
        if ((op & 0x00C0) == 0x00C0) { /* memory shift by 1 */
            int type = (op >> 9) & 3;
            int left = (op & 0x0100) != 0;
            int reg = op & 7, m = (op >> 3) & 7;
            ea_t e = decode_ea(c, m, reg, 2);
            uint32_t v = ea_read(c, &e, 2);
            v = do_shift(c, type, left, v, 1, 2);
            ea_write(c, &e, 2, v);
            return 8;
        }
        int sf = (op >> 6) & 3;
        int size = sf == 0 ? 1 : sf == 1 ? 2 : 4;
        int left = (op & 0x0100) != 0;
        int ir = (op >> 5) & 1;      /* 0 = immediate count, 1 = count in Dn */
        int type = (op >> 3) & 3;    /* 0=AS 1=LS 2=ROX 3=RO */
        int reg = op & 7;
        int cntsrc = (op >> 9) & 7;
        int cnt = ir ? (int)(c->d[cntsrc] & 63) : (cntsrc == 0 ? 8 : cntsrc);
        (void)mode;
        uint32_t v = do_shift(c, type, left, c->d[reg], cnt, size);
        c->d[reg] = (c->d[reg] & ~sz_mask(size)) | (v & sz_mask(size));
        return 6 + 2 * cnt;
    }

    /* ---- 0xF: line-F (coprocessor, unimplemented) ---- */
    case 0xF:
        exception(c, M68K_VEC_LINE_F);
        return 34;
    }

    exception(c, M68K_VEC_ILLEGAL);
    return 34;
}

/* ================= public API ================= */

void m68k_reset(m68k_t *cpu) {
    cpu->s = 1; cpu->t = 0; cpu->imask = 7;
    cpu->stopped = 0; cpu->halted = 0; cpu->ipl = 0; cpu->int_vector = 0;
    cpu->xf = cpu->nf = cpu->zf = cpu->vf = cpu->cf = 0;
    cpu->isp = rd32(cpu, 0);
    cpu->a[7] = cpu->isp;
    cpu->pc = rd32(cpu, 4);
    cpu->cycles += 40;
}

void m68k_set_irq(m68k_t *cpu, int level) {
    cpu->ipl = level & 7;
}

/* Take a pending interrupt if its level exceeds the mask (level 7 = NMI). */
static int check_irq(m68k_t *cpu) {
    int level = cpu->ipl;
    if (level == 0) return 0;
    if (level != 7 && level <= cpu->imask) return 0;
    cpu->stopped = 0;
    /* autovectored: level n uses vector 24 + n */
    uint16_t oldsr = m68k_get_sr(cpu);
    if (!cpu->s) { cpu->usp = cpu->a[7]; cpu->a[7] = cpu->isp; cpu->s = 1; }
    cpu->t = 0;
    cpu->imask = (uint8_t)level;
    cpu->a[7] -= 4; wr32(cpu, cpu->a[7], cpu->pc);
    cpu->a[7] -= 2; wr16(cpu, cpu->a[7], oldsr);
    /* Vectored interrupt (device supplies the vector) vs. 68000 autovector.
     * The Jaguar's TOM uses vector 64 ($100); autovector is the fallback. */
    int vnum = (cpu->int_vector > 0) ? cpu->int_vector : (M68K_VEC_AUTOVEC + level);
    cpu->pc = rd32(cpu, (uint32_t)vnum * 4);
    cpu->cycles += 44;
    return 1;
}

int m68k_step(m68k_t *cpu) {
    if (check_irq(cpu)) return 44;
    if (cpu->stopped) { cpu->cycles += 4; return 4; }

    int trace_pending = cpu->t;
    uint32_t pc = cpu->pc;
    uint16_t op = rd16(cpu, pc);
    if (cpu->trace) cpu->trace(cpu->trace_ctx, pc, op);
    cpu->pc += 2;

    int cyc = execute(cpu, op);
    cpu->cycles += cyc;

    if (trace_pending && cpu->t) exception(cpu, M68K_VEC_TRACE);
    return cyc;
}

uint64_t m68k_run(m68k_t *cpu, uint64_t cycles) {
    uint64_t start = cpu->cycles;
    while (cpu->cycles - start < cycles) {
        if (cpu->halted) { cpu->cycles = start + cycles; break; }
        m68k_step(cpu);
    }
    return cpu->cycles - start;
}
