/*
 * op.c - Atari Jaguar Object Processor (Tom) compositor.
 *
 * Per-scanline compositor: for each displayed line it walks the object list at
 * OLP, evaluating BRANCH conditions against the vertical counter (a non-taken
 * branch falls through to the next phrase, a taken one follows LINK) and drawing
 * one line of each in-range BITMAP / SCALED BITMAP. Type-2 GPU objects
 * deliver GPU interrupt 3 (the mid-screen mode-switch service), STOP ends the
 * list. Supports CLUT-indexed depths (1/2/4/8 bpp) and direct
 * 16 bpp (CRY or RGB16); 24 bpp is decoded as RGB888.
 *
 * Clean-room from the public Tom & Jerry Technical Reference Manual.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op.h"
#include "gpu.h"   /* type-2 objects deliver GPU interrupt 3 (OP source) */

/* Tom register byte addresses. */
#define R_OLP_LO 0xF00020
#define R_OLP_HI 0xF00022
#define R_VMODE  0xF00028
#define R_VDB    0xF00046
#define R_BG     0xF00058
#define R_CLUT   0xF00400

/* CRY -> RGB565. A 16-bit CRY pixel is [red:4][cyan:4][Y:8] - the upper byte is
 * chroma (red nibble 15-12, cyan nibble 11-8), the low byte is 8-bit intensity.
 * The exact hardware uses a documented 16x16 chroma table scaled by Y; this is a
 * faithful approximation with the correct hue orientation (validated against the
 * dev-kit CRY test images - the scaled jaguar renders tan, not blue). Swapping in
 * the exact table later is a change confined to this function. */
/* Exact CRY chroma tables from the Tech Ref ("Physical Implementation"):
 * the colour byte indexes 16x16 modifier tables for R/G/B, each multiplied
 * by the 8-bit intensity. BLUE is RED mirrored vertically and GREEN is
 * symmetric about its middle rows, so only RED (16 rows) and half of GREEN
 * are stored. Row = red nibble (bits 15-12), column = cyan nibble (11-8). */
