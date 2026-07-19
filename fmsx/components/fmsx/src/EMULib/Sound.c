/**
    EMULib Emulation Library — Sound.c

    Core sound rendering and MIDI logging.
    Adapted for ESP32-S3 (Xtensa) by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com

    Multi-Chip Audio Engine:
    ─────────────────────────────────────────────────────────
    SND_MODE_FAST     — Marat Fayzullin (branchless meander)
                        Original fMSX sound: fast, loud,
                        square-wave synthesis (1 cycle/sample).

    SND_MODE_ACCURATE — Mitsutaka Okazaki + Ivan Svarkovsky
                        Cycle-accurate chip emulation:
                        PSG  (emu2149) — AY-3-8910 with DC Blocker
                        OPLL (emu2413) — YM2413 FM-PAC, IRAM-optimized
                        SCC  (emu2212) — Konami SCC with LPF (9.5 kHz)
                        OPM  (ym2151)  — YM2151 SFG-05, DRAM-only tables

    Stereo Architecture:
    ─────────────────────────────────────────────────────────
    msx_audio_stereo = false — Mono (PSG, OPLL, SCC, OPM summed)
    msx_audio_stereo = true  — Stereo panorama:
        PSG  → Center (L+R equal)
        OPLL → Right-biased (cos²/sin² law)
        SCC  → Left-biased  (cos²/sin² law)
        OPM  → Native stereo 

    Key Features:
    ─────────────────────────────────────────────────────────
    - Branchless wave generation (Fast engine, 1 cycle/sample on Xtensa)
    - DC Blocker for PSG (removes DC offset, enables clean amplification)
    - YM2151: DRAM-only tables (~16 KB), compressed 4.25× vs original MAME
    - YM2151: Lazy init + sleep mode (zero CPU cost when inactive)
    - YM2151: Linear interpolation 16→32 kHz with bit-shift-only math
    - PDM Driver: DMA buffer in SRAM, flat vectorized loop, fixed-point volume scaling
    - Frame-accurate buffer management for Retro-Go RTOS
    - Safe headroom mixing (no SoftClip needed, all levels < 32767)
    
    Performance:
    ─────────────────────────────────────────────────────────
    Fast mode:          74-75 FPS (all games)
    Accurate mode:      74-75 FPS (PSG+SCC)
                        55-60 FPS (PSG+OPLL+SCC)

    Upcoming:
    ─────────────────────────────────────────────────────────
    Y8950 (MSX-AUDIO) — ADPCM + FM synthesis (optional, menu toggle)

    WARNING: This version is under active testing. Please report
    any audio glitches, distortion, or unexpected silence to:
    ivansvarkovsky@gmail.com

    This file is distributed under the same terms as the original
    EMULib code by Marat Fayzullin. Commercial distribution is
    prohibited without permission from the original author.
*/

#include "Sound.h"
#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

/*  --- Okazaki Studio Chips ---
    PSG	    emu2149.c	AY-3-8910
    OPLL	emu2413.c	YM2413 (FM-PAC)
    SCC	    emu2212.c	Konami SCC
*/
#include "emu/emu2149.h"
#include "emu/emu2413.h"
#include "emu/emu2212.h"

#include "emu/ym2151.h"

SoundMode CurrentSndMode = SND_MODE_ACCURATE; // Mitsutaka Okazaki (studio emulation)        / SND_MODE_FAST;  // Marat Fayzullin (branchless meander)
bool msx_audio_stereo = true;               // Stereo (when dual audio output)               / false; 

// *(Okazaki)
void *psg_acc     = NULL;
void *studio_opll = NULL;
void *studio_scc  = NULL;

// Флаг активности виртуального SFG-05 (объявлен в MSX.c)
extern int sfg_active;
// Анти-LTO для вызова из IRAM-функции RenderAudio
__attribute__((noinline)) static void wrap_YM2151UpdateOne(int16_t **bufs, unsigned int frames) {
    YM2151UpdateOne(0, bufs, frames);
}

#ifdef __C99__
    #define INLINE static inline
#else
    #define INLINE static __inline
#endif

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef unsigned char byte;
typedef unsigned short word;

