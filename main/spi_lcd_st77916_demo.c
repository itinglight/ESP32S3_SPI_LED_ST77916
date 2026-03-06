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

#define EXAMPLE_PIN_NUM_BK_LIGHT       16   //低电平背光亮，高电平背光灭

#define EXAMPLE_LCD_BIT_PER_PIXEL       16 // RGB565 color format, 16 bits per pixel

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

static void lcd_fill_red(esp_lcd_panel_handle_t panel_handle,int color)
{
    static uint16_t red_buf[EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES];

    // RGB565 红色 = 0xF800
    for (int i = 0; i < EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES; i++) {
        red_buf[i] = color;
    }

    esp_lcd_panel_draw_bitmap(panel_handle,
                              0, 0,
                              EXAMPLE_LCD_H_RES,
                              EXAMPLE_LCD_V_RES,
                              red_buf);
}

static bool example_on_color_trans_dome(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    /* 色彩数据传输完成时的回调函数，可以在此处进行一些操作 */

    return false;
}

static const st77916_lcd_init_cmd_t lcd_init_cmds[] = {
        {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x5E}, 1, 0},
    {0xB1, (uint8_t[]){0x55}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB3, (uint8_t[]){0x01}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xB9, (uint8_t[]){0x15}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x07}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xCC, (uint8_t[]){0x7F}, 1, 0},
    {0xCD, (uint8_t[]){0x7F}, 1, 0},
    {0xCE, (uint8_t[]){0xFF}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x40}, 1, 0},
    {0xDE, (uint8_t[]){0x40}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x10, 0x18, 0x0D, 0x0C, 0x38, 0x3E, 0x44, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0F, 0x17, 0x0D, 0x0B, 0x07, 0x3E, 0x33, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x08}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x03}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xE9}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x14}, 1, 0},
    {0xEE, (uint8_t[]){0xFF}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0xFF}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x30}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x05}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xDA}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x04}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD9}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x07}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD7}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x09}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD9}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0B}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDB}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0D}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDD}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x06}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD6}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x08}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD8}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x0A}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xDA}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0C}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDC}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0}
};

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
    
    //步骤二：创建 SPI 接口设备
    void *example_user_ctx = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_LCD_DC,    // 连接 LCD DC（RS） 信号的 IO 编号，可以设为 `-1` 表示不使用
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,    // 连接 LCD CS 信号的 IO 编号，可以设为 `-1` 表示不使用
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,    // SPI 的时钟频率（Hz），ESP 最高支持 80M（SPI_MASTER_FREQ_80M）
                                                // 需根据 LCD 驱动 IC 的数据手册确定其最大值
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,     // 单位 LCD 命令的比特数，应为 8 的整数倍
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS, // 单位 LCD 参数的比特数，应为 8 的整数倍
        .spi_mode = 0,                            // SPI 模式（0-3），需根据 LCD 驱动 IC 的数据手册以及硬件的配置确定（如 IM[3:0]）
        .trans_queue_depth = 10,                  // SPI 设备传输数据的队列深度，一般设为 10 即可
        .on_color_trans_done = example_on_color_trans_dome,   // 单次调用 `esp_lcd_panel_draw_bitmap()` 传输完成后的回调函数
        .user_ctx = &example_user_ctx,            // 传给回调函数的用户参数
        .flags = {    // 以下为 SPI 时序的相关参数，需根据 LCD 驱动 IC 的数据手册以及硬件的配置确定
            .sio_mode = 0,    // 通过一根数据线（MOSI）读写数据，0: Interface I 型，1: Interface II 型
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    //步骤三：创建 LCD 驱动设备

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_LOGI(TAG, "Install ST77916 panel driver");

    const st77916_vendor_config_t vendor_config = { // 用于替换驱动组件中的初始化命令及参数
        .init_cmds = lcd_init_cmds,         // Uncomment these line if use custom initialization commands
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 0,
        },
    };


    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,    // 连接 LCD 复位信号的 IO 编号，可以设为 `-1` 表示不使用
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // 像素色彩的元素顺序（RGB/BGR），
                                                    //  一般通过命令 `LCD_CMD_MADCTL（36h）` 控制
        .bits_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL,  // 色彩格式的位数（RGB565：16，RGB666：18），
                                                    // 一般通过命令 `LCD_CMD_COLMOD（3Ah）` 控制
        .vendor_config = &vendor_config,           // 用于替换驱动组件中的初始化命令及参数
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle));

    /* 初始化 LCD 设备 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle)); //若设备连接了复位引脚，则通过该引脚进行硬件复位，否则通过命令 LCD_CMD_SWRESET(01h) 进行软件复位。
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle)); //通过发送一系列的命令及参数来初始化 LCD 设备。
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));   // 这些函数可以根据需要使用
    // ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    // ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    // ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0)); //通过软件修改画图时的起始和终止坐标，从而实现画图的偏移。
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    while (1)
    {
        /* code */
          // 刷屏测试
        lcd_fill_red(panel_handle,0xF800); // RGB565 红色 = 0xF800
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_fill_red(panel_handle,0x07E0); // RGB565 绿色 = 0x07E0
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_fill_red(panel_handle,0x001F); // RGB565 蓝色 = 0x001F
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
}


