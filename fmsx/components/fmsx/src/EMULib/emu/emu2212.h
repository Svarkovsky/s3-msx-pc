#ifndef _EMUSCC_H_
#define _EMUSCC_H_

#include <stdint.h>
#include "emutypes.h" /* Подключаем типы fMSX вместо их переопределения */

#ifdef __cplusplus
extern "C" {
#endif

#define SCC_STANDARD 0
#define SCC_ENHANCED 1

#ifndef SCC_MASK_CH
#define SCC_MASK_CH(i) (1 << (i))
#endif

typedef struct {
  uint32_t clk;
  uint32_t rate;
  uint32_t base_incr;
  uint32_t quality;
  
  uint32_t realstep;
  uint32_t scctime;
  uint32_t sccstep;

  uint32_t incr[5];

  uint8_t reg[0x100 - 0xC0]; // Регистры состояния (64 байта)

  int8_t wave[5][64];        // Таблицы волн каналов

  uint32_t type;             // SCC_STANDARD или SCC_ENHANCED
  uint32_t mode;             // 0: SCC, 1: SCC+
  uint32_t active;           // Активность чипа
  uint32_t base_adr;         // Базовый адрес маппера (0x9000 или 0xB000)

  uint32_t count[5];
  uint32_t freq[5];
  uint32_t phase[5];
  uint32_t volume[5];
  uint32_t offset[5];
  uint32_t rotate[5];

  int32_t ch_out[5];         // Буферы вывода каналов для аналогового фильтра
  int32_t out;               // Общий микшированный вывод

  uint32_t mask;
  uint32_t ch_enable;
  uint32_t ch_enable_next;

  int cycle_4bit;
  int cycle_8bit;
  int refresh;
} SCC;

#ifdef EMUSCC_DLL_EXPORTS
  #define EMUSCC_API __declspec(dllexport)
#elif  EMUSCC_DLL_IMPORTS
  #define EMUSCC_API __declspec(dllimport)
#else
  #define EMUSCC_API
#endif

/* Классический API для обратной совместимости с fMSX Sound.c */
EMUSCC_API void SCC_init(uint32_t c, uint32_t r);
EMUSCC_API SCC *SCC_new(void);
EMUSCC_API void SCC_close(void);

/* Новый API */
EMUSCC_API void SCC_set_quality(SCC *scc, uint32_t q);
EMUSCC_API void SCC_set_rate(SCC *scc, uint32_t r);
EMUSCC_API void SCC_reset(SCC *scc);
EMUSCC_API void SCC_delete(SCC *scc);
EMUSCC_API int16_t SCC_calc(SCC *scc);
EMUSCC_API void SCC_write(SCC *scc, uint32_t adr, uint32_t val);
EMUSCC_API uint32_t SCC_read(SCC *scc, uint32_t adr);
EMUSCC_API uint32_t SCC_readReg(SCC *scc, uint32_t adr);
EMUSCC_API void SCC_writeReg(SCC *scc, uint32_t adr, uint32_t val);
EMUSCC_API void SCC_set_type(SCC *scc, uint32_t type);
EMUSCC_API uint32_t SCC_setMask(SCC *scc, uint32_t mask);
EMUSCC_API uint32_t SCC_toggleMask(SCC *scc, uint32_t mask);
EMUSCC_API int SCC_save_state(SCC *scc, uint8_t *out);
EMUSCC_API void SCC_load_state(SCC *scc, const uint8_t *in, int size);

#ifdef __cplusplus
}
#endif

#endif // _EMUSCC_H_
