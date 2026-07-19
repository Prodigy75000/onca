/*
 * test_m68k.c - standalone unit tests for the 68000 interpreter.
 *
 * Executes small hand-assembled 68000 programs against a flat 64 KiB
 * big-endian memory and checks register/flag/memory results. No Jaguar
 * hardware, no boot ROM needed.
 *
 * Build: gcc -std=c11 -Wall -O2 src/m68k.c tests/test_m68k.c -o test_m68k
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include <stdio.h>
#include <string.h>

static uint8_t g_mem[65536];

static uint8_t  m_r8 (void *c, uint32_t a) { (void)c; return g_mem[a & 0xFFFF]; }
static uint16_t m_r16(void *c, uint32_t a) { (void)c; a &= 0xFFFF; return (uint16_t)((g_mem[a] << 8) | g_mem[a + 1]); }
static uint32_t m_r32(void *c, uint32_t a) { (void)c; a &= 0xFFFF;
    return ((uint32_t)g_mem[a] << 24) | ((uint32_t)g_mem[a+1] << 16)
         | ((uint32_t)g_mem[a+2] << 8) | g_mem[a+3]; }
static void m_w8 (void *c, uint32_t a, uint8_t v)  { (void)c; g_mem[a & 0xFFFF] = v; }
static void m_w16(void *c, uint32_t a, uint16_t v) { (void)c; a &= 0xFFFF; g_mem[a] = (uint8_t)(v >> 8); g_mem[a+1] = (uint8_t)v; }
static void m_w32(void *c, uint32_t a, uint32_t v) { (void)c; a &= 0xFFFF;
    g_mem[a]=(uint8_t)(v>>24); g_mem[a+1]=(uint8_t)(v>>16); g_mem[a+2]=(uint8_t)(v>>8); g_mem[a+3]=(uint8_t)v; }

static m68k_t g_cpu;

static void bind(void) {
    g_cpu.bus.ctx = NULL;
    g_cpu.bus.read8 = m_r8; g_cpu.bus.read16 = m_r16; g_cpu.bus.read32 = m_r32;
    g_cpu.bus.write8 = m_w8; g_cpu.bus.write16 = m_w16; g_cpu.bus.write32 = m_w32;
}

static void load(uint32_t addr, const uint16_t *prog, int n) {
    for (int i = 0; i < n; i++) m_w16(NULL, addr + i * 2, prog[i]);
}

/* set up direct execution at addr (bypassing the reset vector fetch) */
static void setup(uint32_t pc) {
    memset(&g_cpu, 0, sizeof(g_cpu));
    bind();
    g_cpu.s = 1; g_cpu.imask = 7;
    g_cpu.a[7] = 0x8000;
    g_cpu.pc = pc;
}
static void run(int steps) { for (int i = 0; i < steps; i++) m68k_step(&g_cpu); }

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ----- tests ----- */

static void t_moveq_add(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p[] = { 0x7005, 0x7203, 0xD081 }; /* moveq#5,d0; moveq#3,d1; add.l d1,d0 */
    setup(0x1000); load(0x1000, p, 3); run(3);
    CHECK(g_cpu.d[0] == 8, "d0=%u", g_cpu.d[0]);
    CHECK(g_cpu.zf == 0 && g_cpu.nf == 0 && g_cpu.cf == 0 && g_cpu.vf == 0, "flags");
}

static void t_sub_borrow(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p[] = { 0x7005, 0x7203, 0x9280 }; /* moveq#5,d0; moveq#3,d1; sub.l d0,d1 */
    setup(0x1000); load(0x1000, p, 3); run(3);
    CHECK(g_cpu.d[1] == 0xFFFFFFFEu, "d1=%08X", g_cpu.d[1]);
    CHECK(g_cpu.nf == 1 && g_cpu.cf == 1 && g_cpu.vf == 0 && g_cpu.zf == 0, "flags n%d c%d v%d z%d",
          g_cpu.nf, g_cpu.cf, g_cpu.vf, g_cpu.zf);
}

static void t_cmp(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p[] = { 0x700A, 0x7205, 0xB081 }; /* moveq#10,d0; moveq#5,d1; cmp.l d1,d0 */
    setup(0x1000); load(0x1000, p, 3); run(3);
    CHECK(g_cpu.d[0] == 10, "d0 changed=%u", g_cpu.d[0]);
    CHECK(g_cpu.cf == 0 && g_cpu.zf == 0 && g_cpu.nf == 0, "cmp flags");
}

