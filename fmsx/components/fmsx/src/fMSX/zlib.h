// zlib.h — заглушка-адаптер zlib API -> tinf для fMSX

/** fMSX: portable MSX emulator ******************************/
/**                                                         **/
/**                        zlib.h                           **/
/**                                                         **/
/**   Fake zlib header — provides gz* API declarations      **/
/**   for fMSX source files. Does NOT use real zlib.        **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                      LICENSE NOTICE                     **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** This file is a standalone API adapter created for       **/
/** fMSX on ESP32-S3. It contains no code from zlib.        **/
/**                                                         **/
/** Copyright (C) 2026 Ivan Svarkovsky                      **/
/** Licensed under CC BY-NC-SA 4.0                          **/
/** Contact: ivansvarkovsky@gmail.com                       **/
/**                                                         **/
/** ═══════════════════════════════════════════════════════ **/
/**                    NO WARRANTY                          **/
/** ═══════════════════════════════════════════════════════ **/
/**                                                         **/
/** This software is provided 'as-is', without any express  **/
/** or implied warranty. In no event will the authors be    **/
/** held liable for any damages arising from the use of     **/
/** this software. Use at your own risk.                    **/
/**                                                         **/
/*************************************************************/


#ifndef ZLIB_H
#define ZLIB_H

#include <stdio.h>

// Тип gzFile — fMSX ожидает void*
typedef void *gzFile;

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)

#ifdef __cplusplus
extern "C" {
#endif

gzFile gzopen(const char *path, const char *mode);
int    gzclose(gzFile f);
int    gzread(gzFile f, void *buf, unsigned int len);
int    gzwrite(gzFile f, const void *buf, unsigned int len);
long   gzseek(gzFile f, long offset, int whence);
long   gztell(gzFile f);
int    gzgetc(gzFile f);
int    gzeof(gzFile f);
int    gzrewind(gzFile f);
char  *gzgets(gzFile f, char *buf, int len);

#ifdef __cplusplus
}
#endif

// === Макросы-перехватчики fopen/fread/etc → gz* API ===
// Активируются при -DZLIB во всех файлах, включая MSX.c
// State.h в конце отменит их для save/load кода
#ifndef ZLIB_REDEFINE_FOPEN
#define ZLIB_REDEFINE_FOPEN
#define fopen(N, M)         (FILE *)gzopen(N, M)
#define fclose(F)           gzclose((gzFile)(F))
#define fread(B, L, N, F)   gzread((gzFile)(F), B, (L) * (N))
#define fwrite(B, L, N, F)  gzwrite((gzFile)(F), B, (L) * (N))
#define fgets(B, L, F)      gzgets((gzFile)(F), B, L)
#define fseek(F, O, W)      gzseek((gzFile)(F), O, W)
#define rewind(F)           gzrewind((gzFile)(F))
#define fgetc(F)            gzgetc((gzFile)(F))
#define ftell(F)            gztell((gzFile)(F))
#define feof(F)             gzeof((gzFile)(F))
#endif

#endif /* ZLIB_H */
