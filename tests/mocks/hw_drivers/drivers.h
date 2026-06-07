#pragma once
/*
 * Hardware drivers mock — host-side (x86) testing.
 * Component sources #include "hw_drivers/drivers.h" → resolved here via -Itests/mocks.
 */

#include <stdint.h>

static inline int hw_gpio_read(uint8_t pin) { (void)pin; return 0; }

static inline void hw_gpio_write(uint8_t pin, int value) { (void)pin; (void)value; }

static inline int hw_adc_read(uint8_t pin) { (void)pin; return 0; }

static inline void hw_pwm_write(uint8_t pin, int value) { (void)pin; (void)value; }

static inline int hw_mic_init(void) { return 0; }

static inline double hw_mic_level(void) { return 0.0; }
