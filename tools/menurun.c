/*
 * menurun.c - reproduce the Doom menu item-draw bail and catch the writer.
 *
 * Prior finding: on entering the main menu the item loop at ~$82D2 stops
 * advancing because its own code (an `addq #1,d3`) reads as zero - i.e. some
 * coprocessor store corrupted 68k code in DRAM. This tool boots Doom, presses
 * A to enter the menu, snapshots the $8000-$8400 code window, and watches
 * every write into it: CPU-bus writes via the mem log, GPU/DSP/Blitter writes
 * via the poke watch. Any hit prints the writer's live state (68k/GPU/DSP PC,
 * GPU R14/R15) so the corrupting instruction can be identified.
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

#define CPU_HZ 13295000u
#define AUDIO_HZ 41553u   /* I2S word-strobe rate for SCLK=19 (see libretro_core.c) */
#define CYC_PER_SAMPLE (CPU_HZ / AUDIO_HZ)
#define WLO 0xF1B7F0u
#define WHI 0xF1B900u

static m68k_t cpu;
static onca_mem_t mem;
static onca_gpu_t gpu;
static onca_gpu_t dsp;

static uint8_t snap[WHI - WLO];
static int snapped;
static long hits, cpu_hits;
static int cur_frame;

/* Menu-drawer call tracing: count entries into the two draw helpers the item
 * loop calls (DrawJagobj $4E5C / EraseBlock-side $4F1C per prior RE), and log
 * their stack arguments for a few frames so we can see which items are
 * attempted and with what source lumps. */
static long g_calls_4e5c, g_calls_4f1c;
static int g_logcalls;
static void cpu_trace(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx; (void)op;
    if (pc == 0x4E5C || pc == 0x4F1C) {
        if (pc == 0x4E5C) g_calls_4e5c++; else g_calls_4f1c++;
        if (g_logcalls > 0) {
            g_logcalls--;
            uint32_t sp = cpu.a[7];
            printf("CALL  f%-4d $%04X(args %08X %08X %08X %08X) from=%06X d3=%d\n",
                   cur_frame, pc,
                   onca_peek32(&mem, sp + 4), onca_peek32(&mem, sp + 8),
                   onca_peek32(&mem, sp + 12), onca_peek32(&mem, sp + 16),
                   onca_peek32(&mem, sp) & 0xFFFFFF, (int)cpu.d[3]);
        }
    }
}

/* Blit log during the menu frames: raw command + both channels' full state
 * pulled from the live Tom registers, to correlate DrawJagobj calls with what
 * the Blitter is actually told to do. */
static int g_logblits;
static void blit_cb(void *ctx, uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t count) {
    (void)ctx;
    /* SRCSHADE blits: dump the intensity iterator inputs (SHADEWATCH=frame) */
    static long shn;
    if (getenv("SHADEWATCH") && cur_frame >= atoi(getenv("SHADEWATCH")) &&
        (cmd & 0x40000000u) && shn++ < 60)
        printf("SHADE f%-4d cmd=%08X cnt=%u,%u a1=%06X a2=%06X PATD=%08X %08X SRCD=%08X %08X IINC=%08X\n",
               cur_frame, cmd, count & 0xFFFF, count >> 16, a1 & 0xFFFFFF, a2 & 0xFFFFFF,
               onca_peek32(&mem, 0xF02268), onca_peek32(&mem, 0xF0226C),
               onca_peek32(&mem, 0xF02248), onca_peek32(&mem, 0xF0224C),
               onca_peek32(&mem, 0xF02270));
    if (g_logblits <= 0) return;
    g_logblits--;
    uint32_t t = 0xF02200;
    #define TR(o) onca_peek32(&mem, t + (o))
    printf("BLIT  f%-4d cmd=%08X cnt=%u,%u a1=%06X a2=%06X | A1 flg=%08X pix=%08X stp=%08X | A2 flg=%08X pix=%08X stp=%08X | 68k=%06X gpu=%d\n",
           cur_frame, cmd, count & 0xFFFF, count >> 16, a1 & 0xFFFFFF, a2 & 0xFFFFFF,
           TR(0x04), TR(0x0C), TR(0x10),
           TR(0x24), TR(0x2C), TR(0x30),
           cpu.pc & 0xFFFFFF, gpu.running);
    #undef TR
}

