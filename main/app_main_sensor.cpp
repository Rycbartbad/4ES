/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Sensor firmware entry point
 *
 * SENSOR device role (CONFIG_DEVICE_ROLE_SENSOR):
 *   - Announced via ESP-NOW broadcast
 *   - Responds to DATA_REQ by reading ADC/GPIO
 *   - Executes CMD messages (e.g. digital_write)
 *
 * Prerequisites for building:
 *   idf.py menuconfig -> ESP-LEGO Device Role -> Sensor
 */

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "espnow_comm/comm.h"
#include "espnow_comm/protocol.h"
#include "hw_drivers/pump_control.h"

#include "hw_drivers/drivers.h"
#include "driver/ledc.h"

#include "esp_random.h"

// ====================================================================
// Configuration defaults (when sdkconfig.h not yet fully configured)
// ====================================================================

#ifndef CONFIG_ANNOUNCE_INTERVAL_MS
#define CONFIG_ANNOUNCE_INTERVAL_MS 3000
#endif

// ====================================================================
// Module name (from Kconfig)
// ====================================================================

#ifndef CONFIG_SENSOR_MODULE_NAME
#define CONFIG_SENSOR_MODULE_NAME "sensor"
#endif

static const char* s_module_name = CONFIG_SENSOR_MODULE_NAME;

// ====================================================================
// Active Sensor Configuration (Set one to 1, others to 0)
// ====================================================================
#define USE_SENSOR_DHT11     0   // DHT11 temperature and humidity sensor
#define USE_SENSOR_VIBRATION 0   // Vibration sensor
#define USE_SENSOR_RAINDROP  0   // Raindrop sensor
#define USE_SENSOR_BH1750    0   // BH1750 light sensor
#define USE_SENSOR_JW01      0   // JW01 3-in-1 gas sensor (CO2, TVOC, CH2O)

#define USE_BUZZER           CONFIG_SENSOR_ACTUATOR_BUZZER
#define USE_SERVO            CONFIG_SENSOR_ACTUATOR_SERVO
#define USE_PUMP             CONFIG_SENSOR_ACTUATOR_PUMP

// ====================================================================
// Buzzer Configuration (passive buzzer via LEDC PWM)
// ====================================================================
#if USE_BUZZER
#define BUZZER_PIN              GPIO_NUM_4
#define BUZZER_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER       LEDC_TIMER_2
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_2
#define BUZZER_LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define BUZZER_LEDC_DUTY_ON     512    // 50% duty cycle

// ── Musical note frequencies (Hz) ──
#define NOTE_C4  262   // Middle C (do)
#define NOTE_D4  294   // re
#define NOTE_E4  330   // mi
#define NOTE_F4  349   // fa
#define NOTE_G4  392   // sol
#define NOTE_A4  440   // la
#define NOTE_B4  494   // si
#define NOTE_AS4 466   // la#/sib
#define NOTE_C5  523   // high C
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_R   0     // rest (silence)

// ── Predefined songs ──
// Song index → espnow_send(id, 0x0012, [song_index])
#define SONG_TWINKLE        0  // 小星星
#define SONG_HAPPY_BIRTHDAY 1
#define SONG_JINGLE_BELLS   2

// 小星星 (Twinkle Twinkle Little Star)
static const uint32_t s_twinkle_notes[] = {
    NOTE_C4, NOTE_C4, NOTE_G4, NOTE_G4, NOTE_A4, NOTE_A4, NOTE_G4, NOTE_R,
    NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_D4, NOTE_C4, NOTE_R,
    NOTE_G4, NOTE_G4, NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_R,
    NOTE_G4, NOTE_G4, NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_R,
    NOTE_C4, NOTE_C4, NOTE_G4, NOTE_G4, NOTE_A4, NOTE_A4, NOTE_G4, NOTE_R,
    NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_D4, NOTE_C4, NOTE_R,
};
static const uint32_t s_twinkle_durs[] = {
    400,400,400,400,400,400,800,100,  400,400,400,400,400,400,800,100,
    400,400,400,400,400,400,800,100,  400,400,400,400,400,400,800,100,
    400,400,400,400,400,400,800,100,  400,400,400,400,400,400,800,100,
};
#define TWINKLE_COUNT (sizeof(s_twinkle_notes) / sizeof(s_twinkle_notes[0]))

