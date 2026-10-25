#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Portrait orientation, native panel resolution. */
#define BOARD_LCD_H_RES   320
#define BOARD_LCD_V_RES   480
/* Largest block passed to board_lcd_draw() in one call, in rows. */
#define BOARD_BAND_ROWS   32

/* Bring up I2C, TCA9554 (LCD reset), SPI + ST7796, backlight PWM (off),
 * FT6336 touch and the BOOT button. Touch failure is non-fatal. */
esp_err_t board_init(void);

/* Blocking draw of RGB565 pixels (big-endian byte order as the panel wants).
 * (x1,y1) is exclusive. `data` must be DMA-capable internal RAM and at most
 * BOARD_LCD_H_RES * BOARD_BAND_ROWS pixels. */
esp_err_t board_lcd_draw(int x0, int y0, int x1, int y1, const void *data);

void board_backlight_set(uint8_t percent);

/* Reads the FT6336. Returns true while a finger is on the panel and fills
 * x/y in screen pixels (portrait, 0..319 / 0..479) after applying the
 * swap/mirror options from menuconfig. Returns false if not touched or if
 * touch is unavailable. */
bool board_touch_read(int *x, int *y);
/* True while the BOOT button is held. */
bool board_button_pressed(void);
