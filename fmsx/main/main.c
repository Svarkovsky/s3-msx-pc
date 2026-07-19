/**
    Copyright (C) Marat Fayzullin 1994-2021
    You are not allowed to distribute this software commercially.

    Heavily optimized for ESP32-S3 (Xtensa LX7) architecture.
    Adapted by Ivan Svarkovsky, 2026.
    Contact: ivansvarkovsky@gmail.com

    ─────────────────────────────────────────────────────────
    Rendering Pipeline (Single Buffer + Hardware Scaling):
    ─────────────────────────────────────────────────────────
    
    1. fMSX renders directly into PSRAM framebuffer (updates[0])
    2. PutImage() sends ONE frame per cycle via rg_display_submit()
    3. Retro-Go write_update() applies hardware-accelerated scaling:
       - 256px (Games):  64-bit Super-Scaler (8-Byte Pump), step_x=0.5
       - 512px (MSX-DOS): memcpy PSRAM→SRAM, step_x=1.0
    4. lcd_send_buffer() → vga_fb (PSRAM) via burst writes
    5. LCD_CAM DMA reads vga_fb, hardware byte-swap (lcd_byte_order=1)
    6. Output to GPIO → VGA DAC (R-2R ladder)
    
    Key optimizations:
    - DRAM_ATTR palettes (BPal/XPal) — zero PSRAM access for color lookup
    - Single buffer — no Ping-Pong overhead, minimal PSRAM traffic
    - Single frame submission (ShowVideo → PutImage only)
    - VSync-free: Retro-Go tick_rate handles frame pacing (55/60 Hz)
    - Audio batching with underrun protection
    - Hardware model selection via launcher (msx.json)
    - Disk autosave on exit, reset, and disk change (msx.json)
    - Interactive Turbo Mode toggling on the fly

    Result: Stable 60/50 FPS with zero tearing.
    ─────────────────────────────────────────────────────────
*/

#include <rg_system.h>
#include <string.h>

#include "ds-stlz.h" // STLZ codec: zero-heap screenshot save/preview

// MSX model
#define MSX_MODEL_MSX1  0
#define MSX_MODEL_MSX2  1
#define MSX_MODEL_MSX2P 2

// autofire
static int auto_fire; 

// auto save .dsk
int msx_disk_autosave = 0;   // 0 / 1
extern int msx_disk_modified[2];
extern void MSX_SaveDisks(void);
extern const char *DSKName[];

// Turbo Mode (Z80 Core)
extern uint8_t TurboMode;

extern volatile uint8_t usb_kbd_last_key;

// === Xtensa Memory Barrier Helpers ===
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#define ORDERED_READ(var)         \
    ({                            \
        COMPILER_BARRIER();       \
        typeof(var) _val = (var); \
        COMPILER_BARRIER();       \
        _val;                     \
    })
#define ORDERED_WRITE(var, val) \
    do                          \
    {                           \
        COMPILER_BARRIER();     \
        (var) = (val);          \
        COMPILER_BARRIER();     \
    } while (0)

#define AUDIO_SAMPLE_RATE   (32000)
#define AUDIO_CHUNK_SAMPLES (512)
#define AUDIO_BUF_SIZE      (AUDIO_CHUNK_SAMPLES * 4)

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_task_t *audioQueue;
static rg_app_t *app;

static int JoyState, LastKey, InMenu, InKeyboard;
static int KeyboardCol, KeyboardRow, KeyboardKey;
static int64_t KeyboardDebounce = 0;
static int FrameStartTime;
static int KeyboardEmulation;
static char *PendingLoadSTA = NULL;

//static int pending_audio_samples = 0;
int pending_audio_samples = 0;
static int16_t __attribute__((aligned(16))) audio_buffer[AUDIO_BUF_SIZE];
static volatile int audio_buf_idx = 0;
static volatile int16_t last_snd_l = 0;
static volatile int16_t last_snd_r = 0;

#define BPS16
#define BPP16
#define UNIX
#define GenericSetVideo SetVideo
#define LSB_FIRST
#define NARROW

#define WIDTH  512
#define HEIGHT 228

int frame_max_width = 256;
int msx_active_width = 256;

#define XKEYS 12
#define YKEYS 6

void PutImage(void);

extern bool msx_audio_stereo;

static uint16_t DRAM_ATTR BPal[256];
static uint16_t DRAM_ATTR XPal[80];