// Happy Birthday
static const uint32_t s_hb_notes[] = {
    NOTE_C4,NOTE_C4,NOTE_D4,NOTE_C4,NOTE_F4,NOTE_E4, NOTE_R,
    NOTE_C4,NOTE_C4,NOTE_D4,NOTE_C4,NOTE_G4,NOTE_F4, NOTE_R,
    NOTE_C4,NOTE_C4,NOTE_C5,NOTE_A4,NOTE_F4,NOTE_E4,NOTE_D4,NOTE_R,
    NOTE_AS4,NOTE_AS4,NOTE_A4,NOTE_F4,NOTE_G4,NOTE_F4,NOTE_R,
};
static const uint32_t s_hb_durs[] = {
    250,250,500,500,500,800,200,  250,250,500,500,500,800,200,
    250,250,500,500,500,500,500,200,  250,250,500,500,500,800,200,
};
#define HB_COUNT (sizeof(s_hb_notes) / sizeof(s_hb_notes[0]))

// Jingle Bells
static const uint32_t s_jb_notes[] = {
    NOTE_E4,NOTE_E4,NOTE_E4,NOTE_R,  NOTE_E4,NOTE_E4,NOTE_E4,NOTE_R,
    NOTE_E4,NOTE_G4,NOTE_C4,NOTE_D4,NOTE_E4,NOTE_R,
    NOTE_F4,NOTE_F4,NOTE_F4,NOTE_F4,NOTE_F4,NOTE_E4,NOTE_E4,NOTE_E4,NOTE_E4,
    NOTE_E4,NOTE_D4,NOTE_D4,NOTE_E4,NOTE_D4,NOTE_R,NOTE_G4,NOTE_R,
};
static const uint32_t s_jb_durs[] = {
    300,300,600,100,  300,300,600,100,
    300,500,300,300,800,200,
    250,250,250,250,300,250,250,250,250,
    300,300,300,300,600,100,600,100,
};
#define JB_COUNT (sizeof(s_jb_notes) / sizeof(s_jb_notes[0]))

// Buzzer command IDs are shared in espnow_comm/protocol.h.
// CMD_BUZZER_MELODY payload: [note1, dur1_hi, dur1_lo, note2, ...]
// note_id = semitone index: C4=0, C#4=1, ... B6=35, REST=36

// Note ID to frequency lookup (37 notes: C4-B6 with semitones + REST)
// ID = semitone_index: C=0,C#=1,D=2,D#=3,E=4,F=5,F#=6,G=7,G#=8,A=9,A#=10,B=11
// Octave 4: ID  0-11, Octave 5: ID 12-23, Octave 6: ID 24-35, REST: ID 36
static const uint16_t s_note_freqs[] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,   // C4-B4
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,   // C5-B5
    1047,1109,1175,1245,1319,1397,1480,1568,1661,1760,1865,1976,  // C6-B6
    0,                                                              // REST
};
#endif // USE_BUZZER

// ====================================================================
// Servo Configuration (hobby servo via LEDC PWM)
// ====================================================================
#if USE_SERVO
#define SERVO_PIN               GPIO_NUM_4
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER        LEDC_TIMER_3
#define SERVO_LEDC_CHANNEL      LEDC_CHANNEL_3
#define SERVO_LEDC_DUTY_RES     LEDC_TIMER_14_BIT
#define SERVO_DUTY_SCALE        (1U << 14)
#define SERVO_PWM_PERIOD_US     20000
#define SERVO_MIN_PULSE_US      1000
#define SERVO_MAX_PULSE_US      2000

// CMD_SERVO_WRITE is shared in espnow_comm/protocol.h.
#endif // USE_SERVO

// ====================================================================
// Pump Configuration (water pump via GPIO MOSFET switch, timed auto-off)
// ====================================================================
#if USE_PUMP
#define PUMP_PIN                ((gpio_num_t)CONFIG_PUMP_GPIO)
// CMD_PUMP_WRITE payload: 2-byte duration_ms (big-endian uint16)
//   0          → turn off immediately
//   1..CONFIG_PUMP_MAX_RUN_MS → turn on for N ms, then auto-off

// CMD_PUMP_WRITE is shared in espnow_comm/protocol.h.
#endif // USE_PUMP

// ====================================================================
// Capability descriptors — describes sensor function/data format
// Displayed in web console + injected into LLM prompts.
// ====================================================================

static const char* SENSOR_CAPABILITY =
#if USE_SERVO
    "Servo module: GPIO4 50Hz PWM servo. "
    "Use servo_write(id,angle) and servo_sweep(id,from,to,step,delay). "
#endif
#if USE_PUMP
    "Pump module: configurable GPIO MOSFET switch, 5V water pump. "
    "Use pump_write(id,duration_ms) where duration_ms=0(OFF), "
    "1..configured maximum(on for a finite duration). "
#endif
#if USE_BUZZER
    "Doorbell: GPIO4 passive buzzer. "
    "Use buzzer_beep(id,count), buzzer_note(id,note,dur). "
#endif
#if USE_SENSOR_JW01
    "JW01 air sensor: remote_read returns [co2,tvoc,ch2o]."
