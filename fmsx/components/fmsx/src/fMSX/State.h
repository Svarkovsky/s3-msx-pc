/** fMSX: portable MSX emulator ******************************/
/**                                                         **/
/**                         State.h                         **/
/**                                                         **/
/**   Save/Load emulation state with DS-LZ compression.     **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                      LICENSE NOTICE                     **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** This file is a combined work containing code from       **/
/** two independent authors under different licenses:       **/
/**                                                         **/
/** 1. SaveState / LoadState / SaveSTA / LoadSTA framework, **/
/**    all MSX-specific hardware structures (CPU, VDP, PSG, **/
/**    OPLL, SCC, memory mappers, I/O registers), and the   **/
/**    emulator integration macros (SaveSTRUCT, LoadSTRUCT, **/
/**    SaveARRAY, LoadARRAY, SaveDATA, LoadDATA, and their  **/
/**    STRIDE/Skip variants) are:                           **/
/**                                                         **/
/**    Copyright (C) Marat Fayzullin 1994-2021              **/
/**    All rights reserved. Commercial use prohibited       **/
/**    without explicit written permission.                 **/
/**                                                         **/
/** 2. DS-LZ (Delta-Stride LZ) compression engine:          **/
/**    - Compressor: write_lz77, find_best_match,           **/
/**      get_match_cost, fast_hash3, hash2_t                **/
/**    - Decompressor: read_lz77, skip_lz77                 **/
/**    - Preprocessor: delta_encode, delta_decode,          **/
/**      get_vram_block_type                                **/
/**    - I/O layer: buf_putc, buf_getc, buf_flush,          **/
/**      buf_write, buf_match_len, IO_BUF_SIZE              **/
/**    - All command format definitions and token tables    **/
/**                                                         **/
/**    Copyright (C) 2026 Ivan Svarkovsky                   **/
/**    Contact: ivansvarkovsky@gmail.com                    **/
/**                                                         **/
/**    Licensed under CC BY-NC-SA 4.0                       **/
/**    https://creativecommons.org/licenses/by-nc-sa/4.0/   **/
/**                                                         **/
/**    Attribution required. Non-commercial only.           **/
/**    ShareAlike: derivatives must use same license.       **/
/**                                                         **/
/**    Based on public-domain LZ77 (Lempel-Ziv, 1977).      **/
/**    All patents expired. Independent implementation.     **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** DS-LZ v2.2 — Delta-Stride LZ                            **/
/** Domain-optimized LZ77 for tile-based graphics           **/
/** Core: LZ77 + delta-XOR stride + rep-match + VLC         **/
/**                                                         **/
/** v2.2 changes:                                           **/
/**    - Vectorized delta (32-bit XOR, 1.5-2x faster)       **/
/**    - Improved hash function (sparse data distribution)  **/
/**                                                         **/
/*************************************************************/

#ifndef STATE_H
#define STATE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#else
#define heap_caps_malloc(s, caps) malloc(s)
#define MALLOC_CAP_SPIRAM 0
#endif

// =========================================================================
// === БУФЕРИЗОВАННЫЙ ВВОД/ВЫВОД ==========================================
// =========================================================================

#define IO_BUF_SIZE 4096

static FILE *StateFile = NULL;

static uint8_t out_buf[IO_BUF_SIZE];
static size_t  out_pos = 0;

static uint8_t in_buf[IO_BUF_SIZE];
static size_t  in_pos = 0;
static size_t  in_avail = 0;

static inline void buf_putc(uint8_t c) {
    out_buf[out_pos++] = c;
    if (out_pos >= IO_BUF_SIZE) {
        fwrite(out_buf, 1, out_pos, StateFile);
        out_pos = 0;
    }
}

static inline void buf_flush(void) {
    if (out_pos > 0 && StateFile) {
        fwrite(out_buf, 1, out_pos, StateFile);
        out_pos = 0;
    }
}

static inline void buf_write(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        buf_putc(p[i]);
    }
}

static inline int buf_getc(FILE *F) {
    (void)F;
    if (in_pos >= in_avail) {
        in_avail = fread(in_buf, 1, IO_BUF_SIZE, StateFile);
        in_pos = 0;
        if (in_avail == 0) return EOF;
    }
    return in_buf[in_pos++];
}

// =========================================================================
// === 32-БИТНОЕ УСКОРЕННОЕ СРАВНЕНИЕ =====================================
// =========================================================================