static uint16_t XPal0;
static uint16_t *XBuf;

#include <fmsx.h>

static Image NormScreen;
const char *Title = "fMSX 6.0";
const char *Disks[2][MAXDISKS + 1];

static const unsigned char KBDKeys[YKEYS][XKEYS] = {
    {0x1B,    CON_F1, CON_F2, CON_F3, CON_F4, CON_F5, CON_F6, CON_F7, CON_F8, CON_INSERT, CON_DELETE, CON_STOP },
    {'1',     '2',    '3',    '4',    '5',    '6',    '7',    '8',    '9',    '0',        '-',        '='      },
    {CON_TAB, 'Q',    'W',    'E',    'R',    'T',    'Y',    'U',    'I',    'O',        'P',        CON_BS   },
    {'^',     'A',    'S',    'D',    'F',    'G',    'H',    'J',    'K',    'L',        ';',        CON_ENTER},
    {'Z',     'X',    'C',    'V',    'B',    'N',    'M',    ',',    '.',    '/',        0,          0        },
    {'[',     ']',    ' ',    ' ',    ' ',    ' ',    ' ',    '\\',   '\'',   0,          0,          0        }
};

static const char *BiosFolder = RG_BASE_PATH_BIOS "/msx";
static const char *BiosFiles[] = {
    "MSX.ROM",      // Базовый BIOS
    "MSX2.ROM",     // MSX2 BIOS
    "MSX2EXT.ROM",  // MSX2 Extensions

    "MSX2P.ROM",    // MSX2+ BIOS (нужен для -msx2+)
    "MSX2PEXT.ROM", // MSX2+ Extensions

    "FMPAC.ROM",    // FM-PAC
    "DISK.ROM",     // DiskROM (BDOS)
    "MSXDOS2.ROM",  // MSX-DOS 2
    //"PAINTER.ROM",  // Yamaha Painter
    //"KANJI.ROM",    // Kanji Font
};


extern volatile uint8_t hid_report[8];
extern volatile uint8_t hid_report_seq;

static inline void read_hid_report_safe(uint8_t *out)
{
    uint8_t seq1, seq2;
    do {
        seq1 = ORDERED_READ(hid_report_seq);
        COMPILER_BARRIER();
        memcpy(out, (const void *)hid_report, 8);
        COMPILER_BARRIER();
        seq2 = ORDERED_READ(hid_report_seq);
    } while (seq1 != seq2);
}

static inline void SubmitFrame(void)
{    // SubmitFrame вызывается при выходе из меню Retro-Go.
    PutImage(); // Просто перенаправляем на отрисовку кадра.
}

static bool dsk_validator(const char *path)
{
    if (!path) return false;
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".dsk") == 0);
}