struct SndDriverStruct SndDriver = {
    (void (*)(int,int))0, (void (*)(int,int))0, (void (*)(int,int))0,
    (void (*)(int,int,int))0, (void (*)(int,const signed char *,int,int))0,
    (const signed char *(*)(int))0
};

static const struct { byte Note; word Wheel; } Freqs[4096] = {
#include "MIDIFreq.h"
};

static const int Programs[] = { 80, 80, 122, 122, 80 };

static struct {
    int Type;
    int Note;
    int Pitch;
    int Level;
    int Power;
} MidiCH[MIDI_CHANNELS] = {
    { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 },
    { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 },
    { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 },
    { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }, { -1,-1,-1,-1,256 }
};

static struct {
    int Type;
    int Freq;
    int Volume;
    const signed char *Data;
    int Length;
    int Rate;
    int Pos;
    int Count;
} WaveCH[SND_CHANNELS] = {
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 },
    { SND_MELODIC,0,0,0,0,0,0,0 }, { SND_MELODIC,0,0,0,0,0,0,0 }
};

static int SndRate    = 0;
static int NoiseGen   = 0x10000;
static int NoiseOut   = 16;
static int NoiseXor   = 14;
int MasterSwitch      = 0xFFFF;
int MasterVolume      = 192;

static const char *LogName = 0;
static int  Logging   = MIDI_OFF;
static int  TickCount = 0;
static int  LastMsg   = -1;
static int  DrumOn    = 0;
static FILE *MIDIOut  = 0;

static void MIDISound(int Channel,int Freq,int Volume);
static void MIDISetSound(int Channel,int Type);
static void MIDIDrum(int Type,int Force);
static void MIDIMessage(byte D0,byte D1,byte D2);
static void NoteOn(byte Channel,byte Note,byte Level);
static void NoteOff(byte Channel);
static void WriteDelta(void);
static void WriteTempo(int Freq);

#define SHIFT(Ch) (Ch==15? 9:Ch>8? Ch+1:Ch)

unsigned int GetSndRate(void) { return(SndRate); }

void Sound(int Channel,int Freq,int Volume) {
    if((Channel<0)||(Channel>=SND_CHANNELS)) { return; }

    Freq   = Freq<0? 0:Freq;
    Volume = Volume<0? 0:Volume>255? 255:Volume;
    WaveCH[Channel].Volume = Volume;
    WaveCH[Channel].Freq   = Freq;

    if(!Freq||!Volume) {
        WaveCH[Channel].Pos    = 0;
        WaveCH[Channel].Count  = 0;
    }

    if(SndDriver.Sound) { (*SndDriver.Sound)(Channel,Freq,Volume); }

    MIDISound(Channel,Freq,Volume);
}

void Drum(int Type,int Force) {
    Force = Force<0? 0:Force>255? 255:Force;

    if(SndDriver.Drum) { (*SndDriver.Drum)(Type,Force); }

    MIDIDrum(Type,Force);
}

void SetSound(int Channel,int Type) {
    if((Channel<0)||(Channel>=SND_CHANNELS)) { return; }

    WaveCH[Channel].Type = Type;

    if(SndDriver.SetSound) { (*SndDriver.SetSound)(Channel,Type); }

    MIDISetSound(Channel,Type);
}

void SetChannels(int Volume,int Switch) {
    Volume = Volume<0? 0:Volume>255? 255:Volume;

    if(SndDriver.SetChannels) { (*SndDriver.SetChannels)(Volume,Switch); }

    MasterVolume = Volume;
    MasterSwitch = Switch&((1<<SND_CHANNELS)-1);
}

void SetNoise(int Seed,int OUTBit,int XORBit) {
    NoiseGen = Seed;
    NoiseOut = OUTBit;
    NoiseXor = XORBit;
}

