/*
 * libretro_core.c - libretro entry points for Onca.
 *
 * Per frame: runs the 68000 for a field's worth of cycles with the Tom GPU and
 * Jerry DSP interleaved, pulses the video interrupt once per field, delivers
 * the DSP's audio-sample (I2S) interrupt at the exact rate (games derive their
 * master timebase from it), and composites the display list with the Object
 * Processor into an RGB565 framebuffer.
 *
 * Content: a cartridge image (.j64/.rom/.bin, boot ROM loaded from the system
 * directory as "jagboot.rom") or a dev-kit ".jag" absolute executable, which
 * loads straight into DRAM without a BIOS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "libretro.h"
#include "m68k.h"
#include "memory.h"
#include "op.h"
#include "gpu.h"
#include "bios.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Optional per-frame diagnostic logging (build with -DONCA_LOG). On Android it
 * routes to logcat (tag ONCA); elsewhere to stderr. Used to compare on-device
 * timing against the headless harness. */
#if defined(ONCA_LOG) && defined(__ANDROID__)
#include <android/log.h>
#define TLOG(...) __android_log_print(ANDROID_LOG_INFO, "ONCA", __VA_ARGS__)
#elif defined(ONCA_LOG)
#define TLOG(...) fprintf(stderr, "[ONCA] " __VA_ARGS__)
#else
#define TLOG(...) ((void)0)
#endif

/* NTSC: 68000 @ 13.295 MHz, ~60 fields/sec. */
#define ONCA_CPU_HZ    13295000u
#define ONCA_FPS       59.94
#define FB_W            320
#define FB_H            240

static retro_environment_t   env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;

static onca_mem_t g_mem;
static m68k_t      g_cpu;
static onca_gpu_t g_gpu;
static onca_gpu_t g_dsp;
static int         g_loaded;
static int         g_launched;   /* CPU has entered cartridge code (past boot) */
/* Audio-sample (I2S) interrupt timebase. Games derive their master clock from
 * the DSP audio interrupt, so its rate sets both game speed and audio pitch.
 * The real rate follows from the serial clock the game programs: Doom writes
 * SCLK=19, SMODE with word-strobe interrupts, so the I2S bit clock is
 * 26.5939 MHz / (2*(SCLK+1)) = 664.85 kHz and the interrupt fires once per
 * 16-bit word = 41553/s (20776 Hz stereo frames - Doom's DSP kernel units all
 * check out against this: counter>>3 windows x4 mix steps = 20776 samples/s).
 * Owed interrupts accrue by 68000 cycle and are delivered when the DSP can
 * take one; a refused delivery stays owed and is retried, never dropped. */
#define ONCA_AUDIO_HZ         41553u
#define ONCA_CYC_PER_SAMPLE   (ONCA_CPU_HZ / ONCA_AUDIO_HZ)
static uint64_t    g_isr_acc;    /* 68000 cycles since the last owed sample     */
static int         g_isr_owed;   /* audio interrupts accrued, not yet delivered */
static uint16_t    g_fb[FB_W * FB_H];

/* Audio output. The DSP's sample handler writes the stereo DAC latches (LTXD
 * $F1A148 / RTXD $F1A14C, per the Jerry serial-interface register list); the
 * I2S interrupt itself is the sample clock. Each time an interrupt is
 * delivered, the latches hold the pair produced by the previous handler run,
 * so we capture them right before delivery: exactly one stereo frame per
 * interrupt, at ONCA_AUDIO_HZ. A ring buffer decouples capture from the once-
 * per-video-frame drain to the frontend; when the DSP produced fewer pairs
 * than the timebase accrued (masked DSP, boot ROM, no game), the drain pads
 * with the last pair so the stream never starves. */
#define AUD_RING 8192                     /* stereo frames; power of two */
static int16_t   g_aud_ring[AUD_RING][2];
static uint32_t  g_aud_wr, g_aud_rd;      /* free-running indices            */
static uint32_t  g_aud_ticks;             /* sample ticks accrued this frame */
static int16_t   g_aud_last[2];           /* pad value: last pair sent       */

