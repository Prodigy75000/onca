/*
 * blitter.h - Tom's Blitter (2D graphics coprocessor).
 *
 * The Blitter runs an inner x outer loop moving/combining pixels between two
 * addressed windows (A1 = destination, A2 = source) with an optional pattern
 * and logic-function unit. Games use it to clear the framebuffer (pattern fill)
 * and to draw bitmaps (copy). A write to B_CMD ($F02238) kicks a blit; the core
 * runs it to completion synchronously and reports "idle" on the status read.
 *
 * Built clean-room from the Tom & Jerry Technical Reference Manual plus register
 * behaviour observed on the boot ROM / test carts (e.g. the A1 width float, whose
 * encoding was recovered from A1_FLAGS vs A1_STEP on real blits).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_BLITTER_H
#define ONCA_BLITTER_H

#include "memory.h"

/* Blitter register block base and the command register (byte addresses). */
#define ONCA_BLIT_BASE  0xF02200u
#define ONCA_B_CMD      0xF02238u

/* Run one blit using the current Blitter register state. Called when the 68000
 * (or GPU) writes B_CMD. */
void onca_blitter_run(onca_mem_t *m);

#endif