void SetWave(int Channel,const signed char *Data,int Length,int Rate) {
    unsigned int I,J;

    if((Channel<0)||(Channel>=SND_CHANNELS)||(Length<=0)) { return; }

    WaveCH[Channel].Type   = SND_WAVE;
    WaveCH[Channel].Length = Length;
    WaveCH[Channel].Rate   = Rate;
    WaveCH[Channel].Pos    = Length? WaveCH[Channel].Pos%Length:0;
    WaveCH[Channel].Count  = 0;
    WaveCH[Channel].Data   = Data;

    if(SndDriver.SetWave) { (*SndDriver.SetWave)(Channel,Data,Length,Rate); }

    MIDISetSound(Channel,Rate? -1:SND_WAVE);

    if(Rate) { I=0; }

    else {
        for(J=I=0; J<Length; ++J) { I+=Data[J]>0? Data[J]:-Data[J]; }

        I = (I<<1)/Length;
        I = I>256? 256:I;
    }

    MidiCH[Channel].Power = I;
}

const signed char *GetWave(int Channel) {
    if((Channel<0)||(Channel>=SND_CHANNELS)) { return(0); }

    if(SndDriver.GetWave) { return((*SndDriver.GetWave)(Channel)); }

    return(WaveCH[Channel].Rate&&(WaveCH[Channel].Type==SND_WAVE)? WaveCH[Channel].Data+WaveCH[Channel].Pos:0);
}

void InitMIDI(const char *FileName) {
    int WasLogging;

    if(!FileName) { return; }

    WasLogging=Logging;

    if(MIDIOut) { TrashMIDI(); }

    LogName   = FileName;
    Logging   = MIDI_OFF;
    LastMsg   = -1;
    TickCount = 0;
    MIDIOut   = 0;
    DrumOn    = 0;

    if(WasLogging) { MIDILogging(MIDI_ON); }
}

void TrashMIDI(void) {
    long Length;
    int J;

    if(!MIDIOut) { return; }

    for(J=0; J<MIDI_CHANNELS; J++) { NoteOff(J); }

    MIDIMessage(0xFF,0x2F,0x00);
    fseek(MIDIOut,0,SEEK_END);
    Length=ftell(MIDIOut)-22;
    fseek(MIDIOut,18,SEEK_SET);
    fputc((Length>>24)&0xFF,MIDIOut);
    fputc((Length>>16)&0xFF,MIDIOut);
    fputc((Length>>8)&0xFF,MIDIOut);
    fputc(Length&0xFF,MIDIOut);
    fclose(MIDIOut);
    Logging   = MIDI_OFF;
    LastMsg   = -1;
    TickCount = 0;
    MIDIOut   = 0;
}

int MIDILogging(int Switch) {
    static const char MThd[] = "MThd\0\0\0\006\0\0\0\1";
    static const char MTrk[] = "MTrk\0\0\0\0";
    int J,I;

    if(Switch==MIDI_TOGGLE) { Switch=!Logging; }

    if((Switch==MIDI_ON)||(Switch==MIDI_OFF))
        if(Switch^Logging) {
            if(!Switch&&MIDIOut) for(J=0; J<MIDI_CHANNELS; J++) { NoteOff(J); }

            if(Switch&&!MIDIOut&&LogName) {
                LastMsg=-1;

                for(J=0; J<MIDI_CHANNELS; J++) { MidiCH[J].Note=MidiCH[J].Pitch=MidiCH[J].Level=-1; }

                MIDIOut=fopen(LogName,"wb");

                if(!MIDIOut) { return(MIDI_OFF); }

                if(fwrite(MThd,1,12,MIDIOut)!=12) { fclose(MIDIOut); MIDIOut=0; return(MIDI_OFF); }

                fputc((MIDI_DIVISIONS>>8)&0xFF,MIDIOut);
                fputc(MIDI_DIVISIONS&0xFF,MIDIOut);

                if(fwrite(MTrk,1,8,MIDIOut)!=8) { fclose(MIDIOut); MIDIOut=0; return(MIDI_OFF); }

                WriteTempo(MIDI_DIVISIONS);
            }

            if(!MIDIOut) { Switch=MIDI_OFF; }

            Logging=Switch;

            if(Switch) {
                TickCount=0;

                for(J=0; J<MIDI_CHANNELS; J++)
                    if((MidiCH[J].Type>=0)&&(MidiCH[J].Type&0x10000)) {
                        I=MidiCH[J].Type&~0x10000;
                        MidiCH[J].Type=-1;
                        MIDISetSound(J,I);
                    }
            }
        }

    return(Logging);
}