static inline uint32_t buf_match_len(const uint8_t *a, const uint8_t *b, uint32_t max_len) {
    uint32_t cur = 0;
    while (cur + 4 <= max_len && *(const uint32_t*)(a + cur) == *(const uint32_t*)(b + cur)) {
        cur += 4;
    }
    while (cur < max_len && a[cur] == b[cur]) cur++;
    return cur;
}

// =========================================================================
// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ДЕЛЬТА-КОДИРОВАНИЯ (XOR) ====================
// =========================================================================

static void delta_encode(unsigned char *data, unsigned int size, int stride) {
    if (size < stride) return;
    unsigned int i;

    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = size - 4; i >= stride; i -= 4) {
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
        }
    } else {
        for (i = size - 1; i >= stride; i--) {
            data[i] ^= data[i - stride];
        }
    }

    for (i = size - 1; i > 0; i--) {
        if (i % stride != 0) {
            data[i] ^= data[i - 1];
        }
    }
}

static void delta_decode(unsigned char *data, unsigned int size, int stride) {
    if (size < stride) return;
    unsigned int i;

    for (i = 1; i < size; i++) {
        if (i % stride != 0) {
            data[i] ^= data[i - 1];
        }
    }

    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = stride; i + 3 < size; i += 4) {
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
        }
    } else {
        for (i = stride; i < size; i++) {
            data[i] ^= data[i - stride];
        }
    }
}

static inline int get_vram_block_type(void) {
    if (ScrMode <= 5 || ScrMode == 13) return 1;
    return 2;
}

// =========================================================================
// === ДЕКОДЕР DS-LZ (0 байт heap RAM) ====================================
// =========================================================================

static int read_lz77(unsigned char *dst, unsigned int len, FILE *F) {
    (void)F;
    unsigned int i = 0;
    unsigned int last_offset = 0;

    while (i < len) {
        int cmd_val = buf_getc(StateFile);
        if (cmd_val == EOF) return 0;
        unsigned char cmd = (unsigned char)cmd_val;

        if (cmd <= 127) {
            unsigned int lit_len = cmd + 1;
            if (i + lit_len > len) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = buf_getc(StateFile);
                if (c == EOF) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        }
        else if (cmd <= 191) {
            unsigned int match_len = (cmd & 0x3F) + 3;
            int off = buf_getc(StateFile);
            if (off == EOF) return 0;
            unsigned int offset = (unsigned char)off;
            if (offset == 0 || offset > i || i + match_len > len) return 0;
            unsigned int src_pos = i - offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
            last_offset = offset;
        }
        else if (cmd <= 223) {
            unsigned int match_len = (cmd & 0x1F) + 4;
            int off_low = buf_getc(StateFile), off_high = buf_getc(StateFile);
            if (off_low == EOF || off_high == EOF) return 0;
            unsigned int offset = (unsigned char)off_low | ((unsigned char)off_high << 8);
            if (offset == 0 || offset > i || i + match_len > len) return 0;
            unsigned int src_pos = i - offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
            last_offset = offset;
        }
        else if (cmd <= 239) {
            unsigned int match_len = (cmd & 0x0F) + 3;
            if (last_offset == 0 || last_offset > i || i + match_len > len) return 0;
            unsigned int src_pos = i - last_offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
        }
        else if (cmd <= 247) {
            unsigned int lit_len = (cmd & 0x07) + 129;
            if (i + lit_len > len) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = buf_getc(StateFile);
                if (c == EOF) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        }
        else if (cmd == 248) {
            int sub = buf_getc(StateFile);
            if (sub == EOF) return 0;

            if (sub == 0) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF) return 0;
                unsigned int lit_len = ((unsigned char)len_low | ((unsigned char)len_high << 8)) + 137;
                if (i + lit_len > len) return 0;
                for (unsigned int j = 0; j < lit_len; j++) {
                    int c = buf_getc(StateFile);
                    if (c == EOF) return 0;
                    dst[i + j] = (unsigned char)c;
                }
                i += lit_len;
            }
            else if (sub == 1) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                int off_low = buf_getc(StateFile), off_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF || off_low == EOF || off_high == EOF) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                unsigned int offset = (unsigned char)off_low | ((unsigned char)off_high << 8);
                if (offset == 0 || offset > i || i + match_len > len) return 0;
                unsigned int src_pos = i - offset;
                for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
                i += match_len;
                last_offset = offset;
            }
            else if (sub == 2) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                if (last_offset == 0 || last_offset > i || i + match_len > len) return 0;
                unsigned int src_pos = i - last_offset;
                for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
                i += match_len;
            }
            else if (sub == 3) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                int off = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF || off == EOF) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                unsigned int offset = (unsigned char)off;
                if (offset == 0 || offset > i || i + match_len > len) return 0;
                unsigned int src_pos = i - offset;
                for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
                i += match_len;
                last_offset = offset;
            }
            else return 0;
        }
        else return 0;
    }
    return 1;
}

