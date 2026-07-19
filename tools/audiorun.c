/*
 * audiorun.c - headless audio-path verifier.
 *
 * Boots a cart exactly like the shipping libretro core (same 68000/GPU/DSP
 * interleave, same I2S interrupt delivery) and captures the stereo DAC latches
 * (LTXD $F1A148 / RTXD $F1A14C) right before each delivered sample interrupt -
 * the same capture point retro_run uses. Reports per-second capture counts and
 * amplitude stats, and writes the whole capture as a 44.1 kHz stereo WAV so a
 * human can listen to what the DSP sound driver is actually producing.
 *
 * Optional input injection (JOYHOLD/JOYA/JOYB env vars, as gamerun.c) to get
 * past the title screen and trigger in-game sound effects.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CPU_HZ 13295000u
#define AUDIO_HZ 44100u
#define CYC_PER_SAMPLE (CPU_HZ / AUDIO_HZ)

static m68k_t cpu;
static onca_mem_t mem;
static onca_gpu_t gpu;
static onca_gpu_t dsp;

static int16_t *wav;
static size_t wav_n, wav_cap;

/* Where does the DSP program actually store? Count STOREs by target: Jerry
 * register space by 64K offset, plus which DSP-RAM PCs execute at all (does
 * the I2S ISR at $F1B010 run its mix loop or early-out?). */
static unsigned long g_store_jerry[0x10000];
static unsigned long g_store_dram_lo, g_store_dram_hi;
static unsigned long g_pc_hist[0x1000];        /* per word of DSP RAM */
static void dsp_trace(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx;
    if (pc >= 0xF1B000 && pc < 0xF1D000) g_pc_hist[((pc - 0xF1B000) >> 1) & 0xFFF]++;
    int opc = (op >> 10) & 0x3F;
    int sfield = (op >> 5) & 31;
    uint32_t rs;
    if (opc >= 45 && opc <= 48)      rs = dsp.reg[dsp.bank * 32 + sfield];
    else if (opc == 49)              rs = dsp.reg[dsp.bank * 32 + 14] + (uint32_t)sfield * 4;
    else if (opc == 50)              rs = dsp.reg[dsp.bank * 32 + 15] + (uint32_t)sfield * 4;
    else if (opc == 60 || opc == 61) rs = dsp.reg[dsp.bank * 32 + (opc == 60 ? 14 : 15)]
                                        + dsp.reg[dsp.bank * 32 + sfield];
    else return;
    if (rs >= 0xF10000 && rs < 0xF20000) g_store_jerry[rs & 0xFFFF]++;
    else if (rs < 0x100000) g_store_dram_lo++;
    else if (rs < 0x200000) g_store_dram_hi++;
}

static void capture_pair(void) {
    if (wav_n + 2 > wav_cap) return;
    wav[wav_n++] = (int16_t)(onca_peek32(&mem, 0xF1A148) & 0xFFFF);
    wav[wav_n++] = (int16_t)(onca_peek32(&mem, 0xF1A14C) & 0xFFFF);
}

