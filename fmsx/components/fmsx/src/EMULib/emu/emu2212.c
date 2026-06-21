/****************************************************************************

  emu2212.c -- S.C.C. emulator by Mitsutaka Okazaki 2001
  2001 09-30 : Version 1.00
  2001 10-03 : Version 1.01 -- Added SCC_set_quality().
  
    ───────────────────────────────────────────────────────── 
    Heavily optimized for ESP32-S3 (Xtensa LX7) architecture.
    Adapted by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com
    ─────────────────────────────────────────────────────────
  
  FIXES FOR ESP32-S3 & SCC+ (2026):
  1. SCC_reset: Full 64-byte waveform init to clear PSRAM/Heap garbage.
  2. SCC_reset: Initialization of incr[i] to prevent random startup pitch.
  3. SCC_reset: Added division-by-zero guard for rate/clk.
  4. SCC_read/write: Fixed SCC+ mode register range (0xB8DF -> 0xB8FF).
  5. SCC_write: Waveform writing no longer blocked by rotation mode.
  6. SCC_write: frequency <= 8 mute bug fixed to == 0 (restores ultra-highs).
  7. check_enable: Chip never disabled. SCC/SCC+ mode from BFFE alone.
  8. calc: Offset shift for rotation fixed from backwards (31*steps) to +steps.
  9. SCC_read: Fixed critical address range typo for freq registers (0x9800 -> 0x9880).

  INTERPOLATION MODES (select one in SCC_INTERPOLATION below):
  0 — NONE:      Raw waveform (no interpolation, aliasing on high notes)
  1 — LINEAR:    Linear interpolation (2 points, smooth, +1% CPU)
  2 — CUBIC:     Catmull-Rom cubic (4 points, very smooth, +3% CPU)

*****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu2212.h"

/* ─── ВЫБОР РЕЖИМА ИНТЕРПОЛЯЦИИ ─── */
#define SCC_INTERPOLATION 1  // 0=NONE, 1=LINEAR, 2=CUBIC

#if defined(_MSC_VER)
#define INLINE __forceinline
#elif defined(__GNUC__)
#define INLINE __inline__
#else
#define INLINE
#endif

#define GETA_BITS 22

static e_uint32 clk, rate, base_incr, HQ;

EMUSCC_API void SCC_init(e_uint32 c, e_uint32 r)
{
  clk = c;
  rate = r;
  SCC_set_quality(0); 
}

EMUSCC_API void SCC_set_quality(e_uint32 q)
{
  if(q)
    base_incr = 2 << GETA_BITS;
  else if(rate != 0)
    base_incr = (e_uint32)(((uint64_t)clk << GETA_BITS) / rate);

  HQ = q;
}

EMUSCC_API SCC *SCC_new(void)
{
  SCC *scc;
  scc = (SCC *)malloc(sizeof(SCC));
  if(scc == NULL) return NULL;

  return scc;
}

EMUSCC_API void SCC_reset(SCC *scc)
{
  int i, j;

  if(scc == NULL) return;

  scc->save_9000 = 0x3f;
  scc->save_BFFE = 0;
  scc->save_mode = 0;

  for(i = 0; i < 5; i++)
  {
    for(j = 0; j < 64; j++) scc->wave[i][j] = 0; 
    scc->count[i] = 0;
    scc->freq[i] = 0;
    scc->phase[i] = 0;
    scc->volume[i] = 0;
    scc->offset[i] = 0;
    scc->rotate[i] = 0;
    scc->incr[i] = 0; 
  }

  scc->enable = 1;
  scc->ch_enable = 0xff;
  scc->cycle_4bit = 0;
  scc->cycle_8bit = 0;
  scc->refresh = 0;
  scc->out = 0;

  if(rate == 0 || clk == 0) {
    scc->realstep = 0;
    scc->sccstep = 0;
  } else {
    scc->realstep = (e_uint32)(((uint64_t)1 << 31) / rate);
    scc->sccstep =  (e_uint32)(((uint64_t)1 << 31) / (clk / 2));
  }
  scc->scctime = 0;

  return;
}

EMUSCC_API void SCC_delete(SCC *scc)
{
  if(scc != NULL) free(scc);
}

