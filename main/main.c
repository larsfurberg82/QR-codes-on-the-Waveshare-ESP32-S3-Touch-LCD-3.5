/*
 * QR code generator for the Waveshare ESP32-S3-Touch-LCD-3.5 (ST7796, 320x480).
 *
 * Type a string on the on-screen keyboard; the QR code below the text field
 * updates after every key. The counter shows characters used / capacity of
 * the configured QR version + error-correction level.
 *
 * BOOT button = clear the text.
 */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board.h"
#include "ui.h"

static const char *TAG = "qr_demo";

_Static_assert(UI_W == BOARD_LCD_H_RES && UI_H == BOARD_LCD_V_RES, "UI and panel size differ");
_Static_assert(UI_BAND_ROWS <= BOARD_BAND_ROWS, "band size mismatch");

static void flush_cb(int y0, int y1, const uint16_t *px)
{
    ESP_ERROR_CHECK(board_lcd_draw(0, y0, UI_W, y1, px));
}

void app_main(void)
{
    ESP_ERROR_CHECK(board_init());

    uint16_t *band = heap_caps_malloc(UI_W * UI_BAND_ROWS * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(band ? ESP_OK : ESP_ERR_NO_MEM);

    ui_init(band, flush_cb, CONFIG_QR_DEMO_INITIAL_TEXT);
    ESP_LOGI(TAG, "capacity: %u characters", (unsigned)ui_max_chars());
    board_backlight_set(CONFIG_QR_DEMO_BACKLIGHT_PERCENT);   /* after 1st frame: no garbage flash */

    bool was_down = false, was_btn = false;
    int x = 0, y = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        bool down = board_touch_read(&x, &y);
#if CONFIG_QR_DEMO_LOG_TOUCH
        if (down && !was_down) ESP_LOGI(TAG, "touch at x=%d y=%d", x, y);
#endif
        was_down = down;
        ui_input(down, x, y, now);

        bool btn = board_button_pressed();
        if (btn && !was_btn) ui_clear();
        was_btn = btn;

        ui_tick(now);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