int ProcessEvents(int Wait)
{
    for (int i = 0; i < 16; ++i) KeyState[i] = 0xFF;
    JoyState = 0;

    uint8_t local_report[8];
    read_hid_report_safe(local_report);

    bool has_ctrl = (local_report[0] & 0x11) != 0;

    if (local_report[0] & 0x02) KBD_SET(KBD_SHIFT);
    if (local_report[0] & 0x20) KBD_SET(KBD_SHIFT);
    if (local_report[0] & 0x01) KBD_SET(KBD_CONTROL);
    if (local_report[0] & 0x10) KBD_SET(KBD_CONTROL);
    if (local_report[0] & 0x04) KBD_SET(KBD_GRAPH);
    if (local_report[0] & 0x40) KBD_SET(KBD_GRAPH);

    if (__builtin_expect(has_ctrl, 0))
    {
        for (int i = 2; i < 8; i++)
        {
            if (local_report[i] == 0x3F) {
                rg_input_wait_for_key(RG_KEY_ANY, false, 500);
                char *path = rg_gui_file_picker("Select disk for Drive A:", "/sd/roms/msx", dsk_validator, false, false);
                if (path) { 
                    rg_gui_draw_message("Disk...");        // Сообщение
                    rg_system_tick(0);                     // Сбрасываем сторожевой таймер
                    MSX_SaveDisks();                       // Сохраняем дискету
                    ChangeDisk(0, path); 
                    DSKName[0] = strdup(path);             // Запоминаем новый путь
                    msx_disk_modified[0] = 0;              // Сбрасываем флаг
                    free(path); 
                }
                rg_input_wait_for_key(RG_KEY_ANY, false, 500);
                return 0;
            }
            if (local_report[i] == 0x40) {
                rg_input_wait_for_key(RG_KEY_ANY, false, 500);
                char *path = rg_gui_file_picker("Select disk for Drive B:", "/sd/roms/msx", dsk_validator, false, false);
                if (path) { 
                    rg_gui_draw_message("Disk...");
                    rg_system_tick(0);
                    MSX_SaveDisks(); 
                    ChangeDisk(1, path); 
                    DSKName[1] = strdup(path); 
                    msx_disk_modified[1] = 0;
                    free(path); 
                }
                rg_input_wait_for_key(RG_KEY_ANY, false, 500);
                return 0;
            }
        }
    }

    for (int i = 2; i < 8; i++)
    {
        uint8_t hid = local_report[i];
        if (hid == 0) continue;
    //    if (hid == 0x35) KBD_SET(KBD_ESCAPE); // '`'
        if (hid == 0x29) KBD_SET(KBD_ESCAPE);
        else if (hid >= 0x04 && hid <= 0x1D) KBD_SET(hid - 0x04 + 'A');
        else if (hid >= 0x1E && hid <= 0x27) KBD_SET(hid == 0x27 ? '0' : hid - 0x1E + '1');
        else if (hid == 0x2D) KBD_SET('-');
        else if (hid == 0x2E) KBD_SET('=');
        else if (hid == 0x2F) KBD_SET('[');
        else if (hid == 0x30) KBD_SET(']');
        else if (hid == 0x31 || hid == 0x32 || hid == 0x64) KBD_SET('\\');
        else if (hid == 0x33) KBD_SET(';');
        else if (hid == 0x34) KBD_SET('\'');
        else if (hid == 0x36) KBD_SET(',');
        else if (hid == 0x37) KBD_SET('.');
        else if (hid == 0x38 || hid == 0x54) KBD_SET('/');
        else if (hid == 0x2A) KBD_SET(KBD_BS);
        else if (hid == 0x2B) KBD_SET(KBD_TAB);
        else if (hid == 0x28) KBD_SET(KBD_ENTER);
        else if (hid == 0x2C) KBD_SET(KBD_SPACE);
        else if (hid == 0x39) KBD_SET(KBD_CAPSLOCK);
        else if (hid == 0x4F) KBD_SET(KBD_RIGHT);
        else if (hid == 0x50) KBD_SET(KBD_LEFT);
        else if (hid == 0x51) KBD_SET(KBD_DOWN);
        else if (hid == 0x52) KBD_SET(KBD_UP);
        else if (hid >= 0x3A && hid <= 0x3E) KBD_SET(KBD_F1 + (hid - 0x3A));
    }

    uint32_t joystick = rg_input_read_gamepad();
    if (!has_ctrl) joystick &= ~RG_KEY_MENU;

    if (joystick == RG_KEY_MENU) {
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        rg_gui_game_menu();
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        return 0;
    } else if (joystick == RG_KEY_OPTION) {
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        rg_gui_options_menu();
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        return 0;
    } else if (joystick == RG_KEY_SELECT) {
        InKeyboard = !InKeyboard;
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
    } else if (joystick == RG_KEY_START) {
        rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        InMenu = 2;
        return 0;
    }

    if (__builtin_expect(InMenu == 2, 0)) {
        InMenu = 1;
        rg_audio_set_mute(true);
        MenuMSX();
        rg_audio_set_mute(false);
        rg_input_wait_for_key(RG_KEY_ANY, false, 1000);
        InMenu = 0;
    } else if (InMenu) {
        if (joystick == RG_KEY_LEFT) LastKey = CON_LEFT;
        if (joystick == RG_KEY_RIGHT) LastKey = CON_RIGHT;
        if (joystick == RG_KEY_UP) LastKey = CON_UP;
        if (joystick == RG_KEY_DOWN) LastKey = CON_DOWN;
        if (joystick == RG_KEY_A) LastKey = CON_OK;
        if (joystick == RG_KEY_B) LastKey = CON_EXIT;
    } else if (__builtin_expect(InKeyboard, 0)) {
        if (joystick & (RG_KEY_LEFT | RG_KEY_RIGHT | RG_KEY_UP | RG_KEY_DOWN)) {
            if (rg_system_timer() > KeyboardDebounce) {
                if (joystick == RG_KEY_LEFT) KeyboardCol--;
                if (joystick == RG_KEY_RIGHT) KeyboardCol++;
                if (joystick == RG_KEY_UP) KeyboardRow--;
                if (joystick == RG_KEY_DOWN) KeyboardRow++;
                KeyboardCol = RG_MIN(RG_MAX(KeyboardCol, 0), XKEYS - 1);
                KeyboardRow = RG_MIN(RG_MAX(KeyboardRow, 0), YKEYS - 1);
                PutImage();
                KeyboardDebounce = rg_system_timer() + 250000;
            }
        } else if (joystick == RG_KEY_A) {
            KeyboardKey = KBDKeys[KeyboardRow][KeyboardCol];
            KBD_SET(KeyboardKey);
            rg_input_wait_for_key(RG_KEY_ANY, false, 500);
        } else if (joystick == RG_KEY_B) {
            rg_input_wait_for_key(RG_KEY_ANY, false, 500);
            InKeyboard = false;
        }
    } else if (__builtin_expect(KeyboardEmulation, 0)) {
        if (joystick & RG_KEY_LEFT) KBD_SET(KBD_LEFT);
        if (joystick & RG_KEY_RIGHT) KBD_SET(KBD_RIGHT);
        if (joystick & RG_KEY_UP) KBD_SET(KBD_UP);
        if (joystick & RG_KEY_DOWN) KBD_SET(KBD_DOWN);
        if (joystick & RG_KEY_A) KBD_SET(KBD_SPACE);
        if (joystick & RG_KEY_B) KBD_SET(KBD_ENTER);
    } else {
        if (joystick & RG_KEY_LEFT) JoyState |= JST_LEFT;
        if (joystick & RG_KEY_RIGHT) JoyState |= JST_RIGHT;
        if (joystick & RG_KEY_UP) JoyState |= JST_UP;
        if (joystick & RG_KEY_DOWN) JoyState |= JST_DOWN;
        if (joystick & RG_KEY_A) JoyState |= JST_FIREA;
        if (joystick & RG_KEY_B) JoyState |= JST_FIREB;
    }
    return 0;
}

