#include "sdkconfig.h"
#include "hw_drivers/drivers.h"
#include "hw_drivers/mic_level.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "hw_drivers";

#ifndef CONFIG_MIC_I2S_SAMPLE_RATE_HZ
#define CONFIG_MIC_I2S_SAMPLE_RATE_HZ 16000
#endif

// INMP441 wiring per U2 (=J3) schematic: pin 4/7/8 = MIC_SD/SCK/WS.
// Corresponding ESP32-S3 GPIOs: GPIO1 / GPIO41 / GPIO40.
#define MIC_I2S_SD_PIN  GPIO_NUM_1
#define MIC_I2S_SCK_PIN GPIO_NUM_41
#define MIC_I2S_WS_PIN  GPIO_NUM_40

#define MIC_I2S_READ_SAMPLES 512
#define MIC_I2S_TIMEOUT_MS   100

static i2s_chan_handle_t s_mic_rx_chan = NULL;
static bool s_mic_ready = false;
static StaticSemaphore_t s_mic_mutex_storage;
static SemaphoreHandle_t s_mic_mutex = NULL;
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

static esp_err_t hw_adc_init_once(void)
{
    if (s_adc1_ready) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &s_adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed: %d", ret);
        s_adc1_handle = NULL;
        return ret;
    }

    s_adc1_ready = true;
    return ESP_OK;
}

int hw_adc_read(uint8_t pin)
{
    adc_channel_t channel;
#if CONFIG_IDF_TARGET_ESP32C3
    // ESP32-C3 has ADC1 channels on GPIO 0-4 only
    switch (pin) {
        case 0:  channel = ADC_CHANNEL_0; break;
        case 1:  channel = ADC_CHANNEL_1; break;
        case 2:  channel = ADC_CHANNEL_2; break;
        case 3:  channel = ADC_CHANNEL_3; break;
        case 4:  channel = ADC_CHANNEL_4; break;
        default:
            ESP_LOGW(TAG, "ADC pin %u not available on ESP32-C3 (only GPIO0-4), using ch0", pin);
            channel = ADC_CHANNEL_0;
            break;
    }
#else
    // ESP32-S3 has ADC1 channels on GPIO 1-10
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

    if (hw_adc_init_once() != ESP_OK) {
        return 0;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
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

esp_err_t hw_mic_init(void)
{
    if (s_mic_mutex == NULL) {
        s_mic_mutex = xSemaphoreCreateMutexStatic(&s_mic_mutex_storage);
        if (s_mic_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_mic_mutex, pdMS_TO_TICKS(MIC_I2S_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_mic_ready) {
        xSemaphoreGive(s_mic_mutex);
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_mic_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S mic channel alloc failed: %d", ret);
        s_mic_rx_chan = NULL;
        xSemaphoreGive(s_mic_mutex);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_MIC_I2S_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_SCK_PIN,
            .ws = MIC_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_mic_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S mic std init failed: %d", ret);
        i2s_del_channel(s_mic_rx_chan);
        s_mic_rx_chan = NULL;
        xSemaphoreGive(s_mic_mutex);
        return ret;
    }

    ret = i2s_channel_enable(s_mic_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S mic enable failed: %d", ret);
        i2s_del_channel(s_mic_rx_chan);
        s_mic_rx_chan = NULL;
        xSemaphoreGive(s_mic_mutex);
        return ret;
    }

    s_mic_ready = true;
    ESP_LOGI(TAG, "INMP441 mic ready: SCK=%d WS=%d SD=%d sample_rate=%d",
             (int)MIC_I2S_SCK_PIN, (int)MIC_I2S_WS_PIN, (int)MIC_I2S_SD_PIN,
             CONFIG_MIC_I2S_SAMPLE_RATE_HZ);
    xSemaphoreGive(s_mic_mutex);
    return ESP_OK;
}

esp_err_t hw_mic_read_level(double* out_percent)
{
    if (out_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_percent = 0.0;

    if (!s_mic_ready) {
        esp_err_t init_ret = hw_mic_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    if (xSemaphoreTake(s_mic_mutex,
                       pdMS_TO_TICKS(MIC_I2S_TIMEOUT_MS + 50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int32_t samples[MIC_I2S_READ_SAMPLES];
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_mic_rx_chan, samples, sizeof(samples),
                                     &bytes_read, MIC_I2S_TIMEOUT_MS);
    xSemaphoreGive(s_mic_mutex);
    if (ret != ESP_OK || bytes_read == 0) {
        ESP_LOGW(TAG, "I2S mic read failed: ret=%d bytes=%u",
                 ret, (unsigned)bytes_read);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }

    *out_percent = hw_mic_calculate_level_percent(
        samples, bytes_read / sizeof(samples[0]));
    return ESP_OK;
}

double hw_mic_level(void)
{
    double percent = 0.0;
    (void)hw_mic_read_level(&percent);
    return percent;
}