static void wav_write(const char *path, const int16_t *pcm, uint32_t frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t data = frames * 4, rate = AUDIO_HZ, bps = rate * 4, riff = 36 + data;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmtsz = 16; uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
    fwrite(&fmtsz, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, frames * 2, f);
    fclose(f);
    printf("wrote %s (%u frames, %.1f s)\n", path, frames, frames / (double)AUDIO_HZ);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s jagboot.rom cart.bin [frames] [out.wav]\n", argv[0]); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 900;
    const char *out = argc > 4 ? argv[4] : "audio.wav";

    FILE *fr = fopen(argv[1], "rb");
    if (!fr) { perror("bios"); return 1; }
    static uint8_t rom[ONCA_ROM_SIZE];
    size_t rn = fread(rom, 1, ONCA_ROM_SIZE, fr); fclose(fr);

    FILE *fc = fopen(argv[2], "rb");
    if (!fc) { perror("cart"); return 1; }
    fseek(fc, 0, SEEK_END); long cs = ftell(fc); fseek(fc, 0, SEEK_SET);
    if (cs > (long)ONCA_CART_MAX) cs = ONCA_CART_MAX;
    uint8_t *cart = malloc(cs);
    if (fread(cart, 1, cs, fc) != (size_t)cs) return 1; fclose(fc);

    wav_cap = (size_t)frames * 800 * 2;
    wav = malloc(wav_cap * 2);

    onca_mem_init(&mem);
    memcpy(mem.rom, rom, rn); mem.rom_loaded = rn;
    onca_mem_set_cart(&mem, cart, cs);
    mem.security_bypass = 1;
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    onca_gpu_init(&gpu, &mem); mem.gpu = &gpu;
    onca_gpu_init(&dsp, &mem); dsp.is_dsp = 1; mem.dsp = &dsp;
    if (getenv("AUDTRACE")) dsp.trace = dsp_trace;
    m68k_reset(&cpu);
    cpu.int_vector = 64;
    mem.cpu_pc = &cpu.pc;

    uint32_t joyhold = getenv("JOYHOLD") ? (uint32_t)strtoul(getenv("JOYHOLD"), 0, 0) : 0;
    int ta = getenv("JOYA") ? atoi(getenv("JOYA")) : (frames - 60);
    int tb = getenv("JOYB") ? atoi(getenv("JOYB")) : frames;

    uint64_t budget = (uint64_t)(CPU_HZ / 59.94);
    uint64_t isr_acc = 0; int isr_owed = 0, launched = 0;
    long delivered = 0, sec_delivered = 0;
    size_t sec_mark = 0;

    for (int f = 0; f < frames; f++) {
        mem.joypad1 = (f >= ta && f < tb) ? joyhold : 0;
        mem.video_irq = 1;
        uint64_t target = cpu.cycles + budget, prev = cpu.cycles;
        while (cpu.cycles < target && !cpu.halted) {
            m68k_set_irq(&cpu, mem.video_irq ? 2 : 0);
            m68k_step(&cpu);
            if (!launched && cpu.pc >= ONCA_CART_BASE && cpu.pc < ONCA_CART_END) launched = 1;
            isr_acc += cpu.cycles - prev; prev = cpu.cycles;
            while (isr_acc >= CYC_PER_SAMPLE) { isr_acc -= CYC_PER_SAMPLE; if (isr_owed < 64) isr_owed++; }
            if (gpu.running)
                for (int k = 0; k < 16 && gpu.running; k++) onca_gpu_step(&gpu);
            if (dsp.running)
                for (int k = 0; k < 16 && dsp.running; k++) {
                    onca_gpu_step(&dsp);
                    if (launched && isr_owed > 0) {
                        uint32_t df = onca_gpu_read_ctrl(&dsp, 0xF1A100);
                        if ((df & DF_I2SENA) && !(df & GF_IMASK)) {
                            capture_pair();
                            onca_gpu_interrupt(&dsp, 1);
                            isr_owed--; delivered++; sec_delivered++;
                        }
                    }
                }
        }
        if ((f % 60) == 59) {
            /* per-second stats over the samples captured this second */
            size_t n = wav_n - sec_mark;
            int16_t mn = 0, mx = 0; long nz = 0; double acc = 0;
            for (size_t i = sec_mark; i < wav_n; i++) {
                if (wav[i]) nz++;
                if (wav[i] < mn) mn = wav[i];
                if (wav[i] > mx) mx = wav[i];
                acc += (double)wav[i] * wav[i];
            }
            printf("sec %2d: pc=%06X dsp=%d delivered=%ld pairs=%zu nonzero=%ld min=%d max=%d rms=%.0f rd=%08X wr_cur=%08X cnt=%08X\n",
                   f / 60, cpu.pc & 0xFFFFFF, dsp.running, sec_delivered, n / 2, nz, mn, mx,
                   n ? sqrt(acc / (double)n) : 0.0,
                   dsp.reg[21], onca_peek32(&mem, 0x4DC14), onca_peek32(&mem, 0xF1B02C));
            sec_delivered = 0; sec_mark = wav_n;
        }
    }

    printf("total delivered=%ld captured_pairs=%zu\n", delivered, wav_n / 2);
    wav_write(out, wav, (uint32_t)(wav_n / 2));

    if (getenv("DUMPDSP")) {
        FILE *d = fopen(getenv("DUMPDSP"), "wb");
        if (d) {
            for (uint32_t a = 0xF1B000; a < 0xF1D000; a++) fputc(onca_peek8(&mem, a), d);
            fclose(d);
            printf("dumped DSP RAM to %s\n", getenv("DUMPDSP"));
        }
    }
    if (getenv("AUDTRACE")) {
        printf("--- DSP stores to Jerry space (offset: count) ---\n");
        for (int i = 0; i < 0x10000; i++)
            if (g_store_jerry[i]) printf("  F1%04X : %lu\n", i, g_store_jerry[i]);
        printf("DRAM stores: <1MB=%lu >=1MB=%lu\n", g_store_dram_lo, g_store_dram_hi);
        printf("--- DSP RAM PC coverage (first/last executed word per 16-word row) ---\n");
        for (int row = 0; row < 0x1000; row += 16) {
            unsigned long tot = 0; for (int i = 0; i < 16; i++) tot += g_pc_hist[row + i];
            if (tot) printf("  F1B%03X: %lu\n", row * 2, tot);
        }
    }
    return 0;
}
