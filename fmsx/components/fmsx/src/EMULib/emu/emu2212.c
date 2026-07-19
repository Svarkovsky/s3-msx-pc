/****************************************************************************

  emu2212.c -- S.C.C. emulator by Mitsutaka Okazaki 2001-2016

    ───────────────────────────────────────────────────────── 
    Heavily optimized for ESP32-S3 (Xtensa LX7) architecture.
    Adapted by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com
    ─────────────────────────────────────────────────────────
    
  Optimizations & Critical Bugfixes (2026):
    - Fixed Okazaki's reset wave-clearing bug via safe memset()
    - Replaced slow double-precision floats with 64-bit integer math
    - CRITICAL FIX: Fixed 16-bit to 32-bit sign extension bug in IIR filter (& ~15 instead of & 0xfff0)
    - CRITICAL FIX: Forced int32_t type promotion in interpolation to prevent catastrophic audio clipping
    - Added high-quality Linear/Cubic interpolation blended with the IIR filter
    - Embedded main render loop in IRAM to eliminate cache stalls
    - Added platform-independent memory mapping (ESP32 SRAM / calloc fallback)
    - Reintroduced backward compatible API for fMSX (SCC_init / SCC_new(void))
     
    
  INTERPOLATION MODES (select one in SCC_INTERPOLATION below):
  0 — NONE:      Raw waveform (no interpolation, aliasing on high notes)
  1 — LINEAR:    Linear interpolation (2 points, smooth, +1% CPU)
  2 — CUBIC:     Catmull-Rom cubic (4 points, very smooth, +3% CPU)

*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include <esp_attr.h>
#endif

#include "emu2212.h"

/* ─── ВЫБОР РЕЖИМА ИНТЕРПОЛЯЦИИ ─── */
#define SCC_INTERPOLATION 2  // 0 = NONE, 1 = LINEAR (Рекомендуется), 2 = CUBIC

#define GETA_BITS 22

#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL (1 << 3)
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT (1 << 2)
#endif

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Глобальные параметры для обратной совместимости с fMSX (Sound.c) */
static uint32_t g_scc_clk = 3579545;
static uint32_t g_scc_rate = 44100;

static void
internal_refresh (SCC * scc)
{
  if (scc->quality)
  {
    scc->base_incr = 2 << GETA_BITS;
    scc->realstep = (uint32_t) ((1 << 31) / scc->rate);
    scc->sccstep = (uint32_t) ((1 << 31) / (scc->clk / 2));
    scc->scctime = 0;
  }
  else
  {
    scc->base_incr = (uint32_t) (((uint64_t) scc->clk * (1ULL << GETA_BITS)) / scc->rate);
  }
}

/* ─── Функции обратной совместимости для fMSX ─── */
void SCC_init(uint32_t c, uint32_t r) {
    g_scc_clk = c;
    g_scc_rate = r ? r : 44100;
}

void SCC_close(void) {
}

