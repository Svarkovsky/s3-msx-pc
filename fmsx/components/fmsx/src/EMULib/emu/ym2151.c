/*****************************************************************************
*
*  Yamaha YM2151 (OPM) Emulator — ESP32-S3 Specialized Implementation
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
*    - Branchless frequency lookup (get_freq)
*    - State-change tracking for active channel mask
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <esp_heap_caps.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Мост типов для ESP32 (Замена MAME типов) */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef int8_t    INT8;
typedef int32_t   INT32;

#include "ym2151.h"

#define logerror(...)
#define INLINE static __inline__ __attribute__((always_inline))
#define LIKELY(x)   __builtin_expect(!!(x), 1)  
#define UNLIKELY(x) __builtin_expect(!!(x), 0)  

/* struct describing a single operator */
typedef struct{
	UINT32		phase;					
	UINT32		freq;					
	INT32		dt1;					
	UINT32		mul;					
	UINT32		dt1_i;					
	UINT32		dt2;					

	signed int *connect;				

	signed int *mem_connect;			
	INT32		mem_value;				

	UINT32		fb_shift;				
	INT32		fb_out_curr;			
	INT32		fb_out_prev;			
	UINT32		kc;						
	UINT32		kc_i;					
	UINT32		pms;					
	UINT32		ams;					

	UINT32		AMmask;					
	UINT32		state;					
	UINT8		eg_sh_ar;				
	UINT8		eg_sel_ar;				
	UINT32		tl;						
	INT32		volume;					
	UINT8		eg_sh_d1r;				
	UINT8		eg_sel_d1r;				
	UINT32		d1l;					
	UINT8		eg_sh_d2r;				
	UINT8		eg_sel_d2r;				
	UINT8		eg_sh_rr;				
	UINT8		eg_sel_rr;				

	UINT32		key;					

	UINT32		ks;						
	UINT32		ar;						
	UINT32		d1r;					
	UINT32		d2r;					
	UINT32		rr;						

	UINT32		reserved0;				
	UINT32		reserved1;				

} __attribute__((aligned(4))) YM2151Operator;

typedef struct
{
	YM2151Operator	oper[32];			

	UINT32		pan[16];				

	UINT32		eg_cnt;					
	UINT32		eg_timer;				
	UINT32		eg_timer_add;			
	UINT32		eg_timer_overflow;		

	UINT32		lfo_phase;				
	UINT32		lfo_timer;				
	UINT32		lfo_timer_add;			
	UINT32		lfo_overflow;			
	UINT32		lfo_counter;			
	UINT32		lfo_counter_add;		
	UINT8		lfo_wsel;				
	UINT8		amd;					
	INT8		pmd;					
	UINT32		lfa;					
	INT32		lfp;					

	UINT8		test;					
	UINT8		ct;						

	UINT32		noise;					
	UINT32		noise_rng;				
	UINT32		noise_p;				
	UINT32		noise_f;				

	UINT32		csm_req;				

	UINT32		irq_enable;				
	UINT32		status;					
	UINT8		connect[8];				

	UINT8		tim_A;					
	UINT8		tim_B;					
	INT32		tim_A_val;				
	INT32		tim_B_val;				
	UINT32		tim_A_tab[1024];		
	UINT32		tim_B_tab[256];			
	UINT32		timer_A_index;			
	UINT32		timer_B_index;			
	UINT32		timer_A_index_old;		
	UINT32		timer_B_index_old;		

	INT32		dt1_freq[8*32];			
	UINT32		noise_tab[32];			

	void (*irqhandler)(int irq);		
	write8_handler porthandler;		

	unsigned int clock;					
	unsigned int sampfreq;				
	uint32_t active_chan_mask; 

} YM2151;

#define FREQ_SH			16  
#define EG_SH			16  
#define LFO_SH			10  
#define TIMER_SH		16  
#define FREQ_MASK		((1<<FREQ_SH)-1)
#define ENV_BITS		10
#define ENV_LEN			(1<<ENV_BITS)
#define ENV_STEP		(128.0/ENV_LEN)
#define MAX_ATT_INDEX	(ENV_LEN-1) 
#define MIN_ATT_INDEX	(0)			
#define EG_ATT			4
#define EG_DEC			3
#define EG_SUS			2
#define EG_REL			1
#define EG_OFF			0
#define SIN_BITS		10
#define SIN_LEN			(1<<SIN_BITS)
#define SIN_MASK		(SIN_LEN-1)
#define TL_RES_LEN		(256) 
#define FINAL_SH	(0)
#define MAXOUT		(+32767)
#define MINOUT		(-32768)

#define TL_TAB_LEN (13*2*TL_RES_LEN)
#define ENV_QUIET		(TL_TAB_LEN>>3)

/* ========================================================================= */
/* ТАБЛИЦЫ В DRAM (1 КБ + 4 КБ)                                              */
/* ========================================================================= */
static unsigned int *sin_tab_dram = NULL;
static signed int *tl_tab_base = NULL;

// Указатель на активную таблицу синуса (всегда будет указывать на sin_tab_dram)
static unsigned int *sin_tab_ptr = NULL;

/* ========================================================================= */
/* ОПТИМИЗАЦИЯ ПАМЯТИ: Базовые частоты                                       */
/* ========================================================================= */
static UINT32 *freq_base = NULL;

static const UINT8 segment_table[33] = {
    0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
    5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10
};

static inline UINT32 get_freq(UINT32 index) {
    if (UNLIKELY(!freq_base)) return 0;
    if (index >= 8448) index = 8447;
    
    UINT32 idx_shifted = index >> 8;
    UINT32 segment = segment_table[idx_shifted];
    UINT32 note = index - (segment * 768);
    
    if (segment == 0) return (freq_base[0] >> 2) & 0xffffffc0;
    if (segment >= 9) return freq_base[767] << 5;
    UINT32 base = freq_base[note];
    if (segment == 1) return (base >> 2) & 0xffffffc0;
    if (segment == 2) return (base >> 1) & 0xffffffc0;
    if (segment == 3) return base;
    return base << (segment - 3);
}

/* ========================================================================= */

static UINT32 d1l_tab[16];

#define RATE_STEPS (8)
static const UINT8 eg_inc[19*RATE_STEPS]={
/* 0 */ 0,1, 0,1, 0,1, 0,1, /* 1 */ 0,1, 0,1, 1,1, 0,1, 
/* 2 */ 0,1, 1,1, 0,1, 1,1, /* 3 */ 0,1, 1,1, 1,1, 1,1, 
/* 4 */ 1,1, 1,1, 1,1, 1,1, /* 5 */ 1,1, 1,2, 1,1, 1,2, 
/* 6 */ 1,2, 1,2, 1,2, 1,2, /* 7 */ 1,2, 2,2, 1,2, 2,2, 
/* 8 */ 2,2, 2,2, 2,2, 2,2, /* 9 */ 2,2, 2,4, 2,2, 2,4, 
/*10 */ 2,4, 2,4, 2,4, 2,4, /*11 */ 2,4, 4,4, 2,4, 4,4, 
/*12 */ 4,4, 4,4, 4,4, 4,4, /*13 */ 4,4, 4,8, 4,4, 4,8, 
/*14 */ 4,8, 4,8, 4,8, 4,8, /*15 */ 4,8, 8,8, 4,8, 8,8, 
/*16 */ 8,8, 8,8, 8,8, 8,8, /*17 */ 16,16,16,16,16,16,16,16, 
/*18 */ 0,0, 0,0, 0,0, 0,0, 
};

