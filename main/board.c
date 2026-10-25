#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define LCD_HOST        SPI2_HOST
#define I2C_TIMEOUT_MS  100

/* TCA9554 registers */
#define TCA_REG_OUTPUT  0x01
#define TCA_REG_CONFIG  0x03   /* 1 = input, 0 = output */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tca;
static i2c_master_dev_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_draw_done;

/* ---------------------------------------------------------------- I2C */
static esp_err_t tca_read(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_tca, &reg, 1, v, 1, I2C_TIMEOUT_MS);
}
static esp_err_t tca_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_tca, b, 2, I2C_TIMEOUT_MS);
}

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_BOARD_PIN_I2C_SDA,
        .scl_io_num = CONFIG_BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "i2c bus");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BOARD_TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_tca), TAG, "tca add");

    dev.device_address = CONFIG_BOARD_FT6336_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_touch), TAG, "touch add");
    return ESP_OK;
}

/* Hardware-reset the ST7796 through the TCA9554 (LCD_RST is EXIO1). */
static esp_err_t lcd_hw_reset(void)
{
    const uint8_t bit = (uint8_t)(1u << CONFIG_BOARD_TCA9554_LCD_RST_BIT);
    uint8_t out, cfg;

    ESP_RETURN_ON_ERROR(tca_read(TCA_REG_OUTPUT, &out), TAG, "tca read (check I2C / address)");
    ESP_RETURN_ON_ERROR(tca_write(TCA_REG_OUTPUT, out | bit), TAG, "tca out");   /* start high */
    ESP_RETURN_ON_ERROR(tca_read(TCA_REG_CONFIG, &cfg), TAG, "tca cfg read");
    ESP_RETURN_ON_ERROR(tca_write(TCA_REG_CONFIG, cfg & (uint8_t)~bit), TAG, "tca cfg"); /* make output */
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(tca_write(TCA_REG_OUTPUT, out & (uint8_t)~bit), TAG, "tca low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(tca_write(TCA_REG_OUTPUT, out | bit), TAG, "tca high");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/* ---------------------------------------------------------------- LCD */
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_draw_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t lcd_init(void)
{
    s_draw_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_draw_done, ESP_ERR_NO_MEM, TAG, "sem");

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_BOARD_PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_BAND_ROWS * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    /* LCD_CS is hard-wired low on this board (pulled down), so no CS GPIO. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_BOARD_PIN_LCD_DC,
        .cs_gpio_num = -1,
        .pclk_hz = CONFIG_BOARD_LCD_SPI_MHZ * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   /* reset is done via the I/O expander */
        .rgb_ele_order = CONFIG_BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &panel_cfg, &s_panel), TAG, "st7796");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, CONFIG_BOARD_LCD_INVERT));
#if CONFIG_BOARD_LCD_MIRROR_X
    const bool mirror_x = true;
#else
    const bool mirror_x = false;
#endif
#if CONFIG_BOARD_LCD_MIRROR_Y
    const bool mirror_y = true;
#else
    const bool mirror_y = false;
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    return ESP_OK;
}

esp_err_t board_lcd_draw(int x0, int y0, int x1, int y1, const void *data)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, data), TAG, "draw");
    /* Wait until DMA is done so the caller may reuse its buffer. */
    if (xSemaphoreTake(s_draw_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------- backlight */
static esp_err_t backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
    ledc_channel_config_t c = {
        .gpio_num = CONFIG_BOARD_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&c);
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)percent * 1023u / 100u);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* -------------------------------------------------------------- input */
bool board_touch_read(int *x, int *y)
{
    if (!s_touch) return false;
    uint8_t reg = 0x02, d[5];              /* TD_STATUS, XH, XL, YH, YL */
    if (i2c_master_transmit_receive(s_touch, &reg, 1, d, sizeof d, 20) != ESP_OK) return false;
    uint8_t n = d[0] & 0x0F;
    if (n < 1 || n > 2) return false;

    int tx = ((d[1] & 0x0F) << 8) | d[2];
    int ty = ((d[3] & 0x0F) << 8) | d[4];
#if CONFIG_BOARD_TOUCH_SWAP_XY
    { int t = tx; tx = ty; ty = t; }
#endif
#if CONFIG_BOARD_TOUCH_MIRROR_X
    tx = BOARD_LCD_H_RES - 1 - tx;
#endif
#if CONFIG_BOARD_TOUCH_MIRROR_Y
    ty = BOARD_LCD_V_RES - 1 - ty;
#endif
    if (tx < 0) tx = 0;
    if (tx > BOARD_LCD_H_RES - 1) tx = BOARD_LCD_H_RES - 1;
    if (ty < 0) ty = 0;
    if (ty > BOARD_LCD_V_RES - 1) ty = BOARD_LCD_V_RES - 1;
    *x = tx;
    *y = ty;
    return true;
}

bool board_button_pressed(void)
{
    return gpio_get_level(CONFIG_BOARD_PIN_BOOT_BUTTON) == 0;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(lcd_hw_reset(), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_BOARD_PIN_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button");

    uint8_t reg = 0xA8, id = 0;            /* FT6336 vendor ID, just a probe */
    if (i2c_master_transmit_receive(s_touch, &reg, 1, &id, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "FT6336 not responding - touch disabled, use the BOOT button");
        i2c_master_bus_rm_device(s_touch);
        s_touch = NULL;
    }
    return ESP_OK;
}
