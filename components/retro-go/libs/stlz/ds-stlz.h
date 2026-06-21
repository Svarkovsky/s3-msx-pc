/**
 * ds-stlz.h — Striped Delta-Stride LZ (STLZ) Image Codec
 * 
 * 
 * A domain-optimized image codec for memory-constrained embedded
 * systems. Splits framebuffer into horizontal stripes, applies
 * delta-XOR stride preprocessing per stripe, then compresses each
 * independently with LZ77. Enables zero-heap-RAM image rendering
 * by decoding and scaling one scanline at a time using only small
 * stack buffers.
 * 
 * Copyright (C) 2026 Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * 
 * This algorithm and its implementation are distributed under the
 * Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 * (CC BY-NC-SA 4.0) license.
 * 
 * Full license text: https://creativecommons.org/licenses/by-nc-sa/4.0/
 * 
 * Under this license, you are free to:
 *   - Share — copy and redistribute the material in any medium or format
 *   - Adapt — remix, transform, and build upon the material
 * 
 * Under the following terms:
 *   - Attribution — You must give appropriate credit to Ivan Svarkovsky,
 *     provide a link to the original repository, and indicate if changes
 *     were made.
 *   - NonCommercial — You may not use the material for commercial purposes.
 *   - ShareAlike — If you remix, transform, or build upon the material,
 *     you must distribute your contributions under the same license.
 * 
 * Based on public-domain LZ77 (Lempel-Ziv, 1977).
 * All patents expired. Independent implementation.
 * 
 * Original repository: https://github.com/Svarkovsky
 * 
 * Core algorithm: stripe-split + delta-XOR stride + LZ77 + VLC
 * 
 * Key features:
 *   - Horizontal stripe partitioning (8-pixel rows typical)
 *   - 2D delta-XOR preprocessing with framebuffer stride
 *   - Independent stripe compression with hash table reset
 *   - Zero heap RAM decompression (stack-only stripe buffer)
 *   - On-the-fly nearest-neighbor scaling during decode
 *   - Variable-length coding: near/far/rep match tokens
 *   - 32-bit accelerated match extension
 *   - Cost-based lazy matching for optimal parsing
 * 
 * File format:
 *   [STLZ header: 24 bytes]
 *     - magic "STLZ" (4 bytes)
 *     - width, height (uint16_t each)
 *     - stripe_h, num_stripes (uint8_t each)
 *     - format, reserved[7]
 *   [Offset table: num_stripes × uint32_t]
 *   [Compressed stripes: LZ77 streams]
 * 
 * Usage:
 *   #include "ds-stlz.h"
 *   Links with ds-stlz.c for public functions.
 *   All codec functions are static inline in this header.
 * 
 * API:
 *   bool is_stlz_file(const char *filepath);
 *   bool rg_stlz_get_dimensions(const char *filepath, int *w, int *h);
 *   (rg_surface_save_stlz — in rg_surface.c)
 *   (rg_gui_draw_stlz_image — in rg_gui.c)
 */