static int skip_lz77(unsigned int len, FILE *F) {
    (void)F;
    unsigned int i = 0;
    while (i < len) {
        int cmd_val = buf_getc(StateFile);
        if (cmd_val == EOF) return 0;
        unsigned char cmd = (unsigned char)cmd_val;

        if (cmd <= 127) {
            unsigned int lit_len = cmd + 1;
            for (unsigned int j = 0; j < lit_len; j++) if (buf_getc(StateFile) == EOF) return 0;
            i += lit_len;
        }
        else if (cmd <= 191) {
            if (buf_getc(StateFile) == EOF) return 0;
            i += (cmd & 0x3F) + 3;
        }
        else if (cmd <= 223) {
            if (buf_getc(StateFile) == EOF) return 0;
            if (buf_getc(StateFile) == EOF) return 0;
            i += (cmd & 0x1F) + 4;
        }
        else if (cmd <= 239) {
            i += (cmd & 0x0F) + 3;
        }
        else if (cmd <= 247) {
            unsigned int lit_len = (cmd & 0x07) + 129;
            for (unsigned int j = 0; j < lit_len; j++) if (buf_getc(StateFile) == EOF) return 0;
            i += lit_len;
        }
        else if (cmd == 248) {
            int sub = buf_getc(StateFile);
            if (sub == EOF) return 0;
            if (sub == 0) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF) return 0;
                unsigned int lit_len = ((unsigned char)len_low | ((unsigned char)len_high << 8)) + 137;
                for (unsigned int j = 0; j < lit_len; j++) if (buf_getc(StateFile) == EOF) return 0;
                i += lit_len;
            }
            else if (sub == 1) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                int off_low = buf_getc(StateFile), off_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF || off_low == EOF || off_high == EOF) return 0;
                i += (unsigned char)len_low | ((unsigned char)len_high << 8);
            }
            else if (sub == 2) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF) return 0;
                i += (unsigned char)len_low | ((unsigned char)len_high << 8);
            }
            else if (sub == 3) {
                int len_low = buf_getc(StateFile), len_high = buf_getc(StateFile);
                int off = buf_getc(StateFile);
                if (len_low == EOF || len_high == EOF || off == EOF) return 0;
                i += (unsigned char)len_low | ((unsigned char)len_high << 8);
            }
            else return 0;
        }
        else return 0;
    }
    return 1;
}

// =========================================================================
// === ОПТИМИЗИРОВАННЫЙ СЖИМАТЕЛЬ (32KB PSRAM) ============================
// =========================================================================

typedef struct { uint32_t pos[2]; } hash2_t;

static inline uint32_t fast_hash3(const uint8_t *p) {
    uint32_t h = (p[0] * 0x9E3779B1u) ^ (p[1] * 0x85EBCA77u) ^ p[2];
    return (h ^ (h >> 12)) & 4095;
}

