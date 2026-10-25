idf_component_register(
    SRCS "main.c" "board.c" "ui.c" "font8x16.c"
    INCLUDE_DIRS "."
    REQUIRES qrcode esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc
             esp_timer heap freertos log
)