static void t_overflow(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* 0x7F + 1 as bytes -> signed overflow. moveq#$7F,d0; addi.b #1,d0 */
    uint16_t p[] = { 0x707F, 0x0600, 0x0001 }; /* moveq #$7F,d0 ; addi.b #1,d0 */
    setup(0x1000); load(0x1000, p, 3); run(2);
    CHECK((g_cpu.d[0] & 0xFF) == 0x80, "d0.b=%02X", g_cpu.d[0] & 0xFF);
    CHECK(g_cpu.vf == 1 && g_cpu.nf == 1, "v%d n%d", g_cpu.vf, g_cpu.nf);
}

static void t_logic(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p1[] = { 0x700C, 0x720A, 0xC081 }; /* and.l d1,d0 : 12&10=8 */
    setup(0x1000); load(0x1000, p1, 3); run(3);
    CHECK(g_cpu.d[0] == 8, "and=%u", g_cpu.d[0]);
    uint16_t p2[] = { 0x700C, 0x720A, 0x8081 }; /* or.l d1,d0 : 12|10=14 */
    setup(0x1000); load(0x1000, p2, 3); run(3);
    CHECK(g_cpu.d[0] == 14, "or=%u", g_cpu.d[0]);
    uint16_t p3[] = { 0x700C, 0x720A, 0xB380 }; /* eor.l d1,d0 : 12^10=6 */
    setup(0x1000); load(0x1000, p3, 3); run(3);
    CHECK(g_cpu.d[0] == 6, "eor=%u", g_cpu.d[0]);
}

static void t_shift(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p[] = { 0x7001, 0xE988 }; /* moveq#1,d0; lsl.l #4,d0 -> 16 */
    setup(0x1000); load(0x1000, p, 2); run(2);
    CHECK(g_cpu.d[0] == 16, "lsl=%u", g_cpu.d[0]);
    uint16_t p2[] = { 0x70F0, 0xE480 }; /* moveq#-16,d0; asr.l #2,d0 -> -4 */
    setup(0x1000); load(0x1000, p2, 2); run(2);
    CHECK(g_cpu.d[0] == 0xFFFFFFFCu, "asr=%08X", g_cpu.d[0]);
}

static void t_dbcc(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* moveq#0,d0; moveq#4,d1; loop: addq.w#1,d0; dbf d1,loop */
    uint16_t p[] = { 0x7000, 0x7204, 0x5240, 0x51C9, 0xFFFC };
    setup(0x1000); load(0x1000, p, 5); run(2 + 5 * 2);
    CHECK((g_cpu.d[0] & 0xFFFF) == 5, "loop count=%u", g_cpu.d[0] & 0xFFFF);
    CHECK((g_cpu.d[1] & 0xFFFF) == 0xFFFF, "d1=%04X", g_cpu.d[1] & 0xFFFF);
}

static void t_lea_move(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* lea $2000,a0 ; move.l #$12345678,(a0) ; move.l (a0),d0 */
    uint16_t p[] = { 0x41F8, 0x2000, 0x20BC, 0x1234, 0x5678, 0x2010 };
    setup(0x1000); load(0x1000, p, 6); run(3);
    CHECK(g_cpu.a[0] == 0x2000, "a0=%06X", g_cpu.a[0]);
    CHECK(g_cpu.d[0] == 0x12345678u, "d0=%08X", g_cpu.d[0]);
    CHECK(m_r32(NULL, 0x2000) == 0x12345678u, "mem");
}

static void t_incdec(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* lea $3000,a0; move.b #$AA,(a0)+; move.b #$BB,(a0)+; move.w $3000,d0 */
    uint16_t p[] = { 0x41F8, 0x3000, 0x10FC, 0x00AA, 0x10FC, 0x00BB, 0x3038, 0x3000 };
    setup(0x1000); load(0x1000, p, 8); run(4);
    CHECK(g_cpu.a[0] == 0x3002, "a0=%06X", g_cpu.a[0]);
    CHECK((g_cpu.d[0] & 0xFFFF) == 0xAABB, "readback=%04X (big-endian order)", g_cpu.d[0] & 0xFFFF);
}