#define O(a) (a*RATE_STEPS)
static const UINT8 eg_rate_select[32+64+32]={	
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),O( 0),O( 1),O( 2),O( 3),
O( 4),O( 5),O( 6),O( 7),O( 8),O( 9),O(10),O(11),
O(12),O(13),O(14),O(15),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16)
};
#undef O

#define O(a) (a*1)
static const UINT8 eg_rate_shift[32+64+32]={	
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(11),O(11),O(11),O(11),O(10),O(10),O(10),O(10),
O( 9),O( 9),O( 9),O( 9),O( 8),O( 8),O( 8),O( 8),
O( 7),O( 7),O( 7),O( 7),O( 6),O( 6),O( 6),O( 6),
O( 5),O( 5),O( 5),O( 5),O( 4),O( 4),O( 4),O( 4),
O( 3),O( 3),O( 3),O( 3),O( 2),O( 2),O( 2),O( 2),
O( 1),O( 1),O( 1),O( 1),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0)
};
#undef O

static const UINT32 dt2_tab[4] = { 0, 384, 500, 608 };

static const UINT8 dt1_tab[4*32] = { 
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
  2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8,
  1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
  5, 6, 6, 7, 8, 8, 9,10,11,12,13,14,16,16,16,16,
  2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7,
  8, 8, 9,10,11,12,13,14,16,17,19,20,22,22,22,22
};

static const UINT16 phaseinc_rom[768]={
1299,1300,1301,1302,1303,1304,1305,1306,1308,1309,1310,1311,1313,1314,1315,1316,
1318,1319,1320,1321,1322,1323,1324,1325,1327,1328,1329,1330,1332,1333,1334,1335,
1337,1338,1339,1340,1341,1342,1343,1344,1346,1347,1348,1349,1351,1352,1353,1354,
1356,1357,1358,1359,1361,1362,1363,1364,1366,1367,1368,1369,1371,1372,1373,1374,
1376,1377,1378,1379,1381,1382,1383,1384,1386,1387,1388,1389,1391,1392,1393,1394,
1396,1397,1398,1399,1401,1402,1403,1404,1406,1407,1408,1409,1411,1412,1413,1414,
1416,1417,1418,1419,1421,1422,1423,1424,1426,1427,1429,1430,1431,1432,1434,1435,
1437,1438,1439,1440,1442,1443,1444,1445,1447,1448,1449,1450,1452,1453,1454,1455,
1458,1459,1460,1461,1463,1464,1465,1466,1468,1469,1471,1472,1473,1474,1476,1477,
1479,1480,1481,1482,1484,1485,1486,1487,1489,1490,1492,1493,1494,1495,1497,1498,
1501,1502,1503,1504,1506,1507,1509,1510,1512,1513,1514,1515,1517,1518,1520,1521,
1523,1524,1525,1526,1528,1529,1531,1532,1534,1535,1536,1537,1539,1540,1542,1543,
1545,1546,1547,1548,1550,1551,1553,1554,1556,1557,1558,1559,1561,1562,1564,1565,
1567,1568,1569,1570,1572,1573,1575,1576,1578,1579,1580,1581,1583,1584,1586,1587,
1590,1591,1592,1593,1595,1596,1598,1599,1601,1602,1604,1605,1607,1608,1609,1610,
1613,1614,1615,1616,1618,1619,1621,1622,1624,1625,1627,1628,1630,1631,1632,1633,
1637,1638,1639,1640,1642,1643,1645,1646,1648,1649,1651,1652,1654,1655,1656,1657,
1660,1661,1663,1664,1666,1667,1669,1670,1672,1673,1675,1676,1678,1679,1681,1682,
1685,1686,1688,1689,1691,1692,1694,1695,1697,1698,1700,1701,1703,1704,1706,1707,
1709,1710,1712,1713,1715,1716,1718,1719,1721,1722,1724,1725,1727,1728,1730,1731,
1734,1735,1737,1738,1740,1741,1743,1744,1746,1748,1749,1751,1752,1754,1755,1757,
1759,1760,1762,1763,1765,1766,1768,1769,1771,1773,1774,1776,1777,1779,1780,1782,
1785,1786,1788,1789,1791,1793,1794,1796,1798,1799,1801,1802,1804,1806,1807,1809,
1811,1812,1814,1815,1817,1819,1820,1822,1824,1825,1827,1828,1830,1832,1833,1835,
1837,1838,1840,1841,1843,1845,1846,1848,1850,1851,1853,1854,1856,1858,1859,1861,
1864,1865,1867,1868,1870,1872,1873,1875,1877,1879,1880,1882,1884,1885,1887,1888,
1891,1892,1894,1895,1897,1899,1900,1902,1904,1906,1907,1909,1911,1912,1914,1915,
1918,1919,1921,1923,1925,1926,1928,1930,1932,1933,1935,1937,1939,1940,1942,1944,
1946,1947,1949,1951,1953,1954,1956,1958,1960,1961,1963,1965,1967,1968,1970,1972,
1975,1976,1978,1980,1982,1983,1985,1987,1989,1990,1992,1994,1996,1997,1999,2001,
2003,2004,2006,2008,2010,2011,2013,2015,2017,2019,2021,2022,2024,2026,2028,2029,
2032,2033,2035,2037,2039,2041,2043,2044,2047,2048,2050,2052,2054,2056,2058,2059,
2062,2063,2065,2067,2069,2071,2073,2074,2077,2078,2080,2082,2084,2086,2088,2089,
2092,2093,2095,2097,2099,2101,2103,2104,2107,2108,2110,2112,2114,2116,2118,2119,
2122,2123,2125,2127,2129,2131,2133,2134,2137,2139,2141,2142,2145,2146,2148,2150,
2153,2154,2156,2158,2160,2162,2164,2165,2168,2170,2172,2173,2176,2177,2179,2181,
2185,2186,2188,2190,2192,2194,2196,2197,2200,2202,2204,2205,2208,2209,2211,2213,
2216,2218,2220,2222,2223,2226,2227,2230,2232,2234,2236,2238,2239,2242,2243,2246,
2249,2251,2253,2255,2256,2259,2260,2263,2265,2267,2269,2271,2272,2275,2276,2279,
2281,2283,2285,2287,2288,2291,2292,2295,2297,2299,2301,2303,2304,2307,2308,2311,
2315,2317,2319,2321,2322,2325,2326,2329,2331,2333,2335,2337,2338,2341,2342,2345,
2348,2350,2352,2354,2355,2358,2359,2362,2364,2366,2368,2370,2371,2374,2375,2378,
2382,2384,2386,2388,2389,2392,2393,2396,2398,2400,2402,2404,2407,2410,2411,2414,
2417,2419,2421,2423,2424,2427,2428,2431,2433,2435,2437,2439,2442,2445,2446,2449,
2452,2454,2456,2458,2459,2462,2463,2466,2468,2470,2472,2474,2477,2480,2481,2484,
2488,2490,2492,2494,2495,2498,2499,2502,2504,2506,2508,2510,2513,2516,2517,2520,
2524,2526,2528,2530,2531,2534,2535,2538,2540,2542,2544,2546,2549,2552,2553,2556,
2561,2563,2565,2567,2568,2571,2572,2575,2577,2579,2581,2583,2586,2589,2590,2593
};