/*
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef DS_STLZ_H
#define DS_STLZ_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STLZ_MAGIC "STLZ"
#define STLZ_HASH_SIZE 1024  

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint8_t stripe_h;
    uint8_t num_stripes;
    uint8_t format;
    uint8_t reserved[7];
} stlz_header_t;

typedef struct { uint32_t pos[2]; } stlz_hash_t;

typedef struct {
    uint8_t *buf;
    size_t pos;
    size_t max_size;
} stlz_mem_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t size;
} stlz_mem_reader_t;

static inline void stlz_mem_putc(stlz_mem_writer_t *w, uint8_t c) {
    if (w->pos < w->max_size) w->buf[w->pos++] = c;
}

static inline int stlz_mem_getc(stlz_mem_reader_t *r) {
    if (r->pos < r->size) return r->buf[r->pos++];
    return -1;
}

static inline uint32_t stlz_match_len(const uint8_t *a, const uint8_t *b, uint32_t max_len) {
    uint32_t cur = 0;
    while (cur + 4 <= max_len && *(const uint32_t*)(a + cur) == *(const uint32_t*)(b + cur)) cur += 4;
    while (cur < max_len && a[cur] == b[cur]) cur++;
    return cur;
}

static inline uint32_t stlz_hash3(const uint8_t *p) {
    uint32_t h = (p[0] * 0x9E3779B1u) ^ (p[1] * 0x85EBCA77u) ^ p[2];
    return (h ^ (h >> 12)) & (STLZ_HASH_SIZE - 1);
}

static inline unsigned int stlz_match_cost(unsigned int len, unsigned int offset, int is_rep) {
    if (is_rep) {
        if (len < 3) return 999;
        return (len <= 18) ? 1 : 4;
    }
    if (offset <= 255) {
        if (len < 3) return 999;
        return (len <= 66) ? 2 : 5;
    }
    if (len < 4) return 999;
    return (len <= 35) ? 3 : 6;
}

static inline void stlz_find_match(const unsigned char *src, unsigned int pos, unsigned int len, 
                                   stlz_hash_t *hash, uint32_t last_offset,
                                   uint32_t *out_len, uint32_t *out_off, int *out_is_rep) {
    uint32_t best_len = 0, best_off = 0;
    int is_rep = 0;

    if (pos + 3 <= len) {
        uint32_t h = stlz_hash3(&src[pos]);
        for (int e = 0; e < 2; e++) {
            uint32_t prev = hash[pos & 1 ? h : (h ^ 1)].pos[e];
            if (prev != 0xFFFFFFFF && pos > prev && (pos - prev) <= 65535) {
                uint32_t off = pos - prev;
                uint32_t max_match = len - pos;
                if (max_match > 65535) max_match = 65535;
                uint32_t cur = stlz_match_len(&src[pos], &src[prev], max_match);
                unsigned int min_needed = (off <= 255) ? 3 : 4;
                if (cur >= min_needed && cur > best_len) {
                    best_len = cur; best_off = off; is_rep = 0;
                }
            }
        }
    }

    if (last_offset > 0 && last_offset <= pos) {
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t rep_len = stlz_match_len(&src[pos], &src[pos - last_offset], max_match);
        if (rep_len >= 3) {
            int cost_rep = stlz_match_cost(rep_len, last_offset, 1);
            int cost_best = stlz_match_cost(best_len, best_off, is_rep);
            int save_rep = (int)rep_len - cost_rep, save_best = (int)best_len - cost_best;
            if (save_rep > save_best) {
                best_len = rep_len; best_off = last_offset; is_rep = 1;
            }
        }
    }
    *out_len = best_len; *out_off = best_off; *out_is_rep = is_rep;
}

static inline void stlz_write_lz77(const unsigned char *src, unsigned int len, stlz_mem_writer_t *w, stlz_hash_t *hash) {
    uint32_t last_offset = 0;
    unsigned int i = 0, lit_start = 0;

    #define STLZ_FLUSH_LITS() do { \
        unsigned int _ll = i - lit_start; \
        while (_ll > 0) { \
            unsigned int _chunk = (_ll > 65535 + 137) ? 65535 + 137 : _ll; \
            if (_chunk <= 128) stlz_mem_putc(w, (uint8_t)(_chunk - 1)); \
            else if (_chunk <= 136) stlz_mem_putc(w, (uint8_t)(240 | (_chunk - 129))); \
            else { \
                stlz_mem_putc(w, 248); stlz_mem_putc(w, 0); \
                unsigned int _ext = _chunk - 137; \
                stlz_mem_putc(w, (uint8_t)(_ext & 0xFF)); stlz_mem_putc(w, (uint8_t)((_ext >> 8) & 0xFF)); \
            } \
            for (unsigned int _j = 0; _j < _chunk; _j++) stlz_mem_putc(w, src[lit_start + _j]); \
            lit_start += _chunk; _ll -= _chunk; \
        } \
    } while(0)

    while (i < len) {
        uint32_t best_len = 0, best_off = 0;
        int is_rep = 0;
        stlz_find_match(src, i, len, hash, last_offset, &best_len, &best_off, &is_rep);

        if (best_len >= 3 && best_len < 16 && i + 1 < len) {
            uint32_t lazy_len = 0, lazy_off = 0;
            int lazy_rep = 0;
            stlz_find_match(src, i + 1, len, hash, last_offset, &lazy_len, &lazy_off, &lazy_rep);
            if (lazy_len >= 3) {
                int cost_best = stlz_match_cost(best_len, best_off, is_rep);
                int cost_lazy = stlz_match_cost(lazy_len, lazy_off, lazy_rep);
                if ((int)lazy_len - cost_lazy > (int)best_len - cost_best) best_len = 0;
            }
        }

        if (best_len >= 3) {
            STLZ_FLUSH_LITS();
            if (is_rep) {
                if (best_len <= 18) stlz_mem_putc(w, (uint8_t)(0xE0 | (best_len - 3)));
                else {
                    stlz_mem_putc(w, 248); stlz_mem_putc(w, 2);
                    stlz_mem_putc(w, (uint8_t)(best_len & 0xFF)); stlz_mem_putc(w, (uint8_t)((best_len >> 8) & 0xFF));
                }
            } else if (best_off <= 255 && best_len <= 66) {
                stlz_mem_putc(w, (uint8_t)(0x80 | (best_len - 3)));
                stlz_mem_putc(w, (uint8_t)(best_off & 0xFF));
            } else if (best_len <= 35) {
                stlz_mem_putc(w, (uint8_t)(0xC0 | (best_len - 4)));
                stlz_mem_putc(w, (uint8_t)(best_off & 0xFF)); stlz_mem_putc(w, (uint8_t)((best_off >> 8) & 0xFF));
            } else {
                stlz_mem_putc(w, 248); stlz_mem_putc(w, 1);
                stlz_mem_putc(w, (uint8_t)(best_len & 0xFF)); stlz_mem_putc(w, (uint8_t)((best_len >> 8) & 0xFF));
                stlz_mem_putc(w, (uint8_t)(best_off & 0xFF)); stlz_mem_putc(w, (uint8_t)((best_off >> 8) & 0xFF));
            }
            last_offset = best_off;
            unsigned int ulim = (best_len > 4) ? 4 : best_len;
            for (unsigned int j = 0; j < ulim; j++) {
                if (i + j + 3 <= len) {
                    uint32_t h = stlz_hash3(&src[i + j]);
                    hash[h].pos[1] = hash[h].pos[0];
                    hash[h].pos[0] = i + j;
                }
            }
            i += best_len; lit_start = i;
        } else {
            if (i + 3 <= len) {
                uint32_t h = stlz_hash3(&src[i]);
                hash[h].pos[1] = hash[h].pos[0];
                hash[h].pos[0] = i;
            }
            i++;
        }
    }
    STLZ_FLUSH_LITS();
    #undef STLZ_FLUSH_LITS
}

static inline int stlz_read_lz77(unsigned char *dst, unsigned int len, stlz_mem_reader_t *r) {
    unsigned int i = 0, last_offset = 0;
    while (i < len) {
        int cmd_val = stlz_mem_getc(r);
        if (cmd_val == -1) return 0;
        unsigned char cmd = (unsigned char)cmd_val;

        if (cmd <= 127) {
            unsigned int lit_len = cmd + 1;
            if (i + lit_len > len) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = stlz_mem_getc(r);
                if (c == -1) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        } else if (cmd <= 191) {
            unsigned int mlen = (cmd & 0x3F) + 3;
            int off = stlz_mem_getc(r);
            if (off == -1) return 0;
            unsigned int offset = (unsigned char)off;
            if (offset == 0 || offset > i || i + mlen > len) return 0;
            unsigned int src = i - offset;
            for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
            i += mlen; last_offset = offset;
        } else if (cmd <= 223) {
            unsigned int mlen = (cmd & 0x1F) + 4;
            int ol = stlz_mem_getc(r), oh = stlz_mem_getc(r);
            if (ol == -1 || oh == -1) return 0;
            unsigned int offset = (unsigned char)ol | ((unsigned char)oh << 8);
            if (offset == 0 || offset > i || i + mlen > len) return 0;
            unsigned int src = i - offset;
            for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
            i += mlen; last_offset = offset;
        } else if (cmd <= 239) {
            unsigned int mlen = (cmd & 0x0F) + 3;
            if (last_offset == 0 || last_offset > i || i + mlen > len) return 0;
            unsigned int src = i - last_offset;
            for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
            i += mlen;
        } else if (cmd <= 247) {
            unsigned int lit_len = (cmd & 0x07) + 129;
            if (i + lit_len > len) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = stlz_mem_getc(r);
                if (c == -1) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        } else if (cmd == 248) {
            int sub = stlz_mem_getc(r);
            if (sub == -1) return 0;
            if (sub == 0) {
                int ll = stlz_mem_getc(r), lh = stlz_mem_getc(r);
                if (ll == -1 || lh == -1) return 0;
                unsigned int lit_len = ((unsigned char)ll | ((unsigned char)lh << 8)) + 137;
                if (i + lit_len > len) return 0;
                for (unsigned int j = 0; j < lit_len; j++) {
                    int c = stlz_mem_getc(r);
                    if (c == -1) return 0;
                    dst[i + j] = (unsigned char)c;
                }
                i += lit_len;
            } else if (sub == 1) {
                int ll = stlz_mem_getc(r), lh = stlz_mem_getc(r);
                int ol = stlz_mem_getc(r), oh = stlz_mem_getc(r);
                if (ll == -1 || lh == -1 || ol == -1 || oh == -1) return 0;
                unsigned int mlen = (unsigned char)ll | ((unsigned char)lh << 8);
                unsigned int offset = (unsigned char)ol | ((unsigned char)oh << 8);
                if (offset == 0 || offset > i || i + mlen > len) return 0;
                unsigned int src = i - offset;
                for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
                i += mlen; last_offset = offset;
            } else if (sub == 2) {
                int ll = stlz_mem_getc(r), lh = stlz_mem_getc(r);
                if (ll == -1 || lh == -1) return 0;
                unsigned int mlen = (unsigned char)ll | ((unsigned char)lh << 8);
                if (last_offset == 0 || last_offset > i || i + mlen > len) return 0;
                unsigned int src = i - last_offset;
                for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
                i += mlen;
            } else if (sub == 3) {
                int ll = stlz_mem_getc(r), lh = stlz_mem_getc(r);
                int off = stlz_mem_getc(r);
                if (ll == -1 || lh == -1 || off == -1) return 0;
                unsigned int mlen = (unsigned char)ll | ((unsigned char)lh << 8);
                unsigned int offset = (unsigned char)off;
                if (offset == 0 || offset > i || i + mlen > len) return 0;
                unsigned int src = i - offset;
                for (unsigned int j = 0; j < mlen; j++) dst[i + j] = dst[src + j];
                i += mlen; last_offset = offset;
            } else return 0;
        } else return 0;
    }
    return 1;
}

static inline void stlz_delta_encode(uint8_t *data, size_t size, int stride) {
    if (size < (size_t)stride || stride <= 0) return;
    size_t i;
    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = size - 4; i >= (size_t)stride; i -= 4)
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
    } else {
        for (i = size - 1; i >= (size_t)stride; i--)
            data[i] ^= data[i - stride];
    }
    for (i = size - 1; i > 0; i--) {
        if (i % stride != 0) data[i] ^= data[i - 1];
    }
}

static inline void stlz_delta_decode(uint8_t *data, size_t size, int stride) {
    if (size < (size_t)stride || stride <= 0) return;
    size_t i;
    for (i = 1; i < size; i++) {
        if (i % stride != 0) data[i] ^= data[i - 1];
    }
    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = stride; i + 3 < size; i += 4)
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
    } else {
        for (i = stride; i < size; i++)
            data[i] ^= data[i - stride];
    }
}

// Публичные функции (реализованы в ds-stlz.c)
bool is_stlz_file(const char *filepath);
bool rg_stlz_get_dimensions(const char *filepath, int *width, int *height);


#ifdef __cplusplus
}
#endif

#endif /* DS_STLZ_H */