void MIDITicks(int N) { if(Logging&&MIDIOut&&(N>0)) TickCount+=N; }

void MIDISound(int Channel,int Freq,int Volume) {
    int MIDIVolume,MIDINote,MIDIWheel;

    if(!Logging||!MIDIOut||(Channel>=MIDI_CHANNELS-1)||(Channel<0)) { return; }

    if((Freq<MIDI_MINFREQ)||(Freq>MIDI_MAXFREQ)) { Freq=0; }

    if(MidiCH[Channel].Type<0) { Freq=0; }

    Volume = MidiCH[Channel].Type==SND_TRIANGLE? ((Volume+3)>>2)
             : MidiCH[Channel].Type==SND_WAVE? (((Volume*MidiCH[Channel].Power)+511)>>9)
             : ((Volume+1)>>1);

    if(Volume<0) { Volume=0; }

    else if(Volume>127) { Volume=127; }

    if(!Volume||!Freq) { NoteOff(Channel); }

    else {
        MIDIVolume = Volume;
        MIDINote   = Freqs[Freq/3].Note;
        MIDIWheel  = Freqs[Freq/3].Wheel;
        NoteOn(Channel,MIDINote,MIDIVolume);

        if(MidiCH[Channel].Pitch!=MIDIWheel) {
            MIDIMessage(0xE0+SHIFT(Channel),MIDIWheel&0x7F,(MIDIWheel>>7)&0x7F);
            MidiCH[Channel].Pitch=MIDIWheel;
        }
    }
}

void MIDISetSound(int Channel,int Type) {
    if((Channel>=MIDI_CHANNELS-1)||(Channel<0)) { return; }

    if(MidiCH[Channel].Type!=Type) {
        if(!Logging||!MIDIOut) { MidiCH[Channel].Type=Type|0x10000; }

        else {
            MidiCH[Channel].Type=Type;

            if(Type<0) { NoteOff(Channel); }

            else {
                Type=Type&SND_MIDI? (Type&0x7F):Programs[Type%5];
                MIDIMessage(0xC0+SHIFT(Channel),Type,255);
            }
        }
    }
}

void MIDIDrum(int Type,int Force) {
    if(!Logging||!MIDIOut) { return; }

    Type=Type&DRM_MIDI? (Type&0x7F):77;

    if(DrumOn) { MIDIMessage(0x89,DrumOn,127); }

    if(Type) { MIDIMessage(0x99,Type,(Force&0xFF)>>1); }

    DrumOn=Type;
}

void MIDIMessage(byte D0,byte D1,byte D2) {
    WriteDelta();

    if(D0!=LastMsg) { LastMsg=D0; fputc(D0,MIDIOut); }

    if(D1<128) { fputc(D1,MIDIOut); if(D2<128) fputc(D2,MIDIOut); }
}

void NoteOn(byte Channel,byte Note,byte Level) {
    Note  = Note>0x7F? 0x7F:Note;
    Level = Level>0x7F? 0x7F:Level;

    if((MidiCH[Channel].Note!=Note)||(MidiCH[Channel].Level!=Level)) {
        if(MidiCH[Channel].Note>=0) { NoteOff(Channel); }

        MIDIMessage(0x90+SHIFT(Channel),Note,Level);
        MidiCH[Channel].Note=Note;
        MidiCH[Channel].Level=Level;
    }
}

void NoteOff(byte Channel) {
    if(MidiCH[Channel].Note>=0) {
        MIDIMessage(0x80+SHIFT(Channel),MidiCH[Channel].Note,127);
        MidiCH[Channel].Note=-1;
    }
}

void WriteDelta(void) {
    if(TickCount<128) { fputc(TickCount,MIDIOut); }

    else {
        if(TickCount<128*128) {
            fputc((TickCount>>7)|0x80,MIDIOut);
            fputc(TickCount&0x7F,MIDIOut);
        }

        else {
            fputc(((TickCount>>14)&0x7F)|0x80,MIDIOut);
            fputc(((TickCount>>7)&0x7F)|0x80,MIDIOut);
            fputc(TickCount&0x7F,MIDIOut);
        }
    }

    TickCount=0;
}