static void audio_capture_pair(void) {
    if (((g_aud_wr - g_aud_rd) & 0xFFFFFFFFu) >= AUD_RING) return;  /* full */
    g_aud_ring[g_aud_wr % AUD_RING][0] = (int16_t)(onca_peek32(&g_mem, 0xF1A148) & 0xFFFF);
    g_aud_ring[g_aud_wr % AUD_RING][1] = (int16_t)(onca_peek32(&g_mem, 0xF1A14C) & 0xFFFF);
    g_aud_wr++;
}

/* Absolute ".jag" executable (dev-kit format) loaded directly into DRAM,
 * bypassing the boot ROM: these are homebrew/demo programs meant to be dropped
 * at a fixed RAM address by a dev board and jumped to. They set up their own
 * video/OP/GPU, so they exercise the whole pipeline without a BIOS or the DRM. */
static uint8_t  *g_jag_code;     /* owned copy of the program image */
static uint32_t  g_jag_size;
static uint32_t  g_jag_load;     /* DRAM load address */
static uint32_t  g_jag_entry;    /* 68000 entry point */
static int       g_jag_mode;

#define JAG_STACK_TOP 0x1FFFF8u  /* top of 2 MB DRAM, even */

/* Recognise the ".jag" header ("JAGR" magic at 0x1C) and extract the load
 * address, entry point, and a pointer to the code payload. Returns 1 on match. */
static int jag_parse(const uint8_t *b, size_t n,
                     uint32_t *load, uint32_t *entry, const uint8_t **code, uint32_t *csize) {
    if (n < 0x2E) return 0;
    if (b[0x1C] != 'J' || b[0x1D] != 'A' || b[0x1E] != 'G' || b[0x1F] != 'R') return 0;
    uint32_t ld = ((uint32_t)b[0x22] << 24) | (b[0x23] << 16) | (b[0x24] << 8) | b[0x25];
    uint32_t sz = ((uint32_t)b[0x26] << 24) | (b[0x27] << 16) | (b[0x28] << 8) | b[0x29];
    uint32_t en = ((uint32_t)b[0x2A] << 24) | (b[0x2B] << 16) | (b[0x2C] << 8) | b[0x2D];
    if (sz > n - 0x2E) sz = (uint32_t)(n - 0x2E);
    *load = ld; *entry = en; *code = b + 0x2E; *csize = sz;
    return 1;
}

RETRO_API void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name    = "Onca";
    info->library_version = "0.0.1";
    info->valid_extensions = "rom|bin|j64|jag";
    info->need_fullpath   = false;
    info->block_extract   = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->geometry.base_width  = FB_W;
    info->geometry.base_height = FB_H;
    info->geometry.max_width   = FB_W;
    info->geometry.max_height  = FB_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = ONCA_FPS;
    info->timing.sample_rate = (double)ONCA_AUDIO_HZ;
}

RETRO_API void retro_init(void) { g_loaded = 0; }
RETRO_API void retro_deinit(void) { g_loaded = 0; }
RETRO_API void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }

static void boot_from_rom(void) {
    memset(g_mem.dram, 0, sizeof(g_mem.dram));
    /* Clear volatile hardware state too, so a reset is a clean slate. Tom holds
     * the video registers + CLUT ($F00400) and the GPU RAM; Jerry holds the
     * joypad/DSP regs. Leaving these across a reset let a previous run's palette
     * (e.g. a mis-loaded blue logo) persist. Re-seed the idle joypad bits. */
    memset(g_mem.tom, 0, sizeof(g_mem.tom));
    memset(g_mem.jerry, 0, sizeof(g_mem.jerry));
    g_mem.jerry[JERRY_JOYBUTS]     = 0x00;
    g_mem.jerry[JERRY_JOYBUTS + 1] = 0x1F;
    g_mem.video_irq = 0;
    g_mem.overlay = 1;
    memset(&g_cpu, 0, sizeof(g_cpu));
    onca_mem_bind(&g_mem, &g_cpu.bus);
    g_mem.cycles = &g_cpu.cycles;
    onca_gpu_init(&g_gpu, &g_mem);
    g_mem.gpu = &g_gpu;
    /* Jerry DSP: same RISC core, DSP mode. Games gate startup on a 68000<->DSP
     * handshake (and use it for sound), so it must run alongside the GPU. */
    onca_gpu_init(&g_dsp, &g_mem);
    g_dsp.is_dsp = 1;
    g_mem.dsp = &g_dsp;
    m68k_reset(&g_cpu);
    /* TOM is a vectored interrupt source: the video/GPU interrupt uses 68000
     * vector 64 ($100), which the boot installs, not the level-2 autovector. */
    g_cpu.int_vector = 64;
    g_mem.cpu_pc = &g_cpu.pc;   /* lets the security bypass scope to the OS check */
    g_launched = 0;
    g_isr_acc = 0;
    g_isr_owed = 0;
    g_aud_wr = g_aud_rd = 0;
    g_aud_ticks = 0;
    g_aud_last[0] = g_aud_last[1] = 0;

    if (g_jag_mode && g_jag_code) {
        /* Drop the program image into DRAM and jump straight to it, as a dev
         * board would. m68k_reset already set S=1/IPL=7; just point PC/SP. */
        uint32_t load = g_jag_load & 0x1FFFFF;
        uint32_t n = g_jag_size;
        if (load + n > sizeof(g_mem.dram)) n = (uint32_t)sizeof(g_mem.dram) - load;
        g_mem.overlay = 0;   /* no boot ROM: DRAM directly visible at low addresses */
        memcpy(g_mem.dram + load, g_jag_code, n);
        g_cpu.pc  = g_jag_entry & 0xFFFFFF;
        g_cpu.a[7] = JAG_STACK_TOP;
        g_cpu.isp  = JAG_STACK_TOP;
        fprintf(stderr, "[jaguar] .jag loaded at $%06X, entry $%06X (%u bytes)\n",
                load, g_cpu.pc, n);
    }
}

RETRO_API void retro_reset(void) { if (g_loaded) boot_from_rom(); }

static uint8_t *g_cart;      /* cartridge image (owned) */

/* Load the boot ROM from the frontend's system directory. Games are booted the
 * real way - through the boot ROM - so the cartridge is validated and started
 * by the Jaguar's own code (running on our 68000 + GPU). */
static bool load_bios_from_system(onca_bios_info_t *info) {
    const char *sysdir = NULL;
    if (!env_cb || !env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir)
        return false;
    static const char *names[] = { "jagboot.rom", "jagboot.bin", "jaguar.rom",
                                   "[BIOS] Atari Jaguar (World).j64", NULL };
    char path[1200];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s%c%s", sysdir,
                 (sysdir[0] && sysdir[strlen(sysdir) - 1] == '/') ? '\0' : '/', names[i]);
        if (onca_bios_load_file(path, &g_mem, info) == 0 && info->size == ONCA_ROM_SIZE) {
            fprintf(stderr, "[jaguar] boot ROM from system dir: %s\n", path);
            return true;
        }
    }
    return false;
}

/* Distinguish the boot ROM itself (loaded as content) from a cartridge. */
static int content_is_bios(const struct retro_game_info *game) {
    return game->size <= ONCA_ROM_SIZE;
}

/* Tell the frontend which RetroPad buttons this core uses (labels +, on some
 * frontends, a prerequisite for routing input). */
