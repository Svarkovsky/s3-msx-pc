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
/** 1. SaveState / LoadState / SaveSTA / LoadSTA framework, **/
/**    Copyright (C) Marat Fayzullin 1994-2021              **/
/**                                                         **/
/** 2. DS-LZ (Delta-Stride LZ) compression engine:          **/
/**    Copyright (C) 2026 Ivan Svarkovsky                   **/
/**    Licensed under CC BY-NC-SA 4.0                       **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** DS-LZ v2.5a — DRAM Hash (VGA buffer) + Signature Filter **/
/**                                                         **/
/**   Zero heap allocations. Zero PSRAM for hash.           **/
/**   Backward compatible V3 format.                        **/
/**                                                         **/
/*************************************************************/

#ifndef STATE_H
#define STATE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#else
#define heap_caps_malloc(s, caps) malloc(s)
#define MALLOC_CAP_SPIRAM 0
#endif

#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

extern int RAMPages2;
extern unsigned char *RAMData2;
extern unsigned char RAMMapper2[4];
extern int StateVersion;

// =========================================================================
// === БУФЕРИЗОВАННЫЙ ВВОД/ВЫВОД (256 байт, unified, 16-byte aligned) =====
// =========================================================================

#define IO_BUF_SIZE 256

static int StateFD = -1;

static uint8_t io_buf[IO_BUF_SIZE] __attribute__((aligned(16)));
static size_t  io_pos = 0;
static size_t  io_avail = 0;

static inline void buf_putc(uint8_t c) {
    io_buf[io_pos++] = c;
    if (UNLIKELY(io_pos >= IO_BUF_SIZE)) {
        if (StateFD >= 0) write(StateFD, io_buf, io_pos);
        io_pos = 0;
    }
}

static inline void buf_flush(void) {
    if (io_pos > 0 && StateFD >= 0) {
        write(StateFD, io_buf, io_pos);
        io_pos = 0;
    }
}

static inline void buf_write(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        size_t space = IO_BUF_SIZE - io_pos;
        size_t chunk = (len < space) ? len : space;
        memcpy(io_buf + io_pos, p, chunk);
        io_pos += chunk;
        p += chunk;
        len -= chunk;
        if (UNLIKELY(io_pos >= IO_BUF_SIZE)) {
            if (StateFD >= 0) write(StateFD, io_buf, io_pos);
            io_pos = 0;
        }
    }
}

static inline int buf_getc(void) {
    if (UNLIKELY(io_pos >= io_avail)) {
        if (StateFD >= 0) {
            int r = read(StateFD, io_buf, IO_BUF_SIZE);
            io_avail = (r > 0) ? (size_t)r : 0;
        } else {
            io_avail = 0;
        }
        io_pos = 0;
        if (UNLIKELY(io_avail == 0)) return EOF;
    }
    return io_buf[io_pos++];
}

// =========================================================================
// === 32-БИТНОЕ УСКОРЕННОЕ СРАВНЕНИЕ =====================================
// =========================================================================

static inline uint32_t buf_match_len(const uint8_t *a, const uint8_t *b, uint32_t max_len) {
    uint32_t cur = 0;
    while (cur + 4 <= max_len && *(const uint32_t*)(a + cur) == *(const uint32_t*)(b + cur)) cur += 4;
    while (cur < max_len && a[cur] == b[cur]) cur++;
    return cur;
}

// =========================================================================
// === ДЕЛЬТА-КОДИРОВАНИЕ (XOR, без %) ====================================
// =========================================================================

static void delta_encode(unsigned char *data, unsigned int size, int stride) {
    if (size < (unsigned int)stride || stride <= 0) return;
    unsigned int i;

    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = size - 4; i >= (unsigned int)stride; i -= 4)
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
    } else {
        for (i = size - 1; i >= (unsigned int)stride; i--)
            data[i] ^= data[i - stride];
    }

    for (unsigned int row = 0; row < size; row += stride) {
        unsigned int row_end = (row + stride < size) ? row + stride : size;
        for (i = row_end - 1; i > row; i--)
            data[i] ^= data[i - 1];
    }
}

