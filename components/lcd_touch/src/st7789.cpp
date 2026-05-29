/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ST7789V SPI LCD Driver  (240 × 240 RGB565)
 *
 * Protocol:
 *   SPI mode 0 (CPOL=0, CPHA=0), MSB first.
 *   DC pin: Low  = command byte,
 *           High = pixel data / parameter bytes.
 *   Write-only: the ST7789 has no MISO (DOUT pin is unused).
 *
 * Init sequence (typical for 240×240 1.3" / 1.54" modules):
 *   SWRESET → delay → SLPOUT → delay → COLMOD=0x55 → MADCTL → gamma
 *   → porch/VCOMS/VDV set → DISPON → delay → backlight ON
 */

#include "sdkconfig.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "lcd_touch/lcd_touch.h"

/* ====================================================================
 * Local constants
 * ==================================================================== */

static const char* TAG = "st7789";

/* ---- ST7789 command set (subset) ---- */

#define ST7789_NOP      0x00
#define ST7789_SWRESET  0x01
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11
#define ST7789_INVOFF   0x20
#define ST7789_INVON    0x21
#define ST7789_DISPOFF  0x28
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A
#define ST7789_RASET    0x2B
#define ST7789_RAMWR    0x2C
#define ST7789_RAMRD    0x2E
#define ST7789_MADCTL   0x36
#define ST7789_COLMOD   0x3A
#define ST7789_PORCTRL  0xB2
#define ST7789_GCTRL    0xB7
#define ST7789_VCOMS    0xBB
#define ST7789_LCMCTRL  0xC0
#define ST7789_VDVVRHEN 0xC2
#define ST7789_VRHS     0xC3
#define ST7789_VDVSET   0xC4
#define ST7789_FRCTRL2  0xC6
#define ST7789_PWCTRL1  0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

/* ---- MADCTL flag bits ---- */
#define MADCTL_MY  0x80   /* Row address order (Y mirror) */
#define MADCTL_MX  0x40   /* Column address order (X mirror) */
#define MADCTL_MV  0x20   /* Row / Column exchange (landscape) */
#define MADCTL_ML  0x10   /* Vertical refresh order */
#define MADCTL_RGB 0x00   /* RGB colour filter order */
#define MADCTL_BGR 0x08   /* BGR colour filter order */

/* Use DMA for large pixel transfers (hardware FIFO is only 64 bytes).
 * SPI_DMA_CH_AUTO lets the driver pick the best DMA channel. */
#define LCD_SPI_DMA_CHAN  SPI_DMA_CH_AUTO
#define LCD_SPI_QUEUE_SIZE  7

/* ====================================================================
 * Local state
 * ==================================================================== */

static spi_device_handle_t s_spi_dev = NULL;

/* ====================================================================
 * Low-level SPI helpers
 * ==================================================================== */

/**
 * Write a single command byte to the display (DC = low).
 */
static void lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(PIN_LCD_DC, 0);          /* command mode */
    spi_device_transmit(s_spi_dev, &t);
}

/**
 * Write one or more parameter / data bytes (DC = high).
 */
static void lcd_write_data(const uint8_t* data, uint32_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(PIN_LCD_DC, 1);          /* data mode */
    spi_device_transmit(s_spi_dev, &t);
}

/**
 * Convenience: send a command followed by a single parameter byte.
 */
static void lcd_write_cmd_param(uint8_t cmd, uint8_t param)
{
    lcd_write_cmd(cmd);
    lcd_write_data(&param, 1);
}

/**
 * Convenience: send a command followed by an array of parameter bytes.
 */
static void lcd_write_cmd_params(uint8_t cmd, const uint8_t* params,
                                  uint32_t len)
{
    lcd_write_cmd(cmd);
    if (len > 0) {
        lcd_write_data(params, len);
    }
}

/* ====================================================================
 * Public API
 * ==================================================================== */

esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "ST7789 init start (240 × 240)");

    /* ---- 1. Configure control GPIOs ---- */
    uint64_t pin_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_CS);
#if PIN_LCD_RST >= 0
    pin_mask |= (1ULL << PIN_LCD_RST);
#endif
#if PIN_LCD_BL >= 0
    pin_mask |= (1ULL << PIN_LCD_BL);
