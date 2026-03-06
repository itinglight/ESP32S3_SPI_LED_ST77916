/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"



#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#include "esp_lcd_ili9341.h"
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#include "esp_lcd_gc9a01.h"
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916
#include "esp_lcd_st77916.h"
#endif

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
#include "esp_lcd_touch_stmpe610.h"
#elif CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_XPT2046
#include "esp_lcd_touch_xpt2046.h"
#endif

static const char *TAG = "example";

// Using SPI2 in the example
#define LCD_HOST  SPI2_HOST


#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  0
#endif

#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

#define EXAMPLE_PIN_NUM_SCLK           4
#define EXAMPLE_PIN_NUM_MOSI           5
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_LCD_DC         7
#define EXAMPLE_PIN_NUM_LCD_RST        6
#define EXAMPLE_PIN_NUM_LCD_CS         15

#define EXAMPLE_PIN_NUM_BK_LIGHT       16


#define EXAMPLE_PIN_NUM_TOUCH_CS       11

// The pixel number in horizontal and vertical
#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              320
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              240
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916
#define EXAMPLE_LCD_H_RES              360
#define EXAMPLE_LCD_V_RES              360
#endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8

#define EXAMPLE_LVGL_DRAW_BUF_LINES    20 // number of display lines in each draw buffer
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2


void app_main(void)
{
    printf("Hello world!\n");
    spi_bus_config_t buscfg = {
    .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,  // 连接 LCD SCK（SCL） 信号的 IO 编号
    .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,  // 连接 LCD MOSI（SDO、SDA） 信号的 IO 编号
    .miso_io_num = EXAMPLE_PIN_NUM_MISO,  // 连接 LCD MISO（SDI） 信号的 IO 编号，如果不需要从 LCD 读取数据，可以设为 `-1`
    .quadwp_io_num = -1,                  // 必须设置且为 `-1`
    .quadhd_io_num = -1,                  // 必须设置且为 `-1`
    .max_transfer_sz = EXAMPLE_LCD_H_RES * 80 * sizeof(uint16_t), // 表示 SPI 单次传输允许的最大字节数上限，通常设为全屏大小即可
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
                                            // 第 1 个参数表示使用的 SPI 主机 ID，和后续创建接口设备时保持一致
                                            // 第 3 个参数表示使用的 DMA 通道号，默认设置为 `SPI_DMA_CH_AUTO` 即可
}
