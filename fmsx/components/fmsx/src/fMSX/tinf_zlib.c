// tinf_zlib.c — ультракомпактная реализация zlib API через tinf для fMSX

/** fMSX: portable MSX emulator ******************************/
/**                                                         **/
/**                      tinf_zlib.c                        **/
/**                                                         **/
/**   GZip decompression adapter for fMSX on ESP32-S3.      **/
/**   Provides zlib-compatible gz* API using tinf engine.   **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                      LICENSE NOTICE                     **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** 1. tinf — tiny inflate library (deflate/gzip/zlib):     **/
/**    Copyright (c) 2003-2019 Joergen Ibsen                **/
/**    Licensed under zlib/libpng license (see below).      **/
/**    This software is provided 'as-is', without any       **/
/**    express or implied warranty.                         **/
/** 2. zlib API adapter, Huffman tables, ESP32-S3 VGA/DRAM  **/
/**    buffer integration:                                  **/
/**    Copyright (C) 2026 Ivan Svarkovsky                   **/
/**    Licensed under CC BY-NC-SA 4.0                       **/
/**    Contact: ivansvarkovsky@gmail.com                    **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** tinf_zlib v1.1 — GZip decompression for ESP32-S3        **/
/**                                                         **/
/**  - tinf_data in VGA DRAM buffer (zero heap allocs)      **/
/**  - Huffman tables in VGA DRAM buffer                    **/
/**  - Optional CRC32 via TINF_CRC32_ENABLE                 **/
/**  - All large buffers (compressed/uncompressed) in PSRAM **/
/**                                                         **/
/*************************************************************/

/**
 * tinf (tiny inflate) original license:
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must
 *      not claim that you wrote the original software. If you use this
 *      software in a product, an acknowledgment in the product
 *      documentation would be appreciated but is not required.
 *
 *   2. Altered source versions must be plainly marked as such, and must
 *      not be misrepresented as being the original software.
 *
 *   3. This notice may not be removed or altered from any source
 *      distribution.
 */

// === Опция: включить CRC32 (раскомментировать для проверки целостности) ===
// #define TINF_CRC32_ENABLE

#ifndef ZLIB_H
#define ZLIB_H

#include <stdio.h>

typedef void *gzFile;

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define TINF_OK         0
#define TINF_DATA_ERROR (-3)
#define TINF_BUF_ERROR  (-5)

#endif /* ZLIB_H */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_attr.h>
#else
#define heap_caps_malloc(sz, caps) malloc(sz)
#define MALLOC_CAP_SPIRAM 0
#define DRAM_ATTR
#define esp_task_wdt_reset()
#endif

// Внешний буфер VGA (5 КБ в DRAM) — используем для tinf_data и таблиц
extern uint16_t sram_render_buffer[];
extern bool rg_display_sync(bool block);

// tinf_data в VGA-буфере (1.3 КБ из 5 КБ)
#define TINF_DATA_BUF  ((struct tinf_data *)sram_render_buffer)

// Таблицы Хаффмана в DRAM — ускоряет доступ (было во Flash)
// Размещаем их в хвосте VGA-буфера, после tinf_data (1.3 КБ = ~650 uint16_t)
// Таблицы: 30+30+30+30 = 120 записей = ~240 байт — помещается
#define HUFF_TABLES    ((uint16_t *)(sram_render_buffer + 700))

// =========================================================================
// === ВНУТРЕННИЕ СТРУКТУРЫ =================================================
// =========================================================================

struct tinf_tree {
    uint16_t counts[16];
    uint16_t symbols[288];
    int max_sym;
};

struct tinf_data {
    const uint8_t *source, *source_end;
    uint32_t tag;
    int bitcount, overflow;
    uint8_t *dest_start, *dest, *dest_end;
    struct tinf_tree ltree, dtree;
};

// =========================================================================
// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ =============================================
// =========================================================================