static const UINT8 lfo_noise_waveform[256] = {
0xFF,0xEE,0xD3,0x80,0x58,0xDA,0x7F,0x94,0x9E,0xE3,0xFA,0x00,0x4D,0xFA,0xFF,0x6A,
0x7A,0xDE,0x49,0xF6,0x00,0x33,0xBB,0x63,0x91,0x60,0x51,0xFF,0x00,0xD8,0x7F,0xDE,
0xDC,0x73,0x21,0x85,0xB2,0x9C,0x5D,0x24,0xCD,0x91,0x9E,0x76,0x7F,0x20,0xFB,0xF3,
0x00,0xA6,0x3E,0x42,0x27,0x69,0xAE,0x33,0x45,0x44,0x11,0x41,0x72,0x73,0xDF,0xA2,
0x32,0xBD,0x7E,0xA8,0x13,0xEB,0xD3,0x15,0xDD,0xFB,0xC9,0x9D,0x61,0x2F,0xBE,0x9D,
0x23,0x65,0x51,0x6A,0x84,0xF9,0xC9,0xD7,0x23,0xBF,0x65,0x19,0xDC,0x03,0xF3,0x24,
0x33,0xB6,0x1E,0x57,0x5C,0xAC,0x25,0x89,0x4D,0xC5,0x9C,0x99,0x15,0x07,0xCF,0xBA,
0xC5,0x9B,0x15,0x4D,0x8D,0x2A,0x1E,0x1F,0xEA,0x2B,0x2F,0x64,0xA9,0x50,0x3D,0xAB,
0x50,0x77,0xE9,0xC0,0xAC,0x6D,0x3F,0xCA,0xCF,0x71,0x7D,0x80,0xA6,0xFD,0xFF,0xB5,
0xBD,0x6F,0x24,0x7B,0x00,0x99,0x5D,0xB1,0x48,0xB0,0x28,0x7F,0x80,0xEC,0xBF,0x6F,
0x6E,0x39,0x90,0x42,0xD9,0x4E,0x2E,0x12,0x66,0xC8,0xCF,0x3B,0x3F,0x10,0x7D,0x79,
0x00,0xD3,0x1F,0x21,0x93,0x34,0xD7,0x19,0x22,0xA2,0x08,0x20,0xB9,0xB9,0xEF,0x51,
0x99,0xDE,0xBF,0xD4,0x09,0x75,0xE9,0x8A,0xEE,0xFD,0xE4,0x4E,0x30,0x17,0xDF,0xCE,
0x11,0xB2,0x28,0x35,0xC2,0x7C,0x64,0xEB,0x91,0x5F,0x32,0x0C,0x6E,0x00,0xF9,0x92,
0x19,0xDB,0x8F,0xAB,0xAE,0xD6,0x12,0xC4,0x26,0x62,0xCE,0xCC,0x0A,0x03,0xE7,0xDD,
0xE2,0x4D,0x8A,0xA6,0x46,0x95,0x0F,0x8F,0xF5,0x15,0x97,0x32,0xD4,0x28,0x1E,0x55
};

static YM2151 * YMPSG = NULL;
static unsigned int YMNumChips;	

static YM2151 * PSG;
static signed int chanout[8];
static signed int m2,c1,c2; 
static signed int mem;		

static inline void refresh_active_channels(YM2151 *chip)
{
	uint32_t mask = 0;
	for (int c = 0; c < 8; c++)
	{
		YM2151Operator *op = &chip->oper[c * 4];
		if (op[0].state != EG_OFF || op[1].state != EG_OFF ||
		    op[2].state != EG_OFF || op[3].state != EG_OFF)
		{
			mask |= (1 << c);
		}
	}
	chip->active_chan_mask = mask;
}

static void init_tables(void)
{
	int i, x;
	double m;
	signed int n;

	for (i=0; i<16; i++)
	{
		m = (i!=15 ? i : i+16) * (4.0/ENV_STEP);  
		d1l_tab[i] = m;
	}

	// Выделяем и генерируем базовую 1 КБ таблицу в DRAM
	if (!tl_tab_base) {
		tl_tab_base = (signed int *)heap_caps_malloc(256 * sizeof(signed int), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (tl_tab_base) {
			for (x=0; x<256; x++)
			{
				m = (1<<16) / pow(2, (x+1) * (ENV_STEP/4.0) / 8.0);
				m = floor(m);

				n = (int)m;		
				n >>= 4;		
				if (n&1)		
					n = (n>>1)+1;
				else
					n = n>>1;
									
				n <<= 1;		
				tl_tab_base[x] = n;
			}
			printf("YM2151: tl_tab_base (1KB) generated in fast DRAM\n");
		}
	}
}

static void init_chip_tables(YM2151 *chip)
{
	int i,j;
	double mult,phaseinc,Hz;
	double scaler;

	// Выделяем freq_base при первом вызове
	if (!freq_base) {
		freq_base = (UINT32 *)malloc(768 * sizeof(UINT32));
		if (!freq_base) return;  
	}

	scaler = ( (double)chip->clock / 64.0 ) / ( (double)chip->sampfreq );

	mult = (1<<(FREQ_SH-10)); 
	for (i=0; i<768; i++)
	{
		phaseinc = phaseinc_rom[i] * scaler;			
		freq_base[i] = ((int)(phaseinc*mult)) & 0xffffffc0; 
	}

	mult = (1<<FREQ_SH);
	for (j=0; j<4; j++)
	{
		for (i=0; i<32; i++)
		{
			Hz = ( (double)dt1_tab[j*32+i] * ((double)chip->clock/64.0) ) / (double)(1<<20);
			phaseinc = (Hz*SIN_LEN) / (double)chip->sampfreq;
			chip->dt1_freq[ (j+0)*32 + i ] = phaseinc * mult;
			chip->dt1_freq[ (j+4)*32 + i ] = -chip->dt1_freq[ (j+0)*32 + i ];
		}
	}

	mult = (1<<TIMER_SH);
	for (i=0; i<1024; i++)
	{
		double pom= ( 64.0  *  (1024.0-i) / (double)chip->clock );
		chip->tim_A_tab[i] = pom * (double)chip->sampfreq * mult; 
	}
	for (i=0; i<256; i++)
	{
		double pom= ( 1024.0 * (256.0-i)  / (double)chip->clock );
		chip->tim_B_tab[i] = pom * (double)chip->sampfreq * mult;  
	}

	scaler = ( (double)chip->clock / 64.0 ) / ( (double)chip->sampfreq );
	for (i=0; i<32; i++)
	{
		j = (i!=31 ? i : 30);				
		j = 32-j;
		j = (65536.0 / (double)(j*32.0));	
		chip->noise_tab[i] = j * 64 * scaler;
	}
}

#define KEY_ON(op, key_set){									\
		if (!(op)->key)											\
		{														\
			(op)->phase = 0;									\
			(op)->state = EG_ATT;								\
			(op)->volume += (~(op)->volume *					\
                           (eg_inc[(op)->eg_sel_ar + ((PSG->eg_cnt>>(op)->eg_sh_ar)&7)])	\
                          ) >>4;								\
			if ((op)->volume <= MIN_ATT_INDEX)					\
			{													\
				(op)->volume = MIN_ATT_INDEX;					\
				(op)->state = EG_DEC;							\
			}													\
		}														\
		(op)->key |= key_set;									\
}

