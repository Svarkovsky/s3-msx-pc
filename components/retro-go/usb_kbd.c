#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "rg_input.h"

// Глобальные переменные для перехвата (volatile для безопасного чтения из других задач)
volatile uint32_t usb_kbd_gamepad_state = 0;
volatile uint8_t hid_report[8] = {0};
volatile uint8_t hid_report_seq = 0;  // Sequence counter для защиты от гонок

static const char *TAG = "USB_KBD";

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event, void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        size_t data_length = 0;
        hid_host_device_get_raw_input_report_data(hid_device_handle, (uint8_t*)hid_report, sizeof(hid_report), &data_length);

        if (data_length >= 8) {
            uint32_t gp = 0;
            
            // Проверяем, зажат ли Ctrl (0x01 = Left Ctrl, 0x10 = Right Ctrl)
            bool ctrl_pressed = (hid_report[0] & 0x11) != 0;
            
            // Маппинг клавиш для навигации по меню Retro-Go
            for (int i = 2; i < 8; i++) {
                uint8_t key = hid_report[i];
                if (key == 0x4F) gp |= RG_KEY_RIGHT;   // Right arrow
                if (key == 0x50) gp |= RG_KEY_LEFT;    // Left arrow
                if (key == 0x51) gp |= RG_KEY_DOWN;    // Down arrow
                if (key == 0x52) gp |= RG_KEY_UP;      // Up arrow
                if (key == 0x28) gp |= RG_KEY_A;       // Enter = A (confirm)
                if (key == 0x2A) gp |= RG_KEY_B;       // Backspace = B (back)
                // Esc (0x29) + Ctrl = меню Retro-Go
                if (key == 0x29 && ctrl_pressed) gp |= RG_KEY_MENU;
            }
            usb_kbd_gamepad_state = gp;
            hid_report_seq++;
        }
    }   
    else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Keyboard Disconnected");
        hid_host_device_close(hid_device_handle);
        hid_report_seq++;
        usb_kbd_gamepad_state = 0;
    }
}

static void hid_host_driver_callback(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event, void *arg)
{
    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED:
            ESP_LOGI(TAG, "HID Device connected!");
            const hid_host_device_config_t device_config = {
                .callback = hid_host_interface_callback,
                .callback_arg = NULL,
            };
            ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &device_config));
            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
            ESP_LOGI(TAG, "Keyboard ready!");
            break;
        default:
            break;
    }
}
/*
static void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "Starting USB Host Task...");
    ESP_LOGI(TAG, "USB stack initialized. Waiting for device...");
    
    while (1) {
        // Оптимизировано: минимальный таймаут 1мс для USB событий,
        // а HID события опрашиваем мгновенно (0 таймаут).
        usb_host_lib_handle_events(pdMS_TO_TICKS(1), NULL);
        hid_host_handle_events(0);
        
        // Уступаем квант времени операционной системе (ровно 1 системный тик)
        vTaskDelay(1);
    }
}*/
static void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "Starting Event-Driven USB Host Task...");
    while (1) {
        // Блокирующий вызов без таймаута. 
        // Задача засыпает. Процессор просыпается МГНОВЕННО (в микросекунды) 
        // только тогда, когда контроллер USB ловит прерывание от клавиатуры.
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
    }
}

void usb_kbd_init(void)
{
    ESP_LOGI(TAG, "Initializing USB Host...");
    
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

        const hid_host_driver_config_t hid_driver_config = {
                .create_background_task = true,  // true! Драйвер сам создаст быстрый таск в фоне.
                .task_priority = 5,
                .stack_size = 4096,
                .core_id = 1,  
                .callback = hid_host_driver_callback,
                .callback_arg = NULL,
            };
           
    
    ESP_ERROR_CHECK(hid_host_install(&hid_driver_config));
    
    // Создаём задачу обработки USB
    xTaskCreatePinnedToCore(usb_host_task, "usb_task", 4096, NULL, 5, NULL, 1); // CPU 1
    
    ESP_LOGI(TAG, "Connect keyboard to USB OTG port...");
}