static void set_input_descriptors(void) {
    static const struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A (fire)" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B (use)" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "C (strafe)" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Pause" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Option" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "* (strafe L)" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "# (strafe R)" },
        { 0 }
    };
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    set_input_descriptors();

    if (!game || (!game->data && !game->path)) return false;

    onca_mem_init(&g_mem);
    if (g_cart) { free(g_cart); g_cart = NULL; }
    if (g_jag_code) { free(g_jag_code); g_jag_code = NULL; }
    g_jag_mode = 0;

    onca_bios_info_t info;

    /* Absolute ".jag" dev-kit executable: load into DRAM and run it directly,
     * no boot ROM required. Read the header from data or file. */
    {
        uint8_t hdr[0x2E];
        const uint8_t *scan = NULL;
        size_t scan_n = 0;
        uint8_t *filebuf = NULL;
        if (game->data && game->size >= sizeof(hdr)) {
            scan = (const uint8_t *)game->data; scan_n = game->size;
        } else if (game->path) {
            FILE *f = fopen(game->path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                if (sz > 0) { filebuf = (uint8_t *)malloc(sz);
                    if (filebuf && fread(filebuf, 1, sz, f) == (size_t)sz) { scan = filebuf; scan_n = sz; } }
                fclose(f);
            }
        }
        uint32_t load, entry, csize; const uint8_t *code;
        if (scan && jag_parse(scan, scan_n, &load, &entry, &code, &csize)) {
            g_jag_code = (uint8_t *)malloc(csize ? csize : 1);
            if (g_jag_code) {
                memcpy(g_jag_code, code, csize);
                g_jag_size = csize; g_jag_load = load; g_jag_entry = entry; g_jag_mode = 1;
                fprintf(stderr, "[jaguar] .jag executable: load=$%06X entry=$%06X size=%u\n",
                        load, entry, csize);
            }
            if (filebuf) free(filebuf);
            boot_from_rom();
            g_loaded = 1;
            return true;
        }
        if (filebuf) free(filebuf);
    }

    if (game->size && content_is_bios(game)) {
        /* Boot ROM loaded directly as content (no cartridge). */
        if (game->data) {
            memset(g_mem.rom, 0, ONCA_ROM_SIZE);
            size_t n = game->size < ONCA_ROM_SIZE ? game->size : ONCA_ROM_SIZE;
            memcpy(g_mem.rom, game->data, n);
            g_mem.rom_loaded = n;
            onca_bios_identify(g_mem.rom, game->size, &info);
        } else if (onca_bios_load_file(game->path, &g_mem, &info) != 0) {
            return false;
        }
        fprintf(stderr, "[jaguar] boot ROM crc32=%08X %s (no cartridge)\n",
                info.crc32, info.known ? info.desc->name : "(unrecognised)");
    } else {
        /* Cartridge: needs the boot ROM from the system directory. */
        if (!load_bios_from_system(&info)) {
            fprintf(stderr, "[jaguar] ERROR: cartridge loaded but no boot ROM found.\n"
                            "         Put jagboot.rom (128 KB) in RetroArch's system directory.\n");
            return false;
        }
        size_t csize = game->size ? game->size : 0;
        if (csize > ONCA_CART_MAX) csize = ONCA_CART_MAX;
        g_cart = (uint8_t *)malloc(csize ? csize : 1);
        if (!g_cart) return false;
        if (game->data) {
            memcpy(g_cart, game->data, csize);
        } else {
            FILE *f = fopen(game->path, "rb");
            if (!f) { free(g_cart); g_cart = NULL; return false; }
            csize = fread(g_cart, 1, csize ? csize : ONCA_CART_MAX, f);
            fclose(f);
        }
        onca_mem_set_cart(&g_mem, g_cart, csize);
        g_mem.security_bypass = 1;   /* let legitimately loaded carts past the DRM check */
        fprintf(stderr, "[jaguar] cartridge %zu bytes mapped at $800000\n", csize);
    }

    boot_from_rom();
    g_loaded = 1;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) {
    (void)t; (void)i; (void)n; return false;
}
RETRO_API void retro_unload_game(void) {
    g_loaded = 0;
    if (g_cart) { free(g_cart); g_cart = NULL; }
    if (g_jag_code) { free(g_jag_code); g_jag_code = NULL; }
    g_jag_mode = 0;
}