static void delta_decode(unsigned char *data, unsigned int size, int stride) {
    if (size < (unsigned int)stride || stride <= 0) return;
    unsigned int i;

    for (unsigned int row = 0; row < size; row += stride) {
        unsigned int row_end = (row + stride < size) ? row + stride : size;
        for (i = row + 1; i < row_end; i++)
            data[i] ^= data[i - 1];
    }

    if (stride % 4 == 0 && size % 4 == 0 && ((uintptr_t)data % 4) == 0) {
        for (i = stride; i + 3 < size; i += 4)
            *(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
    } else {
        for (i = stride; i < size; i++)
            data[i] ^= data[i - stride];
    }
}

static inline int get_vram_block_type(void) {
    if (ScrMode <= 5 || ScrMode == 13) return 1;
    return 2;
}

// =========================================================================
// === ДЕКОДЕР DS-LZ ======================================================
// =========================================================================

static int read_lz77(unsigned char *dst, unsigned int len) {
    unsigned int i = 0;
    unsigned int last_offset = 0;

    while (i < len) {
        int cmd_val = buf_getc();
        if (UNLIKELY(cmd_val == EOF)) return 0;
        unsigned char cmd = (unsigned char)cmd_val;

        if (cmd <= 127) {
            unsigned int lit_len = cmd + 1;
            if (UNLIKELY(i + lit_len > len)) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = buf_getc();
                if (UNLIKELY(c == EOF)) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        }
        else if (cmd <= 191) {
            unsigned int match_len = (cmd & 0x3F) + 3;
            int off = buf_getc();
            if (UNLIKELY(off == EOF)) return 0;
            unsigned int offset = (unsigned char)off;
            if (UNLIKELY(offset == 0 || offset > i || i + match_len > len)) return 0;
            unsigned int src_pos = i - offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
            last_offset = offset;
        }
        else if (cmd <= 223) {
            unsigned int match_len = (cmd & 0x1F) + 4;
            int off_low = buf_getc(), off_high = buf_getc();
            if (UNLIKELY(off_low == EOF || off_high == EOF)) return 0;
            unsigned int offset = (unsigned char)off_low | ((unsigned char)off_high << 8);
            if (UNLIKELY(offset == 0 || offset > i || i + match_len > len)) return 0;
            unsigned int src_pos = i - offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
            last_offset = offset;
        }
        else if (cmd <= 239) {
            unsigned int match_len = (cmd & 0x0F) + 3;
            if (UNLIKELY(last_offset == 0 || last_offset > i || i + match_len > len)) return 0;
            unsigned int src_pos = i - last_offset;
            for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
            i += match_len;
        }
        else if (cmd <= 247) {
            unsigned int lit_len = (cmd & 0x07) + 129;
            if (UNLIKELY(i + lit_len > len)) return 0;
            for (unsigned int j = 0; j < lit_len; j++) {
                int c = buf_getc();
                if (UNLIKELY(c == EOF)) return 0;
                dst[i + j] = (unsigned char)c;
            }
            i += lit_len;
        }
        else if (cmd == 248) {
            int sub = buf_getc();
            if (UNLIKELY(sub == EOF)) return 0;
            if (sub == 0) {
                int len_low = buf_getc(), len_high = buf_getc();
                if (UNLIKELY(len_low == EOF || len_high == EOF)) return 0;
                unsigned int lit_len = ((unsigned char)len_low | ((unsigned char)len_high << 8)) + 137;
                if (UNLIKELY(i + lit_len > len)) return 0;
                for (unsigned int j = 0; j < lit_len; j++) {
                    int c = buf_getc();
                    if (UNLIKELY(c == EOF)) return 0;
                    dst[i + j] = (unsigned char)c;
                }
                i += lit_len;
            }
            else if (sub == 1) {
                int len_low = buf_getc(), len_high = buf_getc();
                int off_low = buf_getc(), off_high = buf_getc();
                if (UNLIKELY(len_low == EOF || len_high == EOF || off_low == EOF || off_high == EOF)) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                unsigned int offset = (unsigned char)off_low | ((unsigned char)off_high << 8);
                if (UNLIKELY(offset == 0 || offset > i || i + match_len > len)) return 0;
                unsigned int src_pos = i - offset;
                for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
                i += match_len;
                last_offset = offset;
            }
            else if (sub == 2) {
                int len_low = buf_getc(), len_high = buf_getc();
                if (UNLIKELY(len_low == EOF || len_high == EOF)) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                if (UNLIKELY(last_offset == 0 || last_offset > i || i + match_len > len)) return 0;
                unsigned int src_pos = i - last_offset;
                for (unsigned int j = 0; j < match_len; j++) dst[i + j] = dst[src_pos + j];
                i += match_len;
            }
            else if (sub == 3) {
                int len_low = buf_getc(), len_high = buf_getc();
                int off = buf_getc();
                if (UNLIKELY(len_low == EOF || len_high == EOF || off == EOF)) return 0;
                unsigned int match_len = (unsigned char)len_low | ((unsigned char)len_high << 8);
                unsigned int offset = (unsigned char)off;
                if (UNLIKELY(offset == 0 || offset > i || i + match_len > len)) return 0;
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

// =========================================================================
// === СЖИМАТЕЛЬ (DRAM Hash 4KB via VGA buffer + Signature Filter) =========
// =========================================================================

#define HASH_ENTRIES 512
#define HASH_MASK    511
#define HASH_SHIFT   23

typedef struct { uint32_t pos[2]; } hash2_t;

extern uint16_t sram_render_buffer[];
extern bool rg_display_sync(bool block);

/* Knuth multiplicative hash */
static inline uint32_t fast_hash3(const uint8_t *p) {
    uint32_t val = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (val * 0x1E35A7BDu) >> HASH_SHIFT;
}

/* 16-bit signature filter */
static inline uint32_t get_sig16(const uint8_t *p) {
    uint32_t val = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return ((val * 0x85EBCA77u) >> 16) & 0xFFFF;
}

/* Recover absolute position from 16-bit tail */
static inline uint32_t recover_pos(uint32_t pos, uint32_t low16) {
    uint32_t prev = (pos & ~0xFFFFu) | low16;
    if (prev > pos) {
        if (prev < 0x10000u) return 0xFFFFFFFFu;
        prev -= 0x10000u;
    }
    return prev;
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
    uint32_t best_len = 0, best_off = 0;
    int is_rep = 0;

    /* Fixed-offset VRAM patterns */
    if (block_type == 1 && pos >= 128) {
        uint32_t voff = 128, prev = pos - voff;
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
        if (cur >= 3 && cur > best_len) {
            best_len = cur; best_off = voff; 
            is_rep = (voff == last_offset) ? 1 : 0;
        }
    }
    else if (block_type == 2 && pos >= 256) {
        uint32_t voff = 256, prev = pos - voff;
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
        unsigned int min_needed = (voff == last_offset) ? 3 : 4;
        if (cur >= min_needed && cur > best_len) {
            best_len = cur; best_off = voff;
            is_rep = (voff == last_offset) ? 1 : 0;
        }
    }

    /* Signature-filtered hash search (DRAM, 1 cycle per lookup) */
    if (pos + 3 <= len) {
        uint32_t h = fast_hash3(&src[pos]);
        uint32_t cur_sig = get_sig16(&src[pos]);

        /* Linear bucket: both entries in one DRAM cache line */
        uint32_t e0 = hash[h].pos[0];
        uint32_t e1 = hash[h].pos[1];

        if (e0 != 0xFFFFFFFF && (e0 >> 16) == cur_sig) {
            uint32_t prev = recover_pos(pos, e0 & 0xFFFF);
            if (prev != 0xFFFFFFFF && pos > prev && (pos - prev) <= 65535) {
                uint32_t off = pos - prev;
                uint32_t max_match = len - pos;
                if (max_match > 65535) max_match = 65535;
                uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
                unsigned int min_needed = (off <= 255) ? 3 : 4;
                if (cur >= min_needed && cur > best_len) {
                    best_len = cur; best_off = off; is_rep = 0;
                }
            }
        }

        if (e1 != 0xFFFFFFFF && (e1 >> 16) == cur_sig) {
            uint32_t prev = recover_pos(pos, e1 & 0xFFFF);
            if (prev != 0xFFFFFFFF && pos > prev && (pos - prev) <= 65535) {
                uint32_t off = pos - prev;
                uint32_t max_match = len - pos;
                if (max_match > 65535) max_match = 65535;
                uint32_t cur = buf_match_len(&src[pos], &src[prev], max_match);
                unsigned int min_needed = (off <= 255) ? 3 : 4;
                if (cur >= min_needed && cur > best_len) {
                    best_len = cur; best_off = off; is_rep = 0;
                }
            }
        }
    }

    /* Repeat-offset */
    if (last_offset > 0 && last_offset <= pos) {
        uint32_t max_match = len - pos;
        if (max_match > 65535) max_match = 65535;
        uint32_t prev = pos - last_offset;
        uint32_t rep_len = buf_match_len(&src[pos], &src[prev], max_match);
        if (rep_len >= 3) {
            int cost_rep = get_match_cost(rep_len, last_offset, 1);
            int cost_best = get_match_cost(best_len, best_off, is_rep);
            if ((int)rep_len - cost_rep > (int)best_len - cost_best) {
                best_len = rep_len; best_off = last_offset; is_rep = 1;
            }
        }
    }

    *out_len = best_len; *out_off = best_off; *out_is_rep = is_rep;
}

static int write_lz77(const unsigned char *src, unsigned int len, int block_type) {
    /* Wait for VGA to finish, then steal its buffer for hash table */
    rg_display_sync(true);
    hash2_t *hash = (hash2_t *)sram_render_buffer;
    memset(hash, 0xFF, HASH_ENTRIES * sizeof(hash2_t));

    uint32_t last_offset = 0;
    unsigned int i = 0, lit_start = 0;

    #define FLUSH_LITS() do { \
        unsigned int _ll = i - lit_start; \
        while (_ll > 0) { \
            unsigned int _chunk = (_ll > 65535 + 137) ? 65535 + 137 : _ll; \
            if (_chunk <= 128) buf_putc((uint8_t)(_chunk - 1)); \
            else if (_chunk <= 136) buf_putc((uint8_t)(240 | (_chunk - 129))); \
            else { buf_putc(248); buf_putc(0); \
                unsigned int _ext_len = _chunk - 137; \
                buf_putc((uint8_t)(_ext_len & 0xFF)); buf_putc((uint8_t)((_ext_len >> 8) & 0xFF)); } \
            buf_write(&src[lit_start], _chunk); \
            lit_start += _chunk; _ll -= _chunk; \
        } \
    } while(0)

    #define SKIP_LAZY_THRESHOLD 16

    while (i < len) {
        uint32_t best_len = 0, best_off = 0;
        int is_rep = 0;
        find_best_match(src, i, len, hash, last_offset, block_type, &best_len, &best_off, &is_rep);

        if (best_len >= 3 && best_len < SKIP_LAZY_THRESHOLD && i + 1 < len) {
            uint32_t lazy_len = 0, lazy_off = 0;
            int lazy_is_rep = 0;
            find_best_match(src, i + 1, len, hash, last_offset, block_type, &lazy_len, &lazy_off, &lazy_is_rep);
            if (lazy_len >= 3 && (int)lazy_len - get_match_cost(lazy_len, lazy_off, lazy_is_rep) >
                (int)best_len - get_match_cost(best_len, best_off, is_rep)) best_len = 0;
        }

        if (best_len >= 3) {
            FLUSH_LITS();
            if (is_rep) {
                if (best_len <= 18) buf_putc((uint8_t)(0xE0 | (best_len - 3)));
                else { buf_putc(248); buf_putc(2);
                    buf_putc((uint8_t)(best_len & 0xFF)); buf_putc((uint8_t)((best_len >> 8) & 0xFF)); }
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
                    uint32_t sig = get_sig16(&src[i+j]);
                    uint32_t val = ((i + j) & 0xFFFFu) | (sig << 16);
                    hash[h].pos[1] = hash[h].pos[0];
                    hash[h].pos[0] = val;
                }
            }
            i += best_len; lit_start = i;
        } else {
            if (i + 3 <= len) {
                uint32_t h = fast_hash3(&src[i]);
                uint32_t sig = get_sig16(&src[i]);
                uint32_t val = (i & 0xFFFFu) | (sig << 16);
                hash[h].pos[1] = hash[h].pos[0];
                hash[h].pos[0] = val;
            }
            i++;
        }
    }
    FLUSH_LITS();
    #undef FLUSH_LITS
    #undef SKIP_LAZY_THRESHOLD
    return 1;
}

// =========================================================================
// === МАКРОСЫ ============================================================
// =========================================================================

#define SaveSTRUCT(Name) \
  if (StateFD >= 0) { buf_write(&(Name), sizeof(Name)); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(Buf+Size,&(Name),sizeof(Name));Size+=sizeof(Name); } }

#define SaveARRAY(Name) \
  if (StateFD >= 0) { buf_write((Name), sizeof(Name)); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(Buf+Size,(Name),sizeof(Name));Size+=sizeof(Name); } }

#define SaveDATA_STRIDE(Name, DataSize, BlockType) \
  if (StateFD >= 0) { \
    int block_type = (BlockType); int stride = 0, use_delta = 0; \
    if (block_type == 1) { stride = 128; use_delta = 1; } \
    else if (block_type == 2) { stride = 256; use_delta = 1; } \
    buf_putc((uint8_t)block_type); \
    if (use_delta) delta_encode((unsigned char *)(Name), (DataSize), stride); \
    if (!write_lz77((const unsigned char *)(Name), (DataSize), block_type)) return(0); \
    if (use_delta) delta_decode((unsigned char *)(Name), (DataSize), stride); \
  } else { \
    if (Size+(DataSize)>MaxSize) return(0); \
    else { memcpy(Buf+Size,(Name),(DataSize));Size+=(DataSize); } \
  }

#define SaveDATA(Name,DataSize) SaveDATA_STRIDE(Name,DataSize,0)

#define LoadSTRUCT(Name) \
  if (StateFD >= 0) { if (read(StateFD, &(Name), sizeof(Name)) != sizeof(Name)) return(0); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy(&(Name),Buf+Size,sizeof(Name));Size+=sizeof(Name); } }