void WriteTempo(int Freq) {
    int J=500000*MIDI_DIVISIONS*2/Freq;
    WriteDelta();
    fputc(0xFF,MIDIOut);
    fputc(0x51,MIDIOut);
    fputc(0x03,MIDIOut);
    fputc((J>>16)&0xFF,MIDIOut);
    fputc((J>>8)&0xFF,MIDIOut);
    fputc(J&0xFF,MIDIOut);
}

void TrashSound(void) {
    SndRate = 0;
#if !defined(NO_AUDIO_PLAYBACK)
#if defined(WINDOWS)
    WinTrashSound();
#else
    TrashAudio();
#endif
#endif

    extern int opm_inited;
    if (opm_inited) {
        YM2151Shutdown();
        opm_inited = 0;
    }
}

#if !defined(NO_AUDIO_PLAYBACK)

unsigned int InitSound(unsigned int Rate,unsigned int Latency) {
    int I;
    TrashSound();
    SndRate = 0;

    for(I=0; I<SND_CHANNELS; I++) {
        WaveCH[I].Count  = 0;
        WaveCH[I].Volume = 0;
        WaveCH[I].Freq   = 0;
    }

#if defined(WINDOWS)
    Rate = WinInitSound(Rate,Latency);
#else
    Rate = InitAudio(Rate,Latency);
#endif

    if(!Rate) { SndRate=0; return(0); }

    // === Okazaki ===
    if (!psg_acc) {
        PSG_init(3579545, Rate);
        psg_acc = (void *)PSG_new();

        if (psg_acc) {
            PSG_reset((PSG *)psg_acc);
            PSG_setVolumeMode((PSG *)psg_acc, 2);
            PSG_set_quality(0);
        }
    }

    if (!studio_opll) {
        OPLL_init(3579545, Rate);
        studio_opll = (void *)OPLL_new();

        if (studio_opll) {
            OPLL_reset((OPLL *)studio_opll);
            printf("OPLL INIT OK: studio_opll=%p\n", studio_opll);
        }

        else {
            printf("OPLL INIT FAILED! Check heap_caps_malloc in emu2413.c\n");
        }
    }

    if (!studio_scc) {
        SCC_init(3579545, Rate);
        studio_scc = (void *)SCC_new();

        if (studio_scc) {
            SCC_reset((SCC *)studio_scc);
            printf("SCC INIT OK: studio_scc=%p\n", studio_scc);
        }
    }

    // OPM (SFG-05) инициализируется лениво в MSX.c

    SetChannels(MasterVolume,MasterSwitch);
    return(SndRate=Rate);
}

// === (Soft Clipper) ===
__attribute__((always_inline)) INLINE int SoftClip(int x) {
    if (x > 18000) {
        x = 18000 + ((x - 18000) >> 2);

        if (x > 32767) { return 32767; }

        return x;
    }

    if (x < -18000) {
        x = -18000 + ((x + 18000) >> 2);

        if (x < -32768) { return -32768; }

        return x;
    }

    return x;
}