static const uint8_t cry_red_tab[16][16] = {
    {   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    {  34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 19,  0 },
    {  68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 64, 43, 21,  0 },
    { 102,102,102,102,102,102,102,102,102,102,102, 95, 71, 47, 23,  0 },
    { 135,135,135,135,135,135,135,135,135,135,130,104, 78, 52, 26,  0 },
    { 169,169,169,169,169,169,169,169,169,170,141,113, 85, 56, 28,  0 },
    { 203,203,203,203,203,203,203,203,203,183,153,122, 91, 61, 30,  0 },
    { 237,237,237,237,237,237,237,237,230,197,164,131, 98, 65, 32,  0 },
    { 255,255,255,255,255,255,255,255,247,214,181,148,115, 82, 49, 17 },
    { 255,255,255,255,255,255,255,255,255,235,204,173,143,112, 81, 51 },
    { 255,255,255,255,255,255,255,255,255,255,227,198,170,141,113, 85 },
    { 255,255,255,255,255,255,255,255,255,255,249,223,197,171,145,119 },
    { 255,255,255,255,255,255,255,255,255,255,255,248,224,200,177,153 },
    { 255,255,255,255,255,255,255,255,255,255,255,255,252,230,208,187 },
    { 255,255,255,255,255,255,255,255,255,255,255,255,255,255,240,221 },
    { 255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255 },
};
static const uint8_t cry_grn_tab[8][16] = {
    { 0,17,34,51,68, 85,102,119,136,153,170,187,204,221,238,255 },
    { 0,19,38,57,77, 96,115,134,154,173,192,211,231,250,255,255 },
    { 0,21,43,64,86,107,129,150,172,193,215,236,255,255,255,255 },
    { 0,23,47,71,95,119,142,166,190,214,238,255,255,255,255,255 },
    { 0,26,52,78,104,130,156,182,208,234,255,255,255,255,255,255 },
    { 0,28,56,85,113,141,170,198,226,255,255,255,255,255,255,255 },
    { 0,30,61,91,122,153,183,214,244,255,255,255,255,255,255,255 },
    { 0,32,65,98,131,164,197,230,255,255,255,255,255,255,255,255 },
};
static uint16_t cry_to_rgb565(uint16_t v) {
    int y   = v & 0xFF;             /* intensity 0..255           */
    int red = (v >> 12) & 0xF;      /* chroma: red   nibble 15-12 */
    int cyn = (v >> 8)  & 0xF;      /* chroma: cyan  nibble 11-8  */
    int r = cry_red_tab[red][cyn];
    int g = cry_grn_tab[red < 8 ? red : 15 - red][cyn];
    int b = cry_red_tab[15 - red][cyn];
    r = r * y / 255; g = g * y / 255; b = b * y / 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t onca_op_decode16(uint16_t v, int cry) {
    return cry ? cry_to_rgb565(v) : v;   /* RGB16 path: already RGB565 */
}

/* Fetch one `bpp`-bit pixel from a big-endian bitmap line. */
static uint32_t fetch_pixel(onca_mem_t *m, uint32_t line_addr, int col, int bpp) {
    switch (bpp) {
    case 16: return onca_peek16(m, line_addr + col * 2);
    case 8:  return onca_peek8 (m, line_addr + col);
    case 4:  { uint8_t byte = onca_peek8(m, line_addr + col / 2);
               return (col & 1) ? (byte & 0xF) : (byte >> 4); }
    case 2:  { uint8_t byte = onca_peek8(m, line_addr + col / 4);
               int sh = 6 - 2 * (col & 3); return (byte >> sh) & 0x3; }
    case 1:  { uint8_t byte = onca_peek8(m, line_addr + col / 8);
               int sh = 7 - (col & 7); return (byte >> sh) & 0x1; }
    default: return 0;
    }
}

/* Draw one displayed scanline of a bitmap/scaled object. `screen_y` is the
 * output row; `oy` is the object-relative output row (0..out_h). All object
 * fields are pre-decoded by the caller. */
static void draw_object_line(onca_mem_t *m, uint16_t *fb, int w,
                             int screen_y, int oy, int xpos, int depth, int bpp,
                             int dwidth, int index, int trans, int out_w,
                             int hscale, int vscale, uint32_t data, int cry,
                             int xscale) {
    int sy = oy * 32 / vscale;
    uint32_t line_addr = data + (uint32_t)sy * (uint32_t)dwidth * 8;
    for (int ox = 0; ox < out_w; ox++) {
        int sx = ox * 32 / hscale;
        /* Object XPOS is a raster position: games centre their content in the
         * wider overscan raster (Doom: bar at x=14, view at x=7 doubled = 14),
         * and TVs crop the margins. Map the content window onto the canvas by
         * subtracting the display origin, or the right edge falls off. */
        int screen_x = (xpos + ox) * xscale - 14;
        if (screen_x < 0 || screen_x + xscale - 1 >= w) continue;
        uint32_t pv = fetch_pixel(m, line_addr, sx, bpp);
        uint16_t color;
        if (depth <= 3) {                       /* CLUT-indexed */
            if (trans && pv == 0) continue;     /* colour 0 transparent iff TRANS */
            /* Palette address (Tech Ref, OP INDEX field): for 1-4 bpp the TOP
             * (8 - bpp) bits of the 7-bit INDEX supply the high bits of the
             * 8-bit CLUT address above the pixel value; 8 bpp indexes the CLUT
             * with the pixel directly. */
            uint32_t ci = (bpp < 8)
                        ? ((((uint32_t)index >> (bpp - 1)) << bpp) | pv) & 0xFF
                        : (pv & 0xFF);
            color = onca_op_decode16(onca_peek16(m, R_CLUT + ci * 2), cry);
        } else if (depth == 4) {                /* 16 bpp direct */
            if (trans && pv == 0) continue;
            color = onca_op_decode16((uint16_t)pv, cry);
        } else {                                /* 24 bpp RGB888 */
            uint32_t hi = onca_peek16(m, line_addr + sx * 3);
            uint8_t  b8 = onca_peek8 (m, line_addr + sx * 3 + 2);
            int r = (hi >> 8) & 0xFF, g = hi & 0xFF;
            color = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b8 >> 3));
        }
        for (int k = 0; k < xscale; k++)
            fb[screen_y * w + screen_x + k] = color;
    }
}

/* Cached mid-screen mode-switch layout (see the VMODE comment in the render
 * loop): row where the type-2 switch last fired, and the modes either side. */
static int s_band_row = -1;
static uint16_t s_vmode_above, s_vmode_below;
static int s_fired_this_frame;
static int s_saw_gpuobj;   /* any type-2 object in this frame's list? */