#define SkipSTRUCT(Name) \
  if (StateFD >= 0) { for (unsigned int _j = 0; _j < sizeof(Name); _j++) buf_getc(); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); else Size+=sizeof(Name); }

#define LoadARRAY(Name) \
  if (StateFD >= 0) { if (read(StateFD, (Name), sizeof(Name)) != sizeof(Name)) return(0); } \
  else { if (Size+sizeof(Name)>MaxSize) return(0); \
         else { memcpy((Name),Buf+Size,sizeof(Name));Size+=sizeof(Name); } }

#define LoadDATA_STRIDE(Name, DataSize) \
  if (StateFD >= 0) { \
    int block_type = buf_getc(); int stride = 0, use_delta = 0; \
    if (block_type == EOF) return(0); \
    if (block_type == 1) { stride = 128; use_delta = 1; } \
    else if (block_type == 2) { stride = 256; use_delta = 1; } \
    if (!read_lz77((unsigned char *)(Name), (DataSize))) return(0); \
    if (use_delta && stride > 0) delta_decode((unsigned char *)(Name), (DataSize), stride); \
  } else { \
    if (Size+(DataSize)>MaxSize) return(0); \
    else { memcpy((Name),Buf+Size,(DataSize));Size+=(DataSize); } \
  }

#define LoadDATA(Name,DataSize) LoadDATA_STRIDE(Name,DataSize)