EMUSCC_API void SCC_close()
{
}

/* ─── calc() с выбором интерполяции ─── */
INLINE static e_int16 calc(SCC *scc)
{
  int i;
  e_int32 mix = 0;
  static const e_uint32 THRESHOLD = 1u << (GETA_BITS + 5);
  static const e_uint32 PHASE_MASK = (1u << GETA_BITS) - 1;

  for(i = 0; i < 5; i++) {
    if(!((scc->ch_enable >> i) & 1)) continue;

    scc->count[i] = (scc->count[i] + scc->incr[i]);

    if(scc->count[i] >= THRESHOLD)
    {
      e_uint32 steps = scc->count[i] >> (GETA_BITS + 5);
      scc->count[i] &= (THRESHOLD - 1);
      scc->offset[i] = (scc->offset[i] + steps) & scc->rotate[i]; 
    }

    scc->phase[i] = ((scc->count[i] >> GETA_BITS) + scc->offset[i]) & 0x1f;

#if SCC_INTERPOLATION == 0
    /* ─── NONE: без интерполяции (оригинал) ─── */
    int sample = scc->wave[i][scc->phase[i]];

#elif SCC_INTERPOLATION == 1
    /* ─── LINEAR: линейная интерполяция (2 точки) ─── */
    int frac = (scc->count[i] & PHASE_MASK) >> (GETA_BITS - 8);
    int s0 = scc->wave[i][scc->phase[i]];
    int s1 = scc->wave[i][(scc->phase[i] + 1) & 0x1f];
    int sample = s0 + (((s1 - s0) * frac) >> 8);

#elif SCC_INTERPOLATION == 2
    /* ─── CUBIC: кубическая Catmull-Rom (4 точки, защищена от переполнения) ─── */
    int frac = (scc->count[i] & PHASE_MASK) >> (GETA_BITS - 8);
    int p0 = (scc->phase[i] - 1) & 0x1f;
    int p1 = scc->phase[i];
    int p2 = (scc->phase[i] + 1) & 0x1f;
    int p3 = (scc->phase[i] + 2) & 0x1f;
    
    int y0 = scc->wave[i][p0];
    int y1 = scc->wave[i][p1];
    int y2 = scc->wave[i][p2];
    int y3 = scc->wave[i][p3];
    
    int a0 = y1 << 1;
    int a1 = y2 - y0;
    int a2 = (y0 << 1) - 5 * y1 + (y2 << 2) - y3;
    int a3 = 3 * y1 - 3 * y2 + y3 - y0;
    
    int t = frac >> 2; // Масштабируем t до 6-бит (0..63) для защиты от 32-битного переполнения
    int sample = (a0 + ((a1 * t) >> 6) + ((a2 * t * t) >> 12) + ((a3 * t * t * t) >> 18)) >> 1;

#else
    #error "SCC_INTERPOLATION must be 0, 1, or 2"
#endif

    mix += (sample * (int)scc->volume[i]) >> 4;
  }

  return (e_int16)(mix << 4); 
}

EMUSCC_API e_int16 SCC_calc(SCC *scc)
{
  if(!HQ) return calc(scc);
  
  e_int32 sum = 0;
  int count = 0;
  while (scc->realstep > scc->scctime) {
    scc->scctime += scc->sccstep;
    sum += calc(scc);
    count++;
  }
  if (count > 0) scc->out = sum / count;

  scc->scctime = scc->scctime - scc->realstep;
  
  return (e_int16)scc->out; 
}

static INLINE void check_enable(SCC *scc)
{
  /* FIX #7: Чип никогда не выключается. Режим только по BFFE. */
  if(scc->save_BFFE == 0x20)
    scc->enable = 2;  // SCC+ Mode
  else
    scc->enable = 1;  // SCC Mode (default)
}