#define KEY_OFF(op, key_clr){									\
		if ((op)->key)											\
		{														\
				(op)->key &= key_clr;								\
			if (!(op)->key)										\
			{													\
				if ((op)->state>EG_REL)							\
					(op)->state = EG_REL;						\
			}													\
		}														\
}

INLINE void envelope_KONKOFF(YM2151Operator * op, int v)
{
	if (v&0x08)	KEY_ON (op+0, 1) else KEY_OFF(op+0,~1)
	if (v&0x20)	KEY_ON (op+1, 1) else KEY_OFF(op+1,~1)
	if (v&0x10)	KEY_ON (op+2, 1) else KEY_OFF(op+2,~1)
	if (v&0x40)	KEY_ON (op+3, 1) else KEY_OFF(op+3,~1)
}

INLINE void set_connect( YM2151Operator *om1, int cha, int v)
{
	YM2151Operator *om2 = om1+1;
	YM2151Operator *oc1 = om1+2;

	switch( v&7 )
	{
	case 0:
		om1->connect = &c1; oc1->connect = &mem; om2->connect = &c2; om1->mem_connect = &m2; break;
	case 1:
		om1->connect = &mem; oc1->connect = &mem; om2->connect = &c2; om1->mem_connect = &m2; break;
	case 2:
		om1->connect = &c2; oc1->connect = &mem; om2->connect = &c2; om1->mem_connect = &m2; break;
	case 3:
		om1->connect = &c1; oc1->connect = &mem; om2->connect = &c2; om1->mem_connect = &c2; break;
	case 4:
		om1->connect = &c1; oc1->connect = &chanout[cha]; om2->connect = &c2; om1->mem_connect = &mem; break;
	case 5:
		om1->connect = 0; oc1->connect = &chanout[cha]; om2->connect = &chanout[cha]; om1->mem_connect = &m2; break;
	case 6:
		om1->connect = &c1; oc1->connect = &chanout[cha]; om2->connect = &chanout[cha]; om1->mem_connect = &mem; break;
	case 7:
		om1->connect = &chanout[cha]; oc1->connect = &chanout[cha]; om2->connect = &chanout[cha]; om1->mem_connect = &mem; break;
	}
}

INLINE void refresh_EG(YM2151Operator * op)
{
	UINT32 kc = op->kc;
	UINT32 v = kc >> op->ks;
	
	if ((op->ar+v) < 32+62) {
		op->eg_sh_ar  = eg_rate_shift [op->ar  + v ];
		op->eg_sel_ar = eg_rate_select[op->ar  + v ];
	} else {
		op->eg_sh_ar  = 0;
		op->eg_sel_ar = 17*RATE_STEPS;
	}
	op->eg_sh_d1r = eg_rate_shift [op->d1r + v];
	op->eg_sel_d1r= eg_rate_select[op->d1r + v];
	op->eg_sh_d2r = eg_rate_shift [op->d2r + v];
	op->eg_sel_d2r= eg_rate_select[op->d2r + v];
	op->eg_sh_rr  = eg_rate_shift [op->rr  + v];
	op->eg_sel_rr = eg_rate_select[op->rr  + v];

	op+=1; v = kc >> op->ks;
	if ((op->ar+v) < 32+62) {
		op->eg_sh_ar  = eg_rate_shift [op->ar  + v ];
		op->eg_sel_ar = eg_rate_select[op->ar  + v ];
	} else {
		op->eg_sh_ar  = 0;
		op->eg_sel_ar = 17*RATE_STEPS;
	}
	op->eg_sh_d1r = eg_rate_shift [op->d1r + v];
	op->eg_sel_d1r= eg_rate_select[op->d1r + v];
	op->eg_sh_d2r = eg_rate_shift [op->d2r + v];
	op->eg_sel_d2r= eg_rate_select[op->d2r + v];
	op->eg_sh_rr  = eg_rate_shift [op->rr  + v];
	op->eg_sel_rr = eg_rate_select[op->rr  + v];

	op+=1; v = kc >> op->ks;
	if ((op->ar+v) < 32+62) {
		op->eg_sh_ar  = eg_rate_shift [op->ar  + v ];
		op->eg_sel_ar = eg_rate_select[op->ar  + v ];
	} else {
		op->eg_sh_ar  = 0;
		op->eg_sel_ar = 17*RATE_STEPS;
	}
	op->eg_sh_d1r = eg_rate_shift [op->d1r + v];
	op->eg_sel_d1r= eg_rate_select[op->d1r + v];
	op->eg_sh_d2r = eg_rate_shift [op->d2r + v];
	op->eg_sel_d2r= eg_rate_select[op->d2r + v];
	op->eg_sh_rr  = eg_rate_shift [op->rr  + v];
	op->eg_sel_rr = eg_rate_select[op->rr  + v];

	op+=1; v = kc >> op->ks;
	if ((op->ar+v) < 32+62) {
		op->eg_sh_ar  = eg_rate_shift [op->ar  + v ];
		op->eg_sel_ar = eg_rate_select[op->ar  + v ];
	} else {
		op->eg_sh_ar  = 0;
		op->eg_sel_ar = 17*RATE_STEPS;
	}
	op->eg_sh_d1r = eg_rate_shift [op->d1r + v];
	op->eg_sel_d1r= eg_rate_select[op->d1r + v];
	op->eg_sh_d2r = eg_rate_shift [op->d2r + v];
	op->eg_sel_d2r= eg_rate_select[op->d2r + v];
	op->eg_sh_rr  = eg_rate_shift [op->rr  + v];
	op->eg_sel_rr = eg_rate_select[op->rr  + v];
}