#define SkipDATA_STRIDE(DataSize) \
  if (StateFD >= 0) { int block_type = buf_getc(); if (block_type == EOF) return(0); } \
  else { if (Size+(DataSize)>MaxSize) return(0); else Size+=(DataSize); }

#define SkipDATA(DataSize) SkipDATA_STRIDE(DataSize)

// =========================================================================
// === SaveState / LoadState ==============================================
// =========================================================================

unsigned int SaveState(unsigned char *Buf,unsigned int MaxSize)
{
  unsigned int State[80],Size;
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

  extern int RAMPages2;
  extern byte *RAMData2;
  extern byte RAMMapper2[4];
  if (RAMPages2 > 0 && RAMData2) {
      SaveDATA_STRIDE(RAMData2, RAMPages2 * 0x4000, 0);
      for (int _m = 0; _m < 4; _m++) buf_putc(RAMMapper2[_m]);
  }

  return(StateFD >= 0 ? 1 : Size);
}

unsigned int LoadState(unsigned char *Buf,unsigned int MaxSize)
{
  int State[80],J,I,K;
  unsigned int Size;

  extern int StateVersion;
  extern int RAMPages2;
  extern byte *RAMData2;
  extern byte RAMMapper2[4];

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

  if (StateVersion >= 3 && RAMPages2 > 0 && RAMData2) {
      LoadDATA_STRIDE(RAMData2, RAMPages2 * 0x4000);
      for (int _m = 0; _m < 4; _m++) {
          int _c = buf_getc();
          if (_c == EOF) return 0;
          RAMMapper2[_m] = (byte)_c;
      }
  }

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

      if (RAMData2) {
          if (StateVersion < 3) RAMMapper2[I] = 3 - I;
          if (RAMMapper2[I] >= RAMPages2) RAMMapper2[I] %= RAMPages2;
          MemMap[3][3][I*2]   = RAMData2 + RAMMapper2[I]*0x4000;
          MemMap[3][3][I*2+1] = MemMap[3][3][I*2] + 0x2000;
      }
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

  return(StateFD >= 0 ? 1 : Size);
}