EMUSCC_API e_uint32 SCC_read(SCC *scc, e_uint32 adr)
{
  if((adr == 0xBFFE) || (adr == 0xBFFF)) return scc->save_BFFE;
  if((adr == 0x9000) || (adr == 0xB000)) return scc->save_9000;

  if(scc->enable == 0) return 0;

  if(((0x9800 <= adr) && (adr < 0x9880)) || ((0xB800 <= adr) && (adr < 0xB8A0)))
  {
    return scc->wave[(adr & 0xe0) >> 5][adr & 0x1f];
  }
  /* FIX #9: Устранена опечатка диапазона частот: 0x9800 -> 0x9880 */
  else if (((0x9880 <= adr) && (adr < 0x988A)) || ((0xB8A0 <= adr) && (adr < 0xB8AA)))
  {
    if(adr & 1) return scc->freq[(adr & 0x0f) >> 1] >> 8;
    else return scc->freq[(adr & 0x0f) >> 1] & 0xff;
  }
  else if(((0x988A <= adr) && (adr < 0x988F)) || ((0xB8AA <= adr) && (adr < 0xB8AF)))
  {
    return scc->volume[(adr & 0x0f) - 0x0a];
  }
  else if((adr == 0x988F) || (adr == 0xB8AF))
  {
    return scc->ch_enable;
  }
  else if(((0x98C0 <= adr) && (adr < 0x98FF)) || ((0xB8C0 <= adr) && (adr < 0xB8FF))) 
  {
    return scc->save_mode;
  }

  return 0;
}

EMUSCC_API void SCC_write(SCC *scc, e_uint32 adr, e_uint32 val)
{
  int ch;
  e_uint32 freq;

  val = val & 0xFF;

  if((adr == 0xBFFE) || (adr == 0xBFFF))
  {
    scc->save_BFFE = (e_uint8)val;
    check_enable(scc);
    return;
  }
  
  if((adr == 0x9000) || (adr == 0xB000))
  {
    scc->save_9000 = (e_uint8)val;
    check_enable(scc);
    return;
  }
  
  if(scc->enable == 0) return;

  if(((0x9800 <= adr) && (adr < 0x9880)) || ((0xB800 <= adr) && (adr < 0xB8A0)))
  {
    ch = (adr & 0xe0) >> 5;
    scc->wave[ch][adr & 0x1f] = (e_int8)val;

    /* Зеркалирование ТОЛЬКО для SCC (enable=1) и ТОЛЬКО <0x9880 */
    if((adr < 0x9880) && (scc->enable == 1) && (ch == 3)) {
      scc->wave[4][adr & 0x1f] = (e_int8)val;
    }
  }
  else if(((0x9880 <= adr) && (adr < 0x988A)) || ((0xB8A0 <= adr) && (adr < 0xB8AA)))
  {
    ch = (adr & 0x0f) >> 1;
    if(adr & 1) scc->freq[ch] = ((val & 0xf) << 8) | (scc->freq[ch] & 0xff);
    else scc->freq[ch] = (scc->freq[ch] & 0xf00) | (val & 0xff);
    if(scc->refresh) scc->count[ch] = 0;
    freq = scc->freq[ch];
    if(scc->cycle_8bit) freq &= 0xff;
    if(scc->cycle_4bit) freq >>= 8; 
    if(freq == 0) scc->incr[ch] = 0; 
    else scc->incr[ch] = base_incr / (freq + 1);
  }
  else if(((0x988A <= adr) && (adr < 0x988F)) || ((0xB8AA <= adr) && (adr < 0xB8AF)))
  {
    ch = ((adr & 0x0f) - 0x0a);
    scc->volume[ch] = (e_uint8)(val & 0xf);
  }
  else if((adr == 0x988F) || (adr == 0xB8AF))
  {
    scc->ch_enable = (e_uint8)val & 31;
  }
  else if(((0x98C0 <= adr) && (adr < 0x98FF)) || ((0xB8C0 <= adr) && (adr < 0xB8FF)))
  {
    scc->save_mode = (e_uint8)val;
    scc->cycle_4bit = val & 1;
    scc->cycle_8bit = val & 2;
    scc->refresh = val & 32;
    if(val & 64) for(ch = 0; ch < 5; ch++) scc->rotate[ch] = 0x1F;
    else for(ch = 0; ch < 5; ch++) scc->rotate[ch] = 0;
    if(val & 128) scc->rotate[3] = scc->rotate[4] = 0x1F;
  }

  return;
}