static inline uint16_t read_le16(const uint8_t *p) {
    return p[0] | (p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Инлайн-версия tinf_getbits для 1 бита — самый частый вызов
static inline uint32_t tinf_getbits_1(struct tinf_data *d) {
    if (d->bitcount < 1) {
        if (d->source != d->source_end) {
            d->tag |= (uint32_t)*d->source++ << d->bitcount;
        } else {
            d->overflow = 1;
        }
        d->bitcount += 8;
    }
    uint32_t bit = d->tag & 1;
    d->tag >>= 1;
    d->bitcount--;
    return bit;
}

static void tinf_refill(struct tinf_data *d, int num) {
    while (d->bitcount < num) {
        if (d->source != d->source_end) {
            d->tag |= (uint32_t)*d->source++ << d->bitcount;
        } else {
            d->overflow = 1;
        }
        d->bitcount += 8;
    }
}

static uint32_t tinf_getbits(struct tinf_data *d, int num) {
    tinf_refill(d, num);
    uint32_t bits = d->tag & ((1UL << num) - 1);
    d->tag >>= num;
    d->bitcount -= num;
    return bits;
}

static uint32_t tinf_getbits_base(struct tinf_data *d, int num, int base) {
    return base + (num ? tinf_getbits(d, num) : 0);
}

// =========================================================================
// === ТАБЛИЦЫ ХАФФМАНА В DRAM (копируются при первом вызове) ==============
// =========================================================================

static const uint8_t length_bits_flash[30] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0, 127
};

static const uint16_t length_base_flash[30] = {
     3,  4,  5,   6,   7,   8,   9,  10,  11,  13,
    15, 17, 19,  23,  27,  31,  35,  43,  51,  59,
    67, 83, 99, 115, 131, 163, 195, 227, 258,   0
};

static const uint8_t dist_bits_flash[30] = {
    0, 0,  0,  0,  1,  1,  2,  2,  3,  3,
    4, 4,  5,  5,  6,  6,  7,  7,  8,  8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const uint16_t dist_base_flash[30] = {
       1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,  129,  193,  257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

// Указатели на таблицы в DRAM (инициализируются один раз)
static uint8_t  *length_bits_dram;
static uint16_t *length_base_dram;
static uint8_t  *dist_bits_dram;
static uint16_t *dist_base_dram;
static int tables_ready = 0;

static void init_tables(void) {
    if (tables_ready) return;
    rg_display_sync(true);
    uint16_t *p = HUFF_TABLES;
    length_bits_dram = (uint8_t *)p;  memcpy(p, length_bits_flash, 30); p += 15;
    dist_bits_dram   = (uint8_t *)p;  memcpy(p, dist_bits_flash, 30);   p += 15;
    length_base_dram = p;             memcpy(p, length_base_flash, 60); p += 30;
    dist_base_dram   = p;             memcpy(p, dist_base_flash, 60);   p += 30;
    tables_ready = 1;
}

// =========================================================================
// === ДЕКОДЕРЫ ДЕРЕВЬЕВ ===================================================
// =========================================================================

static void tinf_build_fixed_trees(struct tinf_tree *lt, struct tinf_tree *dt) {
    int i;
    for (i = 0; i < 16; ++i) lt->counts[i] = 0;
    lt->counts[7] = 24; lt->counts[8] = 152; lt->counts[9] = 112;
    for (i = 0; i < 24; ++i) lt->symbols[i] = 256 + i;
    for (i = 0; i < 144; ++i) lt->symbols[24 + i] = i;
    for (i = 0; i < 8; ++i) lt->symbols[24 + 144 + i] = 280 + i;
    for (i = 0; i < 112; ++i) lt->symbols[24 + 144 + 8 + i] = 144 + i;
    lt->max_sym = 285;

    for (i = 0; i < 16; ++i) dt->counts[i] = 0;
    dt->counts[5] = 32;
    for (i = 0; i < 32; ++i) dt->symbols[i] = i;
    dt->max_sym = 29;
}

static int tinf_build_tree(struct tinf_tree *t, const uint8_t *lengths, unsigned int num) {
    uint16_t offs[16];
    unsigned int i, num_codes, available;

    for (i = 0; i < 16; ++i) t->counts[i] = 0;
    t->max_sym = -1;
    for (i = 0; i < num; ++i) {
        if (lengths[i]) { t->max_sym = i; t->counts[lengths[i]]++; }
    }

    for (available = 1, num_codes = 0, i = 0; i < 16; ++i) {
        unsigned int used = t->counts[i];
        if (used > available) return TINF_DATA_ERROR;
        available = 2 * (available - used);
        offs[i] = num_codes;
        num_codes += used;
    }

    if ((num_codes > 1 && available > 0) || (num_codes == 1 && t->counts[1] != 1))
        return TINF_DATA_ERROR;

    for (i = 0; i < num; ++i)
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = i;

    if (num_codes == 1) { t->counts[1] = 2; t->symbols[1] = t->max_sym + 1; }
    return TINF_OK;
}

static int tinf_decode_symbol(struct tinf_data *d, const struct tinf_tree *t) {
    int base = 0, offs = 0, len;
    for (len = 1; ; ++len) {
        offs = 2 * offs + tinf_getbits_1(d);
        if (offs < t->counts[len]) break;
        base += t->counts[len];
        offs -= t->counts[len];
    }
    return t->symbols[base + offs];
}

static int tinf_decode_trees(struct tinf_data *d, struct tinf_tree *lt, struct tinf_tree *dt) {
    uint8_t lengths[288 + 32];
    static const uint8_t clcidx[19] = {
        16, 17, 18, 0,  8, 7,  9, 6, 10, 5,
        11,  4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned int hlit, hdist, hclen, i, num, length;
    int res;

    hlit = tinf_getbits_base(d, 5, 257);
    hdist = tinf_getbits_base(d, 5, 1);
    hclen = tinf_getbits_base(d, 4, 4);

    if (hlit > 286 || hdist > 30) return TINF_DATA_ERROR;

    for (i = 0; i < 19; ++i) lengths[i] = 0;
    for (i = 0; i < hclen; ++i) lengths[clcidx[i]] = tinf_getbits(d, 3);

    res = tinf_build_tree(lt, lengths, 19);
    if (res != TINF_OK) return res;
    if (lt->max_sym == -1) return TINF_DATA_ERROR;

    for (num = 0; num < hlit + hdist; ) {
        int sym = tinf_decode_symbol(d, lt);
        if (sym > lt->max_sym) return TINF_DATA_ERROR;

        switch (sym) {
        case 16:
            if (num == 0) return TINF_DATA_ERROR;
            sym = lengths[num - 1];
            length = tinf_getbits_base(d, 2, 3);
            break;
        case 17: sym = 0; length = tinf_getbits_base(d, 3, 3); break;
        case 18: sym = 0; length = tinf_getbits_base(d, 7, 11); break;
        default:  length = 1; break;
        }

        if (length > hlit + hdist - num) return TINF_DATA_ERROR;
        while (length--) lengths[num++] = sym;
    }

    if (lengths[256] == 0) return TINF_DATA_ERROR;
    res = tinf_build_tree(lt, lengths, hlit);
    if (res != TINF_OK) return res;
    return tinf_build_tree(dt, lengths + hlit, hdist);
}

// =========================================================================
// === РАСПАКОВКА БЛОКОВ ===================================================
// =========================================================================

IRAM_ATTR static int tinf_inflate_block_data(struct tinf_data *d, struct tinf_tree *lt, struct tinf_tree *dt) {
    init_tables();

    for (;;) {
        int sym = tinf_decode_symbol(d, lt);
        if (d->overflow) return TINF_DATA_ERROR;

        if (sym < 256) {
            if (d->dest == d->dest_end) return TINF_BUF_ERROR;
            *d->dest++ = sym;
        } else {
            int length, dist, offs;
            if (sym == 256) return TINF_OK;
            if (sym > lt->max_sym || sym - 257 > 28 || dt->max_sym == -1) return TINF_DATA_ERROR;

            sym -= 257;
            length = tinf_getbits_base(d, length_bits_dram[sym], length_base_dram[sym]);
            dist = tinf_decode_symbol(d, dt);
            if (dist > dt->max_sym || dist > 29) return TINF_DATA_ERROR;

            offs = tinf_getbits_base(d, dist_bits_dram[dist], dist_base_dram[dist]);
            if (offs > d->dest - d->dest_start) return TINF_DATA_ERROR;
            if (d->dest_end - d->dest < length) return TINF_BUF_ERROR;

            uint8_t *src = d->dest - offs;
            if (offs >= length) {
                // Нет пересечения — memcpy
                memcpy(d->dest, src, length);
                d->dest += length;
            } else if (offs == 1) {
                // RLE-повтор одного байта — memset (аппаратно быстрый)
                memset(d->dest, *src, length);
                d->dest += length;
            } else if (offs >= 4) {
                // Пересечение, но offset >= 4 — безопасное 32-битное копирование
                while (length >= 4) {
                    *(uint32_t*)d->dest = *(const uint32_t*)src;
                    d->dest += 4;
                    src += 4;
                    length -= 4;
                }
                while (length--) *d->dest++ = *src++;
            } else {
                // offs == 2 или 3 — побайтно (редкий случай)
                while (length--) *d->dest++ = *src++;
            }
        }
    }
}

static int tinf_inflate_uncompressed_block(struct tinf_data *d) {
    unsigned int length, invlength;
    if (d->source_end - d->source < 4) return TINF_DATA_ERROR;
    length = read_le16(d->source);
    invlength = read_le16(d->source + 2);
    if (length != (~invlength & 0x0000FFFF)) return TINF_DATA_ERROR;

    d->source += 4;
    if (d->source_end - d->source < length) return TINF_DATA_ERROR;
    if (d->dest_end - d->dest < (int)length) return TINF_BUF_ERROR;

    memcpy(d->dest, d->source, length);
    d->dest += length;
    d->source += length;
    d->tag = 0; d->bitcount = 0;
    return TINF_OK;
}

static int tinf_inflate_fixed_block(struct tinf_data *d) {
    tinf_build_fixed_trees(&d->ltree, &d->dtree);
    return tinf_inflate_block_data(d, &d->ltree, &d->dtree);
}

static int tinf_inflate_dynamic_block(struct tinf_data *d) {
    int res = tinf_decode_trees(d, &d->ltree, &d->dtree);
    if (res != TINF_OK) return res;
    return tinf_inflate_block_data(d, &d->ltree, &d->dtree);
}

// =========================================================================
// === ОСНОВНАЯ РАСПАКОВКА (VGA-буфер, 0 байт DRAM/IRAM) ===================
// =========================================================================

static int tinf_uncompress(void *dest, unsigned int *destLen, const void *source, unsigned int sourceLen) {
    rg_display_sync(true);
    struct tinf_data *d = TINF_DATA_BUF;
    int bfinal;

    d->source = (const uint8_t *)source;
    d->source_end = d->source + sourceLen;
    d->tag = 0; d->bitcount = 0; d->overflow = 0;
    d->dest = (uint8_t *)dest;
    d->dest_start = d->dest;
    d->dest_end = d->dest + *destLen;

    do {
        bfinal = tinf_getbits_1(d);
        unsigned int btype = tinf_getbits(d, 2);
        int res;
        switch (btype) {
        case 0: res = tinf_inflate_uncompressed_block(d); break;
        case 1: res = tinf_inflate_fixed_block(d); break;
        case 2: res = tinf_inflate_dynamic_block(d); break;
        default: return TINF_DATA_ERROR;
        }
        if (res != TINF_OK) return res;
    } while (!bfinal);

    if (d->overflow) return TINF_DATA_ERROR;
    *destLen = d->dest - d->dest_start;
    return TINF_OK;
}

#ifdef TINF_CRC32_ENABLE
// =========================================================================
// === CRC32 В VGA-БУФЕРЕ DRAM (опционально) ===============================
// =========================================================================

#define CRC32_TABLE_DRAM  ((uint32_t *)(sram_render_buffer + 820))

static int crc32_ready = 0;

static void init_crc32_table(void) {
    if (crc32_ready) return;
    rg_display_sync(true);
    uint32_t *t = CRC32_TABLE_DRAM;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320 : c >> 1;
        t[i] = c;
    }
    crc32_ready = 1;
}

static unsigned int tinf_crc32(const void *data, unsigned int length) {
    init_crc32_table();
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t *t = CRC32_TABLE_DRAM;
    for (unsigned int i = 0; i < length; i++)
        crc = t[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}
#endif /* TINF_CRC32_ENABLE */

static int tinf_gzip_uncompress(void *dest, unsigned int *destLen, const void *source, unsigned int sourceLen) {
    const uint8_t *src = (const uint8_t *)source;
    uint8_t *dst = (uint8_t *)dest;
    const uint8_t *start;
    unsigned int dlen;
    int res;
    uint8_t flg;

    if (sourceLen < 18) return TINF_DATA_ERROR;
    if (src[0] != 0x1F || src[1] != 0x8B) return TINF_DATA_ERROR;
    if (src[2] != 8) return TINF_DATA_ERROR;
    flg = src[3];
    if (flg & 0xE0) return TINF_DATA_ERROR;

    start = src + 10;
    if (flg & 4) {
        unsigned int xlen = read_le16(start);
        if (xlen > sourceLen - 12) return TINF_DATA_ERROR;
        start += xlen + 2;
    }
    if (flg & 8) {
        do { if (start - src >= (int)sourceLen) return TINF_DATA_ERROR; } while (*start++);
    }
    if (flg & 16) {
        do { if (start - src >= (int)sourceLen) return TINF_DATA_ERROR; } while (*start++);
    }
    if (flg & 2) {
#ifdef TINF_CRC32_ENABLE
        unsigned int hcrc;
        if (start - src > (int)sourceLen - 2) return TINF_DATA_ERROR;
        hcrc = read_le16(start);
        if (hcrc != (tinf_crc32(src, start - src) & 0x0000FFFF)) return TINF_DATA_ERROR;
        start += 2;
#else
        start += 2;
#endif
    }

    dlen = read_le32(&src[sourceLen - 4]);
    if (dlen > *destLen) return TINF_BUF_ERROR;
#ifdef TINF_CRC32_ENABLE
    unsigned int crc32 = read_le32(&src[sourceLen - 8]);
#endif

    if ((src + sourceLen) - start < 8) return TINF_DATA_ERROR;
    res = tinf_uncompress(dst, destLen, start, (src + sourceLen) - start - 8);
    if (res != TINF_OK) return TINF_DATA_ERROR;
    if (*destLen != dlen) return TINF_DATA_ERROR;
#ifdef TINF_CRC32_ENABLE
    if (crc32 != tinf_crc32(dst, dlen)) return TINF_DATA_ERROR;
#endif

    return TINF_OK;
}

// =========================================================================
// === GZ* API (PSRAM, VGA-буфер для таблиц) ===============================
// =========================================================================

typedef struct {
    uint8_t *data;
    unsigned int size;
    unsigned int pos;
    int used;
} gz_file_t;

#define MAX_GZ_FILES 8
static gz_file_t gz_files[MAX_GZ_FILES];

gzFile gzopen(const char *path, const char *mode) {
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".gz") != 0)
        return (gzFile)fopen(path, mode);

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long csize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (csize < 18 || csize > 8*1024*1024) { fclose(f); return NULL; }

    uint8_t *cbuf = heap_caps_malloc(csize, MALLOC_CAP_SPIRAM);
    if (!cbuf) { fclose(f); return NULL; }
    if (fread(cbuf, 1, csize, f) != (size_t)csize) { free(cbuf); fclose(f); return NULL; }
    fclose(f);

    esp_task_wdt_reset();

    uint32_t usize = read_le32(cbuf + csize - 4);
    if (usize == 0 || usize > 8*1024*1024) { free(cbuf); return NULL; }

    uint8_t *ubuf = heap_caps_malloc(usize, MALLOC_CAP_SPIRAM);
    if (!ubuf) { free(cbuf); return NULL; }

    unsigned int dlen = usize;
    esp_task_wdt_reset();
    int res = tinf_gzip_uncompress(ubuf, &dlen, cbuf, csize);
    free(cbuf);
    if (res != TINF_OK || dlen != usize) { free(ubuf); return NULL; }

    for (int i = 0; i < MAX_GZ_FILES; i++) {
        if (!gz_files[i].used) {
            gz_files[i].data = ubuf;
            gz_files[i].size = usize;
            gz_files[i].pos = 0;
            gz_files[i].used = 1;
            return (gzFile)(&gz_files[i]);
        }
    }
    free(ubuf);
    return NULL;
}

int gzclose(gzFile f) {
    if (!f) return Z_OK;
    for (int i = 0; i < MAX_GZ_FILES; i++) {
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used) {
            free(gz_files[i].data);
            memset(&gz_files[i], 0, sizeof(gz_file_t));
            return Z_OK;
        }
    }
    return fclose((FILE *)f) == 0 ? Z_OK : Z_ERRNO;
}

int gzread(gzFile f, void *buf, unsigned int len) {
    if (!f || !buf) return 0;
    for (int i = 0; i < MAX_GZ_FILES; i++) {
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used) {
            unsigned int avail = gz_files[i].size - gz_files[i].pos;
            if (avail == 0) return 0;
            unsigned int n = (len < avail) ? len : avail;
            memcpy(buf, gz_files[i].data + gz_files[i].pos, n);
            gz_files[i].pos += n;
            return n;
        }
    }
    return fread(buf, 1, len, (FILE *)f);
}