static inline unsigned int get_match_cost(unsigned int len, unsigned int offset, int is_rep) {
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

static inline void find_best_match(const unsigned char *src, unsigned int pos, unsigned int len, 
                                   hash2_t *hash, uint32_t last_offset, int block_type,
                                   uint32_t *out_len, uint32_t *out_off, int *out_is_rep) {
    uint32_t best_len = 0;
    uint32_t best_off = 0;
    int is_rep = 0;

    // === 1. PRE-EMPTIVE STRIDE REP-MATCH FOR VRAM ===
    if (block_type == 1 && pos >= 128) {
        uint32_t voff = 128;
        uint32_t prev = pos - voff;
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
        if (cur >= 3 && cur > best_len) {
            best_len = cur; 
            best_off = voff; 
            is_rep = (voff == last_offset) ? 1 : 0;
        }
    }
    else if (block_type == 2 && pos >= 256) {
        uint32_t voff = 256;
        uint32_t prev = pos - voff;
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
        if (cur >= 3 && cur > best_len) {
            best_len = cur; 
            best_off = voff; 
            is_rep = (voff == last_offset) ? 1 : 0;
        }
    }

    // === 2. HASH-BASED SEARCH ===
    if (pos + 3 <= len) {
        uint32_t h = fast_hash3(&src[pos]);
        for (int e = 0; e < 2; e++) {
            uint32_t prev = hash[pos & 1 ? h : (h ^ 1)].pos[e];
            if (prev != 0xFFFFFFFF && pos > prev && (pos - prev) <= 65535) {
                uint32_t off = pos - prev;
                uint32_t max_match = len - pos;
                if (max_match > 65535) max_match = 65535;
                uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);

                unsigned int min_needed = (off <= 255) ? 3 : 4;
                if (cur >= min_needed && cur > best_len) {
                    best_len = cur;
                    best_off = off;
                    is_rep = 0;
                }
            }
        }
    }

    // === 3. LAST-OFFSET REP-MATCH ===
    if (last_offset > 0 && last_offset <= pos) {
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t prev = pos - last_offset;
        uint32_t rep_len = buf_match_len(&src[pos], &src[prev], max_match);

        if (rep_len >= 3) {
            int cost_rep = get_match_cost(rep_len, last_offset, 1);
            int cost_best = get_match_cost(best_len, best_off, is_rep);
            int save_rep = (int)rep_len - cost_rep;
            int save_best = (int)best_len - cost_best;

            int is_stride_match = (block_type == 1 && last_offset == 128) ||
                                  (block_type == 2 && last_offset == 256);

            if (save_rep > save_best || (save_rep == save_best && is_stride_match)) {
                best_len = rep_len;
                best_off = last_offset;
                is_rep = 1;
            }
        }
    }

    *out_len = best_len;
    *out_off = best_off;
    *out_is_rep = is_rep;
}

static int write_lz77(const unsigned char *src, unsigned int len, FILE *F, int block_type) {
    (void)F;

    hash2_t *hash = heap_caps_malloc(4096 * sizeof(hash2_t), MALLOC_CAP_SPIRAM);
    if (!hash) return 0;
    memset(hash, 0xFF, 4096 * sizeof(hash2_t));

    uint32_t last_offset = 0;
    unsigned int i = 0;
    unsigned int lit_start = 0;

    #define FLUSH_LITS() do { \
        unsigned int _ll = i - lit_start; \
        while (_ll > 0) { \
            unsigned int _chunk = (_ll > 65535 + 137) ? 65535 + 137 : _ll; \
            if (_chunk <= 128) { \
                buf_putc((uint8_t)(_chunk - 1)); \
            } else if (_chunk <= 136) { \
                buf_putc((uint8_t)(240 | (_chunk - 129))); \
            } else { \
                buf_putc(248); buf_putc(0); \
                unsigned int _ext_len = _chunk - 137; \
                buf_putc((uint8_t)(_ext_len & 0xFF)); \
                buf_putc((uint8_t)((_ext_len >> 8) & 0xFF)); \
            } \
            for (unsigned int _j = 0; _j < _chunk; _j++) buf_putc(src[lit_start + _j]); \
            lit_start += _chunk; _ll -= _chunk; \
        } \
    } while(0)

    #define SKIP_LAZY_THRESHOLD 16

    while (i < len) {
        uint32_t best_len = 0;
        uint32_t best_off = 0;
        int is_rep = 0;

        find_best_match(src, i, len, hash, last_offset, block_type, &best_len, &best_off, &is_rep);

        if (best_len >= 3 && best_len < SKIP_LAZY_THRESHOLD && i + 1 < len) {
            uint32_t lazy_len = 0, lazy_off = 0;
            int lazy_is_rep = 0;

            find_best_match(src, i + 1, len, hash, last_offset, block_type, &lazy_len, &lazy_off, &lazy_is_rep);

            if (lazy_len >= 3) {
                int cost_best = get_match_cost(best_len, best_off, is_rep);
                int cost_lazy = get_match_cost(lazy_len, lazy_off, lazy_is_rep);

                int save_best = (int)best_len - cost_best;
                int save_lazy = (int)lazy_len - cost_lazy;

                if (save_lazy > save_best) {
                    best_len = 0;
                }
            }
        }

        if (best_len >= 3) {
            FLUSH_LITS();

            if (is_rep) {
                if (best_len <= 18) buf_putc((uint8_t)(0xE0 | (best_len - 3)));
                else {
                    buf_putc(248); buf_putc(2);
                    buf_putc((uint8_t)(best_len & 0xFF)); buf_putc((uint8_t)((best_len >> 8) & 0xFF));
                }
            } else if (best_off <= 255 && best_len <= 66) {
                buf_putc((uint8_t)(0x80 | (best_len - 3)));
                buf_putc((uint8_t)(best_off & 0xFF));
            } else if (best_len <= 35) {
                buf_putc((uint8_t)(0xC0 | (best_len - 4)));
                buf_putc((uint8_t)(best_off & 0xFF)); buf_putc((uint8_t)((best_off >> 8) & 0xFF));
            } else {
                buf_putc(248); buf_putc(1);
                buf_putc((uint8_t)(best_len & 0xFF)); buf_putc((uint8_t)((best_len >> 8) & 0xFF));
                buf_putc((uint8_t)(best_off & 0xFF)); buf_putc((uint8_t)((best_off >> 8) & 0xFF));
            }
            last_offset = best_off;

            unsigned int update_limit = (best_len > 4) ? 4 : best_len;
            for (unsigned int j = 0; j < update_limit; j++) {
                if (i + j + 3 <= len) {
                    uint32_t h = fast_hash3(&src[i+j]);
                    hash[h].pos[1] = hash[h].pos[0];
                    hash[h].pos[0] = i + j;
                }
            }
            i += best_len;
            lit_start = i;
        } else {
            if (i + 3 <= len) {
                uint32_t h = fast_hash3(&src[i]);
                hash[h].pos[1] = hash[h].pos[0];
                hash[h].pos[0] = i;
            }
            i++;
        }
    }

    FLUSH_LITS();
    free(hash);
    #undef FLUSH_LITS
    #undef SKIP_LAZY_THRESHOLD
    return 1;
}

