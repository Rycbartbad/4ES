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

#include "espnow_comm/comm.h"
#include "espnow_comm/protocol.h"

#include "hw_drivers/drivers.h"

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
#define USE_SENSOR_JW01      1   // JW01 3-in-1 gas sensor (CO2, TVOC, CH2O)

// ====================================================================
// Capability descriptors — describes sensor function/data format
// Displayed in web console + injected into LLM prompts.
// ====================================================================

#if USE_SENSOR_JW01
static const char* SENSOR_CAPABILITY =
    "JW01 3-in-1 Air Quality Sensor: CO2(0-5000ppm), TVOC(0-5ppm), CH2O(0-5mg/m3). "
    "Returns 3 values: [co2_ppm, tvoc_ppm, ch2o_mg_m3].";
#elif USE_SENSOR_BH1750
static const char* SENSOR_CAPABILITY =
    "BH1750 Light Sensor: ambient light(0-65535 lux). "
    "Returns 1 value: [lux].";
#elif USE_SENSOR_DHT11
static const char* SENSOR_CAPABILITY =
    "DHT11 Temperature and Humidity Sensor: temp(0-50C), humidity(20-90%). "
    "Returns 2 values: [temperature_C, humidity_percent].";
#elif USE_SENSOR_VIBRATION
static const char* SENSOR_CAPABILITY =
    "Vibration Sensor: detects vibration (binary). "
    "Returns 1 value: [vibration_detected] (0 or 1).";
#elif USE_SENSOR_RAINDROP
static const char* SENSOR_CAPABILITY =
    "Raindrop Sensor: detects rain/moisture (binary). "
    "Returns 1 value: [rain_detected] (0 or 1).";
#else
static const char* SENSOR_CAPABILITY =
    "Generic ADC Sensor: reads analog voltages on pins 4,5,6. "
    "Returns 3 values: [adc_pin4, adc_pin5, adc_pin6] (0-4095).";
#endif

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
    uint8_t src_mac[6];   // sender MAC for unicast reply
    uint8_t msg_type;     // MSG_DATA_REQ or MSG_CMD
    uint8_t req_seq;      // seq_id from request header — echoed in response
    uint8_t payload[16];  // CMD payload bytes (pin, val, …)
    int     payload_len;  // number of valid bytes in payload[]
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

#else
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
#endif

    if (resp_len > 0) {
        esp_now_send(src_mac, resp_buf, resp_len);
    }
}

// ====================================================================
// handle_cmd — execute GPIO command and send ACK (called from cmd_task)
// ====================================================================

static void handle_cmd(const uint8_t* src_mac, uint8_t req_seq,
                       const uint8_t* payload, int payload_len)
{
    if (payload_len < 2) return;
    uint8_t pin = payload[0];
    uint8_t val = payload[1];
    hw_gpio_write(pin, val);

    uint8_t ack_buf[64];
    size_t  ack_len = 0;
    protocol_build_ack(ack_buf, &ack_len, 0, req_seq);
    if (ack_len > 0) {
        esp_now_send(src_mac, ack_buf, ack_len);
    }
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
    // Only handle DATA_REQ and CMD messages.
    // MSG_ANNOUNCE from master is not used in the current protocol;
    // master never broadcasts ANNOUNCE, so this case never fires.
    if (msg_type != MSG_DATA_REQ && msg_type != MSG_CMD) {
        return;
    }

    // Register sender as an ESP-NOW peer so esp_now_send() can reply.
    if (!esp_now_is_peer_exist(src_mac)) {
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 0;
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, src_mac, 6);
        esp_now_add_peer(&peerInfo);
    }

    // Build a minimal queued command (avoid copying full 250-byte packet).
    SensorCmd cmd = {};
    memcpy(cmd.src_mac, src_mac, 6);
    cmd.msg_type = msg_type;
    cmd.req_seq  = (len >= (int)MSG_HEADER_SIZE)
                   ? ((const MsgHeader*)data)->seq_id : 0;

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
            handle_cmd(cmd.src_mac, cmd.req_seq,
                       cmd.payload, cmd.payload_len);
            break;
        default:
            break;
        }
    }
}

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

    printf("Sensor ready — name=%s\n", g_espnow_module_name);

    // ---- Main loop ----
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