/* Map the libretro RetroPad to the Jaguar pad-1 button matrix. */
static void poll_input(void) {
    if (!input_state_cb) { g_mem.joypad1 = 0; return; }
    uint32_t p = 0;
    #define RP(id) input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, (id))
    if (RP(RETRO_DEVICE_ID_JOYPAD_UP))     p |= 1u << TJ_UP;
    if (RP(RETRO_DEVICE_ID_JOYPAD_DOWN))   p |= 1u << TJ_DOWN;
    if (RP(RETRO_DEVICE_ID_JOYPAD_LEFT))   p |= 1u << TJ_LEFT;
    if (RP(RETRO_DEVICE_ID_JOYPAD_RIGHT))  p |= 1u << TJ_RIGHT;
    if (RP(RETRO_DEVICE_ID_JOYPAD_B))      p |= 1u << TJ_A;      /* fire   */
    if (RP(RETRO_DEVICE_ID_JOYPAD_A))      p |= 1u << TJ_B;      /* use    */
    if (RP(RETRO_DEVICE_ID_JOYPAD_Y))      p |= 1u << TJ_C;      /* strafe */
    if (RP(RETRO_DEVICE_ID_JOYPAD_START))  p |= 1u << TJ_PAUSE;
    if (RP(RETRO_DEVICE_ID_JOYPAD_SELECT)) p |= 1u << TJ_OPTION;
    if (RP(RETRO_DEVICE_ID_JOYPAD_L))      p |= 1u << TJ_STAR;   /* Doom: strafe L */
    if (RP(RETRO_DEVICE_ID_JOYPAD_R))      p |= 1u << TJ_HASH;   /* Doom: strafe R */
    #undef RP
    g_mem.joypad1 = p;
}