// =========================================================================
// === УМНЫЕ МАКРОСЫ ======================================================
// =========================================================================

#define SaveSTRUCT(Name) \
  if (StateFile) { buf_write(&(Name), sizeof(Name)); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(Buf+Size,&(Name),sizeof(Name));Size+=sizeof(Name); } }

#define SaveARRAY(Name) \
  if (StateFile) { buf_write((Name), sizeof(Name)); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(Buf+Size,(Name),sizeof(Name));Size+=sizeof(Name); } }

#define SaveDATA_STRIDE(Name, DataSize, BlockType) \
  if (StateFile) { \
    int block_type = (BlockType); int stride = 0, use_delta = 0; \
    if (block_type == 1) { stride = 128; use_delta = 1; } \
    else if (block_type == 2) { stride = 256; use_delta = 1; } \
    buf_putc((uint8_t)block_type); \
    if (use_delta) delta_encode((unsigned char *)(Name), (DataSize), stride); \
    if (!write_lz77((const unsigned char *)(Name), (DataSize), StateFile, block_type)) return(0); \
    if (use_delta) delta_decode((unsigned char *)(Name), (DataSize), stride); \
  } else { \
    if (Size+(DataSize)>MaxSize) return(0); \
    else { memcpy(Buf+Size,(Name),(DataSize));Size+=(DataSize); } \
  }

#define SaveDATA(Name,DataSize) SaveDATA_STRIDE(Name,DataSize,0)

#define LoadSTRUCT(Name) \
  if (StateFile) { if (fread(&(Name), 1, sizeof(Name), StateFile) != sizeof(Name)) return(0); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(&(Name),Buf+Size,sizeof(Name));Size+=sizeof(Name); } }

#define SkipSTRUCT(Name) \
  if (StateFile) { \
    if (fseek(StateFile, sizeof(Name), SEEK_CUR) != 0) { \
      for (unsigned int _j = 0; _j < sizeof(Name); _j++) buf_getc(StateFile); \
    } \
  } else { if (Size+sizeof(Name)>MaxSize) return(0); else Size+=sizeof(Name); }

#define LoadARRAY(Name) \
  if (StateFile) { if (fread((Name), 1, sizeof(Name), StateFile) != sizeof(Name)) return(0); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy((Name),Buf+Size,sizeof(Name));Size+=sizeof(Name); } }

