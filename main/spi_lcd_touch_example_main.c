/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "spi_flash_mmap.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lvgl.h"
#include <math.h>

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
#elif CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_CST816S
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#endif

static const char *TAG = "example";

// I2S 句柄
static i2s_chan_handle_t tx_handle = NULL; // 播放通道
static i2s_chan_handle_t rx_handle = NULL; // 录音通道

// 录音缓冲区
static int16_t *record_buffer = NULL;
static size_t recorded_samples = 0;
static bool is_recording = false;
static bool has_recording = false;

// ZTS6672 兼容 ICS-43434：Philips 格式，24-bit 数据，32-bit 槽位
// 24-bit 数据左对齐在 32-bit 槽内，高 24 位有效，低 8 位为 0
static int16_t zts6672_raw_to_pcm16(int32_t raw_sample)
{
    // 取高 16 位（24-bit 左对齐数据的最高有效位）
    int16_t pcm16 = (int16_t)(raw_sample >> 16);
    return pcm16;
}

// 音量控制（0-100）
static int audio_volume = 50;

// 嘀嗒声控制
static bool tick_tock_enabled = false;

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED && CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_CST816S
static esp_lcd_touch_handle_t touch_handle = NULL;
static SemaphoreHandle_t touch_mux = NULL;
#endif

// Using SPI2 in the example
#define LCD_HOST SPI2_HOST

#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 0
#endif

#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

// #define EXAMPLE_PIN_NUM_SCLK           4
// #define EXAMPLE_PIN_NUM_MOSI           5
// #define EXAMPLE_PIN_NUM_MISO           -1
// #define EXAMPLE_PIN_NUM_LCD_DC         7
// #define EXAMPLE_PIN_NUM_LCD_RST        6
// #define EXAMPLE_PIN_NUM_LCD_CS         15
/**
 * LEDK =GPIO20
LEDA = GPIO42
REST = GPIO45
D3   = GPIO12
D2   = GPIO11
D1   = GPIO13
D0   = GPIO46
CS   = GPIO14
SCL  = GPIO18


TP_SCL    = GPIO38
TP_SDA    = GPIO19
TP_RESET  = GPIO17
TP_INT    = GPIO8
 */
#define EXAMPLE_PIN_NUM_TOUCH_SCL 38
#define EXAMPLE_PIN_NUM_TOUCH_SDA 19
#define EXAMPLE_PIN_NUM_TOUCH_RST 17
#define EXAMPLE_PIN_NUM_TOUCH_INT 8
#define EXAMPLE_PIN_NUM_LCD_PCLK 18
#define EXAMPLE_PIN_NUM_LCD_DATA0 46
#define EXAMPLE_PIN_NUM_LCD_DATA1 13
#define EXAMPLE_PIN_NUM_LCD_DATA2 11
#define EXAMPLE_PIN_NUM_LCD_DATA3 12
#define EXAMPLE_PIN_NUM_LCD_CS 14
#define EXAMPLE_PIN_NUM_LCD_RST 45
#define EXAMPLE_PIN_NUM_BK_LIGHT_LEDA 42 // LEDA 背光引脚，需要高电平点亮
#define EXAMPLE_PIN_NUM_BK_LIGHT_LEDK 20 // LEDK 背光引脚，需要低电平
#define EXAMPLE_PIN_NUM_LCD_MODE_SEL0 9  // GPIO9，QSPI 模式选择脚，需要低电平
#define EXAMPLE_PIN_NUM_LCD_MODE_SEL1 10 // GPIO10，QSPI 模式选择脚，需要高电平
#define EXAMPLE_LCD_MODE_SEL0_LEVEL 0
#define EXAMPLE_LCD_MODE_SEL1_LEVEL 1

// MAX98357A 功放引脚定义（I2S TX）
#define I2S_TX_DIN_PIN 7   // DIN - 数据输入
#define I2S_TX_BCLK_PIN 15 // BCLK - 位时钟
#define I2S_TX_LRCK_PIN 16 // LRCLK - 左右声道时钟

// ZTS6672 麦克风引脚定义（I2S RX）
#define I2S_RX_SCK_PIN 5 // SCK (BCLK) - 时钟
#define I2S_RX_SD_PIN 6  // SD (DATA) - 数据
#define I2S_RX_WS_PIN 4  // WS (LRCK) - 字选择

// 按钮引脚定义
#define BTN_WAKE_PIN 0      // 唤醒/录音按钮
#define BTN_VOL_UP_PIN 40   // 音量+/播放录音按钮
#define BTN_VOL_DOWN_PIN 39 // 音量-按钮

// 音频参数
#define I2S_SAMPLE_RATE 16000                                          // 采样率（录音用16kHz节省内存）
#define I2S_SAMPLE_BITS 16                                             // 播放位数
#define I2S_RX_SAMPLE_BITS I2S_DATA_BIT_WIDTH_32BIT                    // ZTS6672 需要 32 BCLK/slot，24-bit 数据左对齐在高位
#define I2S_RX_SLOT_MODE I2S_SLOT_MODE_STEREO                          // 用立体声读两个声道，自动判断 SELECT 引脚
#define RECORD_TIME_SECONDS 3                                          // 最大录音时长（秒）
#define RECORD_BUFFER_SIZE (I2S_SAMPLE_RATE * RECORD_TIME_SECONDS * 2) // 回放缓冲区使用立体声