// =========================================================================
// === SaveSTA / LoadSTA ==================================================
// =========================================================================

int SaveSTA(const char *Name)
{
  static byte Header[16] = "STE\032\003\0\0\0\0\0\0\0\0\0\0\0";
  unsigned int J,Size;
  int fd;

  if(!Name) return(0);
  fd = open(Name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if(fd < 0) return(0);

  J=StateID();
  Header[5] = RAMPages;
  Header[6] = VRAMPages;
  Header[7] = J&0x00FF;
  Header[8] = J>>8;
  Header[9] = 3;

  StateFD = fd;
  io_pos = 0;
  io_avail = 0;

  buf_write(Header, 16);
  Size = SaveState(NULL, 0);
  buf_flush();

  StateFD = -1;
  close(fd);

  if(!Size) { unlink(Name); return 0; }
  return 1;
}

int LoadSTA(const char *Name)
{
  int Size,OldMode,OldRAMPages,OldVRAMPages;
  byte Header[16];
  int fd;
  extern int StateVersion;

  if(!Name) return(0);
  fd = open(Name, O_RDONLY);
  if(fd < 0) return(0);

  if(read(fd, Header, 16) != 16)         { close(fd); return(0); }
  if(memcmp(Header, "STE\032\003", 5))     { close(fd); return(0); }
  if(Header[7]+Header[8]*256!=StateID()) { close(fd); return(0); }
  if((Header[5]!=(RAMPages&0xFF))||(Header[6]!=(VRAMPages&0xFF)))
  { close(fd); return(0); }

  if (Header[9] < 2) { close(fd); return 0; }
  StateVersion = Header[9];

  io_pos = 0;
  io_avail = 0;

  OldMode      = Mode;
  OldRAMPages  = RAMPages;
  OldVRAMPages = VRAMPages;

  StateFD = fd;
  Size = LoadState(NULL, 0);
  StateFD = -1;
  close(fd);

  if(!Size) ResetMSX(OldMode,OldRAMPages,OldVRAMPages);
  return(!!Size);
}

#endif /* STATE_H */