#elif USE_SENSOR_BH1750
    "BH1750 Light Sensor: ambient light(0-65535 lux). "
    "Returns 1 value: [lux]."
#elif USE_SENSOR_DHT11
    "DHT11 Temperature and Humidity Sensor: temp(0-50C), humidity(20-90%). "
    "Returns 2 values: [temperature_C, humidity_percent]."
#elif USE_SENSOR_VIBRATION
    "Vibration Sensor: detects vibration (binary). "
    "Returns 1 value: [vibration_detected] (0 or 1)."
#elif USE_SENSOR_RAINDROP
    "Raindrop Sensor: detects rain/moisture (binary). "
    "Returns 1 value: [rain_detected] (0 or 1)."
#elif !USE_BUZZER && !USE_SERVO && !USE_PUMP
    "Generic ADC Sensor: reads analog voltages on pins 4,5,6. "
    "Returns 3 values: [adc_pin4, adc_pin5, adc_pin6] (0-4095)."
#endif
    ;

// ====================================================================
// Buzzer driver (passive buzzer via LEDC PWM)
// ====================================================================
#if USE_BUZZER

static void buzzer_init(void)
{
    static bool init_done = false;
    if (init_done) return;

    ledc_timer_config_t timer = {};
    timer.speed_mode      = BUZZER_LEDC_MODE;
    timer.duty_resolution = BUZZER_LEDC_DUTY_RES;
    timer.timer_num       = BUZZER_LEDC_TIMER;
    timer.freq_hz         = 1000;
    timer.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {};
    channel.gpio_num   = BUZZER_PIN;
    channel.speed_mode = BUZZER_LEDC_MODE;
    channel.channel    = BUZZER_LEDC_CHANNEL;
    channel.intr_type  = LEDC_INTR_DISABLE;
    channel.timer_sel  = BUZZER_LEDC_TIMER;
    channel.duty       = 0;
    channel.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    init_done = true;
}

static void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    buzzer_init();
    if (freq_hz == 0) {
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
        if (duration_ms > 0) vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_LEDC_DUTY_ON);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buzzer_play_song(int song_index)
{
    const uint32_t* notes;
    const uint32_t* durs;
    int count;

    switch (song_index) {
    case SONG_TWINKLE:
        notes = s_twinkle_notes; durs = s_twinkle_durs; count = TWINKLE_COUNT; break;
    case SONG_HAPPY_BIRTHDAY:
        notes = s_hb_notes; durs = s_hb_durs; count = HB_COUNT; break;
    case SONG_JINGLE_BELLS:
        notes = s_jb_notes; durs = s_jb_durs; count = JB_COUNT; break;
    default:
        return;
    }

    for (int i = 0; i < count; i++) {
        if (notes[i] == NOTE_R) {
            vTaskDelay(pdMS_TO_TICKS(durs[i]));
        } else {
            uint32_t play_ms = (durs[i] * 9) / 10;
            buzzer_tone(notes[i], play_ms);
            vTaskDelay(pdMS_TO_TICKS(durs[i] / 10 + 20));
        }
    }
}

static void buzzer_note(int note_id, uint32_t duration_ms)
{
    if (note_id < 0 || note_id >= (int)(sizeof(s_note_freqs)/sizeof(s_note_freqs[0]))) return;
    uint16_t freq = s_note_freqs[note_id];
    buzzer_tone(freq, duration_ms);
}

// melody_raw: payload = [note1, dur1_hi, dur1_lo, note2, dur2_hi, dur2_lo, ...]
// Each note is 3 bytes. Payload length determines note count.
static void buzzer_melody_raw(const uint8_t* data, int len)
{
    int count = len / 3;
    for (int i = 0; i < count; i++) {
        int note_id = (int)data[i * 3];
        uint16_t dur = ((uint16_t)data[i * 3 + 1] << 8) | data[i * 3 + 2];
        uint32_t play_ms = (dur * 9) / 10;
        buzzer_note(note_id, play_ms);
        vTaskDelay(pdMS_TO_TICKS(dur / 10 + 20));
    }
}

#endif // USE_BUZZER

// ====================================================================
// Servo driver (50 Hz hobby servo via LEDC PWM)
// ====================================================================
#if USE_SERVO

static void servo_init(void)
{
    static bool init_done = false;
    if (init_done) return;

    ledc_timer_config_t timer = {};
    timer.speed_mode      = SERVO_LEDC_MODE;
    timer.duty_resolution = SERVO_LEDC_DUTY_RES;
    timer.timer_num       = SERVO_LEDC_TIMER;
    timer.freq_hz         = 50;
    timer.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {};
    channel.gpio_num   = SERVO_PIN;
    channel.speed_mode = SERVO_LEDC_MODE;
    channel.channel    = SERVO_LEDC_CHANNEL;
    channel.intr_type  = LEDC_INTR_DISABLE;
    channel.timer_sel  = SERVO_LEDC_TIMER;
    channel.duty       = 0;
    channel.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    init_done = true;
}