static void watch_cb(void *ctx, uint32_t a, uint8_t v) {
    (void)ctx;
    hits++;
    /* the wipe hunt: log ZERO bytes written over the music job's code tail */
    static long zn;
    if (v == 0 && a >= 0xF1B800 && zn++ < 100)
        printf("WIPE  f%-4d $%06X <- 00  dsp pc=%06X gpu pc=%06X 68k=%06X\n",
               cur_frame, a, dsp.pc & 0xFFFFFF, gpu.pc & 0xFFFFFF, cpu.pc & 0xFFFFFF);
}

static void log_cb(void *ctx, int is_write, int width, onca_region_t region,
                   uint32_t addr, uint32_t val) {
    (void)ctx; (void)region;
    /* Cart EEPROM (93C46 serial) GPIO accesses live in Jerry space above the
     * joypad registers; log every touch to see how Doom reads its settings. */
    static long een;
    if (addr >= 0xF14400 && addr < 0xF16000 && een++ < 60)
        printf("EEPR  f%-4d %s%d $%06X %s %08X pc=%06X\n",
               cur_frame, is_write ? "W" : "R", width, addr,
               is_write ? "<-" : "->", val, cpu.pc & 0xFFFFFF);
    /* Joypad accesses in a chosen frame window (JOYWATCH=startframe) */
    static long joyn;
    if (getenv("JOYWATCH") && cur_frame >= atoi(getenv("JOYWATCH")) &&
        addr >= 0xF14000 && addr < 0xF14004 && joyn++ < 120)
        printf("JOY   f%-4d %s%d $%06X %s %08X pc=%06X row=%02X pad=%08X\n",
               cur_frame, is_write ? "W" : "R", width, addr,
               is_write ? "<-" : "->", val, cpu.pc & 0xFFFFFF,
               mem.joy_row, mem.joypad1);
    if (!is_write) return;
    /* 68k -> DSP job dispatches: the mailbox the DSP kernel polls. Log the
     * job's code size word (at job_ptr-4) too - the kernel copies that many
     * bytes to $F1B140, and anything past $F1B800 lands in the accumulator. */
    static long jobn;
    if (addr <= 0xF1B030 && addr + width > 0xF1B030 && val != 0 && jobn++ < 20)
        printf("JOB   f%-4d 68k pc=%06X dispatches $F1B030 <- %08X size=%u\n",
               cur_frame, cpu.pc & 0xFFFFFF, val, onca_peek32(&mem, (val & 0xFFFFFF) - 4));
    /* 68k writes into DSP RAM above the kernel (params, voice tables) */
    static long parmn;
    if (addr >= 0xF1C000 && addr < 0xF1D000 && val != 0 && parmn++ < 120)
        printf("PARM  f%-4d 68k pc=%06X $%06X <- %08X (w%d)\n",
               cur_frame, cpu.pc & 0xFFFFFF, addr, val, width);
    /* DSP mixer channel array: 4 x 24-byte entries at $42ADC (ptr, start,
     * end, volume). S_StartSound must write here for anything to sound. */
    static long chn;
    if (addr >= 0x42ADC && addr < 0x42B3C && chn++ < 80)
        printf("CHAN  f%-4d 68k pc=%06X $%06X <- %08X (w%d)\n",
               cur_frame, cpu.pc & 0xFFFFFF, addr, val, width);
    if (addr + width <= WLO || addr >= WHI) return;
    cpu_hits++;
    if (snapped && cpu_hits <= 40)
        printf("CPU   f%-4d $%06X <- %0*X  pc=%06X\n",
               cur_frame, addr, width * 2, val, cpu.pc & 0xFFFFFF);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s jagboot.rom cart.bin [frames] [pressA] [pressB]\n", argv[0]); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 900;
    int ta = argc > 4 ? atoi(argv[4]) : 560;
    int tb = argc > 5 ? atoi(argv[5]) : 620;

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

    onca_mem_init(&mem);
    memcpy(mem.rom, rom, rn); mem.rom_loaded = rn;
    onca_mem_set_cart(&mem, cart, cs);
    mem.security_bypass = 1;
    memset(&cpu, 0, sizeof(cpu));
    onca_mem_bind(&mem, &cpu.bus);
    mem.cycles = &cpu.cycles;
    onca_gpu_init(&gpu, &mem); mem.gpu = &gpu;
    onca_gpu_init(&dsp, &mem); dsp.is_dsp = 1; mem.dsp = &dsp;
    m68k_reset(&cpu);
    cpu.int_vector = 64;
    mem.cpu_pc = &cpu.pc;
    mem.watch_lo = WLO; mem.watch_hi = WHI; mem.watch_cb = watch_cb;
    mem.log = log_cb;
    cpu.trace = cpu_trace;
    mem.blit_trace = blit_cb;

    uint64_t budget = (uint64_t)(CPU_HZ / 59.94);
    uint64_t isr_acc = 0; int isr_owed = 0, launched = 0;
    long delivered = 0, last_del = 0, last_hits = 0;

    /* Scripted input: SEQ="frame:tjbit:len,frame:tjbit:len,..." overrides the
     * default A-press + cursor-move pattern (tjbit = TJ_* enum index). */
    struct { int f, bit, len; } seq[32]; int nseq = 0;
    if (getenv("SEQ")) {
        char *s = getenv("SEQ");
        while (nseq < 32 && *s) {
            seq[nseq].f = atoi(s);            s = strchr(s, ':'); if (!s) break;
            seq[nseq].bit = atoi(++s);        s = strchr(s, ':'); if (!s) break;
            seq[nseq].len = atoi(++s); nseq++; s = strchr(s, ',');
            if (!s) break; s++;
        }
        printf("SEQ: %d scripted presses\n", nseq);
    }

    for (int f = 0; f < frames; f++) {
        cur_frame = f;
        mem.joypad1 = (f >= ta && f < tb) ? (1u << TJ_A) : 0;
        if (nseq) {
            mem.joypad1 = (f >= ta && f < tb) ? (1u << TJ_A) : 0;
            for (int i = 0; i < nseq; i++)
                if (f >= seq[i].f && f < seq[i].f + seq[i].len)
                    mem.joypad1 |= 1u << seq[i].bit;
        } else if ((f >= tb + 80 && f < tb + 86) || (f >= tb + 120 && f < tb + 126) ||
                   (f >= tb + 160 && f < tb + 166))
            mem.joypad1 = 1u << TJ_DOWN;
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
                        if ((df & DF_I2SENA) && !(df & GF_IMASK) &&
                            onca_gpu_interrupt(&dsp, 1)) {
                            isr_owed--; delivered++;
                        }
                    }
                }
        }
        if (f == ta - 60 && !snapped) {
            for (uint32_t i = 0; i < WHI - WLO; i++) snap[i] = onca_peek8(&mem, WLO + i);
            snapped = 1;
            printf("--- snapshot of $%06X-$%06X at frame %d ---\n", WLO, WHI, f);
        }
        if (f == tb + 40) { g_logcalls = 30; g_logblits = 40; }  /* log once the menu is up */
        if ((f % 60) == 0) {
            /* sound pipeline vitals: ring write cursor, ring content, mix
             * accumulator, and the mix job's parameter block */
            int ring_nz = 0, acc_nz = 0;
            for (int i = 0; i < 0x2000; i++) if (onca_peek8(&mem, 0x1F0000 + i)) ring_nz++;
            for (int i = 0; i < 0x1000; i++) if (onca_peek8(&mem, 0xF1B800 + i)) acc_nz++;
            printf("f%-4d pc=%06X ring_wr_B=%ld drained_B=%ld ring_nz=%d sampcount=%08X isr_cnt=%08X mus5150C=%08X mus51B10=%08X\n",
                   f, cpu.pc & 0xFFFFFF, hits - last_hits, (delivered - last_del) * 2L,
                   ring_nz, onca_peek32(&mem, 0x42C0C), onca_peek32(&mem, 0xF1B02C),
                   onca_peek32(&mem, 0x5150C), onca_peek32(&mem, 0x51B10));
            last_hits = hits; last_del = delivered;
            for (int ch = 0; ch < 4; ch++) {
                uint32_t b = 0x42ADC + ch * 24;
                printf("      ch%d ptr=%08X start=%08X end=%08X vol=%08X x=%08X %08X\n", ch,
                       onca_peek32(&mem, b), onca_peek32(&mem, b + 4),
                       onca_peek32(&mem, b + 8), onca_peek32(&mem, b + 12),
                       onca_peek32(&mem, b + 16), onca_peek32(&mem, b + 20));
            }
        }
    }

    printf("\n=== final: pc=%06X d3=%08X poke_hits=%ld cpu_hits=%ld ===\n",
           cpu.pc & 0xFFFFFF, cpu.d[3], hits, cpu_hits);
    if (snapped) {
        int diffs = 0;
        for (uint32_t i = 0; i < WHI - WLO; i += 2) {
            uint16_t was = ((uint16_t)snap[i] << 8) | snap[i + 1];
            uint16_t now = onca_peek16(&mem, WLO + i);
            if (was != now && diffs++ < 40)
                printf("CHANGED $%06X: %04X -> %04X\n", WLO + i, was, now);
        }
        printf("%d changed words in watch window\n", diffs);
    }
    /* the drawer loop code of record */
    printf("code at $82C0-$8300 now:");
    for (uint32_t a = 0x82C0; a < 0x8300; a += 2) {
        if ((a & 0xF) == 0) printf("\n  %06X:", a);
        printf(" %04X", onca_peek16(&mem, a));
    }
    printf("\n");

    /* The jagobjs the drawer passes to $4F1C: logo first (renders), then the
     * menu items (do not). Dump their headers + first pixels for comparison. */
    {
        static const uint32_t objs[] = { 0xA5DD8, 0x90698, 0xA7E00, 0xA93F8,
                                         0xA86A8, 0x8F6A8, 0xA8BD0, 0xABB40 };
        for (unsigned i = 0; i < sizeof objs / sizeof *objs; i++) {
            uint32_t o = objs[i];
            int nz = 0;
            for (int b = 16; b < 2048 + 16; b++) if (onca_peek8(&mem, o + b)) nz++;
            printf("jagobj $%06X: w=%u h=%u flags=%04X %04X %04X %04X | data_nz(2k)=%d | %08X %08X\n",
                   o, onca_peek16(&mem, o), onca_peek16(&mem, o + 2),
                   onca_peek16(&mem, o + 4), onca_peek16(&mem, o + 6),
                   onca_peek16(&mem, o + 8), onca_peek16(&mem, o + 10), nz,
                   onca_peek32(&mem, o + 16), onca_peek32(&mem, o + 20));
        }
    }

    /* Walk the object list (as gamerun.c) to see what layers the menu shows. */
    {
        uint32_t olp = ((uint32_t)onca_peek16(&mem, 0xF00022) << 16) | onca_peek16(&mem, 0xF00020);
        printf("OLP=%06X\n", olp & 0xFFFFFF);
        uint32_t queue[64]; int qh = 0, qt = 0, nseen = 0;
        uint32_t seen[64];
        queue[qt++] = olp & 0xFFFFFF;
        while (qh < qt && nseen < 40) {
            uint32_t addr = queue[qh++];
            int dup = 0; for (int i = 0; i < nseen; i++) if (seen[i] == addr) { dup = 1; break; }
            if (dup) continue;
            seen[nseen++] = addr;
            uint64_t p0 = ((uint64_t)onca_peek32(&mem, addr) << 32) | onca_peek32(&mem, addr + 4);
            int type = (int)(p0 & 7);
            uint32_t link = (uint32_t)(((p0 >> 24) & 0x7FFFF) << 3);
            if (type == 4) { printf("  @%06X STOP\n", addr); continue; }
            if (type == 3) {
                printf("  @%06X BRANCH cc=%d ypos=%u link=%06X\n", addr,
                       (int)((p0 >> 14) & 7), (unsigned)((p0 >> 3) & 0x7FF), link);
                if (qt < 62) { queue[qt++] = addr + 8; if (link) queue[qt++] = link; }
                continue;
            }
            if (type == 2) { printf("  @%06X GPU link=%06X\n", addr, link);
                if (link && qt < 63) queue[qt++] = link; continue; }
            uint32_t ypos = (uint32_t)((p0 >> 3) & 0x7FF), height = (uint32_t)((p0 >> 14) & 0x3FF);
            uint32_t data = (uint32_t)(((p0 >> 43) & 0x1FFFFF) << 3);
            uint64_t p1 = ((uint64_t)onca_peek32(&mem, addr + 8) << 32) | onca_peek32(&mem, addr + 12);
            int depth = (int)((p1 >> 12) & 7), iwidth = (int)((p1 >> 28) & 0x3FF), dwidth = (int)((p1 >> 18) & 0x3FF);
            int pitch = (int)((p1 >> 15) & 7), trans = (int)((p1 >> 46) & 1), index = (int)((p1 >> 38) & 0x7F);
            int xpos = (int)(p1 & 0xFFF); if (xpos & 0x800) xpos -= 0x1000;
            int nzd = 0; for (int b = 0; b < 2048; b++) if (onca_peek8(&mem, data + b)) nzd++;
            printf("  @%06X %s ypos=%u h=%u xpos=%d depth=%d pitch=%d iw=%d dw=%d idx=%d trans=%d data=%06X nz2k=%d link=%06X\n",
                   addr, type == 1 ? "SCALED" : "BITMAP", ypos, height, xpos, depth, pitch,
                   iwidth, dwidth, index, trans, data, nzd, link);
            if (link && qt < 63) queue[qt++] = link;
        }
    }

    /* Occupancy of the 8bpp menu overlay buffer at $1D0000 (320 B/row): does
     * the item text ever land in it, or only the logo? */
    printf("overlay $1D0000 nonzero bytes per 8-row band:\n");
    for (int band = 0; band < 200; band += 8) {
        int nz = 0;
        for (int r = band; r < band + 8 && r < 200; r++)
            for (int x = 0; x < 320; x++)
                if (onca_peek8(&mem, 0x1D0000 + r * 320 + x)) nz++;
        printf("  rows %3d-%3d: %d\n", band, band + 7, nz);
    }

    static uint16_t fb[320 * 240];
    int drawn = onca_op_render(&mem, fb, 320, 240);
    int nz = 0; for (int i = 0; i < 320 * 240; i++) if (fb[i]) nz++;
    printf("final frame: op_drawn=%d nonzero_px=%d\n", drawn, nz);
    FILE *o = fopen("build_dbg/menu.ppm", "wb");
    if (o) {
        fprintf(o, "P6\n320 240\n255\n");
        for (int i = 0; i < 320 * 240; i++) {
            uint16_t p = fb[i];
            fputc(((p >> 11) & 0x1F) << 3, o); fputc(((p >> 5) & 0x3F) << 2, o); fputc((p & 0x1F) << 3, o);
        }
        fclose(o);
        printf("wrote build_dbg/menu.ppm\n");
    }
    return 0;
}
