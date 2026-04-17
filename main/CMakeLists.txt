idf_component_register(SRCS "spi_lcd_touch_example_main.c" "lvgl_demo_ui.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES spi_flash app_update esp_partition esp_psram)