static uint32_t servo_angle_to_duty(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t pulse_us = SERVO_MIN_PULSE_US +
        ((uint32_t)angle * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) / 180U;
    return (pulse_us * SERVO_DUTY_SCALE) / SERVO_PWM_PERIOD_US;
}

static void servo_write_angle(int angle)
{
    servo_init();
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t duty = servo_angle_to_duty(angle);
    esp_err_t err = ledc_set_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE("sensor", "SERVO_WRITE set_duty failed angle=%d duty=%lu err=%s",
                 angle, (unsigned long)duty, esp_err_to_name(err));
        return;
    }
    err = ledc_update_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE("sensor", "SERVO_WRITE update_duty failed angle=%d duty=%lu err=%s",
                 angle, (unsigned long)duty, esp_err_to_name(err));
        return;
    }
    ESP_LOGI("sensor", "SERVO_WRITE pin=GPIO4 angle=%d duty=%lu",
             angle, (unsigned long)duty);
}

#endif // USE_SERVO

// ====================================================================
// Pump driver (water pump via GPIO MOSFET switch, timed auto-off)
// ====================================================================
#if USE_PUMP

#include "driver/gpio.h"
#include "freertos/timers.h"

static TimerHandle_t s_pump_timer = NULL;
static PumpControl s_pump_control = {};

static bool pump_set_output(void* context, bool active)
{
    (void)context;
    const int active_level = CONFIG_PUMP_ACTIVE_HIGH ? 1 : 0;
    const int level = active ? active_level : !active_level;
    return gpio_set_level(PUMP_PIN, level) == ESP_OK;
}

static bool pump_arm_timer(void* context, uint32_t delay_ticks)
{
    TimerHandle_t timer = (TimerHandle_t)context;
    return timer != NULL &&
           xTimerChangePeriod(timer, (TickType_t)delay_ticks, 0) == pdPASS;
}

static void pump_cancel_timer(void* context)
{
    TimerHandle_t timer = (TimerHandle_t)context;
    if (timer != NULL) {
        xTimerStop(timer, 0);
    }
}

// ── Auto-off timer callback (runs in timer task context) ──
static void pump_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    pump_control_timer_fired(&s_pump_control);
    ESP_LOGI("sensor", "PUMP_AUTO_OFF pin=GPIO%d", (int)PUMP_PIN);
}

static bool pump_init(void)
{
    if (gpio_set_direction(PUMP_PIN, GPIO_MODE_OUTPUT) != ESP_OK ||
        !pump_set_output(NULL, false)) {
        ESP_LOGE("sensor", "PUMP: failed to configure safe OFF output");
        return false;
    }

    s_pump_timer = xTimerCreate("pump_to", 1,
                                 pdFALSE,   // one-shot
                                 NULL,
                                 pump_timer_cb);
    if (s_pump_timer == NULL) {
        ESP_LOGE("sensor", "PUMP: failed to create auto-off timer");
        pump_set_output(NULL, false);
        return false;
    }

    PumpControlBackend backend = {
        s_pump_timer, pump_set_output, pump_arm_timer, pump_cancel_timer
    };
    if (!pump_control_init(&s_pump_control, &backend,
                           portTICK_PERIOD_MS, CONFIG_PUMP_MAX_RUN_MS)) {
        ESP_LOGE("sensor", "PUMP: failed to initialize fail-safe control");
        pump_set_output(NULL, false);
        return false;
    }
    ESP_LOGI("sensor", "PUMP_INIT pin=GPIO%d state=OFF", (int)PUMP_PIN);
    return true;
}

// duration_ms = 0          → turn off immediately
// duration_ms = 1..CONFIG_PUMP_MAX_RUN_MS → finite timed run
static bool pump_on_for(uint16_t duration_ms)
{
    if (!pump_control_apply(&s_pump_control, duration_ms)) {
        ESP_LOGE("sensor", "PUMP_REJECT pin=GPIO%d dur=%ums",
                 (int)PUMP_PIN, (int)duration_ms);
        return false;
    }
    if (duration_ms == 0) {
        ESP_LOGI("sensor", "PUMP_OFF pin=GPIO%d", (int)PUMP_PIN);
    } else {
        ESP_LOGI("sensor", "PUMP_ON pin=GPIO%d dur=%ums",
                 (int)PUMP_PIN, (int)duration_ms);
    }
    return true;
}

#endif // USE_PUMP