IRAM_ATTR void RenderAudio(int * restrict Wave, unsigned int Frames) {
    register int J,K,I,L1,V;

    if(UNLIKELY(SndRate<8192)) { return; }
        
    // === STUDIO ENGINE (Okazaki) ===
    if (CurrentSndMode == SND_MODE_ACCURATE) {
        static int psg_dc = 0;

        for(I = 0; I < Frames; I++) {
            int psg_val = 0, opll_val = 0, scc_val = 0;

            if (psg_acc) {
                int raw_psg = PSG_calc(psg_acc);
                psg_dc += (raw_psg - psg_dc) >> 8;
                psg_val = (raw_psg - psg_dc) * 1;        // ±16384
            }

            if (studio_opll) {
                opll_val = OPLL_calc(studio_opll);         // ±1024 (без усиления)
            }

            if (studio_scc) {
                scc_val = SCC_calc((SCC *)studio_scc) * 1; // ±16384
            }

            int out_l = psg_val;
            int out_r = psg_val;

            if (msx_audio_stereo) {
                // OPLL: 50% L, 87% R
                out_l += (opll_val * 128) >> 8;           // ±512
                out_r += (opll_val * 222) >> 8;           // ±888
                // SCC: 87% L, 50% R
                out_l += (scc_val * 222) >> 8;            // ±14216
                out_r += (scc_val * 128) >> 8;            // ±8192
            } else {
                // Моно: дополнительное ослабление ×0.7 для OPLL и SCC
                out_l += (opll_val * 179) >> 8;           // ±717
                out_r += (opll_val * 179) >> 8;
                out_l += (scc_val * 179) >> 8;            // ±11468
                out_r += (scc_val * 179) >> 8;
            }

            Wave[I*2]   += out_l;
            Wave[I*2+1] += out_r;
        }

    } else {

        // === FAST ENGINE (Branchless Meander — Marat) ===
        for(J=0; J<SND_CHANNELS; J++) {
            if(WaveCH[J].Freq && (V=WaveCH[J].Volume) && (MasterSwitch&(1<<J))) {

                int pan_l = (J < 6) ? 256 : ((J & 1) ? 200 : 70);
                int pan_r = (J < 6) ? 256 : ((J & 1) ? 70 : 200);

                switch(WaveCH[J].Type) {
                    case SND_WAVE: {
                        int L2, A1;
                        if(WaveCH[J].Length<=0) { break; }
                        K  = WaveCH[J].Rate>0? (SndRate<<15)/WaveCH[J].Freq/WaveCH[J].Rate
                             : (SndRate<<15)/WaveCH[J].Freq/WaveCH[J].Length;
                        if(K<0x8000) { break; }
                        L1 = WaveCH[J].Pos%WaveCH[J].Length;
                        L2 = WaveCH[J].Count;
                        A1 = WaveCH[J].Data[L1]*V;
                        for(I=0; I<Frames; I++) {
                            if(L2>=K) {
                                L1 = (L1+L2/K)%WaveCH[J].Length;
                                A1 = WaveCH[J].Data[L1]*V;
                                L2 = L2%K;
                            }
                            Wave[I*2]   += (A1 * pan_l) >> 8;
                            Wave[I*2+1] += (A1 * pan_r) >> 8;
                            L2+=0x8000;
                        }
                        WaveCH[J].Pos   = L1;
                        WaveCH[J].Count = L2;
                    }
                    break;

                    case SND_NOISE:
                        if(WaveCH[J].Freq<SndRate) { K=((unsigned int)WaveCH[J].Freq<<16)/SndRate; }
                        else { V = V*SndRate/WaveCH[J].Freq; K = 0x10000; }
                        L1=WaveCH[J].Count;
                        for(I=0; I<Frames; I++) {
                            int amp = ((((NoiseGen >> NoiseOut) & 1) * 255) - 128);
                            int val = amp * V;
                            Wave[I*2]   += (val * pan_l) >> 8;
                            Wave[I*2+1] += (val * pan_r) >> 8;
                            L1+=K;
                            if(UNLIKELY(L1&0xFFFF0000)) {
                                NoiseGen= (((NoiseGen>>NoiseOut)^(NoiseGen>>NoiseXor))&1) | ((NoiseGen<<1)&((2<<NoiseOut)-1));
                                L1&=0xFFFF;
                            }
                        }
                        WaveCH[J].Count=L1;
                        break;

                    case SND_MELODIC:
                    case SND_TRIANGLE:
                    default:
                        if(UNLIKELY(WaveCH[J].Freq>=SndRate/2)) { break; }
                        K=0x10000*WaveCH[J].Freq/SndRate;
                        L1=WaveCH[J].Count;
                        for(I=0; I<Frames; I++,L1+=K) {
                            int trans = (((L1-K)^(L1+K)) >> 15) & 1;
                            int amp   = (((L1 >> 15) & 1) * 255) - 128;
                            int val   = (1 - trans) * amp * V;
                            Wave[I*2]   += (val * pan_l) >> 8;
                            Wave[I*2+1] += (val * pan_r) >> 8;
                        }
                        WaveCH[J].Count=L1&0xFFFF;
                        break;
                }
            }
        }
    }

    // === YAMAHA SFG-05 (OPM) MIXING (2x Decimation) ===
    if (UNLIKELY(sfg_active)) {
        static int16_t opm_l[128];
        static int16_t opm_r[128];
        int16_t *opm_bufs[2] = { opm_l, opm_r };
        
        int opm_frames = (Frames + 1) >> 1;
        wrap_YM2151UpdateOne(opm_bufs, opm_frames);
        
        for (int i = 0; i < Frames; i++) {
            int idx = i >> 1, frac = i & 1;
            int next_idx = (idx + 1 < opm_frames) ? (idx + 1) : idx;
            int l = (frac == 0) ? opm_l[idx] : (opm_l[idx] + opm_l[next_idx]) >> 1;
            int r = (frac == 0) ? opm_r[idx] : (opm_r[idx] + opm_r[next_idx]) >> 1;
            Wave[i*2]   += l;
            Wave[i*2+1] += r;
        }
    }
}

