/*
 * op.h - Atari Jaguar Object Processor (Tom).
 *
 * The Object Processor is Tom's display compositor: it walks a linked list of
 * "objects" in memory (pointed to by OLP, $F00020) and draws bitmap objects
 * into the screen. This is the compositor stage - it renders whatever pixel
 * data exists in memory; producing that pixel data is the job of the Blitter
 * and the GPU RISC core (blitter.c / gpu.c).
 *
 * Clean-room from the public Tom & Jerry Technical Reference Manual object-list
 * format. Renders to a caller-supplied RGB565 framebuffer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_OP_H
#define ONCA_OP_H

#include <stdint.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Object types (low 3 bits of an object's first phrase). */
enum {
    OP_OBJ_BITMAP = 0,
    OP_OBJ_SCALED = 1,
    OP_OBJ_GPU    = 2,
    OP_OBJ_BRANCH = 3,
    OP_OBJ_STOP   = 4
};

/* Render one Object Processor frame from OLP into fb (RGB565, w*h pixels).
 * The framebuffer is first cleared to the background colour (BG, $F00058,
 * decoded like a pixel). Returns the number of bitmap objects drawn. */
int onca_op_render(onca_mem_t *m, uint16_t *fb, int w, int h);

/* Decode a 16-bit Jaguar pixel to RGB565. `cry` selects CRY vs. RGB16. Exposed
 * for unit tests. */
uint16_t onca_op_decode16(uint16_t v, int cry);

#ifdef __cplusplus
}
#endif

#endif /* ONCA_OP_H */