// ─── Прямой аппаратный маппинг цвета на пины VGA DAC ──────
// R: GPIO 13-12 (биты 13-12)
// G: GPIO 8-7   (биты 8-7)
// B: GPIO 3-2   (биты 3-2)
// Возвращает цвет в Little-Endian для хранения в PSRAM
static inline uint16_t get_hardware_color(byte R, byte G, byte B)
{
    uint16_t dac_r = R >> 6;  // 0..3  биты 13-12
    uint16_t dac_g = G >> 6;  // 0..3  биты 8-7
    uint16_t dac_b = B >> 6;  // 0..3  биты 3-2

    uint16_t hw_be = (dac_r << 12) | (dac_g << 7) | (dac_b << 2);
    return (hw_be >> 8) | (hw_be << 8);  // LE для PSRAM
}

int InitMachine(void)
{
    NormScreen = (Image){.Data = currentUpdate->data, .W = WIDTH, .H = HEIGHT, .L = WIDTH, .D = 16};
    XBuf = NormScreen.Data;
    SetScreenDepth(NormScreen.D);
    SetVideo(&NormScreen, 0, 0, WIDTH, HEIGHT);

    for (int J = 0; J < 80; J++) SetColor(J, 0, 0, 0);

    // Палитра Screen 8: собираем биты строго под ваши GPIO
    for (int J = 0; J < 256; J++) {
        uint8_t r = (J >> 2) & 0x07; // 3 бита красного MSX (0-7)
        uint8_t g = (J >> 5) & 0x07; // 3 бита зеленого MSX (0-7)
        uint8_t b = (J & 0x03);      // 2 бита синего MSX (0-3)

        // Сжимаем до 2-битного ЦАПа (0-3)
        uint8_t dac_r = r >> 1; 
        uint8_t dac_g = g >> 1; 
        uint8_t dac_b = b;      // уже 2 бита

        // Ставим биты строго на пины вашей шины данных:
        // R0,R1 -> биты 14,15 | G0,G1 -> биты 9,10 | B0,B1 -> биты 3,4
        uint16_t hw_be = (dac_r << 14) | (dac_g << 9) | (dac_b << 3);
        
        // Переворачиваем в Little-Endian для буфера
        BPal[J] = __builtin_bswap16(hw_be);
    }

    InitSound(AUDIO_SAMPLE_RATE, 150);
    SetChannels(255, 0xFFFFFFFF);
    RPLInit(SaveState, LoadState, MAX_STASIZE);
    RPLRecord(RPL_RESET);
    return 1;
}

