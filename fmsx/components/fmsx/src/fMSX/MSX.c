/**
    fMSX: portable MSX emulator — MSX.c

    MSX-specific hardware implementation: slots, memory mapper,
    PPIs, VDP, PSG, clock, initialization, and machine-dependent
    driver definitions.

    Original code Copyright (C) Marat Fayzullin 1994-2021
    You are not allowed to distribute this software commercially.
    Please notify the original author if you make changes.

    Optimized for ESP32-S3 (Xtensa) by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com

    Optimizations in this version:

    - Branch prediction hints (LIKELY/UNLIKELY) added to I/O port
     handlers (InZ80, OutZ80, RdZ80, WrZ80) for 1-2% IPC gain.

    - Software Line Cache (1 KB, 16×64B direct-mapped) in DRAM
     for ROM reads at 0x4000-0x7FFF. On cache miss, only 64 bytes
     are copied via unrolled uint32_t assignment (no memcpy call).
     Tags auto-invalidate on bank switch; write-through in WrZ80
     maintains coherency. Zero overhead on bank switch, ~2-3 µs
     on miss, 1 cycle on hit.

    - Dual Audio Engine Integration: I/O ports (InZ80/OutZ80) route
     PSG/OPLL/SCC writes to both native fMSX chips (SaveState) and
     Okazaki studio chips (accurate rendering). Switchable via
     CurrentSndMode: SND_MODE_FAST (Marat Fayzullin) or
     SND_MODE_ACCURATE (Mitsutaka Okazaki).

    - SCC bus snooping in MapROM(): full 16-bit address capture
     (0x9000, 0xBFFE/BFFF, 0x9800-0x98FF, 0xB800-0xB8FF) for
     unconditional studio SCC register writes.

    This file is distributed under the same terms as the original
    fMSX code by Marat Fayzullin. Commercial distribution is
    prohibited without permission from the original author.
*/

#include "MSX.h"
#include <esp_attr.h>
#include "Sound.h"
#include "Floppy.h"
#include "SHA1.h"
#include "MCF.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>


#ifdef __BORLANDC__
#include <dir.h>
#endif

#ifdef __WATCOMC__
#include <direct.h>
#endif

#ifdef ZLIB
#include <zlib.h>
#endif

#ifdef ANDROID
#include "MemFS.h"
#define puts LOGI
#define printf LOGI
#endif

#define PRINTOK                                                                \
    if (Verbose)                                                               \
    puts("OK")
#define PRINTFAILED                                                            \
    if (Verbose)                                                               \
    puts("FAILED")
#define PRINTRESULT(R)                                                         \
    if (Verbose)                                                               \
    puts((R) ? "OK" : "FAILED")

#define RGB2INT(R, G, B) ((B) | ((int)(G) << 8) | ((int)(R) << 16))

