#include "sdkconfig.h"
#include "hw_drivers/drivers.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char* TAG = "hw_drivers";

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static bool s_adc1_ready = false;

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
    adc_channel_t channel;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    switch (pin) {
        case 0: channel = ADC_CHANNEL_0; break;
        case 1: channel = ADC_CHANNEL_1; break;
        case 2: channel = ADC_CHANNEL_2; break;
        case 3: channel = ADC_CHANNEL_3; break;
        case 4: channel = ADC_CHANNEL_4; break;
        default:
            ESP_LOGW(TAG, "ADC pin %u not mapped on ESP32-C3, using ch0", pin);
            channel = ADC_CHANNEL_0;
            break;
    }
#else
    switch (pin) {
        case 1:  channel = ADC_CHANNEL_0; break;
        case 2:  channel = ADC_CHANNEL_1; break;
        case 3:  channel = ADC_CHANNEL_2; break;
        case 4:  channel = ADC_CHANNEL_3; break;
        case 5:  channel = ADC_CHANNEL_4; break;
        case 6:  channel = ADC_CHANNEL_5; break;
        case 7:  channel = ADC_CHANNEL_6; break;
        case 8:  channel = ADC_CHANNEL_7; break;
        case 9:  channel = ADC_CHANNEL_8; break;
        case 10: channel = ADC_CHANNEL_9; break;
        default:
            ESP_LOGW(TAG, "ADC pin %u not mapped, using ch0", pin);
            channel = ADC_CHANNEL_0;
            break;
    }
#endif

    if (!s_adc1_ready) {
        adc_oneshot_unit_init_cfg_t init_cfg = {};
        init_cfg.unit_id = ADC_UNIT_1;
        init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
        esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &s_adc1_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC1 init failed: %d", ret);
            s_adc1_handle = NULL;
            return 0;
        }
        s_adc1_ready = true;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    esp_err_t ret = adc_oneshot_config_channel(s_adc1_handle, channel,
                                               &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel config failed: ch=%d ret=%d",
                 (int)channel, ret);
        return 0;
    }

    int raw = 0;
    ret = adc_oneshot_read(s_adc1_handle, channel, &raw);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: ch=%d ret=%d", (int)channel, ret);
        return 0;
    }
    return raw;
}

void hw_pwm_write(uint8_t pin, int val)
{
    static bool pwm_inited = false;
    if (!pwm_inited) {
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_8_BIT;
        timer.timer_num = LEDC_TIMER_0;
        timer.freq_hz = 1000;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&timer);
        pwm_inited = true;
    }
    ledc_channel_config_t ch = {};
    ch.gpio_num = pin;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = (val < 0 ? 0 : (val > 255 ? 255 : (uint32_t)val));
    ch.hpoint = 0;
    ledc_channel_config(&ch);
}
