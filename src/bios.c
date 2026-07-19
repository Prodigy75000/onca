/*
 * bios.c - Atari Jaguar boot ROM load + CRC32 identification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bios.h"
#include <stdio.h>
#include <string.h>

/* ---- CRC32 (reflected, poly 0xEDB88320) ---- */
static uint32_t crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}
uint32_t onca_crc32(const uint8_t *data, size_t len) {
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- known image table ----
 * CRCs are filled in as images are confirmed with ground truth. The 128 KB
 * cartridge-boot ROM (reset vector -> $E00008, first op writes Tom MEMCON1)
 * is the supported boot target. The Jaguar CD unit's separate 256 KB boot
 * ROM is not yet supported and intentionally not listed here. */
static const onca_bios_desc_t g_known[] = {
    /* crc32,    name,                          filename,       region,     supported */
    { 0xFB731AAAu, "Atari Jaguar boot ROM (cart)", "jagboot.rom", ONCA_REGION_NTSC, 1 },
    /* { 0x........, "Atari Jaguar CD boot ROM",  "jagcd.rom",   ONCA_REGION_NTSC, 0 }, */
    { 0, NULL, NULL, ONCA_REGION_UNKNOWN, 0 }
};

const char *onca_region_name(onca_region_id_t r) {
    switch (r) {
        case ONCA_REGION_NTSC: return "NTSC";
        case ONCA_REGION_PAL:  return "PAL";
        default:                return "unknown";
    }
}

int onca_bios_identify(const uint8_t *data, size_t len, onca_bios_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->size = len;
    info->crc32 = onca_crc32(data, len);
    info->region = ONCA_REGION_UNKNOWN;
    info->desc = NULL;
    info->known = 0;

    for (const onca_bios_desc_t *d = g_known; d->name; d++) {
        if (d->crc32 == info->crc32) {
            info->known = 1;
            info->desc = d;
            info->region = d->region;
            break;
        }
    }
    return (len == ONCA_ROM_SIZE) ? 1 : 0;
}

int onca_bios_load_file(const char *path, onca_mem_t *m, onca_bios_info_t *info) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -2; }

    size_t n = (size_t)sz;
    if (n > ONCA_ROM_SIZE) n = ONCA_ROM_SIZE;
    memset(m->rom, 0, ONCA_ROM_SIZE);
    size_t rd = fread(m->rom, 1, n, f);
    fclose(f);
    m->rom_loaded = rd;

    onca_bios_identify(m->rom, (size_t)sz, info);
    return 0;
}