void TrashMachine(void) { RPLTrash(); TrashSound(); }

void SetColor(byte N, byte R, byte G, byte B)
{
    // R, G, B приходят от эмулятора от 0 до 255.
    // Сжимаем их до 2-битного диапазона ЦАПа (0-3)
    uint8_t dac_r = R >> 6;
    uint8_t dac_g = G >> 6;
    uint8_t dac_b = B >> 6;

    // Снова ставим биты строго на аппаратные GPIO
    uint16_t hw_be = (dac_r << 14) | (dac_g << 9) | (dac_b << 3);
    
    // Переворачиваем цвет для Retro-Go
    uint16_t color = __builtin_bswap16(hw_be);
    
    if (N) XPal[N] = color;
    else   XPal0 = color;
}

void PutImage(void)
{
    if (__builtin_expect(InKeyboard, 0))
        DrawKeyboard(&NormScreen, KBDKeys[KeyboardRow][KeyboardCol]);

    // Запоминаем последнюю отрисованную ширину.
    // Если мы в меню (эмулятор на паузе), frame_max_width будет равен 0.
    static int last_valid_w = 256;
    if (frame_max_width > 0) {
        last_valid_w = frame_max_width;
    }
    
    int activeW = last_valid_w;
    
    if (__builtin_expect(activeW < 256, 0)) {
        activeW = 256;
    }

    static int lastActiveW = -1;
    if (activeW != lastActiveW) {
        if (activeW > 256) {
            // MSX-DOS (512px): Растягиваем на весь VGA-монитор (640x480)
            rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);
        } else {
            // Игры (256px): Ставим строгий ZOOM x2 (512x456)
            rg_display_set_scaling(RG_DISPLAY_SCALING_ZOOM);
            rg_display_set_custom_zoom(2.0);
        }
        lastActiveW = activeW;
    }

    currentUpdate->width = activeW;
    currentUpdate->height = HEIGHT;
    currentUpdate->stride = WIDTH * 2;
    if (activeW > 256) {
        currentUpdate->offset = 0;
        currentUpdate->width = WIDTH;
    } else {
        currentUpdate->offset = ((WIDTH - 256) / 2) * 2;
    }
    frame_max_width = 0; // Сбрасываем для следующего кадра эмулятора

    // Безопасный асинхронный Ping-Pong
    if (__builtin_expect(!rg_display_sync(false), 0))
        rg_display_sync(true); // принудительно
    
    rg_display_submit(currentUpdate, 0);
    
    NormScreen.Data = currentUpdate->data;
    XBuf = currentUpdate->data;
    SetVideo(&NormScreen, 0, 0, WIDTH, HEIGHT);
}

unsigned int Joystick(void) { ProcessEvents(0); return JoyState; }

void Keyboard(void)
{
    rg_system_tick(rg_system_timer() - FrameStartTime);
    FrameStartTime = rg_system_timer();
    
    if (PendingLoadSTA) { 
        LoadSTA(PendingLoadSTA);   // Загружаем состояние в ОЗУ
        free(PendingLoadSTA); 
        PendingLoadSTA = NULL; 
        extern void ResetStudioChips(void); // 
        ResetStudioChips();
        pending_audio_samples = 0; // Сбрасываем накопившиеся остатки сэмплов в буфере
        ORDERED_WRITE(audio_buf_idx, 0);
    }
}

unsigned int Mouse(byte N) { return 0; }

int ShowVideo(void)
{
    PutImage();        // Единственная отправка кадра за цикл.
    rg_system_tick(0); // Ограничитель кадров силами Retro-Go
    return 1;  
}

unsigned int GetJoystick(void) { ProcessEvents(0); return 0; }
unsigned int GetMouse(void) { return 0; }
unsigned int GetKey(void) { unsigned int J; ProcessEvents(0); J = LastKey; LastKey = 0; return J; }

unsigned int WaitKey(void)
{
    GetKey();
    rg_input_wait_for_key(RG_KEY_ANY, false, 200);
    while (!rg_input_wait_for_key(RG_KEY_ANY, true, 100)) continue;
    return GetKey();
}

unsigned int WaitKeyOrMouse(void) { LastKey = WaitKey(); return 0; }
unsigned int InitAudio(unsigned int Rate, unsigned int Latency) { return AUDIO_SAMPLE_RATE; }
void TrashAudio(void) {}
unsigned int GetFreeAudio(void) { return 1024; }