// ====================================================================
// DHT11 Sensor Definition
// ====================================================================
#if USE_SENSOR_DHT11
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define DHT11_PIN GPIO_NUM_13

static bool dht11_read(double* temperature, double* humidity) {
    uint8_t data[5] = {0, 0, 0, 0, 0};

    // Send start signal (18 ms low — not timing-critical)
    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(DHT11_PIN, 1);

    // Suspend the FreeRTOS scheduler for the µs-level bit-read window
    // (~6 ms total). Interrupts remain active so the Wi-Fi/ESP-NOW driver
    // continues to function. Each early-return path must call xTaskResumeAll()
    // before returning to restore the scheduler.
    vTaskSuspendAll();

    esp_rom_delay_us(30);

    // Prepare to read
    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);

    // Wait for DHT11 response signal (low then high)
    int wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) { xTaskResumeAll(); return false; }
    wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 0 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) { xTaskResumeAll(); return false; }
    wait_time = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }
    if (wait_time >= 100) { xTaskResumeAll(); return false; }

    // Read 40 bits of data
    for (int i = 0; i < 40; i++) {
        wait_time = 0;
        while (gpio_get_level(DHT11_PIN) == 0 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }

        wait_time = 0;
        while (gpio_get_level(DHT11_PIN) == 1 && wait_time < 100) { esp_rom_delay_us(1); wait_time++; }

        data[i / 8] <<= 1;
        if (wait_time > 40) {
            data[i / 8] |= 1;
        }
    }

    xTaskResumeAll();  // resume scheduler before any non-critical work

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum == data[4]) {
        *humidity = (double)data[0] + ((double)data[1] / 10.0);
        *temperature = (double)data[2] + ((double)data[3] / 10.0);
        return true;
    }
    return false;
}
#endif

// ====================================================================
// Vibration Sensor Definition
// ====================================================================
#if USE_SENSOR_VIBRATION
#include "driver/gpio.h"

#define VIBRATION_PIN GPIO_NUM_6

static void vibration_init() {
    gpio_set_direction(VIBRATION_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(VIBRATION_PIN, GPIO_PULLDOWN_ONLY);
}

static double vibration_read() {
    return (double)gpio_get_level(VIBRATION_PIN);
}
#endif

// ====================================================================
// Raindrop Sensor Definition
// ====================================================================
#if USE_SENSOR_RAINDROP
#include "driver/gpio.h"

#define RAINDROP_PIN GPIO_NUM_6

static void raindrop_init() {
    gpio_set_direction(RAINDROP_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(RAINDROP_PIN, GPIO_PULLDOWN_ONLY);
}

static double raindrop_read() {
    return (double)gpio_get_level(RAINDROP_PIN);
}
#endif

// ====================================================================
// BH1750 Light Sensor Definition
// ====================================================================
#if USE_SENSOR_BH1750
#include "driver/i2c.h"

#define I2C_MASTER_SCL_IO           GPIO_NUM_22
#define I2C_MASTER_SDA_IO           GPIO_NUM_21
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define BH1750_SENSOR_ADDR          0x23

static void bh1750_init() {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    conf.clk_flags = 0;
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

static double bh1750_read() {
    uint8_t data[2] = {0, 0};

    // Send measurement command (One time H-resolution mode 0x20)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x20, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    vTaskDelay(pdMS_TO_TICKS(180));

    // Read 2 bytes of data
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    double lux = ((data[0] << 8) | data[1]) / 1.2;
    return lux;
}
#endif

// ====================================================================
// JW01 Gas Sensor Definition
// ====================================================================
#if USE_SENSOR_JW01
#include "driver/uart.h"
#include "driver/gpio.h"

#define JW01_UART_NUM      UART_NUM_1
#define JW01_TX_PIN        GPIO_NUM_21
#define JW01_RX_PIN        GPIO_NUM_20
#define JW01_BAUD_RATE     9600

static void jw01_init() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = JW01_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    uart_param_config(JW01_UART_NUM, &uart_config);
    uart_set_pin(JW01_UART_NUM, JW01_TX_PIN, JW01_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(JW01_UART_NUM, 256, 0, 0, NULL, 0);
}

static bool jw01_read(double* co2, double* tvoc, double* ch2o) {
    uint8_t data[64];
    int length = uart_read_bytes(JW01_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(100));

    if (length >= 9) {
        *co2  = (double)((data[1] << 8) | data[2]);
        *tvoc = (double)((data[3] << 8) | data[4]);
        *ch2o = (double)((data[5] << 8) | data[6]);
        return true;
    }
    return false;
}
#endif

// ====================================================================
// Command queue — recv_cb enqueues only; cmd_task does the actual work.
//
// This prevents rx_task from blocking during sensor reads (JW01 UART
// 100 ms wait, BH1750 180 ms vTaskDelay, DHT11 busy-wait).  Without
// this separation, rx_task stalls and ESP-NOW packets accumulate until
// the 8-slot queue overflows.
// ====================================================================

typedef struct {
    uint8_t  src_mac[6];
    uint8_t  msg_type;
    uint8_t  req_seq;
    uint16_t cmd_id;
    uint8_t  payload[128];  // up to 42 notes (3 bytes each)
    int      payload_len;
} SensorCmd;

#define SENSOR_CMD_QUEUE_LEN 4
static QueueHandle_t s_cmd_queue = NULL;

// ====================================================================
// handle_data_req — read sensor and send DATA_RESP (called from cmd_task)
// ====================================================================

static void handle_data_req(const uint8_t* src_mac, uint8_t req_seq)
{
    uint8_t resp_buf[128];
    size_t  resp_len = 0;

#if USE_SENSOR_JW01
    double values[3] = {0.0, 0.0, 0.0};
    double co2 = 0.0, tvoc = 0.0, ch2o = 0.0;
    if (jw01_read(&co2, &tvoc, &ch2o)) {
        values[0] = co2;
        values[1] = tvoc;
        values[2] = ch2o;
    }
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, values, 3);

#elif USE_SENSOR_BH1750
    double values[1] = {bh1750_read()};
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, values, 1);

#elif USE_SENSOR_RAINDROP
    double values[1] = {raindrop_read()};
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, values, 1);

#elif USE_SENSOR_VIBRATION
    double values[1] = {vibration_read()};
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, values, 1);