RETRO_API void retro_run(void) {
    if (input_poll_cb) input_poll_cb();
    poll_input();

    if (g_loaded) {
        /* VBLANK raises the video-interrupt latch; the boot's handler clears it
         * by writing INT1, so exactly one interrupt fires per field (no storm).
         * Step so the per-instruction IRQ level tracks the latch. */
        g_mem.video_irq = 1;
        uint64_t budget = (uint64_t)(ONCA_CPU_HZ / ONCA_FPS);
        uint64_t target = g_cpu.cycles + budget;
        uint64_t prev_cyc = g_cpu.cycles;
        while (g_cpu.cycles < target && !g_cpu.halted) {
            m68k_set_irq(&g_cpu, g_mem.video_irq ? 2 : 0);
            m68k_step(&g_cpu);
            /* Latch once the CPU runs cartridge code: the audio-interrupt timebase
             * is only delivered to the game's DSP program, never the boot ROM's
             * (whose DSP has no sample handler at $F1B010). */
            if (!g_launched && g_cpu.pc >= ONCA_CART_BASE && g_cpu.pc < ONCA_CART_END)
                g_launched = 1;
            /* Accrue audio-sample interrupts at the exact rate (by 68000 cycle). */
            g_isr_acc += g_cpu.cycles - prev_cyc;
            prev_cyc = g_cpu.cycles;
            while (g_isr_acc >= ONCA_CYC_PER_SAMPLE) { g_isr_acc -= ONCA_CYC_PER_SAMPLE; if (g_isr_owed < 64) g_isr_owed++; g_aud_ticks++; }
            /* Step the GPU (Tom RISC, ~2x the 68000 clock) concurrently: the boot
             * and games kick the GPU mid-frame and busy-wait on it (and on the
             * Blitter it drives), so it has to make progress during the frame,
             * not only after it. */
            /* (Pacing experiment reverted: slowing the render to ~20fps only
             * added chop - movement speed was unchanged, which localizes the
             * "flying" bug to the DSP physics jobs (momentum/friction), not
             * the frame rate. Turning is 68k-side and feels right; friction
             * is a DSP fixed-point multiply. See memory notes.) */
            if (g_gpu.running)
                for (int k = 0; k < 16 && g_gpu.running; k++) onca_gpu_step(&g_gpu);
            if (g_dsp.running)
                for (int k = 0; k < 16 && g_dsp.running; k++) {
                    onca_gpu_step(&g_dsp);
                    /* Deliver an owed audio interrupt when the DSP can take one. */
                    if (g_launched && g_isr_owed > 0) {
                        uint32_t df = onca_gpu_read_ctrl(&g_dsp, 0xF1A100);
                        if ((df & DF_I2SENA) && !(df & GF_IMASK) &&
                            onca_gpu_interrupt(&g_dsp, 1)) {
                            audio_capture_pair();
                            g_isr_owed--;
                        }
                    }
                }
        }
        onca_op_render(&g_mem, g_fb, FB_W, FB_H);
#ifdef ONCA_LOG
        {
            static unsigned frame = 0; frame++;
            if (frame <= 240) {
                uint32_t olp = ((uint32_t)onca_peek16(&g_mem, 0xF00022) << 16)
                             | onca_peek16(&g_mem, 0xF00020);
                int nz_wm = 0, nz_r = 0, nz_fb = 0;
                for (uint32_t i = 0; i < 0x8000; i++) {
                    if (onca_peek8(&g_mem, 0x100000 + i)) nz_wm++;
                    if (onca_peek8(&g_mem, 0x15D000 + i)) nz_r++;
                }
                for (int i = 0; i < FB_W * FB_H; i++) if (g_fb[i]) nz_fb++;
                TLOG("fr=%u olp=%06X clut1=%04X wm@100000=%d r@15D000=%d fbpx=%d "
                     "gtblX=%04X,%04X,%04X state=%d",
                     frame, olp & 0xFFFFFF, onca_peek16(&g_mem, 0xF00402),
                     nz_wm, nz_r, nz_fb,
                     onca_peek16(&g_mem, 0x372CC), onca_peek16(&g_mem, 0x372D4),
                     onca_peek16(&g_mem, 0x372DC), onca_peek16(&g_mem, 0x371B2));
            }
        }
#endif
    } else {
        memset(g_fb, 0, sizeof(g_fb));
    }

#ifdef ONCA_DIAG
    /* Diagnostic build: paint a fixed colour test pattern regardless of what
     * the emulation produced, to verify frames actually reach the screen on a
     * given device. If this shows, the video path works and any other colours
     * were a frontend artifact; if it does not, RetroArch is not displaying the
     * core's framebuffer. */
    {
        static unsigned tick;
        tick++;
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < FB_W; x++) {
                int r = (x >> 4) & 0x1F, g = (y >> 3) & 0x3F, b = ((x + tick) >> 4) & 0x1F;
                g_fb[y * FB_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
    }
#endif

    if (video_cb) video_cb(g_fb, FB_W, FB_H, FB_W * sizeof(uint16_t));
    /* Drain captured DSP samples; emit exactly as many stereo frames as sample
     * ticks accrued this video frame so the audio stream tracks the emulated
     * timebase (the frontend's dynamic rate control absorbs the residue). */
    if (audio_batch_cb) {
        static int16_t out[2048 * 2];
        uint32_t want = g_aud_ticks;
        if (want > 2048) want = 2048;
        if (want == 0) want = ONCA_AUDIO_HZ / 60;   /* nothing accrued: keep cadence */
        uint32_t have = g_aud_wr - g_aud_rd;
        for (uint32_t i = 0; i < want; i++) {
            if (i < have) {
                g_aud_last[0] = g_aud_ring[g_aud_rd % AUD_RING][0];
                g_aud_last[1] = g_aud_ring[g_aud_rd % AUD_RING][1];
                g_aud_rd++;
            }
            out[i * 2 + 0] = g_aud_last[0];
            out[i * 2 + 1] = g_aud_last[1];
        }
        audio_batch_cb(out, want);
    }
    g_aud_ticks = 0;
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
RETRO_API unsigned retro_get_region(void) { return 0; /* NTSC */ }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
