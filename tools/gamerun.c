/*
 * gamerun.c - headless real-boot cart runner + post-launch OP inspector.
 *
 * Boots the real jagboot.rom with a cartridge mapped at $800000 (security
 * bypass on, like the shipping core), runs N frames, then walks the OLP object
 * list and reports each object: type, position, bitmap-data pointer, and whether
 * the buffer it points at actually holds pixel data. This is how we find why an
 * in-game frame is black - either the OP list is wrong or the buffers are empty.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include "../src/bios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 320
#define FB_H 240
#define CPU_HZ 13295000u
#define FPS 60

static m68k_t cpu;
static onca_mem_t mem;
static onca_gpu_t gpu;
static onca_gpu_t dsp;
static uint16_t fb[FB_W * FB_H];

static unsigned long g_dsp_ops[64];
static unsigned long g_dsp_steps;
static void dsp_trace(void *ctx, uint32_t pc, uint16_t op) {
    (void)ctx; (void)pc; g_dsp_ops[(op >> 10) & 0x3F]++;
}

static int buf_nonzero(uint32_t addr, int bytes) {
    int nz = 0;
    for (int i = 0; i < bytes; i++) if (onca_peek8(&mem, addr + i)) nz++;
    return nz;
}

/* Blit trace: count blits and bucket their A1 (destination) base. */
static unsigned long g_nblits;
static struct { uint32_t a1; unsigned long n; } g_bucket[64];
static int g_nbucket;
static void blit_cb(void *ctx, uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t count) {
    (void)ctx; (void)cmd; (void)a2; (void)count;
    g_nblits++;
    uint32_t base = a1 & 0xFFFF000;   /* coarse bucket */
    for (int i = 0; i < g_nbucket; i++) if (g_bucket[i].a1 == base) { g_bucket[i].n++; return; }
    if (g_nbucket < 64) { g_bucket[g_nbucket].a1 = base; g_bucket[g_nbucket].n = 1; g_nbucket++; }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s jagboot.rom cart.bin [frames] [ppm]\n", argv[0]); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 600;
    const char *ppm = argc > 4 ? argv[4] : NULL;

    /* load boot ROM */
    FILE *fr = fopen(argv[1], "rb");
    if (!fr) { perror("bios"); return 1; }
    static uint8_t rom[ONCA_ROM_SIZE];
    size_t rn = fread(rom, 1, ONCA_ROM_SIZE, fr); fclose(fr);

    /* load cart */
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
    onca_gpu_init(&gpu, &mem);
    mem.gpu = &gpu;
    onca_gpu_init(&dsp, &mem);
    dsp.is_dsp = 1;
    dsp.trace = dsp_trace;
    mem.dsp = &dsp;
    m68k_reset(&cpu);
    cpu.int_vector = 64;
    mem.cpu_pc = &cpu.pc;
    mem.blit_trace = blit_cb;

    uint64_t budget = CPU_HZ / FPS;
    int launched_at = -1;
    uint32_t last_olp = 0xFFFFFFFF;
    /* Optional input injection: JOYHOLD=<TJ bitmask> held for the last 60 frames,
     * to confirm the game responds to the controller. */
    uint32_t joyhold = getenv("JOYHOLD") ? (uint32_t)strtoul(getenv("JOYHOLD"), 0, 0) : 0;
    /* JOYTAP: press JOYHOLD only during frames [JOYA, JOYB), else hold whole tail. */
    int ta = getenv("JOYA") ? atoi(getenv("JOYA")) : (frames - 60);
    int tb = getenv("JOYB") ? atoi(getenv("JOYB")) : frames;
    for (int f = 0; f < frames; f++) {
        mem.joypad1 = (f >= ta && f < tb) ? joyhold : 0;
        mem.video_irq = 1;
        uint64_t target = cpu.cycles + budget;
        while (cpu.cycles < target && !cpu.halted) {
            m68k_set_irq(&cpu, mem.video_irq ? 2 : 0);
            m68k_step(&cpu);
            if (gpu.running)
                for (int k = 0; k < 16 && gpu.running; k++) onca_gpu_step(&gpu);
            if (dsp.running)
                for (int k = 0; k < 16 && dsp.running; k++) { onca_gpu_step(&dsp); g_dsp_steps++; }
        }
        /* note when the CPU first executes cart code (>= $800000) */
        if (launched_at < 0 && cpu.pc >= 0x800000 && cpu.pc < 0xE00000) launched_at = f;
        uint32_t olp = ((uint32_t)onca_peek16(&mem, 0xF00022) << 16) | onca_peek16(&mem, 0xF00020);
        if ((f % 60) == 0 || olp != last_olp) {
            /* coarse DRAM activity: nonzero KB in 64KB windows across low DRAM */
            int busy = 0; for (uint32_t a = 0x10000; a < 0x200000; a += 0x10000) {
                int nz = 0; for (uint32_t i = 0; i < 0x10000; i += 64) if (onca_peek32(&mem, a+i)) nz++;
                if (nz > 200) busy++;
            }
            printf("f%4d pc=%06X olp=%06X vmode=%04X vdb=%04X gpu=%d dsp=%d busyKwin=%d\n",
                   f, cpu.pc & 0xFFFFFF, olp & 0xFFFFFF,
                   onca_peek16(&mem, 0xF00028), onca_peek16(&mem, 0xF00046),
                   gpu.running, dsp.running, busy);
            last_olp = olp;
        }
    }

    int drawn = onca_op_render(&mem, fb, FB_W, FB_H);
    int nz = 0; unsigned long hash = 1469598103u;
    for (int i = 0; i < FB_W*FB_H; i++) { if (fb[i]) nz++; hash = (hash ^ fb[i]) * 16777619u; }
    printf("[joyhold=%08X fb_hash=%08lX]\n", joyhold, hash & 0xFFFFFFFF);
    printf("\n=== blits: %lu total, dest buckets ===\n", g_nblits);
    for (int i = 0; i < g_nbucket; i++)
        printf("  A1~%07X : %lu\n", g_bucket[i].a1, g_bucket[i].n);
    uint32_t olp = ((uint32_t)onca_peek16(&mem, 0xF00022) << 16) | onca_peek16(&mem, 0xF00020);
    printf("\n=== after %d frames ===\n", frames);
    printf("launched(cart pc) at frame %d; final pc=%06X\n", launched_at, cpu.pc & 0xFFFFFF);
    printf("DSP: running=%d steps=%lu D_PC=%06X mailbox $F1B034=%08X\n",
           dsp.running, g_dsp_steps, dsp.pc & 0xFFFFFF, onca_peek32(&mem, 0xF1B034));
    printf("DSP opcode histogram: ");
    for (int i = 0; i < 64; i++) if (g_dsp_ops[i]) printf("op%d=%lu ", i, g_dsp_ops[i]);
    printf("\n");
    printf("OLP=%06X VMODE=%04X VDB=%04X op_drawn=%d fb_nonzero=%d\n",
           olp & 0xFFFFFF, onca_peek16(&mem, 0xF00028), onca_peek16(&mem, 0xF00046), drawn, nz);

    /* Explore ALL reachable objects (branches fork into fall-through addr+8 AND
     * link; bitmaps follow link). Report each distinct object + its buffer fill. */
    printf("--- object list walk (all reachable) ---\n");
    uint32_t queue[256]; int qh = 0, qt = 0;
    uint32_t seen[256]; int nseen = 0;
    queue[qt++] = olp & 0xFFFFFF;
    while (qh < qt && nseen < 200) {
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
                   (int)((p0>>14)&7), (unsigned)((p0>>3)&0x7FF), link);
            if (qt < 254) { queue[qt++] = addr + 8; if (link) queue[qt++] = link; }
            continue;
        }
        if (type == 2) { printf("  @%06X GPU link=%06X\n", addr, link);
            if (link && qt < 255) queue[qt++] = link; continue; }
        uint32_t ypos = (uint32_t)((p0>>3)&0x7FF), height=(uint32_t)((p0>>14)&0x3FF);
        uint32_t data = (uint32_t)(((p0>>43)&0x1FFFFF)<<3);
        uint64_t p1 = ((uint64_t)onca_peek32(&mem, addr+8) << 32) | onca_peek32(&mem, addr+12);
        int depth=(int)((p1>>12)&7), iwidth=(int)((p1>>28)&0x3FF), dwidth=(int)((p1>>18)&0x3FF);
        int xpos=(int)(p1&0xFFF); if(xpos&0x800) xpos-=0x1000;
        int nz_data = buf_nonzero(data, 2048);
        printf("  @%06X %s ypos=%u h=%u xpos=%d depth=%d iw=%d dw=%d data=%06X buf_nz(2k)=%d link=%06X\n",
               addr, type==1?"SCALED":"BITMAP", ypos, height, xpos, depth, iwidth, dwidth,
               data, nz_data, link);
        if (link && qt < 255) queue[qt++] = link;
    }

    if (ppm) {
        FILE *o = fopen(ppm, "wb");
        if (o) { fprintf(o, "P6\n%d %d\n255\n", FB_W, FB_H);
            for (int i=0;i<FB_W*FB_H;i++){ uint16_t p=fb[i];
                fputc(((p>>11)&0x1F)<<3,o); fputc(((p>>5)&0x3F)<<2,o); fputc((p&0x1F)<<3,o);} fclose(o);
            printf("wrote %s\n", ppm); }
    }
    return 0;
}