SCC *SCC_new(void)
{
  SCC *scc;

#ifdef ESP_PLATFORM
  scc = (SCC *) heap_caps_calloc (1, sizeof (SCC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  scc = (SCC *) calloc (1, sizeof (SCC));
#endif

  if (scc == NULL)
    return NULL;

  scc->clk = g_scc_clk;
  scc->rate = g_scc_rate;
  SCC_set_quality (scc, 0);
  scc->type = SCC_ENHANCED;
  return scc;
}
/* ────────────────────────────────────────────── */

uint32_t
SCC_setMask (SCC *scc, uint32_t mask)
{
  uint32_t ret = 0;
  if(scc)
  {
    ret = scc->mask;
    scc->mask = mask;
  }  
  return ret;
}

uint32_t
SCC_toggleMask (SCC *scc, uint32_t mask)
{
  uint32_t ret = 0;
  if(scc)
  {
    ret = scc->mask;
    scc->mask ^= mask;
  }
  return ret;
}

void
SCC_set_quality (SCC * scc, uint32_t q)
{
  scc->quality = q;
  internal_refresh (scc);
}

void
SCC_set_rate (SCC * scc, uint32_t r)
{
  scc->rate = r ? r : 44100;
  internal_refresh (scc);
}

void
SCC_reset (SCC * scc)
{
  int i;

  if (scc == NULL)
    return;

  scc->mode = 0;
  scc->active = 0;
  scc->base_adr = 0x9000;

  memset(scc->wave, 0, sizeof(scc->wave));

  for (i = 0; i < 5; i++)
  {
    scc->count[i] = 0;
    scc->freq[i] = 0;
    scc->phase[i] = 0;
    scc->volume[i] = 0;
    scc->offset[i] = 0;
    scc->rotate[i] = 0;
    scc->ch_out[i] = 0;
    scc->incr[i] = 0;
  }

  memset(scc->reg, 0, 0x100 - 0xC0);

  scc->mask = 0;
  scc->ch_enable = 0xff;
  scc->ch_enable_next = 0xff;
  scc->cycle_4bit = 0;
  scc->cycle_8bit = 0;
  scc->refresh = 0;
  scc->out = 0;

  return;
}

void
SCC_delete (SCC * scc)
{
  if (scc != NULL)
    free (scc);
}

#ifdef ESP_PLATFORM
static inline void IRAM_ATTR update_output (SCC * scc)
#else
static inline void update_output (SCC * scc)
#endif
{
  int i;
  static const uint32_t PHASE_MASK = (1u << GETA_BITS) - 1;
  
  for (i = 0; i < 5; i++)
  {
    scc->count[i] = (scc->count[i] + scc->incr[i]);

    if (UNLIKELY(scc->count[i] & (1 << (GETA_BITS + 5))))
    {
      scc->count[i] &= ((1 << (GETA_BITS + 5)) - 1);
      scc->offset[i] = (scc->offset[i] + 31) & scc->rotate[i];
      scc->ch_enable &= ~(1 << i);
      scc->ch_enable |= scc->ch_enable_next & (1 << i);
    }

    if (LIKELY(scc->ch_enable & (1 << i)))
    {
      scc->phase[i] = ((scc->count[i] >> (GETA_BITS)) + scc->offset[i]) & 0x1F;
      
      if (LIKELY(!(scc->mask & SCC_MASK_CH(i))))
      {
        int32_t sample = 0;

#if SCC_INTERPOLATION == 0
        /* ─── NONE: Без интерполяции ─── */
        sample = (int32_t)scc->wave[i][scc->phase[i]];

#elif SCC_INTERPOLATION == 1
        /* ─── LINEAR: Линейная интерполяция ─── */
        int32_t frac = (int32_t)((scc->count[i] & PHASE_MASK) >> (GETA_BITS - 8));
        int32_t s0 = (int32_t)scc->wave[i][scc->phase[i]];
        int32_t s1 = (int32_t)scc->wave[i][(scc->phase[i] + 1) & 0x1F];
        sample = s0 + (((s1 - s0) * frac) >> 8);

#elif SCC_INTERPOLATION == 2
        /* ─── CUBIC: Кубическая Catmull-Rom интерполяция ─── */
        int32_t frac = (int32_t)((scc->count[i] & PHASE_MASK) >> (GETA_BITS - 8));
        int32_t p0 = (scc->phase[i] - 1) & 0x1F;
        int32_t p1 = scc->phase[i];
        int32_t p2 = (scc->phase[i] + 1) & 0x1F;
        int32_t p3 = (scc->phase[i] + 2) & 0x1F;
        
        int32_t y0 = (int32_t)scc->wave[i][p0];
        int32_t y1 = (int32_t)scc->wave[i][p1];
        int32_t y2 = (int32_t)scc->wave[i][p2];
        int32_t y3 = (int32_t)scc->wave[i][p3];
        
        int32_t a0 = y1 << 1;
        int32_t a1 = y2 - y0;
        int32_t a2 = (y0 << 1) - 5 * y1 + (y2 << 2) - y3;
        int32_t a3 = 3 * y1 - 3 * y2 + y3 - y0;
        
        int32_t t = frac >> 2; 
        //sample = (a0 + ((a1 * t) >> 6) + ((a2 * t * t) >> 12) + ((a3 * t * t * t) >> 18)) >> 1;
        // Схема Горнера - требует меньше умножений
        sample = (a0 + ((t * (a1 + ((t * (a2 + ((t * a3) >> 6))) >> 6))) >> 6)) >> 1;
#endif
        scc->ch_out[i] += ((int32_t)scc->volume[i] * sample) & ~15;
      }
    }

    scc->ch_out[i] >>= 1;
  }
}

#ifdef ESP_PLATFORM
static inline int16_t IRAM_ATTR mix_output (SCC * scc) {
#else
static inline int16_t mix_output (SCC * scc) {
#endif
  scc->out = scc->ch_out[0] + scc->ch_out[1] + scc->ch_out[2] + scc->ch_out[3] + scc->ch_out[4];
  return (int16_t)scc->out;
}

#ifdef ESP_PLATFORM
IRAM_ATTR int16_t SCC_calc (SCC * scc)
#else
int16_t SCC_calc (SCC * scc)
#endif
{
  if (!scc->quality) {
    update_output(scc);
    return mix_output(scc);
  }

  while (scc->realstep > scc->scctime)
  {
    scc->scctime += scc->sccstep;
    update_output(scc);
  }
  scc->scctime -= scc->realstep;

  return mix_output(scc);
}

uint32_t
SCC_readReg (SCC * scc, uint32_t adr)
{
  if (adr < 0xA0) {
    return scc->wave[adr >> 5][adr & 0x1f];
  }
  if (adr > 0xC0 && adr < 0xF0) {
    return scc->reg[adr - 0xC0];
  }
  return 0;
}

void
SCC_writeReg (SCC * scc, uint32_t adr, uint32_t val)
{
  int ch;
  uint32_t freq;

  adr &= 0xFF;

  if (adr < 0xA0)
  {
    ch = (adr & 0xF0) >> 5;
    if (!scc->rotate[ch])
    {
      scc->wave[ch][adr & 0x1F] = (int8_t) val;
      if (scc->mode == 0 && ch == 3)
        scc->wave[4][adr & 0x1F] = (int8_t) val;
    }
  }
  else if (0xC0 <= adr && adr <= 0xC9)
  {
    scc->reg[adr-0xC0] = val;
    ch = (adr & 0x0F) >> 1;
    if (adr & 1)
      scc->freq[ch] = ((val & 0xF) << 8) | (scc->freq[ch] & 0xFF);
    else
      scc->freq[ch] = (scc->freq[ch] & 0xF00) | (val & 0xFF);

    if (scc->refresh)
      scc->count[ch] = 0;
    freq = scc->freq[ch];
    if (scc->cycle_8bit)
      freq &= 0xFF;
    if (scc->cycle_4bit)
      freq >>= 8;
    if (freq <= 8)
      scc->incr[ch] = 0;
    else
      scc->incr[ch] = scc->base_incr / (freq + 1);
  }
  else if (0xD0 <= adr && adr <= 0xD4)
  {
    scc->reg[adr-0xC0] = val;
    scc->volume[adr & 0x0F] = (uint8_t) (val & 0xF);
  }
  else if (adr == 0xE0)
  {
    scc->reg[adr-0xC0] = val;
    scc->mode = (uint8_t) val & 1;
  }
  else if (adr == 0xE1)
  {
    scc->reg[adr-0xC0] = val;
    scc->ch_enable_next = (uint8_t) val & 0x1F;
  }
  else if (adr == 0xE2)
  {
    scc->reg[adr-0xC0] = val;
    scc->cycle_4bit = val & 1;
    scc->cycle_8bit = val & 2;
    scc->refresh = val & 32;
    if (val & 64)
      for (ch = 0; ch < 5; ch++)
        scc->rotate[ch] = 0x1F;
    else
      for (ch = 0; ch < 5; ch++)
        scc->rotate[ch] = 0;
    if (val & 128)
      scc->rotate[3] = scc->rotate[4] = 0x1F;
  }

  return;
}

static inline void
write_standard (SCC * scc, uint32_t adr, uint32_t val)
{
  adr &= 0xFF;

  if (adr < 0x80)               /* wave */
  {
    SCC_writeReg (scc, adr, val);
  }
  else if (adr < 0x8A)          /* freq */
  {
    SCC_writeReg (scc, adr + 0xC0 - 0x80, val);
  }
  else if (adr < 0x8F)          /* volume */
  {
    SCC_writeReg (scc, adr + 0xD0 - 0x8A, val);
  }
  else if (adr == 0x8F)         /* ch enable */
  {
    SCC_writeReg (scc, 0xE1, val);
  }
  else if (0xE0 <= adr)         /* flags */
  {
    SCC_writeReg (scc, 0xE2, val);
  }
}

static inline void
write_enhanced (SCC * scc, uint32_t adr, uint32_t val)
{
  adr &= 0xFF;

  if (adr < 0xA0)               /* wave */
  {
    SCC_writeReg (scc, adr, val);
  }
  else if (adr < 0xAA)          /* freq */
  {
    SCC_writeReg (scc, adr + 0xC0 - 0xA0, val);
  }
  else if (adr < 0xAF)          /* volume */
  {
    SCC_writeReg (scc, adr + 0xD0 - 0xAA, val);
  }
  else if (adr == 0xAF)         /* ch enable */
  {
    SCC_writeReg (scc, 0xE1, val);
  }
  else if (0xC0 <= adr && adr <= 0xDF)  /* flags */
  {
    SCC_writeReg (scc, 0xE2, val);
  }
}

static inline uint32_t 
read_enhanced (SCC * scc, uint32_t adr)
{
  adr &= 0xFF;
  if (adr < 0xA0)
    return SCC_readReg (scc, adr);
  else if (adr < 0xAA)
    return SCC_readReg (scc, adr + 0xC0 - 0xA0);
  else if (adr < 0xAF)
    return SCC_readReg (scc, adr + 0xD0 - 0xAA);
  else if (adr == 0xAF)
    return SCC_readReg (scc, 0xE1);
  else if (0xC0 <= adr && adr <= 0xDF)
    return SCC_readReg (scc, 0xE2);
  else
    return 0;
}

static inline uint32_t
read_standard (SCC * scc, uint32_t adr)
{
  adr &= 0xFF;
  if(adr<0x80)
    return SCC_readReg (scc, adr);
  else if (0xA0<=adr&&adr<=0xBF)
    return SCC_readReg (scc, 0x80+(adr&0x1F));
  else if (adr < 0x8A)          
    return SCC_readReg (scc, adr + 0xC0 - 0x80);
  else if (adr < 0x8F)          
    return SCC_readReg (scc, adr + 0xD0 - 0x8A);
  else if (adr == 0x8F)         
    return SCC_readReg (scc, 0xE1);
  else if (0xE0 <= adr)         
    return SCC_readReg (scc, adr + 0xE2);
  else return 0;
}

uint32_t
SCC_read (SCC * scc, uint32_t adr)
{
  if( scc->type == SCC_ENHANCED && (adr&0xFFFE) == 0xBFFE ) 
    return (scc->base_adr>>8)&0x20;
  
  if( adr < scc->base_adr ) return 0;
  adr -= scc->base_adr;
  
  if( adr == 0 ) 
  {
    if(scc->mode) return 0x80; else return 0x3F;
  }

  if(!scc->active||adr<0x800||0x8FF<adr) return 0;

  switch (scc->type) 
  {
  case SCC_STANDARD:
      return read_standard (scc, adr);
    break;
  case SCC_ENHANCED:
    if(!scc->mode)
      return read_standard (scc, adr);
    else 
      return read_enhanced (scc, adr);
    break;
  default:
    break;
  }

  return 0;
}

void
SCC_write (SCC * scc, uint32_t adr, uint32_t val)
{
  val = val & 0xFF;

  if( scc->type == SCC_ENHANCED && (adr&0xFFFE) == 0xBFFE ) 
  {
    scc->base_adr = 0x9000 | ((val&0x20)<<8);
    return;
  }
  
  if( adr < scc->base_adr ) return;
  adr -= scc->base_adr;

  if(adr == 0) 
  {
    if( val == 0x3F ) 
    {
      scc->mode = 0;
      scc->active = 1;
    }
    else if( val&0x80 && scc->type == SCC_ENHANCED)
    {
      scc->mode = 1;
      scc->active = 1;
    }
    else
    {
      scc->mode = 0;
      scc->active = 0;
    }
    return;
  }
  
  if(!scc->active||adr<0x800||0x8FF<adr) return;

  switch (scc->type) 
  {
  case SCC_STANDARD:
      write_standard (scc, adr, val);
    break;
  case SCC_ENHANCED:
    if(scc->mode)
      write_enhanced (scc, adr, val);
    else 
      write_standard (scc, adr, val);
  default:
    break;
  }

  return;
}

void
SCC_set_type (SCC * scc, uint32_t type)
{
  scc->type = type;
}

int SCC_save_state(SCC *scc, uint8_t *out) {
  if (out) memcpy(out, scc, sizeof(SCC));
  return (int)sizeof(SCC);
}

void SCC_load_state(SCC *scc, const uint8_t *in, int size) {
  if (size >= (int)sizeof(SCC)) memcpy(scc, in, sizeof(SCC));
}
