// test
// 24 мая 2026

#ifndef SOUND_H
#define SOUND_H

#include "EMULib.h"
#include <stdbool.h>



/* Режимы качества звука */
typedef enum {
    SND_MODE_FAST,      /* Branchless-меандр (Marat) */
    SND_MODE_ACCURATE   /* Точная эмуляция (Okazaki) */
} SoundMode;

extern SoundMode CurrentSndMode;
extern bool msx_audio_stereo; // Флаг Стерео / Моно


#ifdef __cplusplus
extern "C" {
#endif

#define SND_MELODIC     0      
#define SND_RECTANGLE   0      
#define SND_TRIANGLE    1      
#define SND_NOISE       2      
#define SND_PERIODIC    3      
#define SND_WAVE        4      
#define SND_MIDI        0x100  

#define DRM_CLICK       0      
#define DRM_MIDI        0x100  

#define MIDI_CHANNELS   16     
#define MIDI_MINFREQ    9      
#define MIDI_MAXFREQ    12285  
#define MIDI_DIVISIONS  1000   

#define MIDI_OFF        0      
#define MIDI_ON         1      
#define MIDI_TOGGLE     2      
#define MIDI_QUERY      3      

unsigned int InitSound(unsigned int Rate,unsigned int Latency);
void TrashSound(void);
void RenderAudio(int *Wave,unsigned int Samples);
unsigned int PlayAudio(int *Wave,unsigned int Samples);
unsigned int RenderAndPlayAudio(unsigned int Samples);
void Sound(int Channel,int Freq,int Volume);
void Drum(int Type,int Force);
void SetSound(int Channel,int NewType);
void SetChannels(int Volume,int Switch);
void SetNoise(int Seed,int OUTBit,int XORBit);
void SetWave(int Channel,const signed char *Data,int Length,int Rate);
const signed char *GetWave(int Channel);
unsigned int GetSndRate(void);
void InitMIDI(const char *FileName);
void TrashMIDI(void);
int MIDILogging(int Switch);
void MIDITicks(int N);

#if !defined(MSDOS) & !defined(UNIX) & !defined(MAEMO) & !defined(WINDOWS) & !defined(S60) & !defined(UIQ) && !defined(ANDROID)
#define SND_CHANNELS MIDI_CHANNELS         
#endif

struct SndDriverStruct
{
  void (*SetSound)(int Channel,int NewType);
  void (*Drum)(int Type,int Force);
  void (*SetChannels)(int Volume,int Switch);
  void (*Sound)(int Channel,int NewFreq,int NewVolume);
  void (*SetWave)(int Channel,const signed char *Data,int Length,int Freq);
  const signed char *(*GetWave)(int Channel);
};
extern struct SndDriverStruct SndDriver;

//
void ResetStudioChips(void);

#ifdef __cplusplus
}
#endif
#endif /* SOUND_H */