// The pixel number in horizontal and vertical
#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#define EXAMPLE_LCD_H_RES 240
#define EXAMPLE_LCD_V_RES 320
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#define EXAMPLE_LCD_H_RES 240
#define EXAMPLE_LCD_V_RES 240
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916
#define EXAMPLE_LCD_H_RES 360
#define EXAMPLE_LCD_V_RES 360
#endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8
#define EXAMPLE_LCD_BIT_PER_PIXEL 16 // RGB565 color format, 16 bits per pixel

#define EXAMPLE_LVGL_DRAW_BUF_LINES 20 // number of display lines in each draw buffer
#define EXAMPLE_LVGL_TICK_PERIOD_MS 2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define EXAMPLE_LVGL_TASK_STACK_SIZE (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY 2

#define EXAMPLE_TOUCH_I2C_NUM 0
#define EXAMPLE_TOUCH_I2C_CLK_HZ 400000

static void lcd_fill_red(esp_lcd_panel_handle_t panel_handle, int color)
{
    // 使用分块绘制，避免大内存分配
    const int lines_per_chunk = 20; // 每次绘制20行
    uint16_t *line_buf = heap_caps_malloc(EXAMPLE_LCD_H_RES * lines_per_chunk * sizeof(uint16_t), MALLOC_CAP_DMA);

    if (line_buf == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for LCD buffer");
        return;
    }

    // 填充缓冲区
    for (int i = 0; i < EXAMPLE_LCD_H_RES * lines_per_chunk; i++)
    {
        line_buf[i] = color;
    }

    // 分块绘制屏幕
    for (int y = 0; y < EXAMPLE_LCD_V_RES; y += lines_per_chunk)
    {
        int current_lines = (y + lines_per_chunk > EXAMPLE_LCD_V_RES) ? (EXAMPLE_LCD_V_RES - y) : lines_per_chunk;

        esp_lcd_panel_draw_bitmap(panel_handle,
                                  0, y,
                                  EXAMPLE_LCD_H_RES,
                                  y + current_lines,
                                  line_buf);
    }

    heap_caps_free(line_buf);
}

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED && CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_CST816S
static void example_touch_callback(esp_lcd_touch_handle_t tp)
{
    BaseType_t task_woken = pdFALSE;

    if (touch_mux != NULL)
    {
        xSemaphoreGiveFromISR(touch_mux, &task_woken);
    }

    if (task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

static void example_touch_task(void *arg)
{
    uint16_t last_x = UINT16_MAX;
    uint16_t last_y = UINT16_MAX;

    while (1)
    {
        if (xSemaphoreTake(touch_mux, portMAX_DELAY) == pdTRUE)
        {
            esp_lcd_touch_point_data_t points[1] = {0};
            uint8_t touch_count = 0;

            esp_err_t ret = esp_lcd_touch_read_data(touch_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "Touch read failed: %s", esp_err_to_name(ret));
                continue;
            }

            ret = esp_lcd_touch_get_data(touch_handle, points, &touch_count, 1);
            if (ret == ESP_OK && touch_count > 0)
            {
                if (points[0].x < EXAMPLE_LCD_H_RES && points[0].y < EXAMPLE_LCD_V_RES)
                {
                    if (points[0].x != last_x || points[0].y != last_y)
                    {
                        ESP_LOGI(TAG, "Touch: x=%u, y=%u", points[0].x, points[0].y);
                        last_x = points[0].x;
                        last_y = points[0].y;
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Invalid touch data: x=%u, y=%u", points[0].x, points[0].y);
                }
            }
        }
    }
}

static void init_touch_cst816s(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_lcd_panel_io_handle_t touch_io_handle = NULL;

    ESP_LOGI(TAG, "Initialize CST816S touch over I2C");

    touch_mux = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(touch_mux != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, &i2c_bus));

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    touch_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_config, &touch_io_handle));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .interrupt_callback = example_touch_callback,
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(touch_io_handle, &touch_config, &touch_handle));
    ESP_LOGI(TAG, "CST816S touch initialized: SCL=GPIO%d SDA=GPIO%d RST=GPIO%d INT=GPIO%d",
             EXAMPLE_PIN_NUM_TOUCH_SCL,
             EXAMPLE_PIN_NUM_TOUCH_SDA,
             EXAMPLE_PIN_NUM_TOUCH_RST,
             EXAMPLE_PIN_NUM_TOUCH_INT);

    xTaskCreate(example_touch_task, "touch_task", 4096, NULL, 5, NULL);
}
#endif

// 初始化 I2S TX 用于 MAX98357A（播放）
static void init_i2s_tx(void)
{
    ESP_LOGI(TAG, "Initializing I2S TX for MAX98357A");

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_SAMPLE_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_TX_BCLK_PIN,
            .ws = I2S_TX_LRCK_PIN,
            .dout = I2S_TX_DIN_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    ESP_LOGI(TAG, "I2S TX initialized successfully");
}