#endif
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* ---- 2. Initialise SPI2_HOST (FSPI) ---- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,              /* ST7789 has no MISO */
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_FRAME_BYTES,
    };
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, LCD_SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,                /* SPI mode 0 */
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .spics_io_num   = PIN_LCD_CS,
        .flags          = SPI_DEVICE_HALFDUPLEX,
        .queue_size     = LCD_SPI_QUEUE_SIZE,
    };
    ret = spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        spi_bus_free(LCD_SPI_HOST);
        return ret;
    }

    /* ---- 3. Hardware reset sequence (if RST pin available) ---- */
#if PIN_LCD_RST >= 0
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif

    /* ---- 4. Initialisation command sequence ---- */
    lcd_write_cmd(ST7789_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));     /* longer delay if no hardware RST */

    /* Sleep out */
    lcd_write_cmd(ST7789_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Colour mode: 16-bit RGB565 (0x55 = 65k colours) */
    lcd_write_cmd_param(ST7789_COLMOD, 0x55);

    /* Memory access control: mirror X to match this panel's scan direction. */
    lcd_write_cmd_param(ST7789_MADCTL, MADCTL_BGR | MADCTL_MX);

    /* Porch timing (common values for 240×240) */
    {
        uint8_t porch[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
        lcd_write_cmd_params(ST7789_PORCTRL, porch, sizeof(porch));
    }

    /* Gate control */
    lcd_write_cmd_param(ST7789_GCTRL, 0x35);

    /* VCOMS setting */
    lcd_write_cmd_param(ST7789_VCOMS, 0x28);

    /* LCM control */
    lcd_write_cmd_param(ST7789_LCMCTRL, 0x2C);

    /* VDV and VRH command enable */
    lcd_write_cmd_param(ST7789_VDVVRHEN, 0x01);

    /* VRHS set */
    lcd_write_cmd_param(ST7789_VRHS, 0x0C);

    /* VDV set */
    lcd_write_cmd_param(ST7789_VDVSET, 0x20);

    /* Frame rate control (60 Hz) */
    lcd_write_cmd_param(ST7789_FRCTRL2, 0x0F);

    /* Power control 1 */
    lcd_write_cmd_param(ST7789_PWCTRL1, 0xA4);

    /* Positive gamma */
    {
        uint8_t gamma_pos[] = {
            0xD0, 0x08, 0x11, 0x08, 0x0C, 0x15, 0x39,
            0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2D,
        };
        lcd_write_cmd_params(ST7789_PVGAMCTRL, gamma_pos, sizeof(gamma_pos));
    }

    /* Negative gamma */
    {
        uint8_t gamma_neg[] = {
            0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39,
            0x44, 0x51, 0x0B, 0x16, 0x14, 0x2F, 0x31,
        };
        lcd_write_cmd_params(ST7789_NVGAMCTRL, gamma_neg, sizeof(gamma_neg));
    }

    /* Display on */
    lcd_write_cmd(ST7789_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* ---- 5. Backlight on (if BL pin available) ---- */
#if PIN_LCD_BL >= 0
    lcd_set_backlight(255);
#else
    ESP_LOGI(TAG, "No BL pin - backlight assumed always-on");
#endif

    /* ---- 6. Clear screen before upper layers draw UI content ---- */
    lcd_fill(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, COLOR_BLACK);

    ESP_LOGI(TAG, "ST7789 init done");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

void lcd_power(bool on)
{
    lcd_write_cmd(on ? ST7789_DISPON : ST7789_DISPOFF);
}

/* ------------------------------------------------------------------ */

void lcd_set_backlight(uint8_t brightness)
{
#ifdef CONFIG_LCD_TOUCH_BACKLIGHT_PWM
    /* Use LEDC PWM for smooth dimming */
    static bool pwm_inited = false;
    if (!pwm_inited) {
        ledc_timer_config_t timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num       = LEDC_TIMER_1,
            .freq_hz         = 5000,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer);

        ledc_channel_config_t ch = {
            .gpio_num   = PIN_LCD_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_1,
            .timer_sel  = LEDC_TIMER_1,
            .duty       = 0,
            .hpoint     = 0,
        };
        ledc_channel_config(&ch);
        pwm_inited = true;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
#elif PIN_LCD_BL >= 0
    /* Simple on/off (threshold at 50 %) */
    gpio_set_level(PIN_LCD_BL, brightness >= 128);
#else
    (void)brightness;
    /* No BL pin — backlight is always-on */
#endif
}

/* ------------------------------------------------------------------ */

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* Column address set */
    {
        uint8_t data[] = {
            (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        };
        lcd_write_cmd_params(ST7789_CASET, data, 4);
    }

    /* Row address set */
    {
        uint8_t data[] = {
            (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
            (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        };
        lcd_write_cmd_params(ST7789_RASET, data, 4);
    }

    lcd_write_cmd(ST7789_RAMWR);   /* ready for pixel data */
}

/* ------------------------------------------------------------------ */

void lcd_write_pixels(const uint16_t* colors, uint32_t len)
{
    /* ST7789 expects big-endian 16-bit pixel data.  ESP32 is little-endian,
     * so each uint16_t in memory has the MSB at byte offset 1.  We must
     * swap bytes before transmitting.  Process in small chunks using a
     * stack buffer to avoid modifying the caller's data. */

    gpio_set_level(PIN_LCD_DC, 1);   /* data mode */

    const uint32_t max_pixels_per_xfer = 512;
    uint32_t remaining = len;
    const uint16_t* src = colors;

    while (remaining > 0) {
        uint32_t chunk = (remaining > max_pixels_per_xfer)
                             ? max_pixels_per_xfer
                             : remaining;

        /* Byte-swap chunk into big-endian */
        uint16_t be_buf[512];
        for (uint32_t i = 0; i < chunk; i++) {
            be_buf[i] = (src[i] >> 8) | (src[i] << 8);
        }

        spi_transaction_t t = {
            .length    = chunk * 16,   /* total bits */
            .tx_buffer = be_buf,
        };
        spi_device_transmit(s_spi_dev, &t);

        src       += chunk;
        remaining -= chunk;
    }
}

/* ------------------------------------------------------------------ */

void lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
              uint16_t color)
{
    uint32_t pixels = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);

    lcd_set_window(x0, y0, x1, y1);

    /* ST7789 expects big-endian 16-bit pixel data.
     * ESP32 (little-endian) stores 0xF800 as bytes 0x00,0xF8.
     * Swap to 0xF8,0x00 so the display sees the MSB first. */
    uint16_t be_color = (color >> 8) | (color << 8);

    /* Write the same colour repeatedly using a small stack buffer */
    uint16_t buf[256];
    for (uint32_t i = 0; i < 256; i++) {
        buf[i] = be_color;
    }

    gpio_set_level(PIN_LCD_DC, 1);

    while (pixels > 0) {
        uint32_t chunk = (pixels > 256) ? 256 : pixels;
        spi_transaction_t t = {
            .length    = chunk * 16,
            .tx_buffer = buf,
        };
        spi_device_transmit(s_spi_dev, &t);
        pixels -= chunk;
    }
}

/* ------------------------------------------------------------------ */

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_set_window(x, y, x, y);
    gpio_set_level(PIN_LCD_DC, 1);
    /* Swap to big-endian for the display */
    uint16_t be_color = (color >> 8) | (color << 8);
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = &be_color,
    };
    spi_device_transmit(s_spi_dev, &t);
}

/* ------------------------------------------------------------------ */

void lcd_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* data)
{
    lcd_set_window(x, y, x + w - 1, y + h - 1);

    uint32_t pixels = (uint32_t)w * h;
    gpio_set_level(PIN_LCD_DC, 1);

    uint32_t remaining = pixels;
    const uint16_t* src = data;
    const uint32_t max_per_xfer = 512;

    while (remaining > 0) {
        uint32_t chunk = (remaining > max_per_xfer) ? max_per_xfer : remaining;

        /* Byte-swap chunk into big-endian for the display */
        uint16_t be_buf[512];
        for (uint32_t i = 0; i < chunk; i++) {
            be_buf[i] = (src[i] >> 8) | (src[i] << 8);
        }

        spi_transaction_t t = {
            .length    = chunk * 16,
            .tx_buffer = be_buf,
        };
        spi_device_transmit(s_spi_dev, &t);
        src       += chunk;
        remaining -= chunk;
    }
}

/* ------------------------------------------------------------------ */

void lcd_set_rotation(uint8_t rotation)
{
    uint8_t madctl = MADCTL_BGR;   /* base: BGR colour order */

    switch (rotation & 0x03) {
    case 0:                         /* portrait (default) */
        madctl |= MADCTL_MX;
        break;
    case 1:                         /* landscape */
        madctl |= MADCTL_MX | MADCTL_MV;
        break;
    case 2:                         /* inverted portrait */
        madctl |= MADCTL_MY;
        break;
    case 3:                         /* inverted landscape */
        madctl |= MADCTL_MV | MADCTL_MY;
        break;
    }

    lcd_write_cmd_param(ST7789_MADCTL, madctl);
}
