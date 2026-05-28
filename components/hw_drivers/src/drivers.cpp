#include "sdkconfig.h"
#include "hw_drivers/drivers.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char* TAG = "hw_drivers";

#ifdef ADC_ATTEN_DB_12
#define HW_ADC_ATTEN ADC_ATTEN_DB_12
#else
#define HW_ADC_ATTEN ADC_ATTEN_DB_11
#endif

int hw_gpio_read(uint8_t pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    return gpio_get_level((gpio_num_t)pin);
}

void hw_gpio_write(uint8_t pin, int val)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, val != 0);
}

int hw_adc_read(uint8_t pin)
{
    adc1_channel_t channel;
    switch (pin) {
        case 1:  channel = ADC1_CHANNEL_0; break;
        case 2:  channel = ADC1_CHANNEL_1; break;
        case 3:  channel = ADC1_CHANNEL_2; break;
        case 4:  channel = ADC1_CHANNEL_3; break;
        case 5:  channel = ADC1_CHANNEL_4; break;
        case 6:  channel = ADC1_CHANNEL_5; break;
        case 7:  channel = ADC1_CHANNEL_6; break;
        case 8:  channel = ADC1_CHANNEL_7; break;
        case 9:  channel = ADC1_CHANNEL_8; break;
        case 10: channel = ADC1_CHANNEL_9; break;
        default:
            ESP_LOGW(TAG, "ADC pin %u not mapped, using ch0", pin);
            channel = ADC1_CHANNEL_0;
            break;
    }
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(channel, HW_ADC_ATTEN);
    return adc1_get_raw(channel);
}

void hw_pwm_write(uint8_t pin, int val)
{
    static bool pwm_inited = false;
    if (!pwm_inited) {
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer);
        pwm_inited = true;
    }
    ledc_channel_config_t ch = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = (val < 0 ? 0 : (val > 255 ? 255 : (uint32_t)val)),
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}