#define LoadDATA_STRIDE(Name, DataSize) \
  if (StateFile) { \
    int block_type = buf_getc(StateFile); int stride = 0, use_delta = 0; \
    if (block_type == EOF) return(0); \
    if (block_type == 1) { stride = 128; use_delta = 1; } \
    else if (block_type == 2) { stride = 256; use_delta = 1; } \
    if (!read_lz77((unsigned char *)(Name), (DataSize), StateFile)) return(0); \
    if (use_delta && stride > 0) delta_decode((unsigned char *)(Name), (DataSize), stride); \
  } else { \
    if (Size+(DataSize)>MaxSize) return(0); \
    else { memcpy((Name),Buf+Size,(DataSize));Size+=(DataSize); } \
  }

#define LoadDATA(Name,DataSize) LoadDATA_STRIDE(Name,DataSize)

#define SkipDATA_STRIDE(DataSize) \
  if (StateFile) { \
    int block_type = buf_getc(StateFile); \
    if (block_type == EOF) return(0); \
    if (!skip_lz77((DataSize), StateFile)) return(0); \
  } else { if (Size+(DataSize)>MaxSize) return(0); else Size+=(DataSize); }

#define SkipDATA(DataSize) SkipDATA_STRIDE(DataSize)

// =========================================================================
// === SaveState / LoadState ==============================================
// =========================================================================

unsigned int SaveState(unsigned char *Buf,unsigned int MaxSize)
{
  unsigned int State[256],Size;
  int J,I,K;

  Size = 0;
  J=0;
  memset(State,0,sizeof(State));
  State[J++] = VDPData;
  State[J++] = PLatch;
  State[J++] = ALatch;
  State[J++] = VAddr;
  State[J++] = VKey;
  State[J++] = PKey;
  State[J++] = 0;
  State[J++] = IRQPending;
  State[J++] = ScanLine;
  State[J++] = RTCReg;
  State[J++] = RTCMode;
  State[J++] = KanLetter;
  State[J++] = KanCount;
  State[J++] = IOReg;
  State[J++] = PSLReg;
  State[J++] = FMPACKey;

  for(I=0;I<4;++I)
  {
    State[J++] = SSLReg[I];
    State[J++] = PSL[I];
    State[J++] = SSL[I];
    State[J++] = EnWrite[I];
    State[J++] = RAMMapper[I];
  }  

  for(I=0;I<MAXSLOTS;++I)
  {
    State[J++] = ROMType[I];
    for(K=0;K<4;++K) State[J++]=ROMMapper[I][K];
  }

  SaveSTRUCT(CPU);
  SaveSTRUCT(PPI);
  SaveSTRUCT(VDP);
  SaveARRAY(VDPStatus);
  SaveARRAY(Palette);
  SaveSTRUCT(PSG);
  SaveSTRUCT(OPLL);
  SaveSTRUCT(SCChip);
  SaveARRAY(State);

  SaveDATA_STRIDE(RAMData, RAMPages*0x4000, 0); 
  SaveDATA_STRIDE(VRAM, VRAMPages*0x4000, get_vram_block_type());

  return(StateFile ? 1 : Size);
}