/** PlayAudio() **********************************************/
/** Clamps and converts 32-bit mix to 16-bit stereo output. **/
/*************************************************************/
unsigned int PlayAudio(int * restrict Wave, unsigned int Frames) {
    sample Buf[512]; // Buffer for 256 stereo frames (512 samples total)
    unsigned int I,J,K;
    int D_L, D_R;

    if(UNLIKELY(SndRate<8192)) { return(0); }

    /* GetFreeAudio() returns total 16-bit words. Divide by 2 for stereo frames */
    J = GetFreeAudio() >> 1;

    if(J<Frames) { Frames=J; }

    for(K=I=J=0; (K<Frames)&&(I==J); K+=I) {
        J = (sizeof(Buf)/sizeof(sample)) >> 1; // 256 frames max per pass
        J = Frames-K>J? J:Frames-K;

        for(I=0; I<J; ++I) {
            D_L = ((*Wave++)*MasterVolume)>>8;
            D_R = ((*Wave++)*MasterVolume)>>8;

            /* Hardware clamping to prevent distortion */
            if (UNLIKELY(D_L > 32767)) { D_L = 32767; }

            else if (UNLIKELY(D_L < -32768)) { D_L = -32768; }

            if (UNLIKELY(D_R > 32767)) { D_R = 32767; }

            else if (UNLIKELY(D_R < -32768)) { D_R = -32768; }

#if defined(BPU16)
            Buf[I*2]   = D_L+32768;
            Buf[I*2+1] = D_R+32768;
#elif defined(BPS16)
            Buf[I*2]   = D_L;
            Buf[I*2+1] = D_R;
#elif defined(BPU8)
            Buf[I*2]   = (D_L>>8)+128;
            Buf[I*2+1] = (D_R>>8)+128;
#else
            Buf[I*2]   = D_L>>8;
            Buf[I*2+1] = D_R>>8;
#endif
        }

        /* WriteAudio returns total words. Divide by 2 for frames. */
        I = WriteAudio(Buf, J*2) >> 1;
    }

    return(K);
}

/** RenderAndPlayAudio() *************************************/
unsigned int RenderAndPlayAudio(unsigned int Frames) {
    int Buf[256 * 2]; // 256 stereo frames (512 integers)
//        static int Buf[256 * 2];  // Было на стеке - теперь в .bss (DRAM)
    unsigned int J,I;

    if(UNLIKELY(SndRate<8192)) { return(0); }

    J      = GetFreeAudio() >> 1;
    Frames = Frames<J? Frames:J;

    for(I=0; I<Frames; I+=J) {
        J = Frames-I;
        J = J < 256 ? J : 256;
        memset(Buf, 0, J * 2 * sizeof(int)); // Clear L and R
        RenderAudio(Buf, J);

        if(PlayAudio(Buf, J)<J) { I+=J; break; }
    }

    return(I); // Returns number of frames played
}

/** ResetStudioChips() *************************************/
void ResetStudioChips(void) {
    if (psg_acc) {
        PSG_reset((PSG *)psg_acc);
        PSG_setVolumeMode((PSG *)psg_acc, 2);
    }
    if (studio_opll) {
        OPLL_reset((OPLL *)studio_opll);
    }
    if (studio_scc) {
        SCC_reset((SCC *)studio_scc);
    }
    
    extern int opm_inited;
    if (opm_inited) {
        YM2151ResetChip(0);
    }
    sfg_active = 0;
    
}

#endif /* !NO_AUDIO_PLAYBACK */