/* Branch prediction macros for micro-optimizations */
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* MSDOS chdir() is broken and has to be replaced :( */
#ifdef MSDOS
#include "LibMSDOS.h"
#define chdir(path) ChangeDir(path)
#endif

/* Studio Audio — forward-декларации */
struct PSG;
struct OPLL;
struct SCC;

extern struct PSG *psg_acc;
extern struct OPLL *studio_opll;
extern struct SCC *studio_scc;

/* fMSX palette dirty flag (defined in Common.h, set in MSX.c OutZ80 0x9A,
 * checked in Common.h RefreshLine8) */
extern bool msx_palette_dirty;

// === ОКАZАКI STUDIO ENGINE ===
extern void PSG_writeIO(void *psg, unsigned int adr, unsigned int val);
extern void OPLL_writeReg(void *opll, unsigned int reg, unsigned int val);
extern void SCC_write(void *scc, unsigned int adr, unsigned int val);
// ==== Флаги изменений для 2 дисководов ====================
extern int msx_disk_modified[2]; // 
extern int msx_disk_autosave;
int msx_disk_modified[2] = {0, 0};
// ==========================================================

/** User-defined parameters for fMSX *************************/
int Mode = MSX_MSX2 | MSX_NTSC | MSX_MSXDOS2 | MSX_GUESSA | MSX_GUESSB;
byte Verbose = 1;          /* Debug msgs ON/OFF      */
byte UPeriod = 75;         /* % of frames to draw    */
int VPeriod = CPU_VPERIOD; /* CPU cycles per VBlank  */
int HPeriod = CPU_HPERIOD; /* CPU cycles per HBlank  */
int RAMPages = 4;          /* Number of RAM pages    */
int VRAMPages = 2;         /* Number of VRAM pages   */
byte ExitNow = 0;          /* 1 = Exit the emulator  */

/** Main hardware: CPU, RAM, VRAM, mappers *******************/
Z80 CPU; /* Z80 CPU state and regs */

byte *VRAM, *VPAGE; /* Video RAM              */

byte *RAM[8];   /* Main RAM (8x8kB pages) */
byte *EmptyRAM; /* Empty RAM page (16kB)  */
/* Software Line Cache: 1KB in DRAM, 16 lines x 64 bytes */
__attribute__((section(".data"))) static uint8_t ROMLineCache[16][64];
__attribute__((section(".data"))) static uint32_t ROMLineTag[16];
#define LINE_IDX(A) (((A) >> 6) & 0x0F)
byte SaveCMOS;         /* Save CMOS.ROM on exit  */
byte *MemMap[4][4][8]; /* Memory maps [PPage][SPage][Addr] */

byte *RAMData;     /* RAM Mapper contents    */
byte RAMMapper[4]; /* RAM Mapper state       */
byte RAMMask;      /* RAM Mapper mask        */

byte *ROMData[MAXSLOTS];     /* ROM Mapper contents    */
byte ROMMapper[MAXSLOTS][4]; /* ROM Mappers state      */
byte ROMMask[MAXSLOTS];      /* ROM Mapper masks       */
byte ROMType[MAXSLOTS];      /* ROM Mapper types       */

byte EnWrite[4];        /* 1 if write enabled     */
byte PSL[4], SSL[4];    /* Lists of current slots */
byte PSLReg, SSLReg[4]; /* Storage for A8h port and (FFFFh) */

/** Memory blocks to free in TrashMSX() **********************/
void *Chunks[MAXCHUNKS]; /* Memory blocks to free  */
int NChunks;             /* Number of memory blcks */

/** Working directory names **********************************/
const char *ProgDir = 0; /* Program directory      */
const char *WorkDir;     /* Working directory      */

/** Cartridge files used by fMSX *****************************/
const char *ROMName[MAXCARTS] = {"CARTA.ROM", "CARTB.ROM"};

/** On-cartridge SRAM data ***********************************/
char *SRAMName[MAXSLOTS] = {0, 0, 0, 0, 0, 0}; /* Filenames (gen-d)*/
byte SaveSRAM[MAXSLOTS] = {0, 0, 0, 0, 0, 0};  /* Save SRAM on exit*/
byte *SRAMData[MAXSLOTS];                      /* SRAM (battery backed)  */

/** Disk images used by fMSX *********************************/
const char *DSKName[MAXDRIVES] = {"DRIVEA.DSK", "DRIVEB.DSK"};

/** Soundtrack logging ***************************************/
const char *SndName = "LOG.MID"; /* Sound log file         */

/** Emulation state saving ***********************************/
const char *STAName = "DEFAULT.STA"; /* State file (autogen-d)*/

/** Fixed font used by fMSX **********************************/
const char *FNTName = "DEFAULT.FNT"; /* Font file for text   */
byte *FontBuf;                       /* Font for text modes    */

/** Printer **************************************************/
const char *PrnName = 0; /* Printer redirect. file */
FILE *PrnStream;

/** Cassette tape ********************************************/
const char *CasName = "DEFAULT.CAS"; /* Tape image file     */
FILE *CasStream;

/** Serial port **********************************************/
const char *ComName = 0; /* Serial redirect. file  */
FILE *ComIStream;
FILE *ComOStream;

/** Kanji font ROM *******************************************/
byte *Kanji;   /* Kanji ROM 4096x32      */
int KanLetter; /* Current letter index   */
byte KanCount; /* Byte count 0..31       */

/** Keyboard, joystick, and mouse ****************************/
volatile byte KeyState[16];      /* Keyboard map state     */
word JoyState;                   /* Joystick states        */
int MouState[2];                 /* Mouse states           */
byte MouseDX[2], MouseDY[2];     /* Mouse offsets          */
byte OldMouseX[2], OldMouseY[2]; /* Old mouse coordinates  */
byte MCount[2];                  /* Mouse nibble counter   */

/** General I/O registers: i8255 *****************************/
I8255 PPI;  /* i8255 PPI at A8h-ABh   */
byte IOReg; /* Storage for AAh port   */

/** Disk controller: WD1793 **********************************/
WD1793 FDC;     /* WD1793 at 7FF8h-7FFFh  */
FDIDisk FDD[4]; /* Floppy disk images     */

/** Sound hardware: PSG, SCC, OPLL ***************************/
AY8910 PSG;    /* PSG registers & state  */
YM2413 OPLL;   /* OPLL registers & state */
SCC SCChip;    /* SCC registers & state  */
byte SCCOn[2]; /* 1 = SCC page active    */
word FMPACKey; /* MAGIC = SRAM active    */

/** Serial I/O hardware: i8251+i8253 *************************/
I8251 SIO; /* SIO registers & state  */

/** Real-time clock ******************************************/
byte RTCReg, RTCMode; /* RTC register numbers   */
byte RTC[4][13];      /* RTC registers          */

/** Video processor ******************************************/
byte *ChrGen, *ChrTab, *ColTab; /* VDP tables (screen)    */
byte *SprGen, *SprTab;          /* VDP tables (sprites)   */
int ChrGenM, ChrTabM, ColTabM;  /* VDP masks (screen)     */
int SprTabM;                    /* VDP masks (sprites)    */
word VAddr;                     /* VRAM address in VDP    */
byte VKey, PKey;                /* Status keys for VDP    */
byte FGColor, BGColor;          /* Colors                 */
byte XFGColor, XBGColor;        /* Second set of colors   */
byte ScrMode;                   /* Current screen mode    */
byte VDP[64], VDPStatus[16];    /* VDP registers          */
byte IRQPending;                /* Pending interrupts     */
int ScanLine;                   /* Current scanline       */
byte VDPData;                   /* VDP data buffer        */
byte PLatch;                    /* Palette buffer         */
byte ALatch;                    /* Address buffer         */
int Palette[16];                /* Current palette        */

/** Cheat entries ********************************************/
int MCFCount = 0;               /* Size of MCFEntries[]   */
MCFEntry MCFEntries[MAXCHEATS]; /* Entries from .MCF file */

/** Cheat codes **********************************************/
byte CheatsON = 0;  /* 1: Cheats are on       */
int CheatCount = 0; /* # cheats, <=MAXCHEATS  */
CheatCode CheatCodes[MAXCHEATS];

/** Places in DiskROM to be patched with ED FE C9 ************/
static const word DiskPatches[] = {0x4010, 0x4013, 0x4016, 0x401C, 0x401F, 0};

/** Places in BIOS to be patched with ED FE C9 ***************/
static const word BIOSPatches[] = {
    0x00E1, 0x00E4, 0x00E7, 0x00EA, 0x00ED, 0x00F0, 0x00F3, 0};

/** Cartridge map, by primary and secondary slots ************/
static const byte CartMap[4][4] = {
    {255, 3, 4, 5}, {0, 0, 0, 0}, {1, 1, 1, 1}, {2, 255, 255, 255}};

/** Screen Mode Handlers [number of screens + 1] *************/
void (*RefreshLine[MAXSCREEN + 2])(byte Y) = {
    RefreshLine0,   /* SCR 0:  TEXT 40x24  */
    RefreshLine1,   /* SCR 1:  TEXT 32x24  */
    RefreshLine2,   /* SCR 2:  BLK 256x192 */
    RefreshLine3,   /* SCR 3:  64x48x16    */
    RefreshLine4,   /* SCR 4:  BLK 256x192 */
    RefreshLine5,   /* SCR 5:  256x192x16  */
    RefreshLine6,   /* SCR 6:  512x192x4   */
    RefreshLine7,   /* SCR 7:  512x192x16  */
    RefreshLine8,   /* SCR 8:  256x192x256 */
    0,              /* SCR 9:  NONE        */
    RefreshLine10,  /* SCR 10: YAE 256x192 */
    RefreshLine10,  /* SCR 11: YAE 256x192 */
    RefreshLine12,  /* SCR 12: YJK 256x192 */
    RefreshLineTx80 /* SCR 0:  TEXT 80x24  */
};

/** VDP Address Register Masks *******************************/
static const struct {
    byte R2, R3, R4, R5, M2, M3, M4, M5;
} MSK[MAXSCREEN + 2] = {
    {0x7F, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, /* SCR 0:  TEXT 40x24  */
    {0x7F, 0xFF, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00}, /* SCR 1:  TEXT 32x24  */
    {0x7F, 0x80, 0x3C, 0xFF, 0x00, 0x7F, 0x03, 0x00}, /* SCR 2:  BLK 256x192 */
    {0x7F, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00}, /* SCR 3:  64x48x16    */
    {0x7F, 0x80, 0x3C, 0xFC, 0x00, 0x7F, 0x03, 0x03}, /* SCR 4:  BLK 256x192 */
    {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 5:  256x192x16  */
    {0x60, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 6:  512x192x4   */
    {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 7:  512x192x16  */
    {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 8:  256x192x256 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* SCR 9:  NONE        */
    {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 10: YAE 256x192 */
    {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 11: YAE 256x192 */
    {0x20, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0x03}, /* SCR 12: YJK 256x192 */
    {0x7C, 0xF8, 0x3F, 0x00, 0x03, 0x07, 0x00, 0x00}  /* SCR 0:  TEXT 80x24  */
};

/** MegaROM Mapper Names *************************************/
static const char *ROMNames[MAXMAPPERS + 1] = {"GENERIC/8kB",
                                               "GENERIC/16kB",
                                               "KONAMI5/8kB",
                                               "KONAMI4/8kB",
                                               "ASCII/8kB",
                                               "ASCII/16kB",
                                               "GMASTER2/SRAM",
                                               "FMPAC/SRAM",
                                               "UNKNOWN"};

/** Keyboard Mapping *****************************************/
const byte Keys[][2] = {
    {0, 0x00},  {8, 0x10},
    {8, 0x20},  {8, 0x80}, /* None,LEFT,UP,RIGHT */
    {8, 0x40},  {6, 0x01},
    {6, 0x02},  {6, 0x04}, /* DOWN,SHIFT,CONTROL,GRAPH */
    {7, 0x20},  {7, 0x08},
    {6, 0x08},  {7, 0x40}, /* BS,TAB,CAPSLOCK,SELECT */
    {8, 0x02},  {7, 0x80},
    {8, 0x08},  {8, 0x04}, /* HOME,ENTER,DELETE,INSERT */
    {6, 0x10},  {7, 0x10},
    {6, 0x20},  {6, 0x40}, /* COUNTRY,STOP,F1,F2 */
    {6, 0x80},  {7, 0x01},
    {7, 0x02},  {9, 0x08}, /* F3,F4,F5,PAD0 */
    {9, 0x10},  {9, 0x20},
    {9, 0x40},  {7, 0x04}, /* PAD1,PAD2,PAD3,ESCAPE */
    {9, 0x80},  {10, 0x01},
    {10, 0x02}, {10, 0x04}, /* PAD4,PAD5,PAD6,PAD7 */
    {8, 0x01},  {0, 0x02},
    {2, 0x01},  {0, 0x08}, /* SPACE,[!],["],[#] */
    {0, 0x10},  {0, 0x20},
    {0, 0x80},  {2, 0x01}, /* [$],[%],[&],['] */
    {1, 0x02},  {0, 0x01},
    {1, 0x01},  {1, 0x08}, /* [(],[)],[*],[=] */
    {2, 0x04},  {1, 0x04},
    {2, 0x08},  {2, 0x10}, /* [,],[-],[.],[/] */
    {0, 0x01},  {0, 0x02},
    {0, 0x04},  {0, 0x08}, /* 0,1,2,3 */
    {0, 0x10},  {0, 0x20},
    {0, 0x40},  {0, 0x80}, /* 4,5,6,7 */
    {1, 0x01},  {1, 0x02},
    {1, 0x80},  {1, 0x80}, /* 8,9,[:],[;] */
    {2, 0x04},  {1, 0x08},
    {2, 0x08},  {2, 0x10}, /* [<],[=],[>],[?] */
    {0, 0x04},  {2, 0x40},
    {2, 0x80},  {3, 0x01}, /* [@],A,B,C */
    {3, 0x02},  {3, 0x04},
    {3, 0x08},  {3, 0x10}, /* D,E,F,G */
    {3, 0x20},  {3, 0x40},
    {3, 0x80},  {4, 0x01}, /* H,I,J,K */
    {4, 0x02},  {4, 0x04},
    {4, 0x08},  {4, 0x10}, /* L,M,N,O */
    {4, 0x20},  {4, 0x40},
    {4, 0x80},  {5, 0x01}, /* P,Q,R,S */
    {5, 0x02},  {5, 0x04},
    {5, 0x08},  {5, 0x10}, /* T,U,V,W */
    {5, 0x20},  {5, 0x40},
    {5, 0x80},  {1, 0x20}, /* X,Y,Z,[[] */
    {1, 0x10},  {1, 0x40},
    {0, 0x40},  {1, 0x04}, /* [\],[]],[^],[_] */
    {2, 0x02},  {2, 0x40},
    {2, 0x80},  {3, 0x01}, /* [`],a,b,c */
    {3, 0x02},  {3, 0x04},
    {3, 0x08},  {3, 0x10}, /* d,e,f,g */
    {3, 0x20},  {3, 0x40},
    {3, 0x80},  {4, 0x01}, /* h,i,j,k */
    {4, 0x02},  {4, 0x04},
    {4, 0x08},  {4, 0x10}, /* l,m,n,o */
    {4, 0x20},  {4, 0x40},
    {4, 0x80},  {5, 0x01}, /* p,q,r,s */
    {5, 0x02},  {5, 0x04},
    {5, 0x08},  {5, 0x10}, /* t,u,v,w */
    {5, 0x20},  {5, 0x40},
    {5, 0x80},  {1, 0x20}, /* x,y,z,[{] */
    {1, 0x10},  {1, 0x40},
    {2, 0x02},  {8, 0x08}, /* [|],[}],[~],DEL */
    {10, 0x08}, {10, 0x10} /* PAD8,PAD9 */
};

/** Internal Functions ***************************************/
byte *LoadROM(const char *Name, int Size, byte *Buf);
int GuessROM(const byte *Buf, int Size);
int FindState(const char *Name);
void SetMegaROM(int Slot, byte P0, byte P1, byte P2, byte P3);
void MapROM(word A, byte V);
void PSlot(byte V);
void SSlot(byte V);
void VDPOut(byte R, byte V);
void Printer(byte V);
void PPIOut(byte New, byte Old);
int CheckSprites(void);
byte RTCIn(byte R);
byte SetScreen(void);
word SetIRQ(byte IRQ);
word StateID(void);
int ApplyCheats(void);

static int hasext(const char *FileName, const char *Ext);
static byte *GetMemory(int Size);
static void FreeMemory(const void *Ptr);
static void FreeAllMemory(void);

/** hasext() *************************************************/
static int hasext(const char *FileName, const char *Ext) {
    const char *P;
    int J;

    for (P = FileName + strlen(FileName);
         (P >= FileName) && (*P != '/') && (*P != '\\');
         --P) {
        for (--P;
             (P >= FileName) && (*P != '/') && (*P != '\\') && (*P != *Ext);
             --P)
            ;
        if ((P < FileName) || (*P == '/') || (*P == '\\')) {
            return (0);
        }
        for (J = 0; P[J] && Ext[J] && (toupper(P[J]) == toupper(Ext[J])); ++J)
            ;
        if (!Ext[J] && (!P[J] || (P[J] == *Ext))) {
            return (1);
        }
    }
    return (0);
}

/** GetMemory() **********************************************/
static byte *GetMemory(int Size) {
    byte *P;
    if ((Size <= 0) || (NChunks >= MAXCHUNKS)) {
        return (0);
    }
    P = (byte *)malloc(Size);
    if (P) {
        Chunks[NChunks++] = P;
    }
    return (P);
}

/** FreeMemory() *********************************************/
static void FreeMemory(const void *Ptr) {
    int J;
    if (!Ptr || (Ptr == (void *)EmptyRAM)) {
        return;
    }
    for (J = 0; (J < NChunks) && (Ptr != Chunks[J]); ++J)
        ;
    if (J < NChunks) {
        free(Chunks[J]);
        for (--NChunks; J < NChunks; ++J) {
            Chunks[J] = Chunks[J + 1];
        }
    }
}

/** FreeAllMemory() ******************************************/
static void FreeAllMemory(void) {
    int J;
    for (J = 0; J < NChunks; ++J) {
        free(Chunks[J]);
    }
    NChunks = 0;
}

/** StartMSX() ***********************************************/
int StartMSX(int NewMode, int NewRAMPages, int NewVRAMPages) {
    static const char *JoyTypes[] = {"nothing",
                                     "normal joystick",
                                     "mouse in joystick mode",
                                     "mouse in real mode"};
    static const byte RTCInit[4][13] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 40, 80, 15, 4, 4, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    int *T, I, J, K;
    byte *P;
    word A;

    T = (int *)"\01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
#ifdef LSB_FIRST
    if (*T != 1) {
        printf("********** This machine is high-endian. **********\n");
        return (0);
    }
#else
    if (*T == 1) {
        printf("********* This machine is low-endian. **********\n");
        return (0);
    }
#endif

    CasStream = PrnStream = ComIStream = ComOStream = 0;
    FontBuf = 0;
    RAMData = 0;
    VRAM = 0;
    Kanji = 0;
    WorkDir = 0;
    SaveCMOS = 0;
    FMPACKey = 0x0000;
    ExitNow = 0;
    NChunks = 0;
    CheatsON = 0;
    CheatCount = 0;
    MCFCount = 0;

    for (J = 0; J < MAXSLOTS; ++J) {
        ROMMask[J] = 0;
        ROMData[J] = 0;
        ROMType[J] = 0;
        SRAMData[J] = 0;
        SRAMName[J] = 0;
        SaveSRAM[J] = 0;
    }
    UPeriod = UPeriod < 1 ? 1 : UPeriod > 100 ? 100 : UPeriod;
    if (Verbose) {
        printf("Allocating 16kB for empty space...\n");
    }
    if (!(EmptyRAM = GetMemory(0x4000))) {
        PRINTFAILED;
        return (0);
    }
    memset(EmptyRAM, NORAM, 0x4000);
    for (I = 0; I < 4; ++I)
        for (J = 0; J < 4; ++J)
            for (K = 0; K < 8; ++K) {
                MemMap[I][J][K] = EmptyRAM;
            }
    if (ProgDir && (WorkDir = getcwd(0, 1024))) {
        Chunks[NChunks++] = (void *)WorkDir;
    }
    Mode = ~NewMode;
    RAMPages = 0;
    VRAMPages = 0;
    if ((ResetMSX(NewMode, NewRAMPages, NewVRAMPages) ^ NewMode) & MSX_MODEL) {
        return (0);
    }
    if (!RAMPages || !VRAMPages) {
        return (0);
    }
    if (ProgDir && chdir(ProgDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", ProgDir);
    }
    if (FNTName) {
        if (Verbose) {
            printf("Loading %s font...", FNTName);
        }
        J = LoadFNT(FNTName);
        PRINTRESULT(J);
    }
    if (Verbose) {
        printf("Loading optional ROMs: ");
    }
    if (LoadROM("CMOS.ROM", sizeof(RTC), (byte *)RTC)) {
        if (Verbose)
            printf("CMOS.ROM..");
    } else {
        memcpy(RTC, RTCInit, sizeof(RTC));
    }
    if ((Kanji = LoadROM("KANJI.ROM", 0x20000, 0))) {
        if (Verbose)
            printf("KANJI.ROM..");
    }
    if ((P = LoadROM("RS232.ROM", 0x4000, 0))) {
        if (Verbose) {
            printf("RS232.ROM..");
        }
        MemMap[3][3][2] = P;
        MemMap[3][3][3] = P + 0x2000;
    }
    PRINTOK;
    J = MAXCARTS;
    if (!MODEL(MSX_MSX1) && OPTION(MSX_MSXDOS2) && (MemMap[3][1][2] != EmptyRAM)
        && !ROMData[2])
        if (LoadCart("MSXDOS2.ROM", 2, MAP_GEN16)) {
            SetMegaROM(2, 0, 1, ROMMask[J] - 1, ROMMask[J]);
        }
    if (!MODEL(MSX_MSX1)) {
        for (; (J < MAXSLOTS) && ROMData[J]; ++J)
            ;
        if ((J < MAXSLOTS) && LoadCart("PAINTER.ROM", J, 0)) {
            ++J;
        }
    }
    for (; (J < MAXSLOTS) && ROMData[J]; ++J)
        ;
    if ((J < MAXSLOTS) && LoadCart("FMPAC.ROM", J, MAP_FMPAC)) {
        ++J;
    }
    for (; (J < MAXSLOTS) && ROMData[J]; ++J)
        ;
    if (J < MAXSLOTS) {
        if (LoadCart("GMASTER2.ROM", J, MAP_GMASTER2)) {
            ++J;
        } else if (LoadCart("GMASTER.ROM", J, 0)) {
            ++J;
        }
    }
    if (WorkDir && chdir(WorkDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", WorkDir);
    }
    for (J = 0; J < MAXCARTS; ++J) {
        LoadCart(ROMName[J], J, ROMGUESS(J) | ROMTYPE(J));
    }
    if (Verbose) {
        printf("Redirecting printer output to %s...OK\n",
               PrnName ? PrnName : "STDOUT");
    }
    ChangePrinter(PrnName);
    if (!ComName) {
        ComIStream = stdin;
        ComOStream = stdout;
    } else {
        if (Verbose) {
            printf("Redirecting serial I/O to %s...", ComName);
        }
        if (!(ComOStream = ComIStream = fopen(ComName, "r+b"))) {
            ComIStream = stdin;
            ComOStream = stdout;
        }
        PRINTRESULT(ComOStream != stdout);
    }
    if (CasName && ChangeTape(CasName))
        if (Verbose) {
            printf("Using %s as a tape\n", CasName);
        }
    Reset1793(&FDC, FDD, WD1793_INIT);
    FDC.Verbose = Verbose & 0x04;
    for (J = 0; J < MAXDRIVES; ++J) {
        FDD[J].Verbose = Verbose & 0x04;
        if (ChangeDisk(J, DSKName[J]))
            if (Verbose) {
                printf("Inserting %s into drive %c\n", DSKName[J], J + 'A');
            }
    }
    InitMIDI(SndName);
    if (Verbose) {
        printf("Initializing VDP, FDC, PSG, OPLL, SCC, and CPU...\n");
        printf("  Attached %s to joystick port A\n", JoyTypes[JOYTYPE(0)]);
        printf("  Attached %s to joystick port B\n", JoyTypes[JOYTYPE(1)]);
        printf("  %d CPU cycles per HBlank\n", HPeriod);
        printf("  %d CPU cycles per VBlank\n", VPeriod);
        printf("  %d scanlines\n", VPeriod / HPeriod);
    }
    if (Verbose) {
        printf("RUNNING ROM CODE...\n");
    }
    A = RunZ80(&CPU);
    if (Verbose) {
        printf("EXITED at PC = %04Xh.\n", A);
    }
    return (1);
}

/** TrashMSX() ***********************************************/
void TrashMSX(void) {
    FILE *F;
    int J;
    if (ProgDir && chdir(ProgDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", ProgDir);
    }
    if (SaveCMOS) {
        if (Verbose) {
            printf("Writing CMOS.ROM...");
        }
        if (!(F = fopen("CMOS.ROM", "wb"))) {
            SaveCMOS = 0;
        } else {
            if (fwrite(RTC, 1, sizeof(RTC), F) != sizeof(RTC)) {
                SaveCMOS = 0;
            }
            fclose(F);
        }
        PRINTRESULT(SaveCMOS);
    }
    if (WorkDir && chdir(WorkDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", WorkDir);
    }
    TrashMIDI();
    Reset1793(&FDC, FDD, WD1793_EJECT);
    ChangePrinter(0);
    ChangeTape(0);
    if (ComOStream && (ComOStream != stdout)) {
        fclose(ComOStream);
    }
    if (ComIStream && (ComIStream != stdin)) {
        fclose(ComIStream);
    }
    for (J = 0; J < MAXSLOTS; ++J) {
        LoadCart(0, J, ROMType[J]);
    }
    for (J = 0; J < MAXDRIVES; ++J) {
        ChangeDisk(J, 0);
    }
    FreeAllMemory();
}

/** ResetMSX() ***********************************************/
int ResetMSX(int NewMode, int NewRAMPages, int NewVRAMPages) {
    static const byte VDPSInit[16] = {
        0x9F, 0, 0x6C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const byte VDPInit[64] = {
        0x00, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const unsigned int PalInit[16] = {0x00000000,
                                             0x00000000,
                                             0x0020C020,
                                             0x0060E060,
                                             0x002020E0,
                                             0x004060E0,
                                             0x00A02020,
                                             0x0040C0E0,
                                             0x00E02020,
                                             0x00E06060,
                                             0x00C0C020,
                                             0x00C0C080,
                                             0x00208020,
                                             0x00C040A0,
                                             0x00A0A0A0,
                                             0x00E0E0E0};
    byte *P1, *P2;
    int J, I;

    if ((Mode ^ NewMode) & MSX_MODEL) {
        if (ProgDir && chdir(ProgDir)) {
            if (Verbose)
                printf("  Failed changing to '%s' directory!\n", ProgDir);
        }
        switch (NewMode & MSX_MODEL) {
            case MSX_MSX1:
                if (Verbose) {
                    printf("  Opening MSX.ROM...");
                }
                P1 = LoadROM("MSX.ROM", 0x8000, 0);
                PRINTRESULT(P1);
                if (!P1) {
                    NewMode = (NewMode & ~MSX_MODEL) | (Mode & MSX_MODEL);
                } else {
                    FreeMemory(MemMap[0][0][0]);
                    FreeMemory(MemMap[3][1][0]);
                    MemMap[0][0][0] = P1;
                    MemMap[0][0][1] = P1 + 0x2000;
                    MemMap[0][0][2] = P1 + 0x4000;
                    MemMap[0][0][3] = P1 + 0x6000;
                    MemMap[3][1][0] = EmptyRAM;
                    MemMap[3][1][1] = EmptyRAM;
                }
                break;
            case MSX_MSX2:
                if (Verbose) {
                    printf("  Opening MSX2.ROM...");
                }
                P1 = LoadROM("MSX2.ROM", 0x8000, 0);
                PRINTRESULT(P1);
                if (Verbose) {
                    printf("  Opening MSX2EXT.ROM...");
                }
                P2 = LoadROM("MSX2EXT.ROM", 0x4000, 0);
                PRINTRESULT(P2);
                if (!P1 || !P2) {
                    NewMode = (NewMode & ~MSX_MODEL) | (Mode & MSX_MODEL);
                    FreeMemory(P1);
                    FreeMemory(P2);
                } else {
                    FreeMemory(MemMap[0][0][0]);
                    FreeMemory(MemMap[3][1][0]);
                    MemMap[0][0][0] = P1;
                    MemMap[0][0][1] = P1 + 0x2000;
                    MemMap[0][0][2] = P1 + 0x4000;
                    MemMap[0][0][3] = P1 + 0x6000;
                    MemMap[3][1][0] = P2;
                    MemMap[3][1][1] = P2 + 0x2000;
                }
                break;
            case MSX_MSX2P:
                if (Verbose) {
                    printf("  Opening MSX2P.ROM...");
                }
                P1 = LoadROM("MSX2P.ROM", 0x8000, 0);
                PRINTRESULT(P1);
                if (Verbose) {
                    printf("  Opening MSX2PEXT.ROM...");
                }
                P2 = LoadROM("MSX2PEXT.ROM", 0x4000, 0);
                PRINTRESULT(P2);
                if (!P1 || !P2) {
                    NewMode = (NewMode & ~MSX_MODEL) | (Mode & MSX_MODEL);
                    FreeMemory(P1);
                    FreeMemory(P2);
                } else {
                    FreeMemory(MemMap[0][0][0]);
                    FreeMemory(MemMap[3][1][0]);
                    MemMap[0][0][0] = P1;
                    MemMap[0][0][1] = P1 + 0x2000;
                    MemMap[0][0][2] = P1 + 0x4000;
                    MemMap[0][0][3] = P1 + 0x6000;
                    MemMap[3][1][0] = P2;
                    MemMap[3][1][1] = P2 + 0x2000;
                }
                break;
            default:
                if (Verbose) {
                    printf("ResetMSX(): INVALID HARDWARE MODEL!\n");
                }
                NewMode = (NewMode & ~MSX_MODEL) | (Mode & MSX_MODEL);
                break;
        }
        if (WorkDir && chdir(WorkDir)) {
            if (Verbose)
                printf("Failed changing to '%s' directory!\n", WorkDir);
        }
    }
    if ((Mode ^ NewMode) & MSX_MODEL) {
        if (Verbose) {
            printf("  Patching BIOS: ");
        }
        for (J = 0; BIOSPatches[J]; ++J) {
            if (Verbose) {
                printf("%04X..", BIOSPatches[J]);
            }
            P1 = MemMap[0][0][0] + BIOSPatches[J];
            P1[0] = 0xED;
            P1[1] = 0xFE;
            P1[2] = 0xC9;
        }
        PRINTOK;
    }
    if ((Mode ^ NewMode) & MSX_PATCHBDOS) {
        if (ProgDir && chdir(ProgDir)) {
            if (Verbose)
                printf("  Failed changing to '%s' directory!\n", ProgDir);
        }
        if (Verbose) {
            printf("  Opening DISK.ROM...");
        }
        P1 = LoadROM("DISK.ROM", 0x4000, 0);
        PRINTRESULT(P1);
        if (WorkDir && chdir(WorkDir)) {
            if (Verbose)
                printf("  Failed changing to '%s' directory!\n", WorkDir);
        }
        if (!P1) {
            NewMode = (NewMode & ~MSX_PATCHBDOS) | (Mode & MSX_PATCHBDOS);
        } else {
            FreeMemory(MemMap[3][1][2]);
            MemMap[3][1][2] = P1;
            MemMap[3][1][3] = P1 + 0x2000;
            if (NewMode & MSX_PATCHBDOS) {
                if (Verbose) {
                    printf("  Patching BDOS: ");
                }
                for (J = 0; DiskPatches[J]; ++J) {
                    if (Verbose) {
                        printf("%04X..", DiskPatches[J]);
                    }
                    P2 = P1 + DiskPatches[J] - 0x4000;
                    P2[0] = 0xED;
                    P2[1] = 0xFE;
                    P2[2] = 0xC9;
                }
                PRINTOK;
            }
        }
    }
    Mode = NewMode;
    ROMType[0] = ROMTYPE(0);
    ROMType[1] = ROMTYPE(1);
    VPeriod = (VIDEO(MSX_PAL) ? VPERIOD_PAL : VPERIOD_NTSC) / 6;
    HPeriod = HPERIOD / 6;
    CPU.IPeriod = CPU_H240;
    CPU.IAutoReset = 0;
    for (J = 1; J < NewRAMPages; J <<= 1)
        ;
    NewRAMPages = J;
    for (J = 1; J < NewVRAMPages; J <<= 1)
        ;
    NewVRAMPages = J;
    if ((NewRAMPages < (MODEL(MSX_MSX1) ? 4 : 8)) || (NewRAMPages > 256)) {
        NewRAMPages = MODEL(MSX_MSX1) ? 4 : 8;
    }
    if ((NewVRAMPages < (MODEL(MSX_MSX1) ? 2 : 8)) || (NewVRAMPages > 8)) {
        NewVRAMPages = MODEL(MSX_MSX1) ? 2 : 8;
    }
    if (NewRAMPages != RAMPages) {
        if (Verbose) {
            printf("Allocating %dkB for RAM...", NewRAMPages * 16);
        }
        if ((P1 = GetMemory(NewRAMPages * 0x4000))) {
            memset(P1, NORAM, NewRAMPages * 0x4000);
            FreeMemory(RAMData);
            RAMPages = NewRAMPages;
            RAMMask = NewRAMPages - 1;
            RAMData = P1;
        }
        PRINTRESULT(P1);
    }
    if (NewVRAMPages != VRAMPages) {
        if (Verbose) {
            printf("Allocating %dkB for VRAM...", NewVRAMPages * 16);
        }
        if ((P1 = GetMemory(NewVRAMPages * 0x4000))) {
            memset(P1, 0x00, NewVRAMPages * 0x4000);
            FreeMemory(VRAM);
            VRAMPages = NewVRAMPages;
            VRAM = P1;
        }
        PRINTRESULT(P1);
    }
    for (J = 0; J < 4; ++J) {
        EnWrite[J] = 0;
        PSL[J] = 0;
        SSL[J] = 0;
        MemMap[3][2][J * 2] = RAMData + (3 - J) * 0x4000;
        MemMap[3][2][J * 2 + 1] = MemMap[3][2][J * 2] + 0x2000;
        RAMMapper[J] = 3 - J;
        RAM[J * 2] = MemMap[0][0][J * 2];
        RAM[J * 2 + 1] = MemMap[0][0][J * 2 + 1];
    }
    for (J = 0; J < MAXSLOTS; ++J)
        if ((I = ROMMask[J] + 1) > 4) {
            if ((ROMData[J][0] == 'A') && (ROMData[J][1] == 'B')) {
                SetMegaROM(J, 0, 1, 2, 3);
            } else if ((ROMData[J][(I - 2) << 13] == 'A')
                       && (ROMData[J][((I - 2) << 13) + 1] == 'B')) {
                SetMegaROM(J, I - 2, I - 1, I - 2, I - 1);
            }
        }

    Reset8910(&PSG, PSG_CLOCK, 0);
    ResetSCC(&SCChip, AY8910_CHANNELS);
    Reset2413(&OPLL, AY8910_CHANNELS);
    //
    /* Сброс студийных чипов Okazaki через Sound.c */
    extern void ResetStudioChips(void);
    ResetStudioChips();
    //
    Sync8910(&PSG, AY8910_SYNC);
    SyncSCC(&SCChip, SCC_SYNC);
    Sync2413(&OPLL, YM2413_SYNC);
    
    
    
    Reset8251(&SIO, ComIStream, ComOStream);
    Reset8255(&PPI);
    PPI.Rout[0] = PSLReg = 0x00;
    PPI.Rout[2] = IOReg = 0x00;
    SSLReg[0] = 0x00;
    SSLReg[1] = 0x00;
    SSLReg[2] = 0x00;
    SSLReg[3] = 0x00;
    Reset1793(&FDC, FDD, WD1793_KEEP);
    memcpy(VDP, VDPInit, sizeof(VDP));
    memcpy(VDPStatus, VDPSInit, sizeof(VDPStatus));
    memset((void *)KeyState, 0xFF, 16);
    for (J = 0; J < 16; ++J) {
        Palette[J] = PalInit[J];
        SetColor(J,
                 (Palette[J] >> 16) & 0xFF,
                 (Palette[J] >> 8) & 0xFF,
                 Palette[J] & 0xFF);
    }
    for (J = 0; J < 2; ++J) {
        MouState[J] = MouseDX[J] = MouseDY[J] = OldMouseX[J] = OldMouseY[J] =
            MCount[J] = 0;
    }
    IRQPending = 0x00;
    SCCOn[0] = SCCOn[1] = 0;
    RTCReg = RTCMode = 0;
    KanCount = 0;
    KanLetter = 0;
    ChrTab = ColTab = ChrGen = VRAM;
    SprTab = SprGen = VRAM;
    ChrTabM = ColTabM = ChrGenM = SprTabM = ~0;
    VPAGE = VRAM;
    FGColor = BGColor = XFGColor = XBGColor = 0;
    ScrMode = 0;
    VKey = PKey = 1;
    VAddr = 0x0000;
    ScanLine = 0;
    VDPData = NORAM;
    JoyState = 0;
    if (MODEL(MSX_MSX2P)) {
        VDPStatus[1] |= 0x04;
    }
    for (int _i = 0; _i < 16; _i++) {
        ROMLineTag[_i] = 0;
    }
    ResetZ80(&CPU);
    return (Mode);
}

/** RdZ80() **************************************************/
IRAM_ATTR byte RdZ80(word A) {
    if (LIKELY((A & 0x3F88) != 0x3F88)) {
        return (RAM[A >> 13][A & 0x1FFF]);
    }
    if (UNLIKELY(A == 0xFFFF)) {
        return (~SSLReg[PSL[3]]);
    }
    if (UNLIKELY((PSL[A >> 14] == 3) && (SSL[A >> 14] == 1)))
        switch (A) {
            case 0x7FF8:
            case 0xBFF8:
            case 0x7F80:
            case 0x7FB8:
            case 0x7FF9:
            case 0xBFF9:
            case 0x7F81:
            case 0x7FB9:
            case 0x7FFA:
            case 0xBFFA:
            case 0x7F82:
            case 0x7FBA:
            case 0x7FFB:
            case 0xBFFB:
            case 0x7F83:
            case 0x7FBB:
                return (Read1793(&FDC, A & 0x0003));
            case 0x7FFF:
            case 0xBFFF:
            case 0x7F84:
            case 0x7FBC:
                return (Read1793(&FDC, WD1793_READY));
        }
    return (RAM[A >> 13][A & 0x1FFF]);
}

/** WrZ80() **************************************************/
IRAM_ATTR void WrZ80(word A, byte V) {
    if (UNLIKELY(A == 0xFFFF)) {
        SSlot(V);
        return;
    }
    if (UNLIKELY(((A & 0x3F88) == 0x3F88) && (PSL[A >> 14] == 3)
                 && (SSL[A >> 14] == 1)))
        switch (A) {
            case 0x7FF8:
            case 0xBFF8:
            case 0x7F80:
            case 0x7FB8:
            case 0x7FF9:
            case 0xBFF9:
            case 0x7F81:
            case 0x7FB9:
            case 0x7FFA:
            case 0xBFFA:
            case 0x7F82:
            case 0x7FBA:
            case 0x7FFB:
            case 0xBFFB:
            case 0x7F83:
            case 0x7FBB:
            // ======= ОТСЛЕЖИВАНИЕ ЗАПИСИ =======
                if ((A & 0x0003) == 0) { // Если идет запись в регистр команд
                    if ((V & 0xE0) == 0xA0 || (V & 0xF0) == 0xF0) { // Команды записи сектора/трека
                        if (FDC.Drive < 2) msx_disk_modified[FDC.Drive] = 1;
                    }
                }
                // ===================================
                Write1793(&FDC, A & 0x0003, V);
                return;
            case 0xBFFC:
            case 0x7FFC:
                Write1793(&FDC,
                          WD1793_SYSTEM,
                          FDC.Drive | S_DENSITY | (V & 0x01 ? 0 : S_SIDE));
                return;
            case 0xBFFD:
            case 0x7FFD:
                Write1793(&FDC,
                          WD1793_SYSTEM,
                          (V & 0x01) | S_DENSITY | (FDC.Side ? 0 : S_SIDE));
                return;
            case 0x7FBC:
            case 0x7F84:
                Write1793(&FDC,
                          WD1793_SYSTEM,
                          (V & 0x03) | S_DENSITY | (V & 0x04 ? 0 : S_SIDE));
                return;
        }
    if (LIKELY(EnWrite[A >> 14])) {
        int pg = A >> 13, off = A & 0x1FFF;
        RAM[pg][off] = V;
        if (pg == 2 || pg == 3) {
            int idx = LINE_IDX(off);
            uint32_t line_base = (uint32_t)(RAM[pg]) + (off & ~0x3F);
            if (ROMLineTag[idx] == line_base) {
                ROMLineCache[idx][off & 0x3F] = V;
            }
        }
        return;
    }
    if ((A > 0x3FFF) && (A < 0xC000)) {
        MapROM(A, V);
    }
}

/** InZ80() **************************************************/
byte InZ80(word Port) {
    Port &= 0xFF;
    if (LIKELY(Port >= 0x98 && Port <= 0xAB)) {
        switch (Port) {
            case 0x98: {
                byte portData = VDPData;
                VKey = 1;
                VDPData = VPAGE[VAddr];
                VAddr = (VAddr + 1) & 0x3FFF;
                if (!VAddr && (ScrMode > 3)) {
                    VDP[14] = (VDP[14] + 1) & (VRAMPages - 1);
                    VPAGE = VRAM + ((int)VDP[14] << 14);
                }
                return (portData);
            }
            case 0x99: {
                byte portData = VDPStatus[VDP[15]];
                switch (VDP[15]) {
                    case 0:
                        VDPStatus[0] &= 0x5F;
                        SetIRQ(~INT_IE0);
                        break;
                    case 1:
                        VDPStatus[1] &= 0xFE;
                        SetIRQ(~INT_IE1);
                        break;
                    case 7:
                        VDPStatus[7] = VDP[44] = VDPRead();
                        break;
                }
                return (portData);
            }
            case 0xA8:
            case 0xA9:
            case 0xAA:
            case 0xAB:
                PPI.Rin[1] = KeyState[PPI.Rout[2] & 0x0F];
                return (Read8255(&PPI, Port - 0xA8));
        }
    }
    switch (Port) {
        case 0x90:
            return (0xFD);
        case 0xB5:
            return (RTCIn(RTCReg));
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            return (RAMMapper[Port - 0xFC] | ~RAMMask);
        case 0xD9:
            Port = Kanji ? Kanji[KanLetter + KanCount] : NORAM;
            KanCount = (KanCount + 1) & 0x1F;
            return (Port);
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
            return (NORAM);
        case 0xA2:
            if (PSG.Latch == 14) {
                int DX, DY, L, J;
                Port = (PSG.R[15] & 0x40) >> 6;
                L = JOYTYPE(Port);
                if (L == JOY_NONE) {
                    return (0x7F);
                }
                if (MCount[Port] == 1) {
                    DX = MouState[Port] & 0xFF;
                    DY = (MouState[Port] >> 8) & 0xFF;
                    J = OldMouseX[Port] - DX;
                    OldMouseX[Port] = DX;
                    DX = J;
                    J = OldMouseY[Port] - DY;
                    OldMouseY[Port] = DY;
                    DY = J;
                    if ((ScrMode == 6) || ((ScrMode == 7) && !ModeYJK)
                        || (ScrMode == MAXSCREEN + 1)) {
                        DX <<= 1;
                    }
                    MouseDX[Port] =
                        (DX > 127 ? 127 : (DX < -127 ? -127 : DX)) & 0xFF;
                    MouseDY[Port] =
                        (DY > 127 ? 127 : (DY < -127 ? -127 : DY)) & 0xFF;
                }
                J = ~(Port ? (JoyState >> 8) : JoyState) & 0x3F;
                switch (MCount[Port]) {
                    case 0:
                        Port = PSG.R[15] & (0x10 << Port) ? 0x3F : J;
                        break;
                    case 1:
                        Port = (MouseDX[Port] >> 4) | (J & 0x30);
                        break;
                    case 2:
                        Port = (MouseDX[Port] & 0x0F) | (J & 0x30);
                        break;
                    case 3:
                        Port = (MouseDY[Port] >> 4) | (J & 0x30);
                        break;
                    case 4:
                        Port = (MouseDY[Port] & 0x0F) | (J & 0x30);
                        break;
                }
                return (Port | 0x40);
            }
            if (PSG.Latch == 15) {
                return (PSG.R[15] & 0xF0);
            }
            return RdData8910(&PSG);
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xD4:
            return (Read1793(&FDC, Port - 0xD0));
    }
    if (Verbose & 0x20) {
        printf("I/O: Read from unknown PORT[%02Xh]\n", Port);
    }
    return (NORAM);
}

/** OutZ80() *************************************************/
void OutZ80(word Port, byte Value) {
    register byte I, J;
    Port &= 0xFF;
    if (LIKELY(Port >= 0x98 && Port <= 0xAB)) {
        switch (Port) {
            case 0x98:
                VKey = 1;
                VDPData = VPAGE[VAddr] = Value;
                VAddr = (VAddr + 1) & 0x3FFF;
                if (!VAddr && (ScrMode > 3)) {
                    VDP[14] = (VDP[14] + 1) & (VRAMPages - 1);
                    VPAGE = VRAM + ((int)VDP[14] << 14);
                }
                return;
            case 0x99:
                if (VKey) {
                    ALatch = Value;
                    VKey = 0;
                } else {
                    VKey = 1;
                    switch (Value & 0xC0) {
                        case 0x80:
                            VDPOut(Value & 0x3F, ALatch);
                            break;
                        case 0x00:
                        case 0x40:
                            VAddr = (((word)Value << 8) + ALatch) & 0x3FFF;
                            if (!(Value & 0x40)) {
                                VDPData = VPAGE[VAddr];
                                VAddr = (VAddr + 1) & 0x3FFF;
                                if (!VAddr && (ScrMode > 3)) {
                                    VDP[14] = (VDP[14] + 1) & (VRAMPages - 1);
                                    VPAGE = VRAM + ((int)VDP[14] << 14);
                                }
                            }
                            break;
                    }
                }
                return;
            case 0x9A:
                if (PKey) {
                    PLatch = Value;
                    PKey = 0;
                } else {
                    byte R, G, B;
                    PKey = 1;
                    J = VDP[16];
                    R = (PLatch & 0x70) * 255 / 112;
                    G = (Value & 0x07) * 255 / 7;
                    B = (PLatch & 0x07) * 255 / 7;
                    Palette[J] = RGB2INT(R, G, B);
                    SetColor(J, R, G, B);
                    msx_palette_dirty = true;
                    VDP[16] = (J + 1) & 0x0F;
                }
                return;
            case 0x9B:
                J = VDP[17] & 0x3F;
                if (J != 17) {
                    VDPOut(J, Value);
                }
                if (!(VDP[17] & 0x80)) {
                    VDP[17] = (J + 1) & 0x3F;
                }
                return;
            case 0xA8:
            case 0xA9:
            case 0xAA:
            case 0xAB:
                Write8255(&PPI, Port - 0xA8, Value);
                if (PPI.Rout[2] != IOReg) {
                    PPIOut(PPI.Rout[2], IOReg);
                    IOReg = PPI.Rout[2];
                }
                if (PPI.Rout[0] != PSLReg) {
                    PSlot(PPI.Rout[0]);
                }
                return;
        }
    }
    switch (Port) {
        case 0x7C:
            OPLL.Latch = Value;
            WrCtrl2413(&OPLL, Value);
            return;
        case 0x7D:
            WrData2413(&OPLL, Value);
            if (CurrentSndMode == SND_MODE_ACCURATE && studio_opll) {
                OPLL_writeReg(studio_opll, OPLL.Latch, Value);
            }
            return;
        case 0x91:
            Printer(Value);
            return;
        case 0xA0:
            PSG.Latch = Value;
            WrCtrl8910(&PSG, Value);
            if (CurrentSndMode == SND_MODE_ACCURATE && psg_acc) {
                PSG_writeIO(psg_acc, 0, Value);
            }
            return;
        case 0xB4:
            RTCReg = Value & 0x0F;
            return;
        case 0xD8:
            KanLetter = (KanLetter & 0x1F800) | ((int)(Value & 0x3F) << 5);
            KanCount = 0;
            return;
        case 0xD9:
            KanLetter = (KanLetter & 0x007E0) | ((int)(Value & 0x3F) << 11);
            KanCount = 0;
            return;
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
            return;
        case 0xA1:
            if (PSG.Latch == 15) {
                if ((Value & 0x0C) == 0x0C) {
                    MCount[1] = 0;
                } else if ((JOYTYPE(1) == JOY_MOUSE)
                           && ((Value ^ PSG.R[15]) & 0x20)) {
                    MCount[1] += MCount[1] == 4 ? -3 : 1;
                }
                if ((Value & 0x03) == 0x03) {
                    MCount[0] = 0;
                } else if ((JOYTYPE(0) == JOY_MOUSE)
                           && ((Value ^ PSG.R[15]) & 0x10)) {
                    MCount[0] += MCount[0] == 4 ? -3 : 1;
                }
            }
            WrData8910(&PSG, Value);
            if (CurrentSndMode == SND_MODE_ACCURATE && psg_acc) {
                PSG_writeIO(psg_acc, 1, Value);
            }
            return;
        case 0xB5:
            if (RTCReg < 13) {
                J = RTCMode & 0x03;
                RTC[J][RTCReg] = Value;
                if (J > 1) {
                    SaveCMOS = 1;
                }
                return;
            }
            if (RTCReg == 13) {
                RTCMode = Value;
            }
            return;
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
            // ======= ОТСЛЕЖИВАНИЕ ЗАПИСИ =======
            if ((Port - 0xD0) == 0) { // Если идет запись в регистр команд
                if ((Value & 0xE0) == 0xA0 || (Value & 0xF0) == 0xF0) {
                    if (FDC.Drive < 2) msx_disk_modified[FDC.Drive] = 1;
                }
            }
            // ===================================
            Write1793(&FDC, Port - 0xD0, Value);
            return;
        case 0xD4:
            Value =
                ((Value & 0x02) >> 1) | S_DENSITY | (Value & 0x10 ? 0 : S_SIDE);
            Write1793(&FDC, WD1793_SYSTEM, Value);
            return;
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            J = Port - 0xFC;
            Value &= RAMMask;
            if (RAMMapper[J] != Value) {
                if (Verbose & 0x08) {
                    printf("RAM-MAPPER: block %d at %Xh\n", Value, J * 0x4000);
                }
                I = J << 1;
                RAMMapper[J] = Value;
                MemMap[3][2][I] = RAMData + ((int)Value << 14);
                MemMap[3][2][I + 1] = MemMap[3][2][I] + 0x2000;
                if ((PSL[J] == 3) && (SSL[J] == 2)) {
                    EnWrite[J] = 1;
                    RAM[I] = MemMap[3][2][I];
                    RAM[I + 1] = MemMap[3][2][I + 1];
                }
            }
            return;
    }
    if (Verbose & 0x20) {
        printf("I/O: Write to unknown PORT[%02Xh]=%02Xh\n", Port, Value);
    }
}

/** MapROM() *************************************************/
void MapROM(register word A, register byte V) {
    byte I, J, PS, SS, *P;
    J = A >> 14;
    PS = PSL[J];
    SS = SSL[J];
    I = CartMap[PS][SS];
    if (I >= MAXSLOTS) {
        return;
    }
    if (CurrentSndMode == SND_MODE_ACCURATE && studio_scc) {
        if (A == 0x9000 || A == 0xBFFE || A == 0xBFFF
            || ((A & 0xDF00) == 0x9800)) {
            SCC_write(studio_scc, A, V);
        }
    }
    if (!ROMData[I] && (A == 0x9000)) {
        SCCOn[I] = (V == 0x3F) ? 1 : 0;
    }
    if (SCCOn[I] && ((A & 0xDF00) == 0x9800)) {
        J = A & 0x00FF;
        if (A & 0x2000) {
            if (!ROMData[I] && (J < 0xA0)) {
                EmptyRAM[0x1800 + J] = V;
            }
            WriteSCCP(&SCChip, J, V);
        } else {
            if (!ROMData[I] && (J < 0x80)) {
                EmptyRAM[0x1800 + J] = V;
            }
            WriteSCC(&SCChip, J, V);
        }
        return;
    }
    if (!ROMData[I] || !ROMMask[I]) {
        return;
    }
    switch (ROMType[I]) {
        case MAP_GEN8:
            if ((A < 0x4000) || (A > 0xBFFF)) {
                break;
            }
            J = (A - 0x4000) >> 13;
            if (J == 2) {
                SCCOn[I] = (V == 0x3F) ? 1 : 0;
            }
            V &= ROMMask[I];
            if (V != ROMMapper[I][J]) {
                RAM[J + 2] = MemMap[PS][SS][J + 2] =
                    ROMData[I] + ((int)V << 13);
                ROMMapper[I][J] = V;
            }
            return;
        case MAP_GEN16:
            if ((A < 0x4000) || (A > 0xBFFF)) {
                break;
            }
            J = (A & 0x8000) >> 14;
            V = (V << 1) & ROMMask[I];
            if (V != ROMMapper[I][J]) {
                RAM[J + 2] = MemMap[PS][SS][J + 2] =
                    ROMData[I] + ((int)V << 13);
                RAM[J + 3] = MemMap[PS][SS][J + 3] = RAM[J + 2] + 0x2000;
                ROMMapper[I][J] = V;
                ROMMapper[I][J + 1] = V | 1;
            }
            return;
        case MAP_KONAMI5:
            if ((A < 0x5000) || (A > 0xB000) || ((A & 0x1FFF) != 0x1000)) {
                break;
            }
            J = (A - 0x5000) >> 13;
            if (J == 2) {
                SCCOn[I] = (V == 0x3F) ? 1 : 0;
            }
            V &= ROMMask[I];
            if (V != ROMMapper[I][J]) {
                RAM[J + 2] = MemMap[PS][SS][J + 2] =
                    ROMData[I] + ((int)V << 13);
                ROMMapper[I][J] = V;
            }
            return;
        case MAP_KONAMI4:
            if ((A < 0x6000) || (A > 0xA000) || (A & 0x1FFF)) {
                break;
            }
            J = (A - 0x4000) >> 13;
            V &= ROMMask[I];
            if (V != ROMMapper[I][J]) {
                RAM[J + 2] = MemMap[PS][SS][J + 2] =
                    ROMData[I] + ((int)V << 13);
                ROMMapper[I][J] = V;
            }
            return;
        case MAP_ASCII8:
            if ((A >= 0x6000) && (A < 0x8000)) {
                J = (A & 0x1800) >> 11;
                if (V & (ROMMask[I] + 1)) {
                    V = 0xFF;
                    P = SRAMData[I];
                } else {
                    V &= ROMMask[I];
                    P = ROMData[I] + ((int)V << 13);
                }
                if (V != ROMMapper[I][J]) {
                    MemMap[PS][SS][J + 2] = P;
                    ROMMapper[I][J] = V;
                    if ((PSL[(J >> 1) + 1] == PS)
                        && (SSL[(J >> 1) + 1] == SS)) {
                        RAM[J + 2] = P;
                    }
                }
                return;
            }
            if ((A >= 0x8000) && (A < 0xC000)
                && (ROMMapper[I][((A >> 13) & 1) + 2] == 0xFF)) {
                RAM[A >> 13][A & 0x1FFF] = V;
                SaveSRAM[I] = 1;
                return;
            }
            break;
        case MAP_ASCII16:
            if ((A >= 0x6000) && (A < 0x8000)
                && ((V <= ROMMask[I] + 1) || !(A & 0x0FFF))) {
                J = (A & 0x1000) >> 11;
                if (V & (ROMMask[I] + 1)) {
                    V = 0xFF;
                    P = SRAMData[I];
                } else {
                    V = (V << 1) & ROMMask[I];
                    P = ROMData[I] + ((int)V << 13);
                }
                if (V != ROMMapper[I][J]) {
                    MemMap[PS][SS][J + 2] = P;
                    MemMap[PS][SS][J + 3] = P + 0x2000;
                    ROMMapper[I][J] = V;
                    ROMMapper[I][J + 1] = V | 1;
                    if ((PSL[(J >> 1) + 1] == PS)
                        && (SSL[(J >> 1) + 1] == SS)) {
                        RAM[J + 2] = P;
                        RAM[J + 3] = P + 0x2000;
                    }
                }
                return;
            }
            if ((A >= 0x8000) && (A < 0xC000) && (ROMMapper[I][2] == 0xFF)) {
                P = RAM[A >> 13];
                A &= 0x07FF;
                P[A + 0x0800] = P[A + 0x1000] = P[A + 0x1800] = P[A + 0x2000] =
                    P[A + 0x2800] = P[A + 0x3000] = P[A + 0x3800] = P[A] = V;
                SaveSRAM[I] = 1;
                return;
            }
            break;
        case MAP_GMASTER2:
            if ((A >= 0x6000) && (A <= 0xA000) && !(A & 0x1FFF)) {
                J = (A - 0x4000) >> 13;
                if (V & 0x10) {
                    RAM[J + 2] = MemMap[PS][SS][J + 2] =
                        SRAMData[I] + (V & 0x20 ? 0x2000 : 0);
                    ROMMapper[I][J] = 0xFF;
                } else {
                    V &= ROMMask[I];
                    if (V != ROMMapper[I][J]) {
                        RAM[J + 2] = MemMap[PS][SS][J + 2] =
                            ROMData[I] + ((int)V << 13);
                        ROMMapper[I][J] = V;
                    }
                }
                return;
            }
            if ((A >= 0xB000) && (A < 0xC000) && (ROMMapper[I][3] == 0xFF)) {
                RAM[5][(A & 0x0FFF) | 0x1000] = RAM[5][A & 0x0FFF] = V;
                SaveSRAM[I] = 1;
                return;
            }
            break;
        case MAP_FMPAC:
            switch (A) {
                case 0x7FF7:
                    V = (V << 1) & ROMMask[I];
                    ROMMapper[I][0] = V;
                    ROMMapper[I][1] = V | 1;
                    if (FMPACKey != FMPAC_MAGIC) {
                        P = ROMData[I] + ((int)V << 13);
                        RAM[2] = MemMap[PS][SS][2] = P;
                        RAM[3] = MemMap[PS][SS][3] = P + 0x2000;
                    }
                    return;
                case 0x7FF6:
                    V &= 0x11;
                    return;
                case 0x5FFE:
                case 0x5FFF:
                    FMPACKey = A & 1 ? ((FMPACKey & 0x00FF) | ((int)V << 8))
                                     : ((FMPACKey & 0xFF00) | V);
                    P = FMPACKey == FMPAC_MAGIC
                            ? SRAMData[I]
                            : (ROMData[I] + ((int)ROMMapper[I][0] << 13));
                    RAM[2] = MemMap[PS][SS][2] = P;
                    RAM[3] = MemMap[PS][SS][3] = P + 0x2000;
                    return;
            }
            if ((A >= 0x4000) && (A < 0x5FFE) && (FMPACKey == FMPAC_MAGIC)) {
                RAM[A >> 13][A & 0x1FFF] = V;
                SaveSRAM[I] = 1;
                return;
            }
            break;
    }
    if (Verbose & 0x08) {
        printf("MEMORY: Bad write (%d:%d:%04Xh) = %02Xh\n", PS, SS, A, V);
    }
}

/** PSlot() **************************************************/
void PSlot(register byte V) {
    register byte J, I;
    if (PSLReg != V)
        for (PSLReg = V, J = 0; J < 4; ++J, V >>= 2) {
            I = J << 1;
            PSL[J] = V & 3;
            SSL[J] = (SSLReg[PSL[J]] >> I) & 3;
            RAM[I] = MemMap[PSL[J]][SSL[J]][I];
            RAM[I + 1] = MemMap[PSL[J]][SSL[J]][I + 1];
            EnWrite[J] =
                (PSL[J] == 3) && (SSL[J] == 2) && (MemMap[3][2][I] != EmptyRAM);
        }
}

/** SSlot() **************************************************/
void SSlot(register byte V) {
    register byte J, I;
    if ((PSL[3] == 1) || (PSL[3] == 2)) {
        V = 0x00;
    }
    if (!PSL[3] && ((Mode & MSX_MODEL) == MSX_MSX1)) {
        V = 0x00;
    }
    if (SSLReg[PSL[3]] != V)
        for (SSLReg[PSL[3]] = V, J = 0; J < 4; ++J, V >>= 2) {
            if (PSL[J] == PSL[3]) {
                I = J << 1;
                SSL[J] = V & 3;
                RAM[I] = MemMap[PSL[J]][SSL[J]][I];
                RAM[I + 1] = MemMap[PSL[J]][SSL[J]][I + 1];
                EnWrite[J] = (PSL[J] == 3) && (SSL[J] == 2)
                             && (MemMap[3][2][I] != EmptyRAM);
            }
        }
}

/** SetIRQ() *************************************************/
word SetIRQ(register byte IRQ) {
    if (IRQ & 0x80) {
        IRQPending &= IRQ;
    } else {
        IRQPending |= IRQ;
    }
    CPU.IRequest = IRQPending ? INT_IRQ : INT_NONE;
    return (CPU.IRequest);
}

/** SetScreen() **********************************************/
byte SetScreen(void) {
    register byte I, J;
    switch (((VDP[0] & 0x0E) >> 1) | (VDP[1] & 0x18)) {
        case 0x10:
            J = 0;
            break;
        case 0x00:
            J = 1;
            break;
        case 0x01:
            J = 2;
            break;
        case 0x08:
            J = 3;
            break;
        case 0x02:
            J = 4;
            break;
        case 0x03:
            J = 5;
            break;
        case 0x04:
            J = 6;
            break;
        case 0x05:
            J = 7;
            break;
        case 0x07:
            J = 8;
            break;
        case 0x12:
            J = MAXSCREEN + 1;
            break;
        default:
            J = ScrMode;
            break;
    }
    I = (J > 6) && (J != MAXSCREEN + 1) ? 11 : 10;
    ChrTab = VRAM + ((int)(VDP[2] & MSK[J].R2) << I);
    ChrGen = VRAM + ((int)(VDP[4] & MSK[J].R4) << 11);
    ColTab = VRAM + ((int)(VDP[3] & MSK[J].R3) << 6) + ((int)VDP[10] << 14);
    SprTab = VRAM + ((int)(VDP[5] & MSK[J].R5) << 7) + ((int)VDP[11] << 15);
    SprGen = VRAM + ((int)VDP[6] << 11);
    ChrTabM = ((int)(VDP[2] | ~MSK[J].M2) << I) | ((1 << I) - 1);
    ChrGenM = ((int)(VDP[4] | ~MSK[J].M4) << 11) | 0x007FF;
    ColTabM = ((int)(VDP[3] | ~MSK[J].M3) << 6) | 0x1C03F;
    SprTabM = ((int)(VDP[5] | ~MSK[J].M5) << 7) | 0x1807F;
    ScrMode = J;
    return (J);
}

/** SetMegaROM() *********************************************/
void SetMegaROM(int Slot, byte P0, byte P1, byte P2, byte P3) {
    byte PS, SS;
    if ((Slot < 0) || (Slot >= MAXSLOTS)) {
        return;
    }
    for (PS = 0; PS < 4; ++PS) {
        for (SS = 0; (SS < 4) && (CartMap[PS][SS] != Slot); ++SS)
            ;
        if (SS < 4) {
            break;
        }
    }
    if (PS >= 4) {
        return;
    }
    P0 &= ROMMask[Slot];
    P1 &= ROMMask[Slot];
    P2 &= ROMMask[Slot];
    P3 &= ROMMask[Slot];
    MemMap[PS][SS][2] = ROMData[Slot] + P0 * 0x2000;
    MemMap[PS][SS][3] = ROMData[Slot] + P1 * 0x2000;
    MemMap[PS][SS][4] = ROMData[Slot] + P2 * 0x2000;
    MemMap[PS][SS][5] = ROMData[Slot] + P3 * 0x2000;
    ROMMapper[Slot][0] = P0;
    ROMMapper[Slot][1] = P1;
    ROMMapper[Slot][2] = P2;
    ROMMapper[Slot][3] = P3;
}

/** VDPOut() *************************************************/
IRAM_ATTR void VDPOut(register byte R, register byte V) {
    register byte J;
    switch (R) {
        case 0:
            if ((VDPStatus[1] & 0x01) && !(V & 0x10)) {
                VDPStatus[1] &= 0xFE;
                SetIRQ(~INT_IE1);
            }
            if (VDP[0] != V) {
                VDP[0] = V;
                SetScreen();
            }
            break;
        case 1:
            if (VDPStatus[0] & 0x80) {
                SetIRQ(V & 0x20 ? INT_IE0 : ~INT_IE0);
            }
            if (VDP[1] != V) {
                VDP[1] = V;
                SetScreen();
            }
            break;
        case 2:
            J = (ScrMode > 6) && (ScrMode != MAXSCREEN + 1) ? 11 : 10;
            ChrTab = VRAM + ((int)(V & MSK[ScrMode].R2) << J);
            ChrTabM = ((int)(V | ~MSK[ScrMode].M2) << J) | ((1 << J) - 1);
            break;
        case 3:
            ColTab =
                VRAM + ((int)(V & MSK[ScrMode].R3) << 6) + ((int)VDP[10] << 14);
            ColTabM = ((int)(V | ~MSK[ScrMode].M3) << 6) | 0x1C03F;
            break;
        case 4:
            ChrGen = VRAM + ((int)(V & MSK[ScrMode].R4) << 11);
            ChrGenM = ((int)(V | ~MSK[ScrMode].M4) << 11) | 0x007FF;
            break;
        case 5:
            SprTab =
                VRAM + ((int)(V & MSK[ScrMode].R5) << 7) + ((int)VDP[11] << 15);
            SprTabM = ((int)(V | ~MSK[ScrMode].M5) << 7) | 0x1807F;
            break;
        case 6:
            V &= 0x3F;
            SprGen = VRAM + ((int)V << 11);
            break;
        case 7:
            FGColor = V >> 4;
            BGColor = V & 0x0F;
            break;
        case 10:
            V &= 0x07;
            ColTab =
                VRAM + ((int)(VDP[3] & MSK[ScrMode].R3) << 6) + ((int)V << 14);
            break;
        case 11:
            V &= 0x03;
            SprTab =
                VRAM + ((int)(VDP[5] & MSK[ScrMode].R5) << 7) + ((int)V << 15);
            break;
        case 14:
            V &= VRAMPages - 1;
            VPAGE = VRAM + ((int)V << 14);
            break;
        case 15:
            V &= 0x0F;
            break;
        case 16:
            V &= 0x0F;
            PKey = 1;
            break;
        case 17:
            V &= 0xBF;
            break;
        case 25:
            VDP[25] = V;
            SetScreen();
            break;
        case 44:
            VDPWrite(V);
            break;
        case 46:
            VDPDraw(V);
            break;
    }
    VDP[R] = V;
}

/** Printer() ************************************************/
void Printer(byte V) {
    if (!PrnStream) {
        PrnStream = PrnName ? fopen(PrnName, "ab") : 0;
        PrnStream = PrnStream ? PrnStream : stdout;
    }
    fputc(V, PrnStream);
}

/** PPIOut() *************************************************/
void PPIOut(register byte New, register byte Old) {
    if ((New ^ Old) & 0x80) {
        Drum(DRM_CLICK, 64);
    }
    if ((New ^ Old) & 0x10) {
        Drum(DRM_CLICK, 255);
    }
}

/** RTCIn() **************************************************/
byte RTCIn(register byte R) {
    static time_t PrevTime;
    static struct tm TM;
    register byte J;
    time_t CurTime;
    R &= 0x0F;
    J = RTCMode & 0x03;
    if (R > 12) {
        J = R == 13 ? RTCMode : NORAM;
    } else if (J) {
        J = RTC[J][R];
    } else {
        CurTime = time(NULL);
        if (CurTime != PrevTime) {
            TM = *localtime(&CurTime);
            PrevTime = CurTime;
        }
        switch (R) {
            case 0:
                J = TM.tm_sec % 10;
                break;
            case 1:
                J = TM.tm_sec / 10;
                break;
            case 2:
                J = TM.tm_min % 10;
                break;
            case 3:
                J = TM.tm_min / 10;
                break;
            case 4:
                J = TM.tm_hour % 10;
                break;
            case 5:
                J = TM.tm_hour / 10;
                break;
            case 6:
                J = TM.tm_wday;
                break;
            case 7:
                J = TM.tm_mday % 10;
                break;
            case 8:
                J = TM.tm_mday / 10;
                break;
            case 9:
                J = (TM.tm_mon + 1) % 10;
                break;
            case 10:
                J = (TM.tm_mon + 1) / 10;
                break;
            case 11:
                J = (TM.tm_year - 80) % 10;
                break;
            case 12:
                J = ((TM.tm_year - 80) / 10) % 10;
                break;
            default:
                J = 0x0F;
                break;
        }
    }
    return (J | 0xF0);
}

/** LoopZ80() ************************************************/
IRAM_ATTR word LoopZ80(Z80 *R) {
    static byte BFlag = 0, BCount = 0, Drawing = 0, ACount = 0;
    static int UCount = 0;
    register int J;
    VDPStatus[2] ^= 0x20;
    if (!(VDPStatus[2] & 0x20)) {
        R->IPeriod =
            !ScrMode || (ScrMode == MAXSCREEN + 1) ? CPU_H240 : CPU_H256;
        ScanLine = ScanLine < (PALVideo ? 312 : 261) ? ScanLine + 1 : 0;
        if (!ScanLine) {
            Drawing = 1;
            VDPStatus[2] &= 0xBF;
            if (UCount >= 100) {
                UCount -= 100;
                RefreshScreen();
            }
            UCount += UPeriod;
            if (BCount) {
                BCount--;
            } else {
                BFlag = !BFlag;
                if (!VDP[13]) {
                    XFGColor = FGColor;
                    XBGColor = BGColor;
                } else {
                    BCount = (BFlag ? VDP[13] & 0x0F : VDP[13] >> 4) * 10;
                    if (BCount) {
                        if (BFlag) {
                            XFGColor = FGColor;
                            XBGColor = BGColor;
                        } else {
                            XFGColor = VDP[12] >> 4;
                            XBGColor = VDP[12] & 0x0F;
                        }
                    }
                }
            }
        }
        J = PALVideo ? 256 : ScanLines212 ? 245 : 235;
        if (ScanLine == J) {
            VDPStatus[1] &= 0xFE;
            SetIRQ(~INT_IE1);
        }
        if (ScanLine < J) {
            J = (((ScanLine + VScroll) & 0xFF) - VDP[19]) & 0xFF;
            if (J == 2) {
                VDPStatus[1] |= 0x01;
                if (VDP[0] & 0x10) {
                    SetIRQ(INT_IE1);
                }
            } else {
                if (!(VDP[0] & 0x10)) {
                    VDPStatus[1] &= 0xFE;
                }
            }
        }
        R->IRequest = IRQPending ? INT_IRQ : INT_NONE;
        return (R->IRequest);
    }
    R->IPeriod = !ScrMode || (ScrMode == MAXSCREEN + 1) ? CPU_H240 : CPU_H256;
    R->IPeriod = HPeriod - R->IPeriod;
    J = PALVideo ? 313 : 262;
    if (ScanLine >= J - 1) {
        J *= CPU_HPERIOD;
        if (VPeriod > J) {
            R->IPeriod += VPeriod - J;
        }
    }
    if (ScanLine == (ScanLines212 ? 212 : 192)) {
        Drawing = 0;
    }
    J = PALVideo ? (ScanLines212 ? 212 + 42 : 192 + 52)
                 : (ScanLines212 ? 212 + 18 : 192 + 28);
    if (!Drawing && (ScanLine == J)) {
        VDPStatus[0] |= 0x80;
        VDPStatus[2] |= 0x40;
        if (VDP[1] & 0x20) {
            SetIRQ(INT_IE0);
        }
    }
    LoopVDP();
    if ((UCount >= 100) && Drawing && (ScanLine < 256)) {
        if (!ModeYJK || (ScrMode < 7) || (ScrMode > 8)) {
            (RefreshLine[ScrMode])(ScanLine);
        } else if (ModeYAE) {
            RefreshLine10(ScanLine);
        } else {
            RefreshLine12(ScanLine);
        }
    }
    if (!(ScanLine & 0x07)) {
        J = (int)(1000000L * (CPU_HPERIOD << 3) / CPU_CLOCK);
        Loop8910(&PSG, J);
        Sync8910(&PSG,
                 AY8910_FLUSH
                     | (!ScanLine && OPTION(MSX_DRUMS) ? AY8910_DRUMS : 0));
        SyncSCC(&SCChip, SCC_FLUSH);
        Sync2413(&OPLL, YM2413_FLUSH);
        PlayAllSound(CPU_HPERIOD << 3);
    }
    if (ScanLine == 192) {
        VDPStatus[0] = (VDPStatus[0] & ~0x40) | 0x1F;
        if (!(VDPStatus[0] & 0x20) && CheckSprites()) {
            VDPStatus[0] |= 0x20;
        }
        MIDITicks(1000 * VPeriod / CPU_CLOCK);
        if (CheatsON && CheatCount) {
            ApplyCheats();
        }
        JoyState = Joystick();
        Keyboard();
        if (ExitNow) {
            return (INT_QUIT);
        }
        if (JOYTYPE(0) >= JOY_MOUSTICK) {
            MouState[0] = Mouse(0);
            JoyState |= (MouState[0] >> 12) & 0x0030;
            if (JOYTYPE(0) == JOY_MOUSTICK) {
                J = MouState[0] & 0xFF;
                JoyState |= J > OldMouseX[0]   ? 0x0008
                            : J < OldMouseX[0] ? 0x0004
                                               : 0;
                OldMouseX[0] = J;
                J = (MouState[0] >> 8) & 0xFF;
                JoyState |= J > OldMouseY[0]   ? 0x0002
                            : J < OldMouseY[0] ? 0x0001
                                               : 0;
                OldMouseY[0] = J;
            }
        }
        if (JOYTYPE(1) >= JOY_MOUSTICK) {
            MouState[1] = Mouse(1);
            JoyState |= (MouState[1] >> 4) & 0x3000;
            if (JOYTYPE(1) == JOY_MOUSTICK) {
                J = MouState[1] & 0xFF;
                JoyState |= J > OldMouseX[1]   ? 0x0800
                            : J < OldMouseX[1] ? 0x0400
                                               : 0;
                OldMouseX[1] = J;
                J = (MouState[1] >> 8) & 0xFF;
                JoyState |= J > OldMouseY[1]   ? 0x0200
                            : J < OldMouseY[1] ? 0x0100
                                               : 0;
                OldMouseY[1] = J;
            }
        }
        if (OPTION(MSX_AUTOSPACE | MSX_AUTOFIREA | MSX_AUTOFIREB))
            if ((ACount = (ACount + 1) & 0x07) > 3) {
                if (OPTION(MSX_AUTOSPACE)) {
                    KBD_RES(' ');
                }
                if (OPTION(MSX_AUTOFIREA)) {
                    JoyState &= ~(JST_FIREA | (JST_FIREA << 8));
                }
                if (OPTION(MSX_AUTOFIREB)) {
                    JoyState &= ~(JST_FIREB | (JST_FIREB << 8));
                }
            }
    }
    R->IRequest = IRQPending ? INT_IRQ : INT_NONE;
    return (R->IRequest);
}

/** CheckSprites() *******************************************/
int CheckSprites(void) {
    unsigned int I, J, LS, LD;
    byte DH, DV, *S, *D, *PS, *PD, *T;
    if (SpritesOFF || !ScrMode || (ScrMode >= MAXSCREEN + 1)) {
        return (0);
    }
    DH = ScrMode > 3 ? 216 : 208;
    LD = 255 - (Sprites16x16 ? 16 : 8);
    LS = ScanLines212 ? 211 : 191;
    for (I = J = 0, S = SprTab; (I < 32) && (S[0] != DH); ++I, S += 4)
        if ((S[0] < LS) || (S[0] > LD)) {
            J |= 1 << I;
        }
    if (Sprites16x16) {
        for (S = SprTab; J; J >>= 1, S += 4)
            if (J & 1)
                for (I = J >> 1, D = S + 4; I; I >>= 1, D += 4)
                    if (I & 1) {
                        DV = S[0] - D[0];
                        if ((DV < 16) || (DV > 240)) {
                            DH = S[1] - D[1];
                            if ((DH < 16) || (DH > 240)) {
                                PS = SprGen + ((int)(S[2] & 0xFC) << 3);
                                PD = SprGen + ((int)(D[2] & 0xFC) << 3);
                                if (DV < 16) {
                                    PD += DV;
                                } else {
                                    DV = 256 - DV;
                                    PS += DV;
                                }
                                if (DH > 240) {
                                    DH = 256 - DH;
                                    T = PS;
                                    PS = PD;
                                    PD = T;
                                }
                                while (DV < 16) {
                                    LS = ((unsigned int)*PS << 8) + *(PS + 16);
                                    LD = ((unsigned int)*PD << 8) + *(PD + 16);
                                    if (LD & (LS >> DH)) {
                                        break;
                                    } else {
                                        ++DV;
                                        ++PS;
                                        ++PD;
                                    }
                                }
                                if (DV < 16) {
                                    return (1);
                                }
                            }
                        }
                    }
    } else {
        for (S = SprTab; J; J >>= 1, S += 4)
            if (J & 1)
                for (I = J >> 1, D = S + 4; I; I >>= 1, D += 4)
                    if (I & 1) {
                        DV = S[0] - D[0];
                        if ((DV < 8) || (DV > 248)) {
                            DH = S[1] - D[1];
                            if ((DH < 8) || (DH > 248)) {
                                PS = SprGen + ((int)S[2] << 3);
                                PD = SprGen + ((int)D[2] << 3);
                                if (DV < 8) {
                                    PD += DV;
                                } else {
                                    DV = 256 - DV;
                                    PS += DV;
                                }
                                if (DH > 248) {
                                    DH = 256 - DH;
                                    T = PS;
                                    PS = PD;
                                    PD = T;
                                }
                                while ((DV < 8) && !(*PD & (*PS >> DH))) {
                                    ++DV;
                                    ++PS;
                                    ++PD;
                                }
                                if (DV < 8) {
                                    return (1);
                                }
                            }
                        }
                    }
    }
    return (0);
}

/** StateID() ************************************************/
word StateID(void) {
    word ID;
    int J, I;
    ID = 0x0000;
    for (I = 0; I < MAXSLOTS; ++I)
        if (ROMData[I])
            for (J = 0; J < (ROMMask[I] + 1) * 0x2000; ++J) {
                ID += I ^ ROMData[I][J];
            }
    if (MemMap[0][0][0] && (MemMap[0][0][0] != EmptyRAM))
        for (J = 0; J < 0x8000; ++J) {
            ID += MemMap[0][0][0][J];
        }
    if (MemMap[3][1][0] && (MemMap[3][1][0] != EmptyRAM))
        for (J = 0; J < 0x4000; ++J) {
            ID += MemMap[3][1][0][J];
        }
    if (MemMap[3][1][2] && (MemMap[3][1][2] != EmptyRAM))
        for (J = 0; J < 0x4000; ++J) {
            ID += MemMap[3][1][2][J];
        }
    return (ID);
}

/** MakeFileName() *******************************************/
char *MakeFileName(const char *Name, const char *Ext) {
    char *Result, *P1, *P2, *P3;
    Result = malloc(strlen(Name) + strlen(Ext) + 1);
    if (!Result) {
        return (0);
    }
    strcpy(Result, Name);
    P1 = strrchr(Result, '.');
    P2 = strrchr(Result, '/');
    P3 = strrchr(Result, '\\');
    P2 = P3 && (P3 > P2) ? P3 : P2;
    P3 = strrchr(Result, ':');
    P2 = P3 && (P3 > P2) ? P3 : P2;
    if (P1 && (!P2 || (P1 > P2))) {
        strcpy(P1, Ext);
    } else {
        strcat(Result, Ext);
    }
    return (Result);
}

/** ChangeTape() *********************************************/
byte ChangeTape(const char *FileName) {
    if (CasStream) {
        fclose(CasStream);
        CasStream = 0;
    }
    if (FileName) {
        CasStream = fopen(FileName, "r+b");
        CasStream = CasStream ? CasStream : fopen(FileName, "rb");
    }
    return (!FileName || CasStream);
}

/** RewindTape() *********************************************/
void RewindTape(void) {
    if (CasStream)
        rewind(CasStream);
}

/** ChangePrinter() ******************************************/
void ChangePrinter(const char *FileName) {
    if (PrnStream && (PrnStream != stdout)) {
        fclose(PrnStream);
    }
    PrnName = FileName;
    PrnStream = 0;
}

/** ChangeDisk() *********************************************/
byte ChangeDisk(byte N, const char *FileName) {
    int NeedState;
    byte *P;
    if (N >= MAXDRIVES) {
        return (0);
    }
    NeedState = FileName && *FileName && !N && !FDD[N].Data;
    Reset1793(&FDC, FDD, WD1793_KEEP);
    if (!FileName) {
        EjectFDI(&FDD[N]);
        return (1);
    }
    if (*FileName && LoadFDI(&FDD[N], FileName, FMT_AUTO)) {
        if (NeedState) {
            FindState(FileName);
        }
        return (1);
    }
    P = FormatFDI(&FDD[N], FMT_MSXDSK);
    if (P
        && !(*FileName ? DSKLoad(FileName, P, "MSX-DISK")
                       : DSKCreate(P, "MSX-DISK"))) {
        EjectFDI(&FDD[N]);
        return (0);
    }
    return (!!P);
}

/** LoadFile() ***********************************************/
int LoadFile(const char *FileName) {
    int J;
    if (hasext(FileName, ".DSK") || hasext(FileName, ".FDI")) {
        if (!ChangeDisk(0, FileName)) {
            return (0);
        }
        for (J = 0; J < MAXCARTS; ++J) {
            LoadCart(0, J, ROMType[J]);
        }
        return (1);
    }
    if (hasext(FileName, ".ROM") || hasext(FileName, ".MX1")
        || hasext(FileName, ".MX2")) {
        return (!!LoadCart(FileName, 0, ROMGUESS(0) | ROMTYPE(0)));
    }
    if (hasext(FileName, ".CAS")) {
        return (!!ChangeTape(FileName));
    }
    if (hasext(FileName, ".FNT")) {
        return (!!LoadFNT(FileName));
    }
    if (hasext(FileName, ".PAL")) {
        return (!!LoadPAL(FileName));
    }
    if (hasext(FileName, ".CHT")) {
        return (!!LoadCHT(FileName));
    }
    if (hasext(FileName, ".MCF")) {
        return (!!LoadMCF(FileName));
    }
    if (hasext(FileName, ".STA")) {
        return (!!LoadSTA(FileName));
    }
    return (0);
}

/** SaveCHT() ************************************************/
int SaveCHT(const char *Name) {
    FILE *F;
    int J;
    F = fopen(Name, "wb");
    if (!F) {
        return (0);
    }
    for (J = 0; J < CheatCount; ++J) {
        fprintf(F, "%s\n", CheatCodes[J].Text);
    }
    fclose(F);
    return (CheatCount);
}

/** ApplyMCFCheat() ******************************************/
int ApplyMCFCheat(int N) {
    int Status;
    if ((N < 0) || (N >= MCFCount) || (MCFEntries[N].Addr > 0xFFFF)
        || (MCFEntries[N].Size > 2)) {
        return (0);
    }
    Status = Cheats(CHTS_QUERY);
    Cheats(CHTS_OFF);
    ResetCheats();
    CheatCodes[0].Addr = MCFEntries[N].Addr;
    CheatCodes[0].Data = MCFEntries[N].Data;
    CheatCodes[0].Size = MCFEntries[N].Size;
    sprintf((char *)CheatCodes[0].Text,
            CheatCodes[0].Size > 1 ? "%04X-%04X" : "%04X-%02X",
            CheatCodes[0].Addr,
            CheatCodes[0].Data);
    CheatCount = 1;
    Cheats(Status);
    return (CheatCount);
}

/** AddCheat() ***********************************************/
int AddCheat(const char *Cheat) {
    static const char *Hex = "0123456789ABCDEF";
    unsigned int A, D;
    char *P;
    int J, N;
    if (CheatCount >= MAXCHEATS) {
        return (0);
    }
    N = strlen(Cheat);
    if (((N == 13) || (N == 11)) && (Cheat[8] == '-')) {
        for (J = 0, A = 0; J < 8; J++) {
            P = strchr(Hex, toupper(Cheat[J]));
            if (!P) {
                return (0);
            } else {
                A = (A << 4) | (P - Hex);
            }
        }
        for (J = 9, D = 0; J < N; J++) {
            P = strchr(Hex, toupper(Cheat[J]));
            if (!P) {
                return (0);
            } else {
                D = (D << 4) | (P - Hex);
            }
        }
    } else if (((N == 9) || (N == 7)) && (Cheat[4] == '-')) {
        for (J = 0, A = 0x0100; J < 4; J++) {
            P = strchr(Hex, toupper(Cheat[J]));
            if (!P) {
                return (0);
            } else {
                A = (A << 4) | (P - Hex);
            }
        }
        for (J = 5, D = 0; J < N; J++) {
            P = strchr(Hex, toupper(Cheat[J]));
            if (!P) {
                return (0);
            } else {
                D = (D << 4) | (P - Hex);
            }
        }
    } else {
        return (0);
    }
    strcpy((char *)CheatCodes[CheatCount].Text, Cheat);
    if (N == 13) {
        CheatCodes[CheatCount].Addr = A;
        CheatCodes[CheatCount].Data = D & 0xFFFF;
        CheatCodes[CheatCount].Size = 2;
    } else {
        CheatCodes[CheatCount].Addr = A;
        CheatCodes[CheatCount].Data = D & 0xFF;
        CheatCodes[CheatCount].Size = 1;
    }
    return (++CheatCount);
}

/** DelCheat() ***********************************************/
int DelCheat(const char *Cheat) {
    int I, J;
    for (J = 0; J < CheatCount; ++J) {
        for (I = 0; Cheat[I] && CheatCodes[J].Text[I]; ++I)
            if (CheatCodes[J].Text[I] != toupper(Cheat[I])) {
                break;
            }
        if (!Cheat[I] && !CheatCodes[J].Text[I]) {
            if (--CheatCount != J) {
                memcpy(&CheatCodes[J],
                       &CheatCodes[J + 1],
                       (CheatCount - J) * sizeof(CheatCodes[0]));
            }
            return (1);
        }
    }
    return (0);
}

/** ResetCheats() ********************************************/
void ResetCheats(void) {
    Cheats(CHTS_OFF);
    CheatCount = 0;
}

/** ApplyCheats() ********************************************/
int ApplyCheats(void) {
    int J, I;
    for (J = I = 0; J < CheatCount; ++J)
        if ((CheatCodes[J].Addr >> 24) == 0x01) {
            WrZ80(CheatCodes[J].Addr & 0xFFFF, CheatCodes[J].Data & 0xFF);
            if (CheatCodes[J].Size > 1) {
                WrZ80((CheatCodes[J].Addr + 1) & 0xFFFF,
                      CheatCodes[J].Data >> 8);
            }
            ++I;
        }
    return (I);
}

/** Cheats() *************************************************/
int Cheats(int Switch) {
    byte *P, *Base;
    int J, Size;
    switch (Switch) {
        case CHTS_ON:
        case CHTS_OFF:
            if (Switch == CheatsON) {
                return (CheatsON);
            }
        case CHTS_TOGGLE:
            Switch = !CheatsON;
            break;
        default:
            return (CheatsON);
    }
    for (J = 1; (J <= 2) && !ROMData[J]; ++J)
        ;
    if (J > 2) {
        return (Switch = CHTS_OFF);
    }
    Base = ROMData[J];
    Size = ((int)ROMMask[J] + 1) << 14;
    if (Switch != CheatsON) {
        if (Switch) {
            for (J = 0; J < CheatCount; ++J)
                if (!(CheatCodes[J].Addr >> 24)
                    && (CheatCodes[J].Addr + CheatCodes[J].Size <= Size)) {
                    P = Base + CheatCodes[J].Addr;
                    CheatCodes[J].Orig = P[0];
                    P[0] = CheatCodes[J].Data;
                    if (CheatCodes[J].Size > 1) {
                        CheatCodes[J].Orig |= (int)P[1] << 8;
                        P[1] = CheatCodes[J].Data >> 8;
                    }
                }
        } else {
            for (J = 0; J < CheatCount; ++J)
                if (!(CheatCodes[J].Addr >> 24)
                    && (CheatCodes[J].Addr + CheatCodes[J].Size <= Size)) {
                    P = Base + CheatCodes[J].Addr;
                    P[0] = CheatCodes[J].Orig;
                    if (CheatCodes[J].Size > 1) {
                        P[1] = CheatCodes[J].Orig >> 8;
                    }
                }
        }
        CheatsON = Switch;
    }
    if (Verbose) {
        printf("Cheats %s\n", CheatsON ? "ON" : "OFF");
    }
    return (CheatsON);
}

#if defined(ANDROID)
#undef feof
#define fopen mopen
#define fclose mclose
#define fread mread
#define fwrite mwrite
#define fgets mgets
#define fseek mseek
#define rewind mrewind
#define fgetc mgetc
#define ftell mtell
#define feof meof
#elif defined(ZLIB)
#undef feof
#define fopen(N, M) (FILE *)gzopen(N, M)
#define fclose(F) gzclose((gzFile)(F))
#define fread(B, L, N, F) gzread((gzFile)(F), B, (L) * (N))
#define fwrite(B, L, N, F) gzwrite((gzFile)(F), B, (L) * (N))
#define fgets(B, L, F) gzgets((gzFile)(F), B, L)
#define fseek(F, O, W) gzseek((gzFile)(F), O, W)
#define rewind(F) gzrewind((gzFile)(F))
#define fgetc(F) gzgetc((gzFile)(F))
#define ftell(F) gztell((gzFile)(F))
#define feof(F) gzeof((gzFile)(F))
#endif

/** GuessROM() ***********************************************/
int GuessROM(const byte *Buf, int Size) {
    int J, I, K, Result, ROMCount[MAXMAPPERS];
    char S[256];
    FILE *F;
    Result = -1;
    if (ProgDir && chdir(ProgDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", ProgDir);
    }
    if ((F = fopen("CARTS.CRC", "rb"))) {
        for (J = K = 0; J < Size; ++J) {
            K += Buf[J];
        }
        while (fgets(S, sizeof(S) - 4, F))
            if (sscanf(S, "%08X %d", &J, &I) == 2)
                if (K == J) {
                    Result = I;
                    break;
                }
        fclose(F);
    }
    if ((Result < 0) && (F = fopen("CARTS.SHA", "rb"))) {
        char S1[41], S2[41];
        SHA1 C;
        ResetSHA1(&C);
        InputSHA1(&C, Buf, Size);
        if (ComputeSHA1(&C) && OutputSHA1(&C, S1, sizeof(S1))) {
            while (fgets(S, sizeof(S) - 4, F))
                if ((sscanf(S, "%40s %d", S2, &J) == 2) && !strcmp(S1, S2)) {
                    Result = J;
                    break;
                }
        }
        fclose(F);
    }
    if (WorkDir && chdir(WorkDir)) {
        if (Verbose)
            printf("Failed changing to '%s' directory!\n", WorkDir);
    }
    if (Result >= 0) {
        return (Result);
    }
    for (J = 0; J < MAXMAPPERS; ++J) {
        ROMCount[J] = 1;
    }
    ROMCount[MAP_GEN8] += 1;
    ROMCount[MAP_ASCII8] -= 1;
    for (J = 0; J < Size - 2; ++J) {
        I = Buf[J] + ((int)Buf[J + 1] << 8) + ((int)Buf[J + 2] << 16);
        switch (I) {
            case 0x500032:
                ROMCount[MAP_KONAMI5]++;
                break;
            case 0x900032:
                ROMCount[MAP_KONAMI5]++;
                break;
            case 0xB00032:
                ROMCount[MAP_KONAMI5]++;
                break;
            case 0x400032:
                ROMCount[MAP_KONAMI4]++;
                break;
            case 0x800032:
                ROMCount[MAP_KONAMI4]++;
                break;
            case 0xA00032:
                ROMCount[MAP_KONAMI4]++;
                break;
            case 0x680032:
                ROMCount[MAP_ASCII8]++;
                break;
            case 0x780032:
                ROMCount[MAP_ASCII8]++;
                break;
            case 0x600032:
                ROMCount[MAP_KONAMI4]++;
                ROMCount[MAP_ASCII8]++;
                ROMCount[MAP_ASCII16]++;
                break;
            case 0x700032:
                ROMCount[MAP_KONAMI5]++;
                ROMCount[MAP_ASCII8]++;
                ROMCount[MAP_ASCII16]++;
                break;
            case 0x77FF32:
                ROMCount[MAP_ASCII16]++;
                break;
        }
    }
    for (I = 0, J = 0; J < MAXMAPPERS; ++J)
        if (ROMCount[J] > ROMCount[I]) {
            I = J;
        }
    return (I);
}

/** LoadFNT() ************************************************/
byte LoadFNT(const char *FileName) {
    FILE *F;
    if (!FileName) {
        FreeMemory(FontBuf);
        FontBuf = 0;
        return (1);
    }
    if (!(F = fopen(FileName, "rb"))) {
        return (0);
    }
    if (!FontBuf) {
        FontBuf = GetMemory(256 * 8);
    }
    if (!FontBuf) {
        fclose(F);
        return (0);
    }
    fread(FontBuf, 1, 256 * 8, F);
    fclose(F);
    return (1);
}

/** LoadROM() ************************************************/
byte *LoadROM(const char *Name, int Size, byte *Buf) {
    FILE *F;
    byte *P;
    int J;
    if (Buf && !Size) {
        return (0);
    }
    if (!(F = fopen(Name, "rb"))) {
        return (0);
    }
    if (!Size) {
        if (!fseek(F, 0, SEEK_END)) {
            Size = ftell(F);
        } else {
            while ((J = fread(EmptyRAM, 1, 0x4000, F)) == 0x4000) {
                Size += J;
            }
            if (J > 0) {
                Size += J;
            }
            memset(EmptyRAM, NORAM, 0x4000);
        }
        rewind(F);
    }
    P = Buf ? Buf : GetMemory(Size);
    if (!P) {
        fclose(F);
        return (0);
    }
    if ((J = fread(P, 1, Size, F)) != Size) {
        if (!Buf) {
            FreeMemory(P);
        }
        fclose(F);
        return (0);
    }
    fclose(F);
    return (P);
}

/** FindState() **********************************************/
int FindState(const char *Name) {
    int J, I;
    char *P;
    J = 0;
    FreeMemory(STAName);
    if ((STAName = MakeFileName(Name, ".sta"))) {
        if (Verbose) {
            printf("Loading state from %s...", STAName);
        }
        J = LoadSTA(STAName);
        PRINTRESULT(J);
    }
    if ((P = MakeFileName(Name, ".cht"))) {
        I = LoadCHT(P);
        if (I && Verbose) {
            printf("Loaded %d cheats from %s\n", I, P);
        }
        FreeMemory(P);
    }
    if ((P = MakeFileName(Name, ".mcf"))) {
        I = LoadMCF(P);
        if (I && Verbose) {
            printf("Loaded %d cheat entries from %s\n", I, P);
        }
        FreeMemory(P);
    }
    if ((P = MakeFileName(Name, ".pal"))) {
        I = LoadPAL(P);
        if (I && Verbose) {
            printf("Loaded palette from %s\n", P);
        }
        FreeMemory(P);
    }
    return (J);
}

/** LoadCart() ***********************************************/
int LoadCart(const char *FileName, int Slot, int Type) {
    int C1, C2, Len, Pages, ROM64, BASIC;
    byte *P, PS, SS;
    char *T;
    FILE *F;
    if ((Slot < 0) || (Slot >= MAXSLOTS)) {
        return (0);
    }
    for (PS = 0; PS < 4; ++PS) {
        for (SS = 0; (SS < 4) && (CartMap[PS][SS] != Slot); ++SS)
            ;
        if (SS < 4) {
            break;
        }
    }
    if (PS >= 4) {
        return (0);
    }
    if (SRAMData[Slot] && SaveSRAM[Slot] && SRAMName[Slot]) {
        if (Verbose) {
            printf("Writing %s...", SRAMName[Slot]);
        }
        if (!(F = fopen(SRAMName[Slot], "wb"))) {
            SaveSRAM[Slot] = 0;
        } else {
            switch (ROMType[Slot]) {
                case MAP_ASCII8:
                case MAP_FMPAC:
                    if (fwrite(SRAMData[Slot], 1, 0x2000, F) != 0x2000) {
                        SaveSRAM[Slot] = 0;
                    }
                    break;

                case MAP_ASCII16:
                    if (fwrite(SRAMData[Slot], 1, 0x0800, F) != 0x0800) {
                        SaveSRAM[Slot] = 0;
                    }
                    break;

                case MAP_GMASTER2:
                    if (fwrite(SRAMData[Slot], 1, 0x1000, F) != 0x1000) {
                        SaveSRAM[Slot] = 0;
                    }
                    if (fwrite(SRAMData[Slot] + 0x2000, 1, 0x1000, F)
                        != 0x1000) {
                        SaveSRAM[Slot] = 0;
                    }
                    break;
            }
            fclose(F);
        }
        PRINTRESULT(SaveSRAM[Slot]);
    }
    if (!FileName) {
        if (ROMData[Slot]) {
            FreeMemory(ROMData[Slot]);
            ROMData[Slot] = 0;
            ROMMask[Slot] = 0;
            for (C1 = 0; C1 < 8; ++C1) {
                MemMap[PS][SS][C1] = EmptyRAM;
            }
            ResetMSX(Mode, RAMPages, VRAMPages);
            if (Verbose) {
                printf("Ejected cartridge from slot %c\n", Slot + 'A');
            }
        }
        return (0);
    }
    if (!(F = fopen(FileName, "rb"))) {
        return (0);
    }
    if (Verbose) {
        printf("Found %s:\n", FileName);
    }
    if (!fseek(F, 0, SEEK_END)) {
        Len = ftell(F);
    } else {
        for (Len = 0; (C2 = fread(EmptyRAM, 1, 0x4000, F)) == 0x4000; Len += C2)
            ;
        if (C2 > 0) {
            Len += C2;
        }
        memset(EmptyRAM, NORAM, 0x4000);
    }
    rewind(F);
    Len = Len >> 13;
    for (Pages = 1; Pages < Len; Pages <<= 1)
        ;
    ROM64 = 0;
    C1 = fgetc(F);
    C2 = fgetc(F);
    if ((C1 != 'A') || (C2 != 'B'))
        if (fseek(F, 0x4000, SEEK_SET) >= 0) {
            C1 = fgetc(F);
            C2 = fgetc(F);
            ROM64 = (C1 == 'A') && (C2 == 'B');
        }
    if ((Len >= 2) && ((C1 != 'A') || (C2 != 'B')))
        if (fseek(F, 0x2000 * (Len - 2), SEEK_SET) >= 0) {
            C1 = fgetc(F);
            C2 = fgetc(F);
        }
    if ((C1 != 'A') || (C2 != 'B')) {
        if (Verbose) {
            puts("  Not a valid cartridge ROM");
        }
        fclose(F);
        return (0);
    }
    if (Verbose) {
        printf("  Cartridge %c: ", 'A' + Slot);
    }
    fclose(F);
    if (Verbose)
        printf("%dkB %s ROM..",
               Len * 8,
               ROM64 || (Len <= 0x8000) ? "NORMAL"
               : Type >= MAP_GUESS      ? "UNKNOWN"
                                        : ROMNames[Type]);
    ROMMask[Slot] = !ROM64 && (Len > 4) ? (Pages - 1) : 0x00;
    ROMData[Slot] = P = GetMemory(Pages << 13);
    if (!P) {
        PRINTFAILED;
        return (0);
    }
    if (!LoadROM(FileName, Len << 13, P)) {
        PRINTFAILED;
        return (0);
    }
    if (Len < Pages) {
        memcpy(P + Len * 0x2000,
               P + (Len - Pages / 2) * 0x2000,
               (Pages - Len) * 0x2000);
    }
    BASIC = (P[0] == 'A') && (P[1] == 'B') && !(P[2] || P[3]) && (P[8] || P[9]);
    switch (Len) {
        case 1:
            if (!BASIC) {
                MemMap[PS][SS][0] = P;
                MemMap[PS][SS][1] = P;
                MemMap[PS][SS][2] = P;
                MemMap[PS][SS][3] = P;
            }
            MemMap[PS][SS][4] = P;
            MemMap[PS][SS][5] = P;
            if (!BASIC) {
                MemMap[PS][SS][6] = P;
                MemMap[PS][SS][7] = P;
            }
            break;
        case 2:
            if (!BASIC) {
                MemMap[PS][SS][0] = P;
                MemMap[PS][SS][1] = P + 0x2000;
                MemMap[PS][SS][2] = P;
                MemMap[PS][SS][3] = P + 0x2000;
            }
            MemMap[PS][SS][4] = P;
            MemMap[PS][SS][5] = P + 0x2000;
            if (!BASIC) {
                MemMap[PS][SS][6] = P;
                MemMap[PS][SS][7] = P + 0x2000;
            }
            break;
        case 3:
        case 4:
            MemMap[PS][SS][0] = P;
            MemMap[PS][SS][1] = P + 0x2000;
            MemMap[PS][SS][2] = P;
            MemMap[PS][SS][3] = P + 0x2000;
            MemMap[PS][SS][4] = P + 0x4000;
            MemMap[PS][SS][5] = P + 0x6000;
            MemMap[PS][SS][6] = P + 0x4000;
            MemMap[PS][SS][7] = P + 0x6000;
            break;
        default:
            if (ROM64) {
                MemMap[PS][SS][0] = P;
                MemMap[PS][SS][1] = P + 0x2000;
                MemMap[PS][SS][2] = P + 0x4000;
                MemMap[PS][SS][3] = P + 0x6000;
                MemMap[PS][SS][4] = P + 0x8000;
                MemMap[PS][SS][5] = P + 0xA000;
                MemMap[PS][SS][6] = P + 0xC000;
                MemMap[PS][SS][7] = P + 0xE000;
            }
            break;
    }
    if (Verbose)
        printf("starts at %04Xh..",
               MemMap[PS][SS][2][2] + 256 * MemMap[PS][SS][2][3]);
    if ((Type >= MAP_GUESS) && (ROMMask[Slot] + 1 > 4)) {
        Type = GuessROM(P, Len << 13);
        if (Verbose) {
            printf("guessed %s..", ROMNames[Type]);
        }
        if (Slot < MAXCARTS) {
            SETROMTYPE(Slot, Type);
        }
    }
    ROMType[Slot] = Type;
    if ((Type == MAP_GEN16) && (ROMMask[Slot] + 1 > 4)) {
        SetMegaROM(Slot, 0, 1, ROMMask[Slot] - 1, ROMMask[Slot]);
    }
    if (MAP_SRAM(Type)) {
        FreeMemory(SRAMData[Slot]);
        FreeMemory(SRAMName[Slot]);
        SRAMData[Slot] = GetMemory(0x4000);
        if (!SRAMData[Slot]) {
            if (Verbose) {
                printf("scratch SRAM..");
            }
            SRAMData[Slot] = EmptyRAM;
        } else {
            if (Verbose) {
                printf("got 16kB SRAM..");
            }
            memset(SRAMData[Slot], NORAM, 0x4000);
        }
        if ((SRAMName[Slot] = (char *)GetMemory(strlen(FileName) + 5))) {
            strcpy(SRAMName[Slot], FileName);
            T = strrchr(SRAMName[Slot], '.');
            if (T) {
                strcpy(T, ".sav");
            } else {
                strcat(SRAMName[Slot], ".sav");
            }
            if ((F = fopen(SRAMName[Slot], "rb"))) {
                Len = fread(SRAMData[Slot], 1, 0x4000, F);
                fclose(F);
                if (Verbose) {
                    printf("loaded %d bytes from %s..", Len, SRAMName[Slot]);
                }
                P = SRAMData[Slot];
                switch (Type) {
                    case MAP_FMPAC:
                        memset(P + 0x2000, NORAM, 0x2000);
                        P[0x1FFE] = FMPAC_MAGIC & 0xFF;
                        P[0x1FFF] = FMPAC_MAGIC >> 8;
                        break;
                    case MAP_GMASTER2:
                        memcpy(P + 0x2000, P + 0x1000, 0x1000);
                        memcpy(P + 0x3000, P + 0x1000, 0x1000);
                        memcpy(P + 0x1000, P, 0x1000);
                        break;
                    case MAP_ASCII16:
                        memcpy(P + 0x0800, P, 0x0800);
                        memcpy(P + 0x1000, P, 0x0800);
                        memcpy(P + 0x1800, P, 0x0800);
                        memcpy(P + 0x2000, P, 0x0800);
                        memcpy(P + 0x2800, P, 0x0800);
                        memcpy(P + 0x3000, P, 0x0800);
                        memcpy(P + 0x3800, P, 0x0800);
                        break;
                }
            }
        }
    }
    ResetMSX(Mode, RAMPages, VRAMPages);
    PRINTOK;
    if (!Slot || ((Slot == 1) && !ROMData[0])) {
        FindState(FileName);
    }
    return (Pages);
}

/** LoadCHT() ************************************************/
int LoadCHT(const char *Name) {
    char Buf[256], S[16];
    int Status;
    FILE *F;
    F = fopen(Name, "rb");
    if (!F) {
        return (0);
    }
    Status = Cheats(CHTS_QUERY);
    Cheats(CHTS_OFF);
    ResetCheats();
    while (!feof(F))
        if (fgets(Buf, sizeof(Buf), F) && (sscanf(Buf, "%13s", S) == 1)) {
            AddCheat(S);
        }
    fclose(F);
    Cheats(Status);
    return (CheatCount);
}

/** LoadPAL() ************************************************/
int LoadPAL(const char *Name) {
    static const char *Hex = "0123456789ABCDEF";
    char S[256], *P, *T, *H;
    FILE *F;
    int J, I;
    if (!(F = fopen(Name, "rb"))) {
        return (0);
    }
    for (J = 0; (J < 16) && fgets(S, sizeof(S), F); ++J) {
        for (P = S; *P && (*P <= ' '); ++P)
            ;
        if (*P == '#') {
            ++P;
        }
        for (T = P, I = 0; *T && (H = strchr(Hex, toupper(*T))); ++T) {
            I = (I << 4) + (H - Hex);
        }
        if (T - P == 6) {
            SetColor(J, I >> 16, (I >> 8) & 0xFF, I & 0xFF);
        }
    }
    fclose(F);
    return (J);
}

/** LoadMCF() ************************************************/
int LoadMCF(const char *Name) {
    MCFCount = LoadFileMCF(
        Name, MCFEntries, sizeof(MCFEntries) / sizeof(MCFEntries[0]));
    return (MCFCount);
}

/** SaveMCF() ************************************************/
int SaveMCF(const char *Name) {
    return (MCFCount > 0 ? SaveFileMCF(Name, MCFEntries, MCFCount) : 0);
}

/** MSX_SaveDisks() ******************************************/
/** Сохраняет изменённые образы дискет A и B на SD-карту.  **/
/** Вызывается при выходе из эмулятора, аппаратном сбросе,  **/
/** смене диска или по требованию из Retro-Go меню.         **/
/*************************************************************/
void MSX_SaveDisks(void) {
    if (!msx_disk_autosave) return;
    
    if (DSKName[0] && msx_disk_modified[0]) {
        SaveFDI(&FDD[0], DSKName[0], FMT_DSK);
        msx_disk_modified[0] = 0;
    }
    if (DSKName[1] && msx_disk_modified[1]) {
        SaveFDI(&FDD[1], DSKName[1], FMT_DSK);
        msx_disk_modified[1] = 0;
    }
}

/** State.h **************************************************/
#include "State.h"


#if defined(ZLIB) || defined(ANDROID)
#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fgets
#undef fseek
#undef ftell
#undef fgetc
#undef feof
#endif
