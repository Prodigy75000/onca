/*
 * test_gpu.c - unit tests for the Jaguar GPU (Tom RISC) interpreter.
 *
 * Hand-assembles small GPU programs into GPU RAM, runs them, and checks
 * register / memory results. Instruction word = (op<<10)|(src<<5)|dst.
 *
 * Build: gcc -std=c11 src/memory.c src/gpu.c tests/test_gpu.c -o test_gpu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/gpu.h"
#include "../src/memory.h"
#include <stdio.h>
#include <string.h>

static onca_mem_t mem;
static onca_gpu_t g;
static uint32_t P;   /* assembler cursor */

#define ENC(op, s, d) ((uint16_t)(((op) << 10) | (((s) & 0x1F) << 5) | ((d) & 0x1F)))
static void emit(uint16_t w) { onca_poke16(&mem, P, w); P += 2; }
static void emiti(uint32_t imm) { emit((uint16_t)imm); emit((uint16_t)(imm >> 16)); }

static void begin(void) {
    onca_mem_init(&mem);
    onca_gpu_init(&g, &mem);
    P = G_RAM_BASE;
}
static void go(int steps) {
    onca_gpu_write_ctrl(&g, G_PC, G_RAM_BASE);
    onca_gpu_write_ctrl(&g, G_CTRL, GC_GPUGO);
    for (int i = 0; i < steps; i++) onca_gpu_step(&g);
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void t_arith(void) {
    begin();
    emit(ENC(35, 5, 0));   /* MOVEQ #5,R0 */
    emit(ENC(35, 3, 1));   /* MOVEQ #3,R1 */
    emit(ENC(0, 1, 0));    /* ADD R1,R0   -> 8 */
    emit(ENC(4, 1, 0));    /* SUB R1,R0   -> 5 */
    go(4);
    CHECK(g.reg[0] == 5, "R0=%u", g.reg[0]);
    CHECK(g.zf == 0 && g.nf == 0, "flags");
}

static void t_logic(void) {
    begin();
    emit(ENC(35, 12, 0));  /* MOVEQ #12,R0 */
    emit(ENC(35, 10, 1));  /* MOVEQ #10,R1 */
    emit(ENC(9, 1, 0));    /* AND R1,R0 -> 8 */
    go(3);
    CHECK(g.reg[0] == 8, "and=%u", g.reg[0]);
    begin();
    emit(ENC(35, 12, 0)); emit(ENC(35, 10, 1)); emit(ENC(11, 1, 0)); /* XOR -> 6 */
    go(3);
    CHECK(g.reg[0] == 6, "xor=%u", g.reg[0]);
}

static void t_movei(void) {
    begin();
    emit(ENC(38, 0, 2)); emiti(0x12345678u);   /* MOVEI #imm,R2 */
    go(1);
    CHECK(g.reg[2] == 0x12345678u, "R2=%08X", g.reg[2]);
}

static void t_shift(void) {
    begin();
    emit(ENC(35, 1, 0));        /* MOVEQ #1,R0 */
    emit(ENC(24, 32 - 4, 0));   /* SHLQ #4,R0 -> 16 (field holds 32-n) */
    go(2);
    CHECK(g.reg[0] == 16, "shlq=%u", g.reg[0]);
    begin();
    emit(ENC(38, 0, 0)); emiti(0x00000080u);   /* MOVEI #0x80,R0 */
    emit(ENC(25, 3, 0));   /* SHRQ #3,R0 -> 16 */
    go(2);
    CHECK(g.reg[0] == 16, "shrq=%u", g.reg[0]);
}

static void t_memory(void) {
    begin();
    emit(ENC(38, 0, 3)); emiti(0x00001000u);   /* MOVEI #0x1000,R3 (DRAM) */
    emit(ENC(38, 0, 4)); emiti(0xDEADBEEFu);   /* MOVEI #imm,R4 */
    emit(ENC(47, 3, 4));   /* STORE R4,(R3): mem[Rs=R3] = Rd=R4 */
    emit(ENC(35, 0, 4));   /* MOVEQ #0,R4 */
    emit(ENC(41, 3, 5));   /* LOAD (R3),R5 */
    emit(ENC(39, 3, 6));   /* LOADB (R3),R6 -> top byte 0xDE (big-endian) */
    go(6);
    CHECK(g.reg[5] == 0xDEADBEEFu, "LOAD=%08X", g.reg[5]);
    CHECK(onca_peek32(&mem, 0x1000) == 0xDEADBEEFu, "mem");
    CHECK(g.reg[6] == 0xDE, "LOADB=%02X", g.reg[6]);
}

static void t_altbank(void) {
    begin();
    emit(ENC(35, 7, 0));   /* MOVEQ #7,R0 */
    emit(ENC(36, 0, 5));   /* MOVETA R0,R5 (alt bank R5 = 7) */
    emit(ENC(37, 5, 1));   /* MOVEFA R5,R1 (R1 = alt R5 = 7) */
    go(3);
    CHECK(g.reg[1] == 7, "movefa=%u", g.reg[1]);
    CHECK(g.reg[32 + 5] == 7, "alt R5=%u", g.reg[32 + 5]);
}

static void t_loop_jr(void) {
    begin();
    emit(ENC(35, 0, 0));   /* MOVEQ #0,R0 */
    emit(ENC(35, 5, 1));   /* MOVEQ #5,R1 */
    /* loop: */
    emit(ENC(2, 1, 0));    /* ADDQ #1,R0 */
    emit(ENC(6, 1, 1));    /* SUBQ #1,R1 (sets Z when 0) */
    emit(ENC(53, 0x1D, 0x01)); /* JR NE,-3 -> back to loop */
    for (int i = 0; i < 6; i++) emit(ENC(57, 0, 0)); /* NOP delay slot + sled */
    go(24);
    CHECK(g.reg[0] == 5, "loop count R0=%u", g.reg[0]);
    CHECK(g.reg[1] == 0, "R1=%u", g.reg[1]);
}

static void t_jump(void) {
    begin();
    emit(ENC(38, 0, 10)); emiti(G_RAM_BASE + 0x20);  /* MOVEI #target,R10 */
    emit(ENC(52, 10, 0));  /* JUMP T,(R10) */
    emit(ENC(57, 0, 0));   /* NOP - delay slot (always executes) */
    emit(ENC(35, 9, 0));   /* MOVEQ #9,R0 - jumped over, must NOT run */
    P = G_RAM_BASE + 0x20;
    emit(ENC(35, 3, 0));   /* target: MOVEQ #3,R0 */
    go(4);
    CHECK(g.reg[0] == 3, "jump landed, R0=%u (not 9)", g.reg[0]);
}

static void t_align_assert(void) {
    /* The PC-alignment facility must flag a jump that lands inside a MOVEI's
     * immediate words, and name the guilty branch. */
    begin();
    emit(ENC(38, 0, 1)); emiti(0xCAFE1234);          /* MOVEI #imm,R1: imms at +2,+4 */
    emit(ENC(38, 0, 10)); emiti(G_RAM_BASE + 2);     /* MOVEI #(imm word!),R10 */
    emit(ENC(52, 10, 0));  /* JUMP T,(R10) - lands mid-MOVEI */
    emit(ENC(57, 0, 0));   /* delay slot */
    go(6);
    CHECK(g.misaligns > 0, "misalign detected (misaligns=%ld)", g.misaligns);
    CHECK(g.misalign_pc == G_RAM_BASE + 2, "misaligned PC=%06X", g.misalign_pc);
    CHECK(g.misalign_jmp_pc == G_RAM_BASE + 12, "guilty branch pc=%06X", g.misalign_jmp_pc);

    /* And a rewritten (stale) claim must NOT fire: overwrite the MOVEI with
     * NOPs, then execute the once-immediate word as a normal instruction. */
    begin();
    emit(ENC(38, 0, 1)); emiti(0xDEAD5678);          /* MOVEI marks +2,+4 */
    emit(ENC(57, 0, 0));
    go(2);                                           /* execute MOVEI + NOP */
    onca_poke16(&mem, G_RAM_BASE, ENC(57, 0, 0));   /* overwrite MOVEI: reload */
    onca_poke16(&mem, G_RAM_BASE + 2, ENC(35, 7, 0));
    onca_gpu_write_ctrl(&g, G_PC, G_RAM_BASE + 2);
    onca_gpu_write_ctrl(&g, G_CTRL, GC_GPUGO);
    onca_gpu_step(&g);
    CHECK(g.misaligns == 0, "stale claim invalidated (misaligns=%ld)", g.misaligns);
    CHECK(g.reg[0] == 7, "rewritten word executed normally R0=%u", g.reg[0]);
}

static void t_mult_16bit(void) {
    /* Tom manual: MULT = unsigned 16x16 (bottom halves) -> 32; IMULT = signed
     * 16x16 -> 32. High halves of the operands must be IGNORED - Doom's
     * FixedMul-style physics (friction!) depends on it. */
    begin();
    emit(ENC(38, 0, 1)); emiti(0xDEAD0003);   /* MOVEI #$DEAD0003,R1 */
    emit(ENC(38, 0, 2)); emiti(0xBEEF0004);   /* MOVEI #$BEEF0004,R2 */
    emit(ENC(16, 1, 2));                       /* MULT R1,R2: 3*4, junk high */
    emit(ENC(38, 0, 3)); emiti(0x7777FFFE);   /* MOVEI #$7777FFFE,R3 (-2)  */
    emit(ENC(38, 0, 4)); emiti(0x12340003);   /* MOVEI #$12340003,R4 (+3)  */
    emit(ENC(17, 3, 4));                       /* IMULT R3,R4: -2*3 = -6    */
    go(6);
    CHECK(g.reg[2] == 12, "MULT low-16 unsigned: R2=%u (want 12)", g.reg[2]);
    CHECK(g.reg[4] == 0xFFFFFFFAu, "IMULT low-16 signed: R4=%08X (want -6)", g.reg[4]);
}

static void t_mac_group(void) {
    /* IMULTN starts the accumulator, IMACN accumulates, RESMAC writes it.
     * All 16-bit signed products. 2*3 + 4*5 + (-1)*7 = 6+20-7 = 19. */
    begin();
    emit(ENC(38, 0, 1)); emiti(0x00010002);
    emit(ENC(38, 0, 2)); emiti(0x00010003);
    emit(ENC(38, 0, 3)); emiti(0x00010004);
    emit(ENC(38, 0, 4)); emiti(0x00010005);
    emit(ENC(38, 0, 5)); emiti(0x0001FFFF);   /* low16 = -1 */
    emit(ENC(38, 0, 6)); emiti(0x00010007);
    emit(ENC(18, 1, 2));                       /* IMULTN R1,R2 */
    emit(ENC(20, 3, 4));                       /* IMACN  R3,R4 */
    emit(ENC(20, 5, 6));                       /* IMACN  R5,R6 */
    emit(ENC(19, 0, 7));                       /* RESMAC -> R7 */
    go(10);
    CHECK(g.reg[7] == 19, "MAC group: R7=%u (want 19)", g.reg[7]);
    CHECK(g.reg[2] == 0x00010003 && g.reg[4] == 0x00010005,
          "IMULTN/IMACN no write-back (R2=%08X R4=%08X)", g.reg[2], g.reg[4]);
}

static void t_dsp_modulo(void) {
    /* DSP ADDQMOD/SUBQMOD: D_MOD bits that are SET are preserved from the
     * original register (ring base); CLEAR bits take the sum (offset wraps).
     * Doom's sample ring: D_MOD=$FFFFE000, 8KB ring at $1F0000 - the pointer
     * must wrap $1F1FFE+4 -> $1F0002, never march out of the ring. */
    begin();
    g.is_dsp = 1;
    onca_gpu_write_ctrl(&g, 0xF1A118, 0xFFFFE000u);   /* D_MOD */
    emit(ENC(38, 0, 0)); emiti(0x001F1FFEu);
    emit(ENC(63, 4, 0));                       /* ADDQMOD #4,R0 */
    emit(ENC(38, 0, 1)); emiti(0x001F0002u);
    emit(ENC(54, 4, 1));                       /* SUBQMOD #4,R1 */
    go(4);
    CHECK(g.reg[0] == 0x001F0002u, "ADDQMOD wrap: R0=%08X (want 001F0002)", g.reg[0]);
    CHECK(g.reg[1] == 0x001F1FFEu, "SUBQMOD wrap: R1=%08X (want 001F1FFE)", g.reg[1]);
}

static void t_dsp_sat16s(void) {
    /* Op 33 is unsigned SAT16 on Tom but signed SAT16S on Jerry; Doom's audio
     * mixer relies on the signed clamp (unsigned would half-wave-rectify). */
    begin();
    g.is_dsp = 1;
    emit(ENC(38, 0, 0)); emiti((uint32_t)-40000);
    emit(ENC(33, 0, 0));                       /* SAT16S -> -32768 */
    emit(ENC(38, 0, 1)); emiti(40000u);
    emit(ENC(33, 0, 1));                       /* SAT16S -> 32767 */
    emit(ENC(38, 0, 2)); emiti((uint32_t)-5);
    emit(ENC(33, 0, 2));                       /* SAT16S -> -5 (unchanged) */
    go(6);
    CHECK(g.reg[0] == (uint32_t)-32768, "SAT16S low clamp: R0=%08X", g.reg[0]);
    CHECK(g.reg[1] == 32767, "SAT16S high clamp: R1=%u", g.reg[1]);
    CHECK(g.reg[2] == (uint32_t)-5, "SAT16S passthrough: R2=%08X", g.reg[2]);
    begin();                                   /* Tom stays unsigned */
    emit(ENC(38, 0, 0)); emiti((uint32_t)-5);
    emit(ENC(33, 0, 0));                       /* SAT16 -> 0 */
    go(2);
    CHECK(g.reg[0] == 0, "Tom SAT16 unsigned clamp: R0=%u", g.reg[0]);
}

int main(void) {
    printf("== Onca GPU (Tom RISC) unit tests ==\n");
    t_arith();
    t_logic();
    t_movei();
    t_shift();
    t_memory();
    t_altbank();
    t_loop_jr();
    t_jump();
    t_align_assert();
    t_mult_16bit();
    t_mac_group();
    t_dsp_modulo();
    t_dsp_sat16s();
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
