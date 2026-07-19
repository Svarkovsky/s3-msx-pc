/*****************************************************************************
*
*  Yamaha YM2151 (OPM) Emulator — ESP32-S3 Specialized Header
*
*  ==========================================================================
*  ORIGINAL CORE:
*  Copyright (C) 1997-2002 Jarek Burczynski
*                        (s0246@poczta.onet.pl, bujar@mame.net)
*  Optimization ideas: (C) Tatsuyuki Satoh
*
*  Derived from the MAME project (pre-2016 / pre-relicensing).
*  Licensed under the original MAME License (non-commercial).
*  See LICENSE.MAME-original for full terms.
*  ==========================================================================
*  ADAPTATION & NEW CODE:
*  Copyright (C) 2026 Ivan Svarkovsky <ivansvarkovsky@gmail.com>
*
*  ESP32-S3 specific contributions:
*    - 1-octave freq_base + segment shift (replaced 34 KB table)
*    - Dynamic sin_tab_dram / tl_tab_base generation in fast DRAM
*    - Zero-Flash static tables (eliminated tl_tab.h + sin_tab.h)
*    - Active channel bitmask (active_chan_mask)
*    - Xtensa LX7 inline optimizations (__attribute__((always_inline)))
*    - On-demand LFO/Noise bypass
*    - heap_caps_malloc integration
*
*  New code and architectural redesign licensed under:
*    Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
*    (CC BY-NC-SA 4.0) — https://creativecommons.org/licenses/by-nc-sa/4.0/
*  ==========================================================================
*
*  IMPORTANT LEGAL NOTICE:
*  This file is a derivative work. The non-commercial restriction of the
*  original MAME license applies to the entire file. You may not use this
*  code in a commercial product or for commercial gain without explicit
*  permission from all copyright holders.
*
*****************************************************************************/

#ifndef _H_YM2151_ESP32_
#define _H_YM2151_ESP32_

#include <stdint.h>

/* Мост типов для ESP32 (Замена MAME типов) */
typedef int16_t INT16;
typedef void (*write8_handler)(int offset, int data);

/* 16- and 8-bit samples (signed) are supported*/
#define SAMPLE_BITS 16

#if (SAMPLE_BITS==16)
	typedef INT16 SAMP;
#endif
#if (SAMPLE_BITS==8)
	typedef signed char SAMP;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
** Initialize YM2151 emulator(s).
**
** 'num' is the number of virtual YM2151's to allocate
** 'clock' is the chip clock in Hz
** 'rate' is sampling rate
*/
int YM2151Init(int num, int clock, int rate);

/* shutdown the YM2151 emulators*/
void YM2151Shutdown(void);

/* reset all chip registers for YM2151 number 'num'*/
void YM2151ResetChip(int num);

/*
** Generate samples for one of the YM2151's
**
** 'num' is the number of virtual YM2151
** '**buffers' is table of pointers to the buffers: left and right
** 'length' is the number of samples that should be generated
*/
void YM2151UpdateOne(int num, INT16 **buffers, int length);

/* write 'v' to register 'r' on YM2151 chip number 'n'*/
void YM2151WriteReg(int n, int r, int v);

/* read status register on YM2151 chip number 'n'*/
int YM2151ReadStatus(int n);

/* set interrupt handler on YM2151 chip number 'n'*/
void YM2151SetIrqHandler(int n, void (*handler)(int irq));

/* set port write handler on YM2151 chip number 'n'*/
void YM2151SetPortWriteHandler(int n, write8_handler handler);

#ifdef __cplusplus
}
#endif

#endif /*_H_YM2151_ESP32_*/