int gzwrite(gzFile f, const void *buf, unsigned int len) {
    if (!f) return 0;
    for (int i = 0; i < MAX_GZ_FILES; i++)
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used) return 0;
    return fwrite(buf, 1, len, (FILE *)f);
}

long gzseek(gzFile f, long offset, int whence) {
    if (!f) return -1;
    for (int i = 0; i < MAX_GZ_FILES; i++) {
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used) {
            long new_pos = (long)gz_files[i].pos;
            if (whence == SEEK_SET) new_pos = offset;
            else if (whence == SEEK_CUR) new_pos += offset;
            else if (whence == SEEK_END) new_pos = (long)gz_files[i].size + offset;
            if (new_pos < 0) return -1;
            if (new_pos > (long)gz_files[i].size) new_pos = (long)gz_files[i].size;
            gz_files[i].pos = (unsigned int)new_pos;
            return 0;
        }
    }
    return fseek((FILE *)f, offset, whence);
}

long gztell(gzFile f) {
    if (!f) return -1;
    for (int i = 0; i < MAX_GZ_FILES; i++)
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used) return gz_files[i].pos;
    return ftell((FILE *)f);
}

int gzgetc(gzFile f) {
    unsigned char c;
    return gzread(f, &c, 1) == 1 ? c : -1;
}

int gzeof(gzFile f) {
    if (!f) return 1;
    for (int i = 0; i < MAX_GZ_FILES; i++)
        if (f == (gzFile)(&gz_files[i]) && gz_files[i].used)
            return gz_files[i].pos >= gz_files[i].size;
    return feof((FILE *)f);
}

int gzrewind(gzFile f) {
    return gzseek(f, 0, SEEK_SET);
}

char *gzgets(gzFile f, char *buf, int len) {
    if (!f || !buf || len < 2) return NULL;
    int i;
    for (i = 0; i < len - 1; i++) {
        int c = gzgetc(f);
        if (c == -1) return i ? buf : NULL;
        buf[i] = c;
        if (c == '\n') { i++; break; }
    }
    buf[i] = '\0';
    return buf;
}