void PlayAllSound(int cycles)
{
    static int64_t sample_accum = 0;
    sample_accum += (int64_t)cycles * AUDIO_SAMPLE_RATE;
    int samples = sample_accum / 3579545LL;
    sample_accum %= 3579545LL;
    pending_audio_samples += samples;
    while (pending_audio_samples >= AUDIO_CHUNK_SAMPLES) {
        rg_task_send(audioQueue, &(rg_task_msg_t){.dataInt = AUDIO_CHUNK_SAMPLES});
        pending_audio_samples -= AUDIO_CHUNK_SAMPLES;
    }
}

unsigned int WriteAudio(sample *Data, unsigned int Length)
{
    int16_t *src = (int16_t *)Data;
    uint32_t count = Length;
    for (uint32_t i = 0; i < count; i++) {
        int idx = ORDERED_READ(audio_buf_idx);
        if (__builtin_expect(idx < AUDIO_BUF_SIZE, 1)) {
            audio_buffer[idx] = src[i];
            ORDERED_WRITE(audio_buf_idx, idx + 1);
        }
    }
    if (count >= 2) {
        ORDERED_WRITE(last_snd_l, src[count - 2]);
        ORDERED_WRITE(last_snd_r, src[count - 1]);
    }
    return Length;
}

static void audioTask(void *arg)
{
    RG_LOGI("audio task started");
    rg_task_msg_t msg;
    while (1) {
        if (rg_task_peek(&msg)) {
            rg_task_receive(&msg);
            RenderAndPlayAudio(msg.dataInt);
            int samples = ORDERED_READ(audio_buf_idx);
            if (samples > 0) {
                COMPILER_BARRIER();
                rg_audio_submit((const rg_audio_frame_t *)audio_buffer, samples >> 1);
                COMPILER_BARRIER();
                ORDERED_WRITE(audio_buf_idx, 0);
            }
        } else {
            int16_t snd_l = ORDERED_READ(last_snd_l);
            int16_t snd_r = ORDERED_READ(last_snd_r);
            for (int i = 0; i < 128; i += 2) {
                audio_buffer[i] = snd_l;
                audio_buffer[i + 1] = snd_r;
            }
            COMPILER_BARRIER();
            rg_audio_submit((const rg_audio_frame_t *)audio_buffer, 64);
            COMPILER_BARRIER();
        }
    }
}

static bool save_state_handler(const char *filename) { return SaveSTA(filename); }

static bool load_state_handler(const char *filename) { PendingLoadSTA = strdup(filename); return true; }

static bool reset_handler(bool hard)
{
    rg_gui_draw_message("Saving Disk..."); // Сохранение при сбросе эмулятора
    rg_system_tick(0);
    MSX_SaveDisks();

    int16_t silence[128] = {0};
    COMPILER_BARRIER();
    rg_audio_submit((const rg_audio_frame_t *)silence, 64);
    rg_audio_submit((const rg_audio_frame_t *)silence, 64);
    COMPILER_BARRIER();
    ORDERED_WRITE(last_snd_l, 0);
    ORDERED_WRITE(last_snd_r, 0);
    ORDERED_WRITE(audio_buf_idx, 0);
    pending_audio_samples = 0;
    rg_task_delay(20);
    ResetMSX(Mode, RAMPages, VRAMPages);
    return true;
}

// Save screenshot directly in STLZ format. Writes current framebuffer stripe-by-stripe (~16KB peak heap) instead of allocating 273KB for LodePNG intermediate buffers.
static bool screenshot_handler(const char *filename, int width, int height)
{    // Напрямую пишем скриншот в формате STLZ, минуя промежуточные тяжелые преобразования
    return rg_surface_save_stlz(currentUpdate, filename);   // и исключая выделение буферов на 273 КБ
}

static void event_handler(int event, void *arg)
{ 
    if (event == RG_EVENT_REDRAW) {
        SubmitFrame(); 
    } else if (event == RG_EVENT_SHUTDOWN) {
        rg_gui_draw_message("Saving Disk..."); // Сохранение при выходе из эмулятора
        rg_system_tick(0);
        MSX_SaveDisks();
    }
}

