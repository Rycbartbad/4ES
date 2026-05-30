#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hardware abstraction layer — design.md §6.10

int  hw_gpio_read(uint8_t pin);
void hw_gpio_write(uint8_t pin, int val);
int  hw_adc_read(uint8_t pin);
void hw_pwm_write(uint8_t pin, int val);
esp_err_t hw_mic_init(void);
double hw_mic_level(void);

#ifdef __cplusplus
}
#endif