unsigned int LoadState(unsigned char *Buf,unsigned int MaxSize)
{
  int State[256],J,I,K;
  unsigned int Size;

  Size = 0;
  LoadSTRUCT(CPU);
  LoadSTRUCT(PPI);
  LoadSTRUCT(VDP);
  LoadARRAY(VDPStatus);
  LoadARRAY(Palette);
  LoadSTRUCT(PSG);
  LoadSTRUCT(OPLL);
  LoadSTRUCT(SCChip);
  LoadARRAY(State);

  LoadDATA_STRIDE(RAMData, RAMPages*0x4000);
  LoadDATA_STRIDE(VRAM, VRAMPages*0x4000);

  J=0;
  VDPData    = State[J++];
  PLatch     = State[J++];
  ALatch     = State[J++];
  VAddr      = State[J++];
  VKey       = State[J++];
  PKey       = State[J++];
  J++;
  IRQPending = State[J++];
  ScanLine   = State[J++];
  RTCReg     = State[J++];
  RTCMode    = State[J++];
  KanLetter  = State[J++];
  KanCount   = State[J++];
  IOReg      = State[J++];
  PSLReg     = State[J++];
  FMPACKey   = State[J++];

  for(I=0;I<4;++I)
  {
    SSLReg[I]       = State[J++];
    PSL[I]          = State[J++];
    SSL[I]          = State[J++];
    EnWrite[I]      = State[J++];
    RAMMapper[I]    = State[J++];
  }  

  for(I=0;I<MAXSLOTS;++I)
  {
    ROMType[I] = State[J++];
    for(K=0;K<4;++K) ROMMapper[I][K]=State[J++];

    if(ROMType[I]==MAP_FMPAC)
      ROMMapper[I][1]=ROMMapper[I][0]|1;
    else if((ROMType[I]==MAP_ASCII16)||(ROMType[I]==MAP_GEN16))
    {
      ROMMapper[I][1]=ROMMapper[I][0]|1;
      ROMMapper[I][3]=ROMMapper[I][2]|1;
    }
  }

  if(RAMMask)
    for(I=0;I<4;++I)
    {
      RAMMapper[I]       &= RAMMask;
      MemMap[3][2][I*2]   = RAMData+RAMMapper[I]*0x4000;
      MemMap[3][2][I*2+1] = MemMap[3][2][I*2]+0x2000;
    }

  for(I=0;I<MAXSLOTS;++I)
    if(ROMData[I]&&ROMMask[I])
      SetMegaROM(I,ROMMapper[I][0],ROMMapper[I][1],ROMMapper[I][2],ROMMapper[I][3]);

  for(I=0;I<4;++I)
  {
    RAM[2*I]   = MemMap[PSL[I]][SSL[I]][2*I];
    RAM[2*I+1] = MemMap[PSL[I]][SSL[I]][2*I+1];
  }

  for(I=0;I<16;++I)
    SetColor(I,(Palette[I]>>16)&0xFF,(Palette[I]>>8)&0xFF,Palette[I]&0xFF);

  SetScreen();

  VPAGE    = VRAM+((int)VDP[14]<<14);
  FGColor  = VDP[7]>>4;
  BGColor  = VDP[7]&0x0F;
  XFGColor = FGColor;
  XBGColor = BGColor;

  PSG.Changed     = (1<<AY8910_CHANNELS)-1;
  SCChip.Changed  = (1<<SCC_CHANNELS)-1;
  SCChip.WChanged = (1<<SCC_CHANNELS)-1;
  OPLL.Changed    = (1<<YM2413_CHANNELS)-1;
  OPLL.PChanged   = (1<<YM2413_CHANNELS)-1;
  OPLL.DChanged   = (1<<YM2413_CHANNELS)-1;

  return(StateFile ? 1 : Size);
}

// =========================================================================
// === SaveSTA / LoadSTA ==================================================
// =========================================================================

int SaveSTA(const char *Name)
{
  static byte Header[16] = "STE\032\003\0\0\0\0\0\0\0\0\0\0\0";
  unsigned int J,Size;
  FILE *F;

  if(!Name) return(0);
  F = fopen(Name,"wb");
  if(!F) return(0);

  J=StateID();
  Header[5] = RAMPages;
  Header[6] = VRAMPages;
  Header[7] = J&0x00FF;
  Header[8] = J>>8;
  Header[9] = 2;

  StateFile = F;
  out_pos = 0;
  in_pos = 0;
  in_avail = 0;

  buf_write(Header, 16);
  Size = SaveState(NULL, 0);
  buf_flush();

  StateFile = NULL;

  if(!Size) { fclose(F); unlink(Name); return 0; }
  fclose(F);
  return 1;
}

int LoadSTA(const char *Name)
{
  int Size,OldMode,OldRAMPages,OldVRAMPages;
  byte Header[16];
  FILE *F;

  if(!Name) return(0);
  if(!(F=fopen(Name,"rb"))) return(0);

  if(fread(Header,1,16,F)!=16)           { fclose(F);return(0); }
  if(memcmp(Header,"STE\032\003",5))     { fclose(F);return(0); }
  if(Header[7]+Header[8]*256!=StateID()) { fclose(F);return(0); }
  if((Header[5]!=(RAMPages&0xFF))||(Header[6]!=(VRAMPages&0xFF)))
  { fclose(F);return(0); }

  if (Header[9] < 2) { fclose(F); return 0; }

  in_pos = 0;
  in_avail = 0;

  OldMode      = Mode;
  OldRAMPages  = RAMPages;
  OldVRAMPages = VRAMPages;

  StateFile = F;
  Size = LoadState(NULL, 0);
  StateFile = NULL;

  if(!Size) ResetMSX(OldMode,OldRAMPages,OldVRAMPages);
  fclose(F);
  return(!!Size);
}

#endif /* STATE_H */