#elif USE_SENSOR_DHT11
    double values[2] = {0.0, 0.0};
    double temp = 0.0, hum = 0.0;
    if (dht11_read(&temp, &hum)) {
        values[0] = temp;
        values[1] = hum;
    }
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, values, 2);

#elif !USE_BUZZER && !USE_SERVO && !USE_PUMP
    // Generic ADC sensor — reads pins 4, 5, 6
    #define SENSOR_ADC_PINS   {4, 5, 6}
    #define SENSOR_ADC_COUNT  3
    static const uint8_t s_adc_pins[SENSOR_ADC_COUNT] = SENSOR_ADC_PINS;
    double values[SENSOR_ADC_COUNT];
    for (int i = 0; i < SENSOR_ADC_COUNT; i++) {
        values[i] = (double)hw_adc_read(s_adc_pins[i]);
    }
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq,
                              values, SENSOR_ADC_COUNT);
#else
    // Actuator-only modules have no sampled values. The Master identifies
    // them from capability and should not poll DATA_REQ periodically.
    protocol_build_data_resp(resp_buf, &resp_len, 0, req_seq, nullptr, 0);
#endif

    if (resp_len > 0) {
        esp_now_send(src_mac, resp_buf, resp_len);
    }
}

// ====================================================================
// handle_cmd — execute GPIO command and send ACK (called from cmd_task)
// ====================================================================

static void send_command_ack(const uint8_t* dst_mac, uint8_t req_seq)
{
    uint8_t ack_buf[64];
    size_t ack_len = 0;
    protocol_build_ack(ack_buf, &ack_len, 0, req_seq);
    if (ack_len > 0) {
        esp_now_send(dst_mac, ack_buf, ack_len);
    }
}

static void handle_cmd(const uint8_t* src_mac, uint8_t req_seq,
                       uint16_t cmd_id,
                       const uint8_t* payload, int payload_len)
{
#if USE_SERVO
    if (cmd_id == CMD_SERVO_WRITE) {
        if (payload_len >= 1) {
            servo_write_angle((int)payload[0]);
            send_command_ack(src_mac, req_seq);
        } else {
            ESP_LOGW("sensor", "CMD_SERVO_WRITE short payload=%d", payload_len);
        }
        return;
    }
#endif

#if USE_PUMP
    if (cmd_id == CMD_PUMP_WRITE) {
        if (payload_len >= 2) {
            uint16_t duration_ms = ((uint16_t)payload[0] << 8) | payload[1];
            if (pump_on_for(duration_ms)) {
                send_command_ack(src_mac, req_seq);
            }
        } else {
            ESP_LOGW("sensor", "CMD_PUMP_WRITE short payload=%d (need 2 bytes)", payload_len);
        }
        return;
    }
#endif

#if USE_BUZZER
    // ── Buzzer commands ──
    if (cmd_id == CMD_BUZZER_SONG) {
        if (payload_len >= 1 && payload[0] <= SONG_JINGLE_BELLS) {
            buzzer_play_song(payload[0]);
            send_command_ack(src_mac, req_seq);
        } else {
            ESP_LOGW("sensor", "CMD_BUZZER_SONG invalid payload=%d", payload_len);
        }
        return;
    }
    if (cmd_id == CMD_BUZZER_NOTE) {
        if (payload_len >= 3 && payload[0] <= 36) {
            int note_id = (int)payload[0];
            uint16_t dur = ((uint16_t)payload[1] << 8) | payload[2];
            buzzer_note(note_id, dur);
            send_command_ack(src_mac, req_seq);
        } else {
            ESP_LOGW("sensor", "CMD_BUZZER_NOTE invalid payload=%d", payload_len);
        }
        return;
    }
    if (cmd_id == CMD_BUZZER_MELODY) {
        if (payload_len > 0 && payload_len % 3 == 0) {
            buzzer_melody_raw(payload, payload_len);
            send_command_ack(src_mac, req_seq);
        } else {
            ESP_LOGW("sensor", "CMD_BUZZER_MELODY invalid payload=%d", payload_len);
        }
        return;
    }
#endif

    // ── Default: GPIO write ──
    if (payload_len < 2) return;
    hw_gpio_write(payload[0], payload[1]);
    send_command_ack(src_mac, req_seq);
}

