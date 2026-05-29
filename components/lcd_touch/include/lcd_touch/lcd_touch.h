/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO — LCD (ST7789) + Capacitive Touch (CST816D) Driver
 *
 * =============================================================================
 * Pin Definitions
 * =============================================================================
 *
 * Override any of these macros before including this header if your
 * hardware wiring differs from the defaults below.
 *
 * Example (in your source file, before the #include):
 *
 *     #define PIN_LCD_CS   GPIO_NUM_4
 *     #define PIN_LCD_DC   GPIO_NUM_5
 *     #define PIN_LCD_MOSI GPIO_NUM_6
 *     #define PIN_LCD_SCLK GPIO_NUM_7
 *     #include "lcd_touch/lcd_touch.h"
 *
 * ---------------------------------------------------------------------------
 * Default Pin Mapping (ESP32-S3 Free SPI + I2C_NUM_0)
 * ---------------------------------------------------------------------------
 *
 *  ST7789 — SPI LCD (240 × 240)
 *
 *    Signal    | GPIO          | ESP-IDF Function     | Description
 *    ----------+---------------+----------------------+------------------------
 *    LCD_CS    | GPIO_NUM_10   | spi_device           | SPI chip select (CS)
 *    LCD_DC    | GPIO_NUM_11   | gpio                 | Data (1) / Command (0)
 *    LCD_RST   | GPIO_NUM_12   | gpio                 | Hardware reset (active low)
 *    LCD_BL    | GPIO_NUM_13   | gpio / ledc          | Backlight enable / PWM
 *    LCD_MOSI  | GPIO_NUM_14   | spi_device (MOSI)    | SPI master-out slave-in
 *    LCD_SCLK  | GPIO_NUM_15   | spi_device (CLK)     | SPI clock
 *    (MISO)    | (unused)      | –                    | ST7789 has no MISO
 *
 *  CST816D — I2C Capacitive Touch
 *
 *    Signal     | GPIO          | ESP-IDF Function     | Description
 *    -----------+---------------+----------------------+------------------------
 *    TOUCH_SDA  | GPIO_NUM_16   | i2c (SDA)            | I2C data line
 *    TOUCH_SCL  | GPIO_NUM_17   | i2c (SCL)            | I2C clock line
 *    TOUCH_RST  | GPIO_NUM_18   | gpio                 | Touch reset (active low)
 *    TOUCH_INT  | GPIO_NUM_21   | gpio (input)         | Touch interrupt (active low)
 *
 * ---------------------------------------------------------------------------
 * I2C Configuration
 * ---------------------------------------------------------------------------
 *   Controller : I2C_NUM_0
 *   Clock      : 400 kHz (Fast Mode)
 *   7-bit addr : 0x15 (CST816D)
 *
 * ---------------------------------------------------------------------------
 * SPI Configuration
 * ---------------------------------------------------------------------------
 *   Host       : SPI2_HOST (FSPI)
 *   Clock      : 40 MHz
 *   Mode       : 0 (CPOL=0, CPHA=0)
 *   Data order : MSB first
 *   Bit depth  : 8-bit command, 16-bit pixel data
 * =============================================================================
 *
 * Memory note:
 *   No full-frame framebuffer is allocated.  All drawing operations write
 *   directly to the display via SPI.  The caller is responsible for any
 *   buffering needed for partial updates.
 *
 * C++ note:
 *   All public symbols are extern "C" for use from both C and C++17 sources.
 */

#pragma once
#ifndef LCD_TOUCH_H
#define LCD_TOUCH_H

#include "sdkconfig.h"

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Pin Override Macros
 *
 * Define these BEFORE including this header to change the pin layout:
 *
 *   #define PIN_LCD_CS   GPIO_NUM_4
 *   #include "lcd_touch/lcd_touch.h"
 * ==================================================================== */

/* ---------- ST7789 SPI LCD Pins ---------- */

/** SPI chip select (CS).  Active low. */
#ifndef PIN_LCD_CS
#define PIN_LCD_CS   GPIO_NUM_10
#endif

/** Data/Command select.  High = pixel data, Low = command byte. */
#ifndef PIN_LCD_DC
#define PIN_LCD_DC   GPIO_NUM_11
#endif

/** Hardware reset (active low).  Pull low ≥10 µs then release. */
#ifndef PIN_LCD_RST
#define PIN_LCD_RST  GPIO_NUM_12
#endif

/** Backlight control.  High = on, or PWM output if backlight PWM enabled. */
#ifndef PIN_LCD_BL
#define PIN_LCD_BL   GPIO_NUM_13
#endif

/** SPI master-out slave-in (MOSI). */
#ifndef PIN_LCD_MOSI
#define PIN_LCD_MOSI GPIO_NUM_14
#endif

/** SPI clock (SCLK). */
#ifndef PIN_LCD_SCLK
#define PIN_LCD_SCLK GPIO_NUM_15
#endif

/* ---------- CST816D I2C Touch Pins ---------- */

/** I2C data line (SDA). */
#ifndef PIN_TOUCH_SDA
#define PIN_TOUCH_SDA GPIO_NUM_16
#endif

/** I2C clock line (SCL). */
#ifndef PIN_TOUCH_SCL
#define PIN_TOUCH_SCL GPIO_NUM_17
#endif

/** Touch reset (active low).  Must hold low ≥ 5 ms after power-up. */
#ifndef PIN_TOUCH_RST
#define PIN_TOUCH_RST GPIO_NUM_18
#endif

/** Touch interrupt output (active low).  Pulses low when touch detected. */
#ifndef PIN_TOUCH_INT
#define PIN_TOUCH_INT GPIO_NUM_21
#endif

/* ====================================================================
 * Display Constants
 * ==================================================================== */

/** Display width in pixels (240). */
#define LCD_WIDTH  240

/** Display height in pixels (240). */
#define LCD_HEIGHT 240

/** Total pixel count (240 × 240 = 57 600). */
#define LCD_PIXEL_COUNT  (LCD_WIDTH * LCD_HEIGHT)

/** Bytes per pixel in RGB565 (2). */
#define LCD_BYTES_PER_PIXEL 2

/** Total bytes for a full frame (115 200). */
#define LCD_FRAME_BYTES (LCD_PIXEL_COUNT * LCD_BYTES_PER_PIXEL)

/* ====================================================================
 * I2C / SPI Constants
 * ==================================================================== */

/** CST816D 7-bit I2C address. */
#define TOUCH_I2C_ADDR  0x15

/** I2C master controller to use (I2C_NUM_0). */
#define TOUCH_I2C_PORT  I2C_NUM_0

/** I2C clock rate in Hz. 100 kHz is conservative for bring-up wiring. */
#define TOUCH_I2C_CLOCK_HZ  100000

/** SPI host controller (SPI2_HOST ≡ FSPI). */
#define LCD_SPI_HOST    SPI2_HOST

/** SPI clock rate in Hz (10 MHz — lowered for jumper-wire reliability). */
#define LCD_SPI_CLOCK_HZ   10000000

/* ====================================================================
 * RGB565 Colour Macros
 *
 * Usage:  lcd_fill(0, 0, 239, 239, RGB565(255, 0, 0));  // fill red
 * ==================================================================== */

/** Pack 8-bit R/G/B values into 16-bit RGB565. */
#define RGB565(r, g, b)  ((uint16_t)(((r >> 3) << 11) | \
                                     ((g >> 2) <<  5) | \
                                     ((b >> 3) <<  0)))