// 初始化 I2S RX 用于 ZTS6672（录音）
static void init_i2s_rx(void)
{
    ESP_LOGI(TAG, "Initializing I2S RX for ZTS6672 (ICS-43434 compatible)");
    ESP_LOGW(TAG, "ZTS6672 requires: Philips format, 24-bit data, SELECT pin must be GND or VDD!");

    // 检查数据线空闲电平
    gpio_reset_pin(I2S_RX_SD_PIN);
    gpio_set_direction(I2S_RX_SD_PIN, GPIO_MODE_INPUT);
    gpio_reset_pin(I2S_RX_SCK_PIN);
    gpio_set_direction(I2S_RX_SCK_PIN, GPIO_MODE_INPUT);
    gpio_reset_pin(I2S_RX_WS_PIN);
    gpio_set_direction(I2S_RX_WS_PIN, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "GPIO idle levels: DIN(GPIO%d)=%d  BCLK(GPIO%d)=%d  WS(GPIO%d)=%d",
             I2S_RX_SD_PIN, gpio_get_level(I2S_RX_SD_PIN),
             I2S_RX_SCK_PIN, gpio_get_level(I2S_RX_SCK_PIN),
             I2S_RX_WS_PIN, gpio_get_level(I2S_RX_WS_PIN));

    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle));

    // ZTS6672/ICS-43434: Philips 格式, 24-bit, 立体声读取两个声道
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE);
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_RX_SAMPLE_BITS, I2S_RX_SLOT_MODE),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_RX_SCK_PIN,
            .ws = I2S_RX_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_RX_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    // 验证 GPIO 配置
    ESP_LOGI(TAG, "I2S RX: Philips / 24-bit / stereo / %d Hz / MCLK x384", I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "  BCLK=GPIO%d  WS=GPIO%d  DIN=GPIO%d", I2S_RX_SCK_PIN, I2S_RX_WS_PIN, I2S_RX_SD_PIN);

    // 硬件诊断：读一帧立体声数据，判断左右声道状态
    {
        int32_t diag_buf[64] = {0};
        size_t diag_bytes = 0;
        // 丢弃前几帧让麦克风稳定
        for (int w = 0; w < 5; w++)
        {
            i2s_channel_read(rx_handle, diag_buf, sizeof(diag_buf), &diag_bytes, 200);
        }
        i2s_channel_read(rx_handle, diag_buf, sizeof(diag_buf), &diag_bytes, 200);
        // 立体声: 偶数=左声道, 奇数=右声道
        ESP_LOGI(TAG, "Stereo diag: L[0]=0x%08lX R[0]=0x%08lX L[1]=0x%08lX R[1]=0x%08lX",
                 (unsigned long)(uint32_t)diag_buf[0], (unsigned long)(uint32_t)diag_buf[1],
                 (unsigned long)(uint32_t)diag_buf[2], (unsigned long)(uint32_t)diag_buf[3]);
        int l_vary = 0, r_vary = 0, l_nonzero = 0, r_nonzero = 0;
        for (int k = 0; k < 32; k++)
        {
            if (diag_buf[k * 2] != diag_buf[0])
                l_vary++;
            if (diag_buf[k * 2 + 1] != diag_buf[1])
                r_vary++;
            if (diag_buf[k * 2] != 0 && (uint32_t)diag_buf[k * 2] != 0xFFFFFFFF)
                l_nonzero++;
            if (diag_buf[k * 2 + 1] != 0 && (uint32_t)diag_buf[k * 2 + 1] != 0xFFFFFFFF)
                r_nonzero++;
        }
        ESP_LOGI(TAG, "Stereo diag: L_vary=%d L_valid=%d R_vary=%d R_valid=%d", l_vary, l_nonzero, r_vary, r_nonzero);
        if (l_vary == 0 && r_vary == 0)
        {
            ESP_LOGE(TAG, "HARDWARE FAULT: Both channels constant! DATA line stuck or mic not working.");
            ESP_LOGE(TAG, "  -> Check: SELECT/LR pin connected? VDD=3.3V? GND shared? DATA soldered?");
        }
        else if (l_vary > 0 && r_vary == 0)
        {
            ESP_LOGI(TAG, "Audio on LEFT channel (SELECT=GND)");
        }
        else if (l_vary == 0 && r_vary > 0)
        {
            ESP_LOGI(TAG, "Audio on RIGHT channel (SELECT=VDD)");
        }
        else
        {
            ESP_LOGI(TAG, "Both channels varying - unusual for single mic");
        }
    }

    // 预热麦克风 - 读取并丢弃初始数据
    ESP_LOGI(TAG, "Warming up ZTS6672...");
    int32_t *warmup_buffer = malloc(1024 * sizeof(int32_t));
    if (warmup_buffer)
    {
        size_t bytes_read = 0;
        int non_zero_count = 0;
        for (int i = 0; i < 10; i++)
        {
            esp_err_t ret = i2s_channel_read(rx_handle, warmup_buffer, 1024 * sizeof(int32_t), &bytes_read, 200);
            if (ret == ESP_OK)
            {
                // 检查是否有非零数据
                size_t sample_count = bytes_read / sizeof(int32_t);
                size_t inspect_count = sample_count < 100 ? sample_count : 100;
                for (size_t j = 0; j < inspect_count; j++)
                {
                    if (zts6672_raw_to_pcm16(warmup_buffer[j]) != 0)
                    {
                        non_zero_count++;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (non_zero_count > 0)
        {
            ESP_LOGI(TAG, "ZTS6672 warmup complete - detected %d non-zero samples", non_zero_count);
        }
        else
        {
            ESP_LOGW(TAG, "ZTS6672 warmup complete - WARNING: All converted samples are ZERO!");
            ESP_LOGW(TAG, "Possible issues: 1) SELECT pin floating 2) DATA not soldered 3) VDD/GND unstable");
        }

        free(warmup_buffer);
    }

    // 分配录音缓冲区 - 先尝试普通heap，失败则尝试PSRAM
    size_t buffer_size_bytes = RECORD_BUFFER_SIZE * sizeof(int16_t);
    ESP_LOGI(TAG, "Attempting to allocate %d bytes for recording buffer", buffer_size_bytes);

    record_buffer = (int16_t *)malloc(buffer_size_bytes);
    if (record_buffer == NULL)
    {
        ESP_LOGW(TAG, "Failed to allocate from internal RAM, trying PSRAM");
        record_buffer = (int16_t *)heap_caps_malloc(buffer_size_bytes, MALLOC_CAP_SPIRAM);
    }

    if (record_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate recording buffer! Recording disabled.");
    }
    else
    {
        ESP_LOGI(TAG, "Recording buffer allocated successfully: %d bytes (%.1f seconds max)",
                 buffer_size_bytes, (float)RECORD_TIME_SECONDS);
    }

    ESP_LOGI(TAG, "I2S RX initialized successfully");
}

// 生成测试音频数据（1kHz正弦波）
static void play_test_tone(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "Playing %ld ms test tone (1kHz)", duration_ms);

    const int samples_per_cycle = I2S_SAMPLE_RATE / 1000; // 1kHz音调
    const int total_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    const int buffer_size = 1024;
    int16_t *audio_buffer = malloc(buffer_size * 2 * sizeof(int16_t)); // 立体声

    if (audio_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }

    size_t bytes_written = 0;
    int sample_index = 0;

    while (sample_index < total_samples)
    {
        int samples_to_write = (total_samples - sample_index) > buffer_size ? buffer_size : (total_samples - sample_index);

        // 生成正弦波数据
        for (int i = 0; i < samples_to_write; i++)
        {
            float angle = 2.0f * M_PI * ((sample_index + i) % samples_per_cycle) / samples_per_cycle;
            int16_t sample = (int16_t)(sin(angle) * 8000); // 音量约25%
            audio_buffer[i * 2] = sample;                  // 左声道
            audio_buffer[i * 2 + 1] = sample;              // 右声道
        }

        // 发送数据到I2S
        i2s_channel_write(tx_handle, audio_buffer, samples_to_write * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        sample_index += samples_to_write;
    }

    // 播放静音片段，确保声音停止
    memset(audio_buffer, 0, buffer_size * 2 * sizeof(int16_t));
    i2s_channel_write(tx_handle, audio_buffer, 512 * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);

    free(audio_buffer);
}

// 播放摆钟"滴"声（短促的咔嗒声，高音）
static void play_tick_sound(void)
{
    const int duration_ms = 30; // 30毫秒非常短促
    const int frequency = 1400; // 1400Hz清脆高音
    const int samples_per_cycle = I2S_SAMPLE_RATE / frequency;
    const int total_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    const int buffer_size = 1024;
    int16_t *audio_buffer = malloc(buffer_size * 2 * sizeof(int16_t));

    if (audio_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }

    size_t bytes_written = 0;
    int sample_index = 0;

    while (sample_index < total_samples)
    {
        int samples_to_write = (total_samples - sample_index) > buffer_size ? buffer_size : (total_samples - sample_index);

        for (int i = 0; i < samples_to_write; i++)
        {
            float angle = 2.0f * M_PI * ((sample_index + i) % samples_per_cycle) / samples_per_cycle;
            float progress = (float)(sample_index + i) / total_samples;

            // 快速起始，快速衰减的包络，模拟机械敲击
            float envelope;
            if (progress < 0.1f)
            {
                envelope = progress / 0.1f; // 快速起始
            }
            else
            {
                envelope = (1.0f - progress) / 0.9f; // 快速衰减
            }

            int16_t sample = (int16_t)(sin(angle) * 15000 * envelope); // 更高音量
            audio_buffer[i * 2] = sample;
            audio_buffer[i * 2 + 1] = sample;
        }

        i2s_channel_write(tx_handle, audio_buffer, samples_to_write * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        sample_index += samples_to_write;
    }

    // 发送静音，确保停止
    memset(audio_buffer, 0, 512 * 2 * sizeof(int16_t));
    i2s_channel_write(tx_handle, audio_buffer, 512 * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);

    free(audio_buffer);
}

// 播放摆钟"答"声（短促的咔嗒声，低音）
static void play_tock_sound(void)
{
    const int duration_ms = 30; // 30毫秒非常短促
    const int frequency = 1100; // 1100Hz稍低音
    const int samples_per_cycle = I2S_SAMPLE_RATE / frequency;
    const int total_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    const int buffer_size = 1024;
    int16_t *audio_buffer = malloc(buffer_size * 2 * sizeof(int16_t));

    if (audio_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }

    size_t bytes_written = 0;
    int sample_index = 0;

    while (sample_index < total_samples)
    {
        int samples_to_write = (total_samples - sample_index) > buffer_size ? buffer_size : (total_samples - sample_index);

        for (int i = 0; i < samples_to_write; i++)
        {
            float angle = 2.0f * M_PI * ((sample_index + i) % samples_per_cycle) / samples_per_cycle;
            float progress = (float)(sample_index + i) / total_samples;

            // 快速起始，快速衰减的包络，模拟机械敲击
            float envelope;
            if (progress < 0.1f)
            {
                envelope = progress / 0.1f; // 快速起始
            }
            else
            {
                envelope = (1.0f - progress) / 0.9f; // 快速衰减
            }

            int16_t sample = (int16_t)(sin(angle) * 15000 * envelope); // 更高音量
            audio_buffer[i * 2] = sample;
            audio_buffer[i * 2 + 1] = sample;
        }

        i2s_channel_write(tx_handle, audio_buffer, samples_to_write * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        sample_index += samples_to_write;
    }

    // 发送静音，确保停止
    memset(audio_buffer, 0, 512 * 2 * sizeof(int16_t));
    i2s_channel_write(tx_handle, audio_buffer, 512 * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);

    free(audio_buffer);
}

// 开始录音
static void start_recording(void *arg)
{
    ESP_LOGI(TAG, "Recording task started");

    if (record_buffer == NULL)
    {
        ESP_LOGE(TAG, "Cannot record: buffer not allocated!");
        vTaskDelete(NULL);
        return;
    }

    if (is_recording)
    {
        ESP_LOGW(TAG, "Already recording, task exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting recording... (Press and hold to record, release to stop)");
    is_recording = true;
    recorded_samples = 0;

    // 记录开始时间
    int64_t start_time = esp_timer_get_time();
    int64_t last_print_time = start_time;
    bool first_read = true; // 用于调试第一次读取

    size_t bytes_read = 0;
    int32_t *temp_buffer = malloc(512 * sizeof(int32_t));

    if (temp_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate temp buffer");
        is_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // 录音循环
    while (is_recording && recorded_samples < RECORD_BUFFER_SIZE)
    {
        // 从麦克风读取数据
        esp_err_t ret = i2s_channel_read(rx_handle, temp_buffer, 512 * sizeof(int32_t), &bytes_read, 100);

        if (ret == ESP_OK && bytes_read > 0)
        {
            size_t samples_read = bytes_read / sizeof(int32_t);

            // 调试：打印第一次读取的样本值（立体声：偶数=L，奇数=R）
            if (first_read && samples_read >= 20)
            {
                ESP_LOGI(TAG, "First read: %d stereo samples (%d frames)", samples_read, samples_read / 2);
                ESP_LOGI(TAG, "L-ch raw: 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX",
                         (unsigned long)(uint32_t)temp_buffer[0], (unsigned long)(uint32_t)temp_buffer[2],
                         (unsigned long)(uint32_t)temp_buffer[4], (unsigned long)(uint32_t)temp_buffer[6],
                         (unsigned long)(uint32_t)temp_buffer[8]);
                ESP_LOGI(TAG, "R-ch raw: 0x%08lX 0x%08lX 0x%08lX 0x%08lX 0x%08lX",
                         (unsigned long)(uint32_t)temp_buffer[1], (unsigned long)(uint32_t)temp_buffer[3],
                         (unsigned long)(uint32_t)temp_buffer[5], (unsigned long)(uint32_t)temp_buffer[7],
                         (unsigned long)(uint32_t)temp_buffer[9]);
                // 判断哪个声道有变化
                int l_vary = 0, r_vary = 0;
                for (size_t j = 2; j < samples_read && j < 40; j += 2)
                {
                    if (temp_buffer[j] != temp_buffer[0])
                        l_vary++;
                    if (temp_buffer[j + 1] != temp_buffer[1])
                        r_vary++;
                }
                ESP_LOGI(TAG, "L-vary=%d R-vary=%d (of %d)", l_vary, r_vary, (int)(samples_read < 40 ? samples_read / 2 : 20) - 1);
                if (l_vary == 0 && r_vary == 0)
                {
                    ESP_LOGE(TAG, "BOTH CHANNELS STUCK! Check: SELECT pin, DATA solder, VDD/GND");
                }
                first_read = false;
            }

            // 立体声数据：提取右声道（奇数索引，SELECT=VDD），复制为双声道回放
            for (size_t i = 1; i < samples_read && recorded_samples < RECORD_BUFFER_SIZE - 1; i += 2)
            {
                int16_t left_sample = zts6672_raw_to_pcm16(temp_buffer[i]);
                record_buffer[recorded_samples++] = left_sample;
                record_buffer[recorded_samples++] = left_sample;
            }
        }

        // 每秒打印一次录音进度
        int64_t current_time = esp_timer_get_time();
        if (current_time - last_print_time >= 1000000)
        { // 1秒
            float duration_sec = (current_time - start_time) / 1000000.0f;
            ESP_LOGI(TAG, "Recording... %.1f seconds", duration_sec);
            last_print_time = current_time;
        }
    }

    // 计算录音总时长
    int64_t end_time = esp_timer_get_time();
    float total_duration = (end_time - start_time) / 1000000.0f;

    free(temp_buffer);
    is_recording = false;
    has_recording = (recorded_samples > 0);

    ESP_LOGI(TAG, "Recording finished! Duration: %.2f seconds, Samples: %d",
             total_duration, recorded_samples / 2);

    vTaskDelete(NULL); // 删除任务
}

// 播放录音
static void play_recording(void)
{
    ESP_LOGI(TAG, "Play recording requested. has_recording=%d, buffer=%p, samples=%d",
             has_recording, record_buffer, recorded_samples);

    if (!has_recording || record_buffer == NULL || recorded_samples == 0)
    {
        ESP_LOGW(TAG, "No recording to play! Please record first by pressing GPIO0");
        return;
    }

    // 调试：查看前几个样本的值
    ESP_LOGI(TAG, "First 10 samples: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
             record_buffer[0], record_buffer[1], record_buffer[2], record_buffer[3], record_buffer[4],
             record_buffer[5], record_buffer[6], record_buffer[7], record_buffer[8], record_buffer[9]);

    // 计算录音时长（recorded_samples已经是立体声样本总数）
    float duration = recorded_samples / 2.0f / (float)I2S_SAMPLE_RATE;
    ESP_LOGI(TAG, "Playing recording: %.2f seconds (%d stereo samples, %d source mono samples)",
             duration, recorded_samples, recorded_samples / 2);

    size_t bytes_written = 0;
    size_t offset = 0;
    const size_t chunk_size = 1024 * 2; // 立体声块大小

    while (offset < recorded_samples)
    {
        size_t samples_to_write = (recorded_samples - offset) > chunk_size ? chunk_size : (recorded_samples - offset);

        // 应用音量控制
        int16_t *temp_buffer = malloc(samples_to_write * sizeof(int16_t));
        if (temp_buffer)
        {
            for (size_t i = 0; i < samples_to_write; i++)
            {
                temp_buffer[i] = (int16_t)((record_buffer[offset + i] * audio_volume) / 100);
            }

            i2s_channel_write(tx_handle, temp_buffer, samples_to_write * sizeof(int16_t), &bytes_written, portMAX_DELAY);
            free(temp_buffer);
        }
        else
        {
            i2s_channel_write(tx_handle, &record_buffer[offset], samples_to_write * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }

        offset += samples_to_write;
    }

    // 发送足够的静音来清空 DMA 缓冲区，防止循环播放残留数据
    int16_t silence[1024] = {0};
    for (int i = 0; i < 4; i++)
    {
        i2s_channel_write(tx_handle, silence, sizeof(silence), &bytes_written, portMAX_DELAY);
    }

    // 停止 TX 通道，防止 DMA 循环播放缓冲区残留
    i2s_channel_disable(tx_handle);
    vTaskDelay(pdMS_TO_TICKS(50));
    i2s_channel_enable(tx_handle);

    ESP_LOGI(TAG, "Playback finished");
}

// 按钮处理任务
static void button_task(void *arg)
{
    bool last_wake_state = true;
    bool last_vol_up_state = true;
    bool last_vol_down_state = true;

    while (1)
    {
        // 读取按钮状态（低电平表示按下）
        bool wake_pressed = (gpio_get_level(BTN_WAKE_PIN) == 0);
        bool vol_up_pressed = (gpio_get_level(BTN_VOL_UP_PIN) == 0);
        bool vol_down_pressed = (gpio_get_level(BTN_VOL_DOWN_PIN) == 0);

        // 调试：检测GPIO39状态变化
        static bool last_vol_down_debug = true;
        if (vol_down_pressed != last_vol_down_debug)
        {
            ESP_LOGI(TAG, "GPIO39 state changed: %s (level=%d)",
                     vol_down_pressed ? "PRESSED" : "RELEASED",
                     gpio_get_level(BTN_VOL_DOWN_PIN));
            last_vol_down_debug = vol_down_pressed;
        }

        // 调试：检测GPIO40状态变化
        static bool last_vol_up_debug = true;
        if (vol_up_pressed != last_vol_up_debug)
        {
            ESP_LOGI(TAG, "GPIO40 state changed: %s (level=%d)",
                     vol_up_pressed ? "PRESSED" : "RELEASED",
                     gpio_get_level(BTN_VOL_UP_PIN));
            last_vol_up_debug = vol_up_pressed;
        }

        // 调试：检测GPIO0状态变化
        static bool last_wake_debug = true;
        if (wake_pressed != last_wake_debug)
        {
            ESP_LOGI(TAG, "GPIO0 state changed: %s (level=%d)",
                     wake_pressed ? "PRESSED" : "RELEASED",
                     gpio_get_level(BTN_WAKE_PIN));
            last_wake_debug = wake_pressed;
        }

        // GPIO0 唤醒/录音按钮 - 按下开始录音，释放停止
        if (wake_pressed && !last_wake_state)
        {
            // 只有在没有正在录音时才启动新的录音任务
            if (!is_recording)
            {
                ESP_LOGI(TAG, "Wake button pressed - Start recording");
                xTaskCreate(start_recording, "record_task", 8192, NULL, 5, NULL);
            }
            else
            {
                ESP_LOGW(TAG, "Already recording, ignoring button press");
            }
        }
        else if (!wake_pressed && last_wake_state)
        {
            if (is_recording)
            {
                ESP_LOGI(TAG, "Wake button released - Stop recording");
                is_recording = false;
            }
        }

        // GPIO40 音量+/播放录音按钮
        if (vol_up_pressed && !last_vol_up_state)
        {
            ESP_LOGI(TAG, "Vol+ button pressed - Play recording");
            play_recording();
        }

        // GPIO39 备用按钮（暂未使用）
        if (vol_down_pressed && !last_vol_down_state)
        {
            ESP_LOGI(TAG, "GPIO39 pressed");
        }
        else if (!vol_down_pressed && last_vol_down_state)
        {
            ESP_LOGI(TAG, "GPIO39 released");
        }

        // 定期打印状态（每5秒）
        static int status_counter = 0;
        if (++status_counter >= 100)
        { // 100 * 50ms = 5秒
            ESP_LOGI(TAG, "Status: GPIO39=%s, TickTock=%s, Recording=%s",
                     vol_down_pressed ? "PRESSED" : "RELEASED",
                     tick_tock_enabled ? "ON" : "OFF",
                     is_recording ? "YES" : "NO");
            status_counter = 0;
        }

        last_wake_state = wake_pressed;
        last_vol_up_state = vol_up_pressed;
        last_vol_down_state = vol_down_pressed;

        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms防抖
    }
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
    {0x29, (uint8_t[]){0x00}, 1, 0}};

static void print_board_info(void)
{
    // 芯片信息
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char *chip_model = "Unknown";
    switch (chip_info.model)
    {
    case CHIP_ESP32:
        chip_model = "ESP32";
        break;
    case CHIP_ESP32S2:
        chip_model = "ESP32-S2";
        break;
    case CHIP_ESP32S3:
        chip_model = "ESP32-S3";
        break;
    case CHIP_ESP32C3:
        chip_model = "ESP32-C3";
        break;
    case CHIP_ESP32H2:
        chip_model = "ESP32-H2";
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "========== 开发板信息 ==========");
    ESP_LOGI(TAG, "芯片型号: %s (Rev %d.%d)", chip_model, chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "CPU 核心数: %d", chip_info.cores);
    ESP_LOGI(TAG, "功能: WiFi%s%s",
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    ESP_LOGI(TAG, "PSRAM: %s", (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "内置" : "外置/无");

    // Flash 信息
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK)
    {
        ESP_LOGI(TAG, "Flash 大小: %lu MB", flash_size / (1024 * 1024));
    }

    // 内部 RAM
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_used = internal_total - internal_free;
    float internal_pct = internal_total > 0 ? (float)internal_used / internal_total * 100.0f : 0;
    ESP_LOGI(TAG, "内部 RAM: 总计 %u KB, 已用 %u KB, 空闲 %u KB (占用 %.1f%%)",
             internal_total / 1024, internal_used / 1024, internal_free / 1024, internal_pct);

    // PSRAM
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0)
    {
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_used = psram_total - psram_free;
        float psram_pct = (float)psram_used / psram_total * 100.0f;
        ESP_LOGI(TAG, "PSRAM:    总计 %u KB, 已用 %u KB, 空闲 %u KB (占用 %.1f%%)",
                 psram_total / 1024, psram_used / 1024, psram_free / 1024, psram_pct);
    }
    else
    {
        ESP_LOGI(TAG, "PSRAM:    未检测到");
    }

    // 所有可用堆内存汇总
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_used = heap_total - heap_free;
    float heap_pct = heap_total > 0 ? (float)heap_used / heap_total * 100.0f : 0;
    ESP_LOGI(TAG, "总堆内存: 总计 %u KB, 已用 %u KB, 空闲 %u KB (占用 %.1f%%)",
             heap_total / 1024, heap_used / 1024, heap_free / 1024, heap_pct);

    // 分区表中的应用分区占用
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running)
    {
        ESP_LOGI(TAG, "应用分区: %s, 大小 %lu KB", running->label, running->size / 1024);
    }
    ESP_LOGI(TAG, "================================");
}

void app_main(void)
{
    print_board_info();

    ESP_LOGI(TAG, "Configure LCD QSPI mode GPIOs");
    gpio_config_t lcd_mode_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_LCD_MODE_SEL0) | (1ULL << EXAMPLE_PIN_NUM_LCD_MODE_SEL1)};
    ESP_ERROR_CHECK(gpio_config(&lcd_mode_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_MODE_SEL0, EXAMPLE_LCD_MODE_SEL0_LEVEL));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_MODE_SEL1, EXAMPLE_LCD_MODE_SEL1_LEVEL));
    ESP_LOGI(TAG, "LCD QSPI mode set: GPIO%d=%d, GPIO%d=%d",
             EXAMPLE_PIN_NUM_LCD_MODE_SEL0, EXAMPLE_LCD_MODE_SEL0_LEVEL,
             EXAMPLE_PIN_NUM_LCD_MODE_SEL1, EXAMPLE_LCD_MODE_SEL1_LEVEL);

    // 配置背光引脚
    ESP_LOGI(TAG, "Configure backlight GPIO");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_BK_LIGHT_LEDA) | (1ULL << EXAMPLE_PIN_NUM_BK_LIGHT_LEDK)};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    // LEDA 设置为高电平，LEDK 设置为低电平，点亮背光
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT_LEDA, 1));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT_LEDK, 0));
    ESP_LOGI(TAG, "Backlight turned ON");

    // 配置按钮引脚（输入，上拉）
    ESP_LOGI(TAG, "Configure button GPIOs");
    gpio_config_t btn_gpio_config = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << BTN_WAKE_PIN) | (1ULL << BTN_VOL_UP_PIN) | (1ULL << BTN_VOL_DOWN_PIN)};
    ESP_ERROR_CHECK(gpio_config(&btn_gpio_config));
    ESP_LOGI(TAG, "Buttons configured: Wake(GPIO%d), Vol+(GPIO%d), Vol-(GPIO%d)",
             BTN_WAKE_PIN, BTN_VOL_UP_PIN, BTN_VOL_DOWN_PIN);

    // 初始化 I2S TX（MAX98357A 功放）
    init_i2s_tx();

    // 初始化 I2S RX（ZTS6672 麦克风）
    init_i2s_rx();

    // 启动按钮处理任务
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button task started");
    // 读取初始GPIO状态，用于调试
    vTaskDelay(pdMS_TO_TICKS(100)); // 稍等一下让按钮任务启动
    int gpio0_level = gpio_get_level(BTN_WAKE_PIN);
    int gpio39_level = gpio_get_level(BTN_VOL_DOWN_PIN);
    int gpio40_level = gpio_get_level(BTN_VOL_UP_PIN);
    ESP_LOGI(TAG, "Initial GPIO states: GPIO0=%d, GPIO39=%d, GPIO40=%d",
             gpio0_level, gpio39_level, gpio40_level);
    ESP_LOGI(TAG, "Expected: All should be 1 (pulled high) when buttons are not pressed");
    // 不播放启动提示音，只有按钮按下时才播放声音

    // 步骤二：创建 QSPI 接口设备
    ESP_LOGI(TAG, "Initialize QSPI bus");
    const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                                  EXAMPLE_PIN_NUM_LCD_DATA0,
                                                                  EXAMPLE_PIN_NUM_LCD_DATA1,
                                                                  EXAMPLE_PIN_NUM_LCD_DATA2,
                                                                  EXAMPLE_PIN_NUM_LCD_DATA3,
                                                                  EXAMPLE_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS, example_on_color_trans_dome, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // 步骤三：创建 LCD 驱动设备

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_LOGI(TAG, "Install ST77916 panel driver");

    st77916_vendor_config_t vendor_config = {
        // 用于替换驱动组件中的初始化命令及参数
        .init_cmds = lcd_init_cmds, // Uncomment these line if use custom initialization commands
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,   // 连接 LCD 复位信号的 IO 编号，可以设为 `-1` 表示不使用
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,  // 像素色彩的元素顺序（RGB/BGR），
                                                     //  一般通过命令 `LCD_CMD_MADCTL（36h）` 控制
        .bits_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL, // 色彩格式的位数（RGB565：16，RGB666：18），
                                                     // 一般通过命令 `LCD_CMD_COLMOD（3Ah）` 控制
        .vendor_config = &vendor_config,             // 用于替换驱动组件中的初始化命令及参数
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle));

    /* 初始化 LCD 设备 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));              // 若设备连接了复位引脚，则通过该引脚进行硬件复位，否则通过命令 LCD_CMD_SWRESET(01h) 进行软件复位。
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));               // 通过发送一系列的命令及参数来初始化 LCD 设备。
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true)); // 这些函数可以根据需要使用
    // ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    // ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    // ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0)); //通过软件修改画图时的起始和终止坐标，从而实现画图的偏移。
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED && CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_CST816S
    init_touch_cst816s();
#endif

    // 初始化屏幕为红色
    ESP_LOGI(TAG, "Filling screen with initial color (RED)");
    lcd_fill_red(panel_handle, 0xF800); // 红色

    // 摆钟效果的计数器
    int clock_tick = 0;
    int color_index = 0;
    uint16_t colors[] = {0xF800, 0x07E0, 0x001F}; // 红、绿、蓝

    while (1)
    {
        if (0)
        {
            // 滴答声已禁用
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else
        {
            // 未启用嘀嗒声时，只做轻量级延迟
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
