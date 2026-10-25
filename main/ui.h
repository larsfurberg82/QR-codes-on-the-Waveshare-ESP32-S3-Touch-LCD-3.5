#pragma once
/*
 * Text field + on-screen keyboard + live QR code, drawn with a tiny
 * immediate-mode renderer (no LVGL, no frame buffer).
 *
 * This file is hardware independent: the screen is reached only through the
 * flush callback given to ui_init(), so it also builds on a PC (see
 * tools/host_test/ui_test.c).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_W          320
#define UI_H          480
#define UI_BAND_ROWS  32       /* rows per flush; band buffer = UI_W*UI_BAND_ROWS px */
#define UI_TEXT_CAP   1024     /* hard upper bound for the input string */

/* Called with rows [y0,y1) of full-width RGB565 pixels (already byte-swapped
 * for the panel). Must be finished with `px` when it returns. */
typedef void (*ui_flush_cb_t)(int y0, int y1, const uint16_t *px);

/* band_buf must hold UI_W * UI_BAND_ROWS uint16_t and be DMA-capable if the
 * flush callback uses DMA. Paints the first frame. */
void ui_init(uint16_t *band_buf, ui_flush_cb_t flush, const char *initial_text);

/* Feed the touch state every poll (down + coordinates in screen pixels). */
void ui_input(bool down, int x, int y, uint32_t now_ms);
/* Call regularly (cursor blink, key repeat). */
void ui_tick(uint32_t now_ms);
void ui_clear(void);

const char *ui_text(void);
size_t ui_max_chars(void);   /* capacity of the configured QR version/ECC */

/* Test helper: centre of the key labelled `s` (or the single char `s`) on
 * the currently visible layer. */
bool ui_debug_key_center(const char *s, int *x, int *y);