int onca_op_render(onca_mem_t *m, uint16_t *fb, int w, int h) {
    uint16_t vmode = onca_peek16(m, R_VMODE);
    uint16_t vmode_entry = vmode;
    s_fired_this_frame = 0;
    s_saw_gpuobj = 0;
    int cry = (vmode & 0x0002) == 0;          /* VMODE bit1 clear = CRY */
    int vdb = onca_peek16(m, R_VDB);          /* display top, half-lines */

    /* Clear to background colour. */
    uint16_t bg = onca_op_decode16(onca_peek16(m, R_BG), cry);
    for (int i = 0; i < w * h; i++) fb[i] = bg;

    uint32_t olp = ((uint32_t)onca_peek16(m, R_OLP_HI) << 16) | onca_peek16(m, R_OLP_LO);
    olp &= 0xFFFFFF;

    /* The Object Processor is a per-scanline state machine: for every displayed
     * line it walks the list from OLP, evaluating BRANCH conditions against the
     * vertical counter (VC) and drawing one line of each in-range bitmap. This
     * mirrors real hardware and is required for the canonical license-screen list
     * (BRANCH-past / BRANCH-prior / BITMAP / STOP) and for game double-buffering.
     *
     * A BRANCH that is NOT taken falls through to the next phrase (addr + 8); a
     * taken BRANCH follows LINK. VC is in half-lines, the same units as YPOS. */
    int drawn = 0;
    int xscale = 1;
    for (int screen_y = 0; screen_y < h; screen_y++) {
        int vc = vdb + screen_y * 2;               /* vertical counter, half-lines */

        /* Real hardware walks the list every HALF-line; we display whole lines
         * (odd vc only). Type-2 GPU objects can sit on the even half-lines in
         * between (Doom's view/bar boundary is at even ypos 410), so walk each
         * row twice: pass 0 = the preceding even half-line, branches and GPU
         * objects only (fires the mid-screen VMODE switch); pass 1 = the
         * displayed line, full drawing. */
        for (int sub = 0; sub < 2; sub++) {
        int hv = vc - 1 + sub;
        uint32_t addr = olp;

        if (sub == 1) {
            /* VMODE may have changed mid-screen (type-2 handler). PWIDTH
             * (bits 9-11, +1 = video clocks per pixel) against the 4-clock
             * 320-pixel baseline gives the horizontal doubling factor.
             *
             * The switch depends on delivering a GPU interrupt to the live
             * GPU, which is not interruptible on every frame (mid-delay-slot,
             * masked, or stopped when we composite). Cache the band layout
             * from the last frame where the switch fired so a busy GPU can't
             * make the status bar blink between scales. */
            uint16_t vmode_row = onca_peek16(m, R_VMODE);
            if (s_band_row >= 0) {
                if (!s_fired_this_frame)
                    vmode_row = (screen_y >= s_band_row) ? s_vmode_below : s_vmode_above;
                else if (screen_y < s_band_row)
                    vmode_row = s_vmode_above;
            }
            cry = (vmode_row & 0x0002) == 0;
            xscale = (((vmode_row >> 9) & 7) + 1) / 4;
            if (xscale < 1) xscale = 1; else if (xscale > 2) xscale = 2;
        }

        for (int guard = 0; guard < 512; guard++) {
            uint64_t p0 = ((uint64_t)onca_peek32(m, addr) << 32) | onca_peek32(m, addr + 4);
            int type = (int)(p0 & 7);
            uint32_t link = (uint32_t)(((p0 >> 24) & 0x7FFFF) << 3);

            if (type == OP_OBJ_STOP) break;

            if (type == OP_OBJ_BRANCH) {
                int cc = (int)((p0 >> 14) & 7);
                uint32_t bypos = (uint32_t)((p0 >> 3) & 0x7FF);
                int taken;
                switch (cc) {   /* Tom Tech Ref: branch condition compares YPOS to VC */
                case 0:  taken = (hv == (int)bypos) || (bypos == 0x7FF); break; /* YPOS==VC / last */
                case 1:  taken = ((int)bypos > hv);  break;   /* branch if YPOS > VC */
                case 2:  taken = ((int)bypos < hv);  break;   /* branch if YPOS < VC */
                default: taken = 0;                  break;   /* OP-flag / halfline: n/a */
                }
                if (taken) {
                    if (!link || link == addr) break;
                    addr = link;
                } else {
                    addr += 8;                         /* fall through to next phrase */
                }
                continue;
            }

            if (type == OP_OBJ_GPU) {
                /* GPU object (one phrase): bits 3-13 are a YPOS; when the beam
                 * reaches it the OP halts and raises GPU interrupt source 3
                 * (vector $F03030). The handler services it (Doom: mid-screen
                 * VMODE switch) and writes OBF ($F00026) to resume the OP,
                 * which continues at the NEXT PHRASE - there is no link field.
                 * (Prior code mis-read data bits as a link and ended the list
                 * here, so nothing after this object was ever displayed.) */
                uint32_t gypos = (uint32_t)((p0 >> 3) & 0x7FF);
                s_saw_gpuobj = 1;
                if (m->gpu && (gypos == 0x7FF || (int)gypos == hv)) {
                    onca_gpu_t *g = m->gpu;
                    uint16_t vm_before = onca_peek16(m, R_VMODE);
                    if (g->running && (g->flags_hi & 0x80)) {   /* src-3 enable */
                        onca_gpu_interrupt(g, 3);
                        /* Run the handler to completion: it returns by
                         * restoring FLAGS, which clears IMASK. Bounded. */
                        for (int s = 0; s < 512 && g->running
                                        && (g->flags_hi & GF_IMASK); s++)
                            onca_gpu_step(g);
                    }
                    uint16_t vm_after = onca_peek16(m, R_VMODE);
                    if (vm_after != vm_before) {   /* switch fired: cache band */
                        s_fired_this_frame = 1;
                        s_band_row    = screen_y;
                        s_vmode_below = vm_after;
                        s_vmode_above = vmode_entry;
                    }
                }
                addr += 8;                             /* resume at next phrase */
                continue;
            }

            /* BITMAP or SCALED BITMAP */
            uint32_t ypos   = (uint32_t)((p0 >> 3)  & 0x7FF);
            uint32_t height = (uint32_t)((p0 >> 14) & 0x3FF);
            uint32_t data   = (uint32_t)(((p0 >> 43) & 0x1FFFFF) << 3);

            uint64_t p1 = ((uint64_t)onca_peek32(m, addr + 8) << 32) | onca_peek32(m, addr + 12);
            int xpos   = (int)(p1 & 0xFFF);
            if (xpos & 0x800) xpos -= 0x1000;          /* 12-bit signed */
            int pitch  = (int)((p1 >> 15) & 7);        /* phrase spacing; 0 = off */
            /* PITCH=0 is "no data fetched" - Doom parks its back-buffer view
             * object with PITCH=0/DWIDTH=0 to hide it. Without this check the
             * dead object reads a zero-stride line of zeros and overpaints the
             * live front-buffer view with opaque black. */
            if (pitch == 0) {
                if (!link || link == addr) break;
                addr = link;
                continue;
            }
            int depth  = (int)((p1 >> 12) & 7);
            static const int bpp_tbl[8] = { 1, 2, 4, 8, 16, 24, 24, 24 };
            int bpp    = bpp_tbl[depth];               /* DEPTH 5 = 24bpp, not 32 */
            int dwidth = (int)((p1 >> 18) & 0x3FF);    /* phrases per line in memory */
            int iwidth = (int)((p1 >> 28) & 0x3FF);    /* phrases per line displayed */
            int index  = (int)((p1 >> 38) & 0x7F);
            int trans  = (int)((p1 >> 47) & 1);        /* TRANS: colour 0 transparent iff set */

            int hscale = 32, vscale = 32;              /* 3.5 fixed, 0x20 = 1.0 */
            if (type == OP_OBJ_SCALED) {
                uint32_t sc = onca_peek32(m, addr + 20);
                hscale = sc & 0xFF;
                vscale = (sc >> 8) & 0xFF;
                if (hscale == 0 || vscale == 0) {      /* invisible: follow link */
                    if (!link || link == addr) break;
                    addr = link;
                    continue;
                }
            }

            int pix_per_phrase = 64 / bpp;
            int src_w = iwidth * pix_per_phrase;
            int out_w = src_w * hscale / 32;
            int out_h = (int)height * vscale / 32;
            int top   = ((int)ypos - vdb) / 2;         /* half-lines -> lines */
            int oy    = screen_y - top;

            if (sub == 1 && oy >= 0 && oy < out_h) {
                draw_object_line(m, fb, w, screen_y, oy, xpos, depth, bpp,
                                 dwidth, index, trans, out_w, hscale, vscale, data, cry,
                                 xscale);
                if (screen_y == (top < 0 ? 0 : top)) drawn++;  /* count each object once */
            }

            if (!link || link == addr) break;
            addr = link;
        }
        }
    }
    /* No mode-switch object in this list (title/menu screens): the cached
     * in-game band layout does not apply - drop it, or the splash renders
     * with the level's doubled view band (stale-cache bug). */
    if (!s_saw_gpuobj) s_band_row = -1;
    return drawn;
}