static rg_gui_event_t input_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        KeyboardEmulation = !KeyboardEmulation;
        rg_settings_set_number(NS_APP, "Input", KeyboardEmulation);
    }
    strcpy(option->value, KeyboardEmulation ? _("Keyboard") : _("Joystick"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t stereo_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        msx_audio_stereo = !msx_audio_stereo;
        rg_settings_set_number(NS_APP, "Stereo", msx_audio_stereo);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, msx_audio_stereo ? _("Stereo") : _("Mono"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t turbo_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        TurboMode = !TurboMode;
        rg_settings_set_number(NS_APP, "TurboMode", TurboMode);
    }
    strcpy(option->value, TurboMode ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Turbo Mode"), "-", RG_DIALOG_FLAG_NORMAL, &turbo_select_cb};
    *dest++ = (rg_gui_option_t){0, _("Input"), "-", RG_DIALOG_FLAG_NORMAL, &input_select_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio Output"), "-", RG_DIALOG_FLAG_NORMAL, &stereo_select_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
    rg_system_set_tick_rate(57); //  55
    
    int hw_model = rg_settings_get_number(NS_APP, "HWModel", MSX_MODEL_MSX2);
    const char *msx_mode = "-msx2"; 
    if (hw_model == MSX_MODEL_MSX1)       msx_mode = "-msx1";
    else if (hw_model == MSX_MODEL_MSX2P) msx_mode = "-msx2+";
    
    RG_LOGI("VGA mode: scaling fixed (hardware 1:1 + 2x fast-path)");

    updates[0] = rg_surface_create(WIDTH, HEIGHT, RG_PIXEL_565_BE, MEM_SLOW); // буфер всего один
    currentUpdate = updates[0];

    KeyboardEmulation = rg_settings_get_number(NS_APP, "Input", 1);
    msx_audio_stereo = rg_settings_get_number(NS_APP, "Stereo", 1);
    auto_fire = rg_settings_get_number(NS_APP, "AutoFire", 0);
    TurboMode = rg_settings_get_number(NS_APP, "TurboMode", 0);
    
    msx_disk_autosave = rg_settings_get_number(NS_APP, "DiskAutoSave", 0);

    for (size_t i = 0; i < RG_COUNT(BiosFiles); ++i)
    {
        char pathbuf[RG_PATH_MAX + 1];
        snprintf(pathbuf, RG_PATH_MAX, "%s/%s", BiosFolder, BiosFiles[i]);
        if (!rg_storage_exists(pathbuf)) {
            char message[512];
            snprintf(message, 512, "File: %s\nYou can find it at:\n%s", rg_relpath(pathbuf), "https://fms.komkon.org/fMSX/");
            rg_gui_alert(_("BIOS file missing!"), message);
        }
    }

    if (app->bootFlags & RG_BOOT_RESUME)
        PendingLoadSTA = rg_emu_get_path(RG_PATH_SAVE_STATE + app->saveSlot, app->romPath);

    const char *argv[] = {
        "fmsx",
        msx_mode,              
        "-ram", "256",        // 
        "-vram", "8",         // 8 * 16KB = 128 KB
        "-skip", "50",        // Пропуск кадров 
        "-home", BiosFolder,  // Путь к BIOS-файлам
        "-joy", "0",          // Джойстик: 0=нет, 1=порт A, 2=порт B
      NULL, NULL, NULL, NULL, // Резерв под 3 аргумента + 1 гарантированный NULL в конце    
    };     // 8 (128 КБ)    16 (256 КБ)    32 (512 КБ)    64 (1 Мегабайт)    128 (2 Мегабайта)    256 (4 Мегабайта)

    int argc = RG_COUNT(argv) - 4; // Теперь вычитаем 4, чтобы argc по-прежнему стартовал с 12

        int is_disk = strstr(app->romPath, ".dsk") != NULL;
        int is_tape = strstr(app->romPath, ".cas") != NULL;
        
        if (auto_fire) argv[argc++] = "-auto";
        if (is_disk) argv[argc++] = "-diska";
        else if (is_tape) argv[argc++] = "-tape";
        argv[argc++] = app->romPath;

     audioQueue = rg_task_create("audioTask", &audioTask, NULL, 4096 , RG_TASK_PRIORITY_3, 1);     // 4096 6144 8192   TASK_PRIORITY 3-6

    RG_LOGI("fMSX start (Hardware Color Mapping)");
    fmsx_main(argc, (char **)argv);
    RG_LOGI("fMSX ended");
    rg_system_exit();
}
