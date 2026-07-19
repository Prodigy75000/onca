/*
 * gpu.h - Atari Jaguar GPU (Tom's RISC processor) and Jerry DSP (shared core).
 *
 * The GPU is Tom's custom 32-bit RISC core (26.591 MHz), separate from the
 * 68000. It executes 16-bit instructions from its 4 KB local RAM ($F03000) and
 * is the engine that actually generates graphics - the boot ROM loads a program
 * into GPU RAM, sets G_PC, and starts it with GPUGO. Two banks of 32 registers,
 * a MAC unit, a divide unit, Z/C/N flags.
 *
 * Clean-room from the public Tom & Jerry Technical Reference Manual. Instruction
 * encoding confirmed against the boot ROM's own GPU program: 16-bit big-endian
 * words, opcode = bits[15:10], source = bits[9:5], dest = bits[4:0].
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_GPU_H
#define ONCA_GPU_H

#include <stdint.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPU control registers (Tom, byte addresses). */
#define G_FLAGS   0xF02100
#define G_MTXC    0xF02104
#define G_MTXA    0xF02108
#define G_END     0xF0210C
#define G_PC      0xF02110
#define G_CTRL    0xF02114
#define G_HIDATA  0xF02118
#define G_REMAIN  0xF0211C   /* divide remainder (read) / G_DIVCTRL (write) */
#define G_RAM_BASE 0xF03000
#define G_RAM_END  0xF04000

/* G_FLAGS bits. */
#define GF_ZERO   0x00000001u
#define GF_CARRY  0x00000002u
#define GF_NEG    0x00000004u
#define GF_REGPAGE 0x00004000u   /* bit 14: active register bank */
#define GF_DMAEN  0x00008000u

/* G_CTRL bits. */
#define GC_GPUGO  0x00000001u

typedef struct onca_gpu {
    uint32_t reg[64];    /* two banks of 32; active bank chosen by GF_REGPAGE */
    uint32_t pc;
    uint32_t ctrl;
    uint32_t hidata;
    uint32_t remain;
    uint32_t divctrl;
    uint32_t mtxc, mtxa;

    uint8_t  zf, cf, nf; /* Z / C / N flags (mirrored into G_FLAGS)          */
    uint8_t  bank;       /* active register bank: 0 or 1 (from GF_REGPAGE)   */
    uint32_t flags_hi;   /* interrupt-enable / misc bits of G_FLAGS we keep  */

    int64_t  acc;        /* MAC accumulator                                   */
    int      running;    /* GPUGO latched                                     */
    uint64_t cycles;
    uint32_t mod;        /* DSP D_MOD: modulo mask for ADDQMOD/SUBQMOD        */
    uint8_t  is_dsp;     /* 0 = Tom GPU, 1 = Jerry DSP (a few opcodes differ) */

    /* Branch delay slot: JUMP/JR take effect one instruction late (the
     * instruction after the branch always executes first). */
    uint32_t delay_target;
    uint8_t  delay_pending;
    uint8_t  mac_lock;       /* inside an IMULTN/IMACN..RESMAC group: no IRQs */
    uint8_t  pend_int0;      /* CTRL bit 2 written: host-forced interrupt 0,
                              * latched until the core can take it (Wolf3D
                              * signals its DSP command mailbox this way)     */

    onca_mem_t *mem;    /* shared system bus for LOAD/STORE + instr fetch    */

    /* Optional trace hook, called before each instruction (diagnostics). */
    void (*trace)(void *ctx, uint32_t pc, uint16_t op);
    void  *trace_ctx;

    /* --- PC-alignment assert (permanent debug facility) ---------------------
     * MOVEI is 6 bytes: opcode word + two immediate words that are DATA, not
     * instructions. A control transfer landing on an immediate word executes
     * data as code (real Tom never does this; it means an emulated jump target
     * or return address is off). We mark the two immediate words of every MOVEI
     * as it executes, and flag any instruction fetch that lands on a marked
     * word. Marks are validated against live RAM (imm_word must still match the
     * owning MOVEI) so overlay reloads self-invalidate stale claims.
     * On the first hit we latch the offending PC and the last taken JUMP/JR
     * (pc/opcode/target) - i.e. the branch that caused the misalignment. */
    uint8_t  imm_off[4096];   /* per local-RAM word: 0=code, 1/2=Nth MOVEI imm */
    uint16_t imm_word[4096];  /* opcode word of the claiming MOVEI             */
    uint32_t last_jmp_pc;     /* last taken JUMP/JR: instruction address       */
    uint16_t last_jmp_op;     /*   ... its opcode word                         */
    uint32_t last_jmp_tgt;    /*   ... its target                              */
    long     misaligns;       /* total misaligned fetches detected             */
    uint32_t misalign_pc;     /* first event: the misaligned PC                */
    uint32_t misalign_jmp_pc; /* first event: the guilty branch's address      */
    uint16_t misalign_jmp_op; /* first event: the guilty branch's opcode       */
    uint32_t misalign_tgt;    /* first event: the guilty branch's target       */
} onca_gpu_t;

/* Bind the GPU to system memory and clear it. */
void onca_gpu_init(onca_gpu_t *g, onca_mem_t *mem);

/* Called when the 68000 writes a GPU control register (so the GPU picks up
 * G_PC / G_CTRL / G_FLAGS writes). addr is the byte address, v the value. */
void onca_gpu_write_ctrl(onca_gpu_t *g, uint32_t addr, uint32_t v);

/* Read a GPU control register back (G_FLAGS assembles the live flags). */
uint32_t onca_gpu_read_ctrl(onca_gpu_t *g, uint32_t addr);

/* Execute one instruction if running. Returns cycles consumed (0 if halted). */
int onca_gpu_step(onca_gpu_t *g);

/* Take RISC interrupt `num` (0..5): vector to RAM_base + num*0x10, primary bank,
 * push resume PC to R31, set the interrupt mask. Used to deliver the DSP's audio
 * (I2S) sample interrupt, which games use as a timebase. Returns 1 if taken,
 * 0 if refused (mid-branch / MAC group / already masked) - the caller must
 * keep a refused interrupt pending and retry, never drop it. */
int onca_gpu_interrupt(onca_gpu_t *g, int num);

/* FLAGS interrupt bits (shared GPU/DSP layout). */
#define GF_IMASK   0x00000008u   /* master interrupt mask (set on entry)      */
#define DF_I2SENA  0x00000020u   /* DSP: I2S (sample-rate) interrupt enable   */

/* Run up to `budget` GPU cycles (stops early if the GPU halts). */
uint64_t onca_gpu_run(onca_gpu_t *g, uint64_t budget);

#ifdef __cplusplus
}
#endif

#endif /* ONCA_GPU_H */