static void t_jsr_rts(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t main_[] = { 0x4EB8, 0x1010, 0x7201, 0x60FE }; /* jsr $1010; moveq#1,d1; bra . */
    uint16_t sub_[]  = { 0x7007, 0x4E75 };                 /* moveq#7,d0; rts */
    setup(0x1000); load(0x1000, main_, 4); load(0x1010, sub_, 2);
    run(4);
    CHECK(g_cpu.d[0] == 7, "d0=%u (sub ran)", g_cpu.d[0]);
    CHECK(g_cpu.d[1] == 1, "d1=%u (returned)", g_cpu.d[1]);
    CHECK(g_cpu.a[7] == 0x8000, "sp restored=%06X", g_cpu.a[7]);
}

static void t_movem(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p[] = {
        0x7011, 0x7222, 0x7433,   /* moveq #$11,d0 ; #$22,d1 ; #$33,d2 */
        0x48E7, 0xE000,           /* movem.l d0-d2,-(a7) */
        0x7000, 0x7200, 0x7400,   /* clear d0-d2 */
        0x4CDF, 0x0007            /* movem.l (a7)+,d0-d2 */
    };
    setup(0x1000); load(0x1000, p, 10); run(8);
    CHECK(g_cpu.d[0] == 0x11 && g_cpu.d[1] == 0x22 && g_cpu.d[2] == 0x33,
          "restored d0=%X d1=%X d2=%X", g_cpu.d[0], g_cpu.d[1], g_cpu.d[2]);
    CHECK(g_cpu.a[7] == 0x8000, "sp balanced=%06X", g_cpu.a[7]);
}

static void t_muldiv(void) {
    memset(g_mem, 0, sizeof(g_mem));
    uint16_t p1[] = { 0x7006, 0x7207, 0xC0C1 }; /* moveq#6,d0; moveq#7,d1; mulu d1,d0 */
    setup(0x1000); load(0x1000, p1, 3); run(3);
    CHECK(g_cpu.d[0] == 42, "mulu=%u", g_cpu.d[0]);
    uint16_t p2[] = { 0x7064, 0x7207, 0x80C1 }; /* moveq#100,d0; moveq#7,d1; divu d1,d0 */
    setup(0x1000); load(0x1000, p2, 3); run(3);
    CHECK(g_cpu.d[0] == 0x0002000Eu, "divu q/r=%08X", g_cpu.d[0]); /* rem2:quot14 */
}

static void t_reset_vector(void) {
    memset(g_mem, 0, sizeof(g_mem));
    m_w32(NULL, 0, 0x00008000);   /* initial SSP */
    m_w32(NULL, 4, 0x00001000);   /* initial PC  */
    uint16_t p[] = { 0x7009 };    /* moveq #9,d0 */
    load(0x1000, p, 1);
    memset(&g_cpu, 0, sizeof(g_cpu)); bind();
    m68k_reset(&g_cpu);
    CHECK(g_cpu.pc == 0x1000, "reset pc=%06X", g_cpu.pc);
    CHECK(g_cpu.a[7] == 0x8000, "reset ssp=%06X", g_cpu.a[7]);
    CHECK(g_cpu.s == 1 && g_cpu.imask == 7, "reset supervisor+mask");
    run(1);
    CHECK(g_cpu.d[0] == 9, "post-reset exec d0=%u", g_cpu.d[0]);
}

static void t_interrupt(void) {
    memset(g_mem, 0, sizeof(g_mem));
    /* autovector level 2 -> vector 26 -> address 0x68 */
    m_w32(NULL, 26 * 4, 0x00002000);  /* handler at 0x2000 */
    uint16_t handler[] = { 0x7042, 0x4E73 }; /* moveq #$42,d0 ; rte */
    uint16_t main_[]   = { 0x60FE };         /* bra . (idle) */
    setup(0x1000); load(0x1000, main_, 1); load(0x2000, handler, 2);
    g_cpu.imask = 1;                  /* allow level 2 through */
    m68k_set_irq(&g_cpu, 2);
    run(4);
    CHECK(g_cpu.d[0] == 0x42, "irq handler ran, d0=%02X", g_cpu.d[0]);
}

int main(void) {
    printf("== Onca 68000 unit tests ==\n");
    t_moveq_add();
    t_sub_borrow();
    t_cmp();
    t_overflow();
    t_logic();
    t_shift();
    t_dbcc();
    t_lea_move();
    t_incdec();
    t_jsr_rts();
    t_movem();
    t_muldiv();
    t_reset_vector();
    t_interrupt();
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