// ====================================================================
// sensor_recv_cb — runs in rx_task context; enqueues only (non-blocking)
//
// This callback must return quickly.  All sensor reads and ESP-NOW
// replies are delegated to cmd_task via s_cmd_queue.
// ====================================================================

static void sensor_recv_cb(const uint8_t* src_mac, uint8_t msg_type,
                            const uint8_t* data, int len)
{
    // Master discovery: respond with an immediate ANNOUNCE so the master
    // can register this sensor without requiring a manual reset.
    if (msg_type == MSG_ANNOUNCE) {
        char ann_name[32];
        char ann_cap[CONFIG_MAX_CAPABILITY_LEN];
        if (protocol_parse_announce(data, len, ann_name, sizeof(ann_name),
                                    ann_cap, sizeof(ann_cap)) &&
            strncmp(ann_name, "master", 6) == 0) {
            espnow_comm_sync_rf();
            espnow_comm_send_announce();
        }
        return;
    }

    if (msg_type != MSG_DATA_REQ && msg_type != MSG_CMD) {
        return;
    }

    // Register sender as an ESP-NOW peer so esp_now_send() can reply.
    if (!esp_now_is_peer_exist(src_mac)) {
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 0;
        peerInfo.ifidx   = WIFI_IF_STA;
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, src_mac, 6);
        esp_now_add_peer(&peerInfo);
    }

    // Build a minimal queued command (avoid copying full 250-byte packet).
    SensorCmd cmd = {};
    memcpy(cmd.src_mac, src_mac, 6);
    cmd.msg_type = msg_type;
    if (len >= (int)MSG_HEADER_SIZE) {
        const MsgHeader* hdr = (const MsgHeader*)data;
        cmd.req_seq = hdr->seq_id;
        cmd.cmd_id  = hdr->cmd_id;
    }

    if (msg_type == MSG_CMD) {
        // Extract payload (strip the 7-byte header).
        int poff = (int)MSG_HEADER_SIZE;
        int plen = len - poff;
        if (plen > 0 && plen <= (int)sizeof(cmd.payload)) {
            memcpy(cmd.payload, data + poff, (size_t)plen);
            cmd.payload_len = plen;
        }
    }

    // Non-blocking enqueue; drop if the queue is full.
    if (s_cmd_queue == NULL ||
        xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW("sensor", "cmd queue full, dropping msg_type=0x%02x",
                 msg_type);
    }
}

// ====================================================================
// cmd_task — dequeues SensorCmd and handles sensor reads / GPIO ops.
// Running as a dedicated task means blocking I/O does not stall rx_task.
// ====================================================================

static void cmd_task(void* arg)
{
    (void)arg;
    SensorCmd cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.msg_type) {
        case MSG_DATA_REQ:
            handle_data_req(cmd.src_mac, cmd.req_seq);
            break;
        case MSG_CMD:
            handle_cmd(cmd.src_mac, cmd.req_seq, cmd.cmd_id,
                       cmd.payload, cmd.payload_len);
            break;
        default:
            break;
        }
    }
}

// ====================================================================
// sensor_hw_init_task — JW01 warmup without blocking ESP-NOW bring-up
// ====================================================================

#if USE_SENSOR_JW01
static void sensor_hw_init_task(void* arg)
{
    (void)arg;
    jw01_init();
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("JW01 initialized\n");
    vTaskDelete(NULL);
}
#endif

// ====================================================================
// announce_task — sends periodic broadcast announces with jitter
// ====================================================================

