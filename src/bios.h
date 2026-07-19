/*
 * bios.h - Atari Jaguar boot ROM loading and content-based identification.
 *
 * The core identifies the boot ROM by CRC32 of its contents, not by filename,
 * so it picks the right image regardless of how the user named the file. The
 * known table starts with the standard 128 KB cartridge-boot ROM and is
 * extended as more revisions are confirmed against ground-truth dumps.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ONCA_BIOS_H
#define ONCA_BIOS_H

#include <stdint.h>
#include <stddef.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ONCA_REGION_NTSC, ONCA_REGION_PAL, ONCA_REGION_UNKNOWN } onca_region_id_t;

typedef struct {
    uint32_t          crc32;
    const char       *name;      /* human label            */
    const char       *filename;  /* canonical rom filename */
    onca_region_id_t region;
    int               supported;   /* a supported boot target */
} onca_bios_desc_t;

typedef struct {
    int                      known;
    uint32_t                 crc32;
    size_t                   size;
    const onca_bios_desc_t *desc;   /* non-NULL iff known */
    onca_region_id_t        region;
} onca_bios_info_t;

/* CRC32 (IEEE 802.3 / zlib polynomial) of a buffer. */
uint32_t onca_crc32(const uint8_t *data, size_t len);

/* Identify a raw boot ROM image. Never fails; sets info->known=0 for
 * unrecognised images (still usable). Returns 1 if the size is a plausible
 * Jaguar boot ROM (128 KB), 0 otherwise. */
int onca_bios_identify(const uint8_t *data, size_t len, onca_bios_info_t *info);

/* Load a boot ROM file from disk into m->rom and identify it. Returns 0 on
 * success, negative on I/O error. */
int onca_bios_load_file(const char *path, onca_mem_t *m, onca_bios_info_t *info);

const char *onca_region_name(onca_region_id_t r);

#ifdef __cplusplus
}
#endif

#endif /* ONCA_BIOS_H */