__attribute__((noinline)) void YM2151WriteReg(int n, int r, int v)
{
	YM2151 *chip = &YMPSG[n];
	YM2151Operator *op = &chip->oper[ (r&0x07)*4+((r&0x18)>>3) ];

	r &= 0xff; v &= 0xff;

	switch(r & 0xe0){
	case 0x00:
		switch(r){
		case 0x01: chip->test = v; if (v&2) chip->lfo_phase = 0; break;
		case 0x08: PSG = &YMPSG[n]; envelope_KONKOFF(&chip->oper[ (v&7)*4 ], v ); break;
		case 0x0f: chip->noise = v; chip->noise_f = chip->noise_tab[ v & 0x1f ]; break;
		case 0x10: chip->timer_A_index = (chip->timer_A_index & 0x003) | (v<<2); break;
		case 0x11: chip->timer_A_index = (chip->timer_A_index & 0x3fc) | (v & 3); break;
		case 0x12: chip->timer_B_index = v; break;
		case 0x14:
			chip->irq_enable = v;
			if (v&0x20) { int oldstate = chip->status & 3; chip->status &= 0xfd; if ((oldstate==2) && (chip->irqhandler)) (*chip->irqhandler)(0); }
			if (v&0x10) { int oldstate = chip->status & 3; chip->status &= 0xfe; if ((oldstate==1) && (chip->irqhandler)) (*chip->irqhandler)(0); }
			if (v&0x02) { if (!chip->tim_B) { chip->tim_B = 1; chip->tim_B_val = chip->tim_B_tab[ chip->timer_B_index ]; } }
			else { chip->tim_B = 0; }
			if (v&0x01) { if (!chip->tim_A) { chip->tim_A = 1; chip->tim_A_val = chip->tim_A_tab[ chip->timer_A_index ]; } }
			else { chip->tim_A = 0; }
			break;
		case 0x18: chip->lfo_overflow = ( 1 << ((15-(v>>4))+3) ) * (1<<LFO_SH); chip->lfo_counter_add = 0x10 + (v & 0x0f); break;
		case 0x19: if (v&0x80) chip->pmd = v & 0x7f; else chip->amd = v & 0x7f; break;
		case 0x1b: chip->ct = v >> 6; chip->lfo_wsel = v & 3; if (chip->porthandler) (*chip->porthandler)(0 , chip->ct ); break;
		}
		break;

	case 0x20:
		op = &chip->oper[ (r&7) * 4 ];
		switch(r & 0x18){
		case 0x00:
			op->fb_shift = ((v>>3)&7) ? ((v>>3)&7)+6:0;
			chip->pan[ (r&7)*2    ] = (v & 0x40) ? ~0 : 0;
			chip->pan[ (r&7)*2 +1 ] = (v & 0x80) ? ~0 : 0;
			chip->connect[r&7] = v&7;
			set_connect(op, r&7, v&7);
			break;
		case 0x08:
			v &= 0x7f;
			if (v != op->kc) {
				UINT32 kc, kc_channel;
				kc_channel = (v - (v>>2))*64 + 768;
				kc_channel |= (op->kc_i & 63);
				(op+0)->kc = v; (op+0)->kc_i = kc_channel;
				(op+1)->kc = v; (op+1)->kc_i = kc_channel;
				(op+2)->kc = v; (op+2)->kc_i = kc_channel;
				(op+3)->kc = v; (op+3)->kc_i = kc_channel;
				kc = v>>2;
				(op+0)->dt1 = chip->dt1_freq[ (op+0)->dt1_i + kc ];
				(op+0)->freq = ( (get_freq(kc_channel + (op+0)->dt2) + (op+0)->dt1) * (op+0)->mul ) >> 1;
				(op+1)->dt1 = chip->dt1_freq[ (op+1)->dt1_i + kc ];
				(op+1)->freq = ( (get_freq(kc_channel + (op+1)->dt2) + (op+1)->dt1) * (op+1)->mul ) >> 1;
				(op+2)->dt1 = chip->dt1_freq[ (op+2)->dt1_i + kc ];
				(op+2)->freq = ( (get_freq(kc_channel + (op+2)->dt2) + (op+2)->dt1) * (op+2)->mul ) >> 1;
				(op+3)->dt1 = chip->dt1_freq[ (op+3)->dt1_i + kc ];
				(op+3)->freq = ( (get_freq(kc_channel + (op+3)->dt2) + (op+3)->dt1) * (op+3)->mul ) >> 1;
				refresh_EG( op );
			}
			break;
		case 0x10:
			v >>= 2;
			if (v != (op->kc_i & 63)) {
				UINT32 kc_channel = v | (op->kc_i & ~63);
				(op+0)->kc_i = kc_channel; (op+1)->kc_i = kc_channel;
				(op+2)->kc_i = kc_channel; (op+3)->kc_i = kc_channel;
				(op+0)->freq = ( (get_freq(kc_channel + (op+0)->dt2) + (op+0)->dt1) * (op+0)->mul ) >> 1;
				(op+1)->freq = ( (get_freq(kc_channel + (op+1)->dt2) + (op+1)->dt1) * (op+1)->mul ) >> 1;
				(op+2)->freq = ( (get_freq(kc_channel + (op+2)->dt2) + (op+2)->dt1) * (op+2)->mul ) >> 1;
				(op+3)->freq = ( (get_freq(kc_channel + (op+3)->dt2) + (op+3)->dt1) * (op+3)->mul ) >> 1;
			}
			break;
		case 0x18: op->pms = (v>>4) & 7; op->ams = (v & 3); break;
		}
		break;

	case 0x40:
		{
			UINT32 olddt1_i = op->dt1_i, oldmul = op->mul;
			op->dt1_i = (v&0x70)<<1;
			op->mul   = (v&0x0f) ? (v&0x0f)<<1: 1;
			if (olddt1_i != op->dt1_i) op->dt1 = chip->dt1_freq[ op->dt1_i + (op->kc>>2) ];
			if ( (olddt1_i != op->dt1_i) || (oldmul != op->mul) )
				op->freq = ( (get_freq(op->kc_i + op->dt2) + op->dt1) * op->mul ) >> 1;
		}
		break;

	case 0x60: op->tl = (v&0x7f)<<(ENV_BITS-7); break;

	case 0x80:
		{
			UINT32 oldks = op->ks, oldar = op->ar;
			op->ks = 5-(v>>6);
			op->ar = (v&0x1f) ? 32 + ((v&0x1f)<<1) : 0;
			if ( (op->ar != oldar) || (op->ks != oldks) ) {
				if ((op->ar + (op->kc>>op->ks)) < 32+62) {
					op->eg_sh_ar  = eg_rate_shift [op->ar  + (op->kc>>op->ks) ];
					op->eg_sel_ar = eg_rate_select[op->ar  + (op->kc>>op->ks) ];
				} else {
					op->eg_sh_ar  = 0;
					op->eg_sel_ar = 17*RATE_STEPS;
				}
			}
			if (op->ks != oldks) {
				op->eg_sh_d1r = eg_rate_shift [op->d1r + (op->kc>>op->ks) ];
				op->eg_sel_d1r= eg_rate_select[op->d1r + (op->kc>>op->ks) ];
				op->eg_sh_d2r = eg_rate_shift [op->d2r + (op->kc>>op->ks) ];
				op->eg_sel_d2r= eg_rate_select[op->d2r + (op->kc>>op->ks) ];
				op->eg_sh_rr  = eg_rate_shift [op->rr  + (op->kc>>op->ks) ];
				op->eg_sel_rr = eg_rate_select[op->rr  + (op->kc>>op->ks) ];
			}
		}
		break;

	case 0xa0:
		op->AMmask = (v&0x80) ? ~0 : 0;
		op->d1r    = (v&0x1f) ? 32 + ((v&0x1f)<<1) : 0;
		op->eg_sh_d1r = eg_rate_shift [op->d1r + (op->kc>>op->ks) ];
		op->eg_sel_d1r= eg_rate_select[op->d1r + (op->kc>>op->ks) ];
		break;

	case 0xc0:
		{
			UINT32 olddt2 = op->dt2;
			op->dt2 = dt2_tab[ v>>6 ];
			if (op->dt2 != olddt2) op->freq = ( (get_freq(op->kc_i + op->dt2) + op->dt1) * op->mul ) >> 1;
		}
		op->d2r = (v&0x1f) ? 32 + ((v&0x1f)<<1) : 0;
		op->eg_sh_d2r = eg_rate_shift [op->d2r + (op->kc>>op->ks) ];
		op->eg_sel_d2r= eg_rate_select[op->d2r + (op->kc>>op->ks) ];
		break;

	case 0xe0:
		op->d1l = d1l_tab[ v>>4 ];
		op->rr  = 34 + ((v&0x0f)<<2);
		op->eg_sh_rr  = eg_rate_shift [op->rr  + (op->kc>>op->ks) ];
		op->eg_sel_rr = eg_rate_select[op->rr  + (op->kc>>op->ks) ];
		break;
	}

	refresh_active_channels(chip);
}