/* ---- Pre-defined colours ---- */

#define COLOR_BLACK       RGB565(  0,   0,   0)
#define COLOR_WHITE       RGB565(255, 255, 255)
#define COLOR_RED         RGB565(255,   0,   0)
#define COLOR_GREEN       RGB565(  0, 255,   0)
#define COLOR_BLUE        RGB565(  0,   0, 255)
#define COLOR_CYAN        RGB565(  0, 255, 255)
#define COLOR_MAGENTA     RGB565(255,   0, 255)
#define COLOR_YELLOW      RGB565(255, 255,   0)
#define COLOR_ORANGE      RGB565(255, 165,   0)
#define COLOR_GRAY        RGB565(128, 128, 128)
#define COLOR_DARK_GRAY   RGB565( 64,  64,  64)
#define COLOR_LIGHT_GRAY  RGB565(192, 192, 192)

/* ====================================================================
 * Touch Types
 * ==================================================================== */

/** CST816D gesture identifiers. */
typedef enum {
    TOUCH_GESTURE_NONE          = 0x00,
    TOUCH_GESTURE_SWIPE_UP      = 0x01,
    TOUCH_GESTURE_SWIPE_DOWN    = 0x02,
    TOUCH_GESTURE_SWIPE_LEFT    = 0x03,
    TOUCH_GESTURE_SWIPE_RIGHT   = 0x04,
    TOUCH_GESTURE_CLICK         = 0x05,
    TOUCH_GESTURE_DOUBLE_CLICK  = 0x0B,
    TOUCH_GESTURE_LONG_PRESS    = 0x0C,
} touch_gesture_t;

/** Single touch point data. */
typedef struct {
    uint16_t x;            /**< X coordinate [0 .. LCD_WIDTH-1]. */
    uint16_t y;            /**< Y coordinate [0 .. LCD_HEIGHT-1]. */
    bool     active;       /**< true if this point is currently pressed. */
} touch_point_t;

/** Touch read result. */
typedef struct {
    uint8_t       points;  /**< Number of touch points detected (0, 1, or 2). */
    touch_point_t p[2];    /**< Up to two touch points. */
    touch_gesture_t gesture; /**< Gesture detected since last read. */
} touch_data_t;

/* ====================================================================
 * LCD Public API
 * ==================================================================== */