static void announce_task(void* arg)
{
    (void)arg;
    while (1) {
        espnow_comm_send_announce();

        // Apply random jitter to avoid collision when multiple sensors
        // power on simultaneously (design.md §4.3).
        int jitter = ((int)esp_random() % (CONFIG_ANNOUNCE_JITTER_MS * 2))
                     - CONFIG_ANNOUNCE_JITTER_MS;
        int delay_ms = CONFIG_ANNOUNCE_INTERVAL_MS + jitter;
        if (delay_ms < 100) delay_ms = 100;  // minimum 100ms guard
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ====================================================================
// app_main — sensor entry point
// ====================================================================

extern "C" void app_main(void)
{
    printf("ESP-LEGO V1.0 SENSOR firmware starting...\n");

    // ---- Initialize NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Initialize WiFi and ESP-NOW ----
    ESP_ERROR_CHECK(espnow_comm_init());

    // ---- Set module name + capability (used in announce) ----
    strncpy(g_espnow_module_name, s_module_name,
            sizeof(g_espnow_module_name));
    g_espnow_module_name[sizeof(g_espnow_module_name) - 1] = '\0';

    strncpy(g_espnow_module_capability, SENSOR_CAPABILITY,
            sizeof(g_espnow_module_capability));
    g_espnow_module_capability[sizeof(g_espnow_module_capability) - 1] = '\0';

    // ---- Initialize sensor hardware once at startup ----
    // Previously done lazily inside recv_cb (static bool init_done),
    // which caused the first DATA_REQ to always return zeros and made
    // uart_driver_install() run in rx_task context.
#if USE_SENSOR_JW01
    jw01_init();
    // Allow JW01 to send its first UART frame before the first DATA_REQ.
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("JW01 initialized\n");
#elif USE_SENSOR_BH1750
    bh1750_init();
    printf("BH1750 initialized\n");
#elif USE_SENSOR_VIBRATION
    vibration_init();
    printf("Vibration sensor initialized\n");
#elif USE_SENSOR_RAINDROP
    raindrop_init();
    printf("Raindrop sensor initialized\n");
#endif
    // DHT11 and generic ADC need no explicit pre-initialization.

#if USE_BUZZER
    buzzer_init();
    printf("Buzzer initialized (GPIO4, LEDC PWM)\n");
#endif

#if USE_SERVO
    servo_init();
    servo_write_angle(90);
    printf("Servo initialized (GPIO4, 50Hz PWM, angle=90)\n");
#endif

#if USE_PUMP
    if (pump_init()) {
        printf("Pump initialized (GPIO%d, timed fail-safe, default OFF)\n",
               (int)PUMP_PIN);
    } else {
        printf("ERROR: Pump unavailable; output forced OFF\n");
    }
#endif

    // ---- Create command processing queue ----
    s_cmd_queue = xQueueCreate(SENSOR_CMD_QUEUE_LEN, sizeof(SensorCmd));
    if (s_cmd_queue == NULL) {
        printf("ERROR: Failed to create cmd queue — halting\n");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ---- Register receive callback AFTER queue is ready ----
    // Packets arriving before this point are silently ignored by rx_task
    // (no callback registered), which is the safe default.
    espnow_comm_register_recv_callback(sensor_recv_cb);

    // ---- Create cmd_task (handles DATA_REQ and CMD out of queue) ----
    BaseType_t tsk = xTaskCreate(cmd_task, "sensor_cmd",
                                  3072, NULL, 4, NULL);
    if (tsk != pdPASS) {
        printf("ERROR: Failed to create cmd_task\n");
    }

    // ---- Create announce task ----
    tsk = xTaskCreate(announce_task, "announce",
                      2048, NULL, 5, NULL);
    if (tsk != pdPASS) {
        printf("ERROR: Failed to create announce task\n");
    }

    // Announce immediately so a master that booted earlier can discover us
    // without waiting for the first periodic interval or a manual reset.
    espnow_comm_send_announce();

    // ---- Sensor hardware init (non-blocking for ESP-NOW) ----
#if USE_SENSOR_JW01
    tsk = xTaskCreate(sensor_hw_init_task, "jw01_init", 2048, NULL, 3, NULL);
    if (tsk != pdPASS) {
        printf("WARN: JW01 init task failed — sensor reads may be zero\n");
    }
#elif USE_SENSOR_BH1750
    bh1750_init();
    printf("BH1750 initialized\n");
#elif USE_SENSOR_VIBRATION
    vibration_init();
    printf("Vibration sensor initialized\n");
#elif USE_SENSOR_RAINDROP
    raindrop_init();
    printf("Raindrop sensor initialized\n");
#endif

    printf("Sensor ready — name=%s\n", g_espnow_module_name);

    // ---- Main loop ----
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