__attribute__((noinline)) int YM2151ReadStatus( int n )
{
	return YMPSG[n].status;
}

int YM2151Init(int num, int clock, int rate)
{
	int i;
	if (YMPSG) return -1;

	// --- ДИНАМИЧЕСКАЯ ГЕНЕРАЦИЯ СИНУСА В DRAM (4 КБ) ---
	if (!sin_tab_dram) {
		sin_tab_dram = (unsigned int *)heap_caps_malloc(SIN_LEN * sizeof(unsigned int), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (sin_tab_dram) {
			int i;
			double m, o;
			int n;
			for (i = 0; i < SIN_LEN; i++)
			{
				m = sin( ((i*2)+1) * M_PI / SIN_LEN );
				if (m > 0.0)
					o = 8.0 * log(1.0/m) / log(2.0);
				else
					o = 8.0 * log(-1.0/m) / log(2.0);

				o = o / (ENV_STEP/4.0);

				n = (int)(2.0 * o);
				if (n & 1)
					n = (n >> 1) + 1;
				else
					n = n >> 1;

				sin_tab_dram[i] = n*2 + (m >= 0.0 ? 0 : 1);
			}
			sin_tab_ptr = sin_tab_dram;
			printf("YM2151: sin_tab_dram (4KB) generated in fast DRAM\n");
		}
	}
	// ------------------------------------------------

	YMNumChips = num;
	YMPSG = (YM2151 *)malloc(sizeof(YM2151) * YMNumChips);
	if (YMPSG == NULL) return 1;
	memset(YMPSG, 0, sizeof(YM2151) * YMNumChips);
	init_tables();

	for (i=0 ; i<YMNumChips; i++)
	{
		YMPSG[i].clock = clock;
		YMPSG[i].sampfreq = rate ? rate : 44100;
		YMPSG[i].irqhandler = NULL;
		YMPSG[i].porthandler = NULL;
		init_chip_tables( &YMPSG[i] );
		YMPSG[i].lfo_timer_add = (1<<LFO_SH) * (clock/64.0) / YMPSG[i].sampfreq;
		YMPSG[i].eg_timer_add  = (1<<EG_SH)  * (clock/64.0) / YMPSG[i].sampfreq;
		YMPSG[i].eg_timer_overflow = ( 3 ) * (1<<EG_SH);
		YMPSG[i].tim_A = 0; YMPSG[i].tim_B = 0;
		YM2151ResetChip(i);
	}
	return 0;
}

void YM2151Shutdown(void)
{
	if (!YMPSG) return;
	free(YMPSG);
	YMPSG = NULL;

	if (sin_tab_dram) {
		free(sin_tab_dram);
		sin_tab_dram = NULL;
		sin_tab_ptr = NULL;  // Просто обнуляем указатель
	}

	if (freq_base) {
		free(freq_base);
		freq_base = NULL;
	}

	if (tl_tab_base) {
		free(tl_tab_base);
		tl_tab_base = NULL;
	}
}

void YM2151ResetChip(int num)
{
	int i;
	YM2151 *chip = &YMPSG[num];
	for (i=0; i<32; i++) {
		memset(&chip->oper[i],'\0',sizeof(YM2151Operator));
		chip->oper[i].volume = MAX_ATT_INDEX;
	}
	chip->eg_timer = 0; chip->eg_cnt = 0;
	chip->lfo_timer = 0; chip->lfo_counter = 0; chip->lfo_phase = 0;
	chip->lfo_wsel = 0; chip->pmd = 0; chip->amd = 0; chip->lfa = 0; chip->lfp = 0;
	chip->test = 0; chip->irq_enable = 0;
	chip->tim_A = 0; chip->tim_B = 0; chip->tim_A_val = 0; chip->tim_B_val = 0;
	chip->timer_A_index = 0; chip->timer_B_index = 0;
	chip->timer_A_index_old = 0; chip->timer_B_index_old = 0;
	chip->noise = 0; chip->noise_rng = 0; chip->noise_p = 0;
	chip->noise_f = chip->noise_tab[0];
	chip->csm_req = 0; chip->status = 0;
	YM2151WriteReg(num, 0x1b, 0);
	YM2151WriteReg(num, 0x18, 0);
	for (i=0x20; i<0x100; i++) YM2151WriteReg(num, i, 0);

	refresh_active_channels(chip);
}

INLINE signed int op_calc(YM2151Operator * OP, unsigned int env, signed int pm)
{
	UINT32 idx = ((OP->phase >> 16) + (pm >> 1)) & SIN_MASK;
	UINT32 p = (env << 3) + sin_tab_ptr[idx];
	
	// Ограничение по тишине (ENV_QUIET в оригинале равен TL_TAB_LEN >> 3 = 832).
	if (p >= 6656) return 0;
	
	// Вычисление значения на основе 1 КБ базовой таблицы в DRAM:
	signed int val = tl_tab_base[(p >> 1) & 255] >> (p >> 9);
	return (p & 1) ? -val : val;
}

INLINE signed int op_calc1(YM2151Operator * OP, unsigned int env, signed int pm)
{
	UINT32 idx = ((OP->phase >> 16) + (pm >> 16)) & SIN_MASK;
	UINT32 p = (env << 3) + sin_tab_ptr[idx];
	
	if (p >= 6656) return 0;
	
	signed int val = tl_tab_base[(p >> 1) & 255] >> (p >> 9);
	return (p & 1) ? -val : val;
}

#define volume_calc(OP) ((OP)->tl + ((UINT32)(OP)->volume) + (AM & (OP)->AMmask))

INLINE void chan_calc(unsigned int chan)
{
	YM2151Operator *op;
	unsigned int env;
	UINT32 AM = 0;

	m2 = c1 = c2 = mem = 0;
	op = &PSG->oper[chan*4];
	*op->mem_connect = op->mem_value;
	if (op->ams) AM = PSG->lfa << (op->ams-1);
	env = volume_calc(op);
	{
		INT32 out = op->fb_out_prev + op->fb_out_curr;
		op->fb_out_prev = op->fb_out_curr;
		if (!op->connect) mem = c1 = c2 = op->fb_out_prev;
		else *op->connect = op->fb_out_prev;
		op->fb_out_curr = 0;
		if (env < ENV_QUIET) {
			INT32 pm = op->fb_shift ? (out << op->fb_shift) : 0;
			op->fb_out_curr = op_calc1(op, env, pm);
		}
	}
	env = volume_calc(op+1);
	if (env < ENV_QUIET) *(op+1)->connect += op_calc(op+1, env, m2);
	env = volume_calc(op+2);
	if (env < ENV_QUIET) *(op+2)->connect += op_calc(op+2, env, c1);
	env = volume_calc(op+3);
	if (env < ENV_QUIET) chanout[chan] += op_calc(op+3, env, c2);
	op->mem_value = mem;
}

INLINE void chan7_calc(void)
{
	YM2151Operator *op;
	unsigned int env;
	UINT32 AM = 0;

	m2 = c1 = c2 = mem = 0;
	op = &PSG->oper[7*4];
	*op->mem_connect = op->mem_value;
	if (op->ams) AM = PSG->lfa << (op->ams-1);
	env = volume_calc(op);
	{
		INT32 out = op->fb_out_prev + op->fb_out_curr;
		op->fb_out_prev = op->fb_out_curr;
		if (!op->connect) mem = c1 = c2 = op->fb_out_prev;
		else *op->connect = op->fb_out_prev;
		op->fb_out_curr = 0;
		if (env < ENV_QUIET) {
			INT32 pm = op->fb_shift ? (out << op->fb_shift) : 0;
			op->fb_out_curr = op_calc1(op, env, pm);
		}
	}
	env = volume_calc(op+1);
	if (env < ENV_QUIET) *(op+1)->connect += op_calc(op+1, env, m2);
	env = volume_calc(op+2);
	if (env < ENV_QUIET) *(op+2)->connect += op_calc(op+2, env, c1);
	env = volume_calc(op+3);
	if (PSG->noise & 0x80) {
		UINT32 noiseout = 0;
		if (env < 0x3ff) noiseout = (env ^ 0x3ff) * 2;
		chanout[7] += ((PSG->noise_rng&0x10000) ? noiseout: -noiseout);
	} else {
		if (env < ENV_QUIET) chanout[7] += op_calc(op+3, env, c2);
	}
	op->mem_value = mem;
}

INLINE void advance_eg(void)
{
	YM2151Operator *op;
	unsigned int i;
	int state_changed = 0;

	PSG->eg_timer += PSG->eg_timer_add;
	
	if (UNLIKELY(PSG->eg_timer >= PSG->eg_timer_overflow))
	{
		PSG->eg_timer -= PSG->eg_timer_overflow;
		PSG->eg_cnt++;

		op = &PSG->oper[0];
		i = 32;
		do
		{
			if (op->state != EG_OFF)
			{
				switch(op->state)
				{
				case EG_ATT:
					if ( !(PSG->eg_cnt & ((1<<op->eg_sh_ar)-1) ) )
					{
						op->volume += (~op->volume *
                                       (eg_inc[op->eg_sel_ar + ((PSG->eg_cnt>>(op)->eg_sh_ar)&7)])
                                      ) >>4;
						if (op->volume <= MIN_ATT_INDEX)
						{
							op->volume = MIN_ATT_INDEX;
							op->state = EG_DEC;
						}
					}
				break;

				case EG_DEC:
					if ( !(PSG->eg_cnt & ((1<<op->eg_sh_d1r)-1) ) )
					{
						op->volume += eg_inc[op->eg_sel_d1r + ((PSG->eg_cnt>>op->eg_sh_d1r)&7)];
						if ( op->volume >= op->d1l )
							op->state = EG_SUS;
					}
				break;

				case EG_SUS:
					if ( !(PSG->eg_cnt & ((1<<op->eg_sh_d2r)-1) ) )
					{
						op->volume += eg_inc[op->eg_sel_d2r + ((PSG->eg_cnt>>op->eg_sh_d2r)&7)];
						if ( op->volume >= MAX_ATT_INDEX )
						{
							op->volume = MAX_ATT_INDEX;
							op->state = EG_OFF;
							state_changed = 1;
						}
					}
				break;

				case EG_REL:
					if ( !(PSG->eg_cnt & ((1<<op->eg_sh_rr)-1) ) )
					{
						op->volume += eg_inc[op->eg_sel_rr + ((PSG->eg_cnt>>op->eg_sh_rr)&7)];
						if ( op->volume >= MAX_ATT_INDEX )
						{
							op->volume = MAX_ATT_INDEX;
							op->state = EG_OFF;
							state_changed = 1;
						}
					}
				break;
				}
			}
			op++;
			i--;
		} while (i);

		// Проверка флага вынесена за пределы цикла
		if (state_changed)
		{
			refresh_active_channels(PSG);
		}
	}
}

INLINE void advance(void)
{
	YM2151Operator *op;
	unsigned int i;
	int a,p;

	// Оптимизация LFO: Обходим весь блок LFO, если модуляция выключена
	if (PSG->amd == 0 && PSG->pmd == 0)
	{
		PSG->lfa = 0;
		PSG->lfp = 0;
	}
	else
	{
		if (PSG->test&2) PSG->lfo_phase = 0;
		else
		{
			PSG->lfo_timer += PSG->lfo_timer_add;
			if (PSG->lfo_timer >= PSG->lfo_overflow)
			{
				PSG->lfo_timer   -= PSG->lfo_overflow;
				PSG->lfo_counter += PSG->lfo_counter_add;
				PSG->lfo_phase   += (PSG->lfo_counter>>4);
				PSG->lfo_phase   &= 255;
				PSG->lfo_counter &= 15;
			}
		}

		i = PSG->lfo_phase;
		switch (PSG->lfo_wsel)
		{
		case 0: a = 255 - i; if (i<128) p = i; else p = i - 255; break;
		case 1: if (i<128){ a = 255; p = 128; }else{ a = 0; p = -128; } break;
		case 2:
			if (i<128) a = 255 - (i*2); else a = (i*2) - 256;
			if (i<64) p = i*2;
			else if (i<128) p = 255 - i*2;
			else if (i<192) p = 256 - i*2;
			else p = i*2 - 511;
			break;
		case 3:
		default:
			a = lfo_noise_waveform[i];
			p = a-128;
			break;
		}
		
		PSG->lfa = (a * PSG->amd) >> 7;
		PSG->lfp = (p * PSG->pmd) >> 7;
	}

	// Оптимизация Шума: Вычисляем шум только тогда, когда он включен на канале 7
	if (PSG->noise & 0x80)
	{
		PSG->noise_p += PSG->noise_f;
		i = (PSG->noise_p>>16);
		PSG->noise_p &= 0xffff;
		while (i) {
			UINT32 j = ( (PSG->noise_rng ^ (PSG->noise_rng>>3) ) & 1) ^ 1;
			PSG->noise_rng = (j<<16) | (PSG->noise_rng>>1);
			i--;
		}
	}

	// Оптимизация инкрементации фазы: Обходим неактивные каналы по битовой маске
	uint32_t active_mask = PSG->active_chan_mask;
	op = &PSG->oper[0];
	for (int c = 0; c < 8; c++)
	{
		if (active_mask & (1 << c))
		{
			if (op->pms)
			{
				INT32 mod_ind = PSG->lfp;
				if (op->pms < 6) mod_ind >>= (6 - op->pms);
				else mod_ind <<= (op->pms - 5);

				if (mod_ind)
				{
					UINT32 kc_channel = op->kc_i + mod_ind;
					(op+0)->phase += ( (get_freq(kc_channel + (op+0)->dt2) + (op+0)->dt1) * (op+0)->mul ) >> 1;
					(op+1)->phase += ( (get_freq(kc_channel + (op+1)->dt2) + (op+1)->dt1) * (op+1)->mul ) >> 1;
					(op+2)->phase += ( (get_freq(kc_channel + (op+2)->dt2) + (op+2)->dt1) * (op+2)->mul ) >> 1;
					(op+3)->phase += ( (get_freq(kc_channel + (op+3)->dt2) + (op+3)->dt1) * (op+3)->mul ) >> 1;
				}
				else
				{
					(op+0)->phase += (op+0)->freq;
					(op+1)->phase += (op+1)->freq;
					(op+2)->phase += (op+2)->freq;
					(op+3)->phase += (op+3)->freq;
				}
			}
			else
			{
				(op+0)->phase += (op+0)->freq;
				(op+1)->phase += (op+1)->freq;
				(op+2)->phase += (op+2)->freq;
				(op+3)->phase += (op+3)->freq;
			}
		}
		op += 4;
	}

	if (PSG->csm_req)
	{
		if (PSG->csm_req==2)
		{
			op = &PSG->oper[0]; i = 32;
			do { KEY_ON(op, 2); op++; i--; }while (i);
			PSG->csm_req = 1;
			refresh_active_channels(PSG);
		}
		else
		{
			op = &PSG->oper[0]; i = 32;
			do { KEY_OFF(op,~2); op++; i--; }while (i);
			PSG->csm_req = 0;
			refresh_active_channels(PSG);
		}
	}
}

__attribute__((noinline)) void YM2151UpdateOne(int num, INT16 **buffers, int length)
{
	int i;
	signed int outl,outr;
	SAMP *bufL, *bufR;

	bufL = buffers[0];
	bufR = buffers[1];
	PSG = &YMPSG[num];

	if (PSG->tim_B)
	{
		PSG->tim_B_val -= ( length << TIMER_SH );
		if (PSG->tim_B_val<=0)
		{
			PSG->tim_B_val += PSG->tim_B_tab[ PSG->timer_B_index ];
			if ( PSG->irq_enable & 0x08 )
			{
				int oldstate = PSG->status & 3;
				PSG->status |= 2;
				if ((!oldstate) && (PSG->irqhandler)) (*PSG->irqhandler)(1);
			}
		}
	}

	uint32_t active_mask = PSG->active_chan_mask;

	for (i=0; i<length; i++)
	{
		advance_eg();  
		active_mask = PSG->active_chan_mask; 

		chanout[0] = 0; chanout[1] = 0; chanout[2] = 0; chanout[3] = 0;
		chanout[4] = 0; chanout[5] = 0; chanout[6] = 0; chanout[7] = 0;

		if (active_mask & 1)   chan_calc(0);
		if (active_mask & 2)   chan_calc(1);
		if (active_mask & 4)   chan_calc(2);
		if (active_mask & 8)   chan_calc(3);
		if (active_mask & 16)  chan_calc(4);
		if (active_mask & 32)  chan_calc(5);
		if (active_mask & 64)  chan_calc(6);
		if (active_mask & 128) chan7_calc();

		outl = chanout[0] & PSG->pan[0]; outr = chanout[0] & PSG->pan[1];
		outl += (chanout[1] & PSG->pan[2]); outr += (chanout[1] & PSG->pan[3]);
		outl += (chanout[2] & PSG->pan[4]); outr += (chanout[2] & PSG->pan[5]);
		outl += (chanout[3] & PSG->pan[6]); outr += (chanout[3] & PSG->pan[7]);
		outl += (chanout[4] & PSG->pan[8]); outr += (chanout[4] & PSG->pan[9]);
		outl += (chanout[5] & PSG->pan[10]); outr += (chanout[5] & PSG->pan[11]);
		outl += (chanout[6] & PSG->pan[12]); outr += (chanout[6] & PSG->pan[13]);
		outl += (chanout[7] & PSG->pan[14]); outr += (chanout[7] & PSG->pan[15]);

		outl >>= FINAL_SH; outr >>= FINAL_SH;
		
		outl = outl > MAXOUT ? MAXOUT : (outl < MINOUT ? MINOUT : outl);
		outr = outr > MAXOUT ? MAXOUT : (outr < MINOUT ? MINOUT : outr);
		
		((SAMP*)bufL)[i] = (SAMP)outl;
		((SAMP*)bufR)[i] = (SAMP)outr;

		if (PSG->tim_A)
		{
			PSG->tim_A_val -= ( 1 << TIMER_SH );
			if (PSG->tim_A_val <= 0)
			{
				PSG->tim_A_val += PSG->tim_A_tab[ PSG->timer_A_index ];
				if (PSG->irq_enable & 0x04)
				{
					int oldstate = PSG->status & 3;
					PSG->status |= 1;
					if ((!oldstate) && (PSG->irqhandler)) (*PSG->irqhandler)(1);
				}
				if (PSG->irq_enable & 0x80) PSG->csm_req = 2;
			}
		}
		advance();
	}
}

void YM2151SetIrqHandler(int n, void(*handler)(int irq))
{
	YMPSG[n].irqhandler = handler;
}

void YM2151SetPortWriteHandler(int n, write8_handler handler)
{
	YMPSG[n].porthandler = handler;
}