/**
 * @brief  Initialise the ST7789 display.
 *
 * Performs the following:
 *   1. Resets the display via RST pin.
 *   2. Sends the initialisation command sequence.
 *   3. Configures colour mode, orientation, and gamma.
 *   4. Turns the display on.
 *   5. Enables the backlight.
 *
 * @note  SPI bus (SPI2_HOST) must be initialised before calling this.
 *        Normally done automatically via lcd_touch_init().
 *
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t lcd_init(void);

/**
 * @brief  Turn the display on/off without re-initialising.
 *
 * @param  on  true = display on (DISPON), false = display off (DISPOFF).
 */
void lcd_power(bool on);

/**
 * @brief  Set backlight brightness.
 *
 * @param  brightness  0 = off, 255 = maximum.
 *
 * @note   If CONFIG_LCD_TOUCH_ENABLE_BACKLIGHT_PWM is set, this uses
 *         LEDC PWM for smooth dimming.  Otherwise it is a simple on/off
 *         (brightness ≥ 128 → on).
 */
void lcd_set_backlight(uint8_t brightness);

/**
 * @brief  Set the active drawing window (column and row address range).
 *
 * All subsequent lcd_write_pixels() calls will write into this window.
 *
 * @param  x0  Start column (inclusive).
 * @param  y0  Start row (inclusive).
 * @param  x1  End column (inclusive).
 * @param  y1  End row (inclusive).
 */
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief  Write a buffer of RGB565 pixel data to the active window.
 *
 * @param  colors  Pointer to RGB565 pixel data.
 * @param  len     Number of pixels to write.
 *
 * @note   The display auto-increments its internal address counter,
 *         so sequential calls fill the window left-to-right, top-to-bottom.
 */
void lcd_write_pixels(const uint16_t* colors, uint32_t len);

/**
 * @brief  Fill a rectangular region with a single colour.
 *
 * @param  x0     Start X (inclusive).
 * @param  y0     Start Y (inclusive).
 * @param  x1     End X (inclusive).
 * @param  y1     End Y (inclusive).
 * @param  color  RGB565 fill colour.
 */
void lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
              uint16_t color);

/**
 * @brief  Draw a single pixel at (x, y).
 *
 * @param  x      Column.
 * @param  y      Row.
 * @param  color  RGB565 colour.
 */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief  Draw a bitmap (RGB565) at position (x, y).
 *
 * @param  x     Start column.
 * @param  y     Start row.
 * @param  w     Width in pixels.
 * @param  h     Height in pixels.
 * @param  data  RGB565 pixel data, row-major, length = w × h.
 */
void lcd_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     const uint16_t* data);

/**
 * @brief  Set display rotation.
 *
 * @param  rotation  0 = portrait (default), 1 = landscape, 2 = inverted,
 *                   3 = landscape inverted.
 *
 * @note   MADCTL register is updated.  Rotation affects subsequent drawing.
 */
void lcd_set_rotation(uint8_t rotation);

/* ====================================================================
 * Touch Public API
 * ==================================================================== */

/**
 * @brief  Initialise the CST816D capacitive touch controller.
 *
 * Performs:
 *   1. Reset sequence on the RST pin.
 *   2. I2C initialisation commands (wake, configuration).
 *   3. Configures the INT pin as input with pull-up.
 *
 * @note  I2C_NUM_0 must be initialised before this call.
 *        Normally done automatically via lcd_touch_init().
 *
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t touch_init(void);

/**
 * @brief  Read the current touch state.
 *
 * Poll this function periodically (e.g. every 10-20 ms from a task).
 *
 * @param  out  [out] Pointer to touch_data_t to receive the result.
 *               All fields are zeroed when no touch is detected.
 *
 * @return ESP_OK on successful I2C read, ESP_ERR_TIMEOUT on bus timeout,
 *         or another esp_err_t on I2C error.
 *
 * @note   out->gesture reports gestures (click, swipe, etc.) that have
 *         occurred since the last read.  Gestures are cleared on read.
 */
esp_err_t touch_read(touch_data_t* out);

/**
 * @brief  Get the latest gesture without performing a full touch read.
 *
 * @return The last detected gesture (TOUCH_GESTURE_NONE if clear).
 */
touch_gesture_t touch_get_gesture(void);

/**
 * @brief  Check whether the CST816D controller was initialised.
 *
 * @return true if touch_read() can poll the controller, false otherwise.
 */
bool touch_is_initialized(void);

/**
 * @brief  Manually reset the touch controller (hardware reset via RST pin).
 */
void touch_reset(void);

/* ====================================================================
 * Combined Init
 * ==================================================================== */

/**
 * @brief  Initialise both the LCD and the touch controller in one call.
 *
 * This is the recommended entry point:
 *   1. Initialises the SPI bus (SPI2_HOST) for the LCD.
 *   2. Initialises the I2C bus (I2C_NUM_0) for the touch controller.
 *   3. Calls lcd_init() — initial display setup, backlight on.
 *   4. Calls touch_init() — touch controller reset and config.
 *
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t lcd_touch_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_TOUCH_H */
