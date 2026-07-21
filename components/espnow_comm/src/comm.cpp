/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — ESP-NOW communication layer.
 *
 * Implements device discovery (announce), peer management integration,
 * synchronous request-read with retry, and command/announce sending.
 *
 * DESIGN CONSTRAINTS:
 *   - No std::vector/map/string, no exceptions, no dynamic allocation.
 *   - TOCTOU-safe: lock → copy to local → unlock → use local.
 *   - RX processing runs in a dedicated FreeRTOS task created during init.
 *   - Master-only features guarded by #ifdef CONFIG_DEVICE_ROLE_MASTER.
 */

#include "sdkconfig.h"
#include "espnow_comm/comm.h"
#include "espnow_comm/protocol.h"
#include "espnow_comm/peer_mgr.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// MAC address formatting (esp_mac.h may not always be includable)
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

// ----------------------------------------------------------------
// Module name + capability (set by sensor before send_announce)
// ----------------------------------------------------------------
char    g_espnow_module_name[17] = "sensor";
char    g_espnow_module_capability[CONFIG_MAX_CAPABILITY_LEN] = "";

// ----------------------------------------------------------------
// Log tag
// ----------------------------------------------------------------
static const char* TAG = "espnow_comm";

// ----------------------------------------------------------------
// Broadcast MAC
// ----------------------------------------------------------------
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ----------------------------------------------------------------
// Queue item — holds received data together with source MAC
// ----------------------------------------------------------------
typedef struct {
    uint8_t src_mac[6];      // sender MAC address
    uint8_t data[250];       // raw packet (max ESP-NOW payload)
    int     len;             // actual data length
} espnow_rx_item_t;

// ----------------------------------------------------------------
// Static globals
// ----------------------------------------------------------------
static SemaphoreHandle_t s_comm_mutex     = NULL;
static SemaphoreHandle_t s_resp_sem       = NULL;   // binary semaphore for sync read
static QueueHandle_t     s_rx_queue       = NULL;
static TaskHandle_t      s_rx_task_handle = NULL;   // rx processing task

static uint8_t  s_current_seq_id    = 0;
static bool     s_resp_pending       = false;
static uint8_t  s_resp_expected_mac[6];
static uint8_t  s_resp_expected_seq  = 0;
static double   s_resp_values[DATA_RESP_MAX_VALUES];
static int      s_resp_value_count   = 0;

typedef struct {
    bool in_use;
    uint8_t mac[6];
    espnow_command_status_t status;
} command_record_t;

static command_record_t s_command_records[CONFIG_MAX_PEERS];
static portMUX_TYPE s_command_lock = portMUX_INITIALIZER_UNLOCKED;

// Sensor-mode callback
static espnow_recv_callback_t s_recv_callback = NULL;

// ----------------------------------------------------------------
// Lock macros
// ----------------------------------------------------------------
#define COMM_LOCK()   xSemaphoreTake(s_comm_mutex, portMAX_DELAY)
#define COMM_UNLOCK() xSemaphoreGive(s_comm_mutex)

// ----------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------
static void  espnow_recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static void  espnow_send_cb(const uint8_t* mac_addr, esp_now_send_status_t status);
static void  rx_task(void* arg);
static int   rx_process_one(void);

static uint32_t command_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t next_sequence_id(void)
{
    taskENTER_CRITICAL(&s_command_lock);
    const uint8_t seq_id = ++s_current_seq_id;
    taskEXIT_CRITICAL(&s_command_lock);
    return seq_id;
}

static uint32_t command_timeout_ms(uint16_t cmd_id,
                                   const uint8_t* payload,
                                   uint8_t payload_len)
{
    if (cmd_id == CMD_SERVO_WRITE) {
        return 2000;
    }
    if (cmd_id == CMD_BUZZER_NOTE && payload != NULL && payload_len >= 3) {
        const uint32_t duration = ((uint32_t)payload[1] << 8) | payload[2];
        return duration + 2000;
    }
    if (cmd_id == CMD_BUZZER_SONG) {
        return 30000;
    }
    if (cmd_id == CMD_BUZZER_MELODY && payload != NULL) {
        uint32_t duration = 0;
        for (uint8_t i = 0; i + 2 < payload_len; i = (uint8_t)(i + 3)) {
            duration += ((uint32_t)payload[i + 1] << 8) | payload[i + 2];
            duration += 20;
        }
        duration += 2000;
        return duration > 60000 ? 60000 : duration;
    }
    return 5000;
}

static command_record_t* find_command_record_locked(uint8_t module_id)
{
    command_record_t* free_slot = NULL;
    command_record_t* oldest = &s_command_records[0];
    for (int i = 0; i < CONFIG_MAX_PEERS; i++) {
        command_record_t* record = &s_command_records[i];
        if (record->in_use && record->status.module_id == module_id) {
            return record;
        }
        if (!record->in_use && free_slot == NULL) {
            free_slot = record;
        }
        if (record->status.sent_at_ms < oldest->status.sent_at_ms) {
            oldest = record;
        }
    }
    return free_slot != NULL ? free_slot : oldest;
}

static void command_record_start(uint8_t module_id, uint8_t seq_id,
                                 uint16_t cmd_id, const uint8_t* payload,
                                 uint8_t payload_len)
{
    taskENTER_CRITICAL(&s_command_lock);
    command_record_t* record = find_command_record_locked(module_id);
    memset(record, 0, sizeof(*record));
    record->in_use = true;
    record->status.valid = true;
    record->status.module_id = module_id;
    record->status.seq_id = seq_id;
    record->status.cmd_id = cmd_id;
    record->status.state = ESPNOW_COMMAND_PENDING;
    record->status.error = ESP_OK;
    record->status.sent_at_ms = command_now_ms();
    record->status.timeout_ms = command_timeout_ms(cmd_id, payload, payload_len);
    record->status.payload_len = payload_len > ESPNOW_COMMAND_PREVIEW_MAX
                                     ? ESPNOW_COMMAND_PREVIEW_MAX
                                     : payload_len;
    if (payload != NULL && record->status.payload_len > 0) {
        memcpy(record->status.payload, payload, record->status.payload_len);
    }
    taskEXIT_CRITICAL(&s_command_lock);
}

static void command_record_set_mac(uint8_t module_id, uint8_t seq_id,
                                   const uint8_t* mac)
{
    if (mac == NULL) return;
    taskENTER_CRITICAL(&s_command_lock);
    command_record_t* record = find_command_record_locked(module_id);
    if (record->in_use && record->status.seq_id == seq_id) {
        memcpy(record->mac, mac, sizeof(record->mac));
    }
    taskEXIT_CRITICAL(&s_command_lock);
}

static void command_record_fail(uint8_t module_id, uint8_t seq_id,
                                esp_err_t error)
{
    taskENTER_CRITICAL(&s_command_lock);
    command_record_t* record = find_command_record_locked(module_id);
    if (record->in_use && record->status.seq_id == seq_id) {
        record->status.state = ESPNOW_COMMAND_SEND_FAILED;
        record->status.error = error;
    }
    taskEXIT_CRITICAL(&s_command_lock);
}

static void command_record_confirm(const uint8_t* mac, uint8_t seq_id)
{
    if (mac == NULL) return;
    const uint32_t now_ms = command_now_ms();
    taskENTER_CRITICAL(&s_command_lock);
    for (int i = 0; i < CONFIG_MAX_PEERS; i++) {
        command_record_t* record = &s_command_records[i];
        if (!record->in_use || record->status.seq_id != seq_id ||
            memcmp(record->mac, mac, sizeof(record->mac)) != 0 ||
            record->status.state != ESPNOW_COMMAND_PENDING) {
            continue;
        }
        if ((uint32_t)(now_ms - record->status.sent_at_ms) >
            record->status.timeout_ms) {
            record->status.state = ESPNOW_COMMAND_TIMED_OUT;
            record->status.error = ESP_ERR_TIMEOUT;
        } else {
            record->status.state = ESPNOW_COMMAND_CONFIRMED;
            record->status.error = ESP_OK;
        }
        break;
    }
    taskEXIT_CRITICAL(&s_command_lock);
}

static esp_err_t ensure_espnow_peer_registered(const uint8_t* mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    peer.channel = CONFIG_SOFTAP_CHANNEL;
#else
    peer.channel = 1;
#endif
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "re-add peer " MACSTR " failed: %d", MAC2STR(mac), ret);
        return ret;
    }

    ESP_LOGI(TAG, "re-added ESP-NOW peer " MACSTR " on channel %u",
             MAC2STR(mac), (unsigned)peer.channel);
    return ESP_OK;
}

// ----------------------------------------------------------------
// Init / Deinit
// ----------------------------------------------------------------
esp_err_t espnow_comm_init(void)
{
    esp_err_t ret;

    // ---- Wi-Fi + netif ----
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_INIT_FAILED) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", ret);
        return ret;
    }

    // Create default event loop (idempotent in newer IDF)
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", ret);
        return ret;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d", ret);
        return ret;
    }

    // Create STA netif BEFORE esp_wifi_start().
    // Without this, lwIP has no netif for STA, DHCP never runs,
    // GOT_IP never fires, and all internet connectivity fails.
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "STA netif created: %p", (void*)sta_netif);

    // IMPORTANT: Must set mode BEFORE esp_wifi_start().
    // Use APSTA mode (not pure STA) to avoid BSSID broadcast filtering.
    // In pure STA mode, ESP32-S3/C3 WiFi hardware drops broadcast packets
    // from non-AP sources (espressif/esp-now#57). APSTA mode disables
    // this filter, allowing ESP-NOW broadcast reception.
    // For master, web_console.cpp start_softap() will transition to full
    // APSTA later, so this is also a compatible starting point.
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %d", ret);
        return ret;
    }

    // Initialize STA config (empty SSID) — required for proper WiFi MAC
    // initialization on ESP32-S3 even when not connecting to an AP.
    wifi_config_t wifi_config = {};
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    // Store WiFi config in RAM only, not NVS (avoids stale config interference)
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %d", ret);
        return ret;
    }

    // ESP-NOW requires modem sleep disabled — otherwise broadcast
    // announces and other ESP-NOW packets are missed while the STA
    // interface is idle (not connected to any AP).
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Reduce TX power on ESP32-C3 sensor to mitigate antenna
    // reflection issues common on C3 boards (Arduino-ESP32 #6767).
    // Also reduces heat on the single-core C3.
    // Value: 34 = 8.5 dBm (ESP-IDF uses 0.25 dBm units).
#if CONFIG_DEVICE_ROLE_SENSOR
    esp_wifi_set_max_tx_power(34);
#endif

    // Set channel — prefer CONFIG_SOFTAP_CHANNEL if available
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    uint8_t channel = CONFIG_SOFTAP_CHANNEL;
#else
    uint8_t channel = 1;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    // ---- ESP-NOW ----
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", ret);
        return ret;
    }

    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    // ---- Add broadcast peer (for announce / discovery) ----
    // channel=0 follows the current Wi-Fi channel after SoftAP or STA events.
    esp_now_peer_info_t peer = {};
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer(broadcast) failed: %d", ret);
    }

    // ---- Create RX queue ----
    s_rx_queue = xQueueCreate(8, sizeof(espnow_rx_item_t));
    if (s_rx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create RX queue");
        return ESP_ERR_NO_MEM;
    }

    // ---- Create sync semaphore ----
    s_resp_sem = xSemaphoreCreateBinary();
    if (s_resp_sem == NULL) {
        ESP_LOGE(TAG, "failed to create response semaphore");
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    // ---- Create comm mutex ----
    s_comm_mutex = xSemaphoreCreateMutex();
    if (s_comm_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create comm mutex");
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        vSemaphoreDelete(s_resp_sem);
        s_resp_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    // ---- Initialize peer manager BEFORE creating rx_task ----
    // MUST be done first: rx_task (prio 5) can preempt app_main and call
    // peer_mgr_handle_announce() which needs s_peer_mutex to exist.
    peer_mgr_init();

    // ---- Create RX processing task ----
    BaseType_t task_ok = xTaskCreate(rx_task, "espnow_rx", 4096, NULL, 5, &s_rx_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create RX task");
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        vSemaphoreDelete(s_resp_sem);
        s_resp_sem = NULL;
        vSemaphoreDelete(s_comm_mutex);
        s_comm_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ESP-NOW comm initialized (channel %d)", channel);
    return ESP_OK;
}

// ----------------------------------------------------------------
// RF sync — restore channel + broadcast peer after Wi-Fi mode changes
// ----------------------------------------------------------------

static void ensure_broadcast_peer(void)
{
    esp_now_peer_info_t peer = {};
    peer.channel = 0;   // follow current primary channel
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_broadcast_mac, 6);

    if (esp_now_is_peer_exist(s_broadcast_mac)) {
        esp_err_t ret = esp_now_mod_peer(&peer);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_mod_peer(broadcast) failed: %d", ret);
        }
    } else {
        esp_err_t ret = esp_now_add_peer(&peer);
        if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "esp_now_add_peer(broadcast) failed: %d", ret);
        }
    }
}

void espnow_comm_sync_rf(void)
{
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    uint8_t want_ch = CONFIG_SOFTAP_CHANNEL;
#else
    uint8_t want_ch = 1;
#endif

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&primary, &second);

    wifi_ps_type_t current_ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&current_ps) != ESP_OK || current_ps != WIFI_PS_NONE) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }

    // Do not ask the driver to change to the channel it is already using.
    // With a phone connected to the SoftAP, even this no-op request is
    // rejected and takes the deeper Wi-Fi error/logging path every 3 seconds.
    if (err != ESP_OK || primary != want_ch) {
        err = esp_wifi_set_channel(want_ch, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %d", want_ch, err);
        } else {
            primary = want_ch;
            ESP_LOGI(TAG, "RF channel restored to %u", want_ch);
        }
    }

    ensure_broadcast_peer();
}

void espnow_comm_send_discovery(void)
{
#if CONFIG_DEVICE_ROLE_MASTER
    uint8_t buf[250];
    size_t  len = 0;

    protocol_build_announce(buf, &len, "master", "");
    esp_err_t ret = esp_now_send(s_broadcast_mac, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "discovery send failed: %d", ret);
    } else {
        ESP_LOGD(TAG, "discovery broadcast sent");
    }
#else
    (void)0;
#endif
}

// ----------------------------------------------------------------
// Suspend / Resume RX (used by wifi_scan — design.md §16.11)
// ----------------------------------------------------------------
void espnow_comm_suspend_rx(void)
{
    if (s_rx_task_handle != NULL) {
        vTaskSuspend(s_rx_task_handle);
    }
}

void espnow_comm_resume_rx(void)
{
    // Discard stale packets accumulated during scan
    if (s_rx_queue != NULL) {
        xQueueReset(s_rx_queue);
    }
    if (s_rx_task_handle != NULL) {
        vTaskResume(s_rx_task_handle);
    }
    espnow_comm_sync_rf();
}

// ----------------------------------------------------------------
// Re-init ESP-NOW layer after WiFi stop/start cycle
// ----------------------------------------------------------------
esp_err_t espnow_comm_reinit_espnow(void)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init after wifi restart failed: %d", ret);
        return ret;
    }

    ret = esp_now_register_send_cb(espnow_send_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_send_cb failed: %d", ret);
        return ret;
    }
    ret = esp_now_register_recv_cb(espnow_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %d", ret);
        return ret;
    }

    // Re-add broadcast peer
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    uint8_t channel = CONFIG_SOFTAP_CHANNEL;
#else
    uint8_t channel = 1;
#endif
    esp_now_peer_info_t peer = {};
    peer.channel = channel;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "re-add broadcast peer failed: %d", ret);
    }

    // Reset rx queue (discard stale packets)
    if (s_rx_queue != NULL) {
        xQueueReset(s_rx_queue);
    }

    ESP_LOGI(TAG, "ESP-NOW re-initialised after wifi restart");
    return ESP_OK;
}

void espnow_comm_deinit(void)
{
    // Stop the RX task first
    if (s_rx_task_handle != NULL) {
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }

    esp_now_deinit();
    esp_wifi_stop();

    // Clean up resources
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    if (s_resp_sem != NULL) {
        vSemaphoreDelete(s_resp_sem);
        s_resp_sem = NULL;
    }
    if (s_comm_mutex != NULL) {
        vSemaphoreDelete(s_comm_mutex);
        s_comm_mutex = NULL;
    }

    s_recv_callback = NULL;
    s_resp_pending = false;
    taskENTER_CRITICAL(&s_command_lock);
    memset(s_command_records, 0, sizeof(s_command_records));
    taskEXIT_CRITICAL(&s_command_lock);

    ESP_LOGI(TAG, "ESP-NOW comm deinitialized");
}

// ----------------------------------------------------------------
// Register sensor-mode callback
// ----------------------------------------------------------------
void espnow_comm_register_recv_callback(espnow_recv_callback_t cb)
{
    s_recv_callback = cb;
}

// ----------------------------------------------------------------
// Send / recv callbacks  (called from Wi-Fi task context, NOT ISR)
// ----------------------------------------------------------------
static void espnow_send_cb(const uint8_t* mac_addr, esp_now_send_status_t status)
{
    static int s_send_fail_count = 0;
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_send_fail_count++;
        ESP_LOGW(TAG, "send status: %d (fail #%d)", (int)status, s_send_fail_count);
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t* info,
                           const uint8_t* data, int len)
{
    // Diagnostic: log every received packet (at most first 3, then periodic)
    static int s_recv_count = 0;
    s_recv_count++;
    if (s_recv_count <= 3 || s_recv_count % 100 == 0) {
        if (info && info->src_addr) {
            ESP_LOGI(TAG, "ESP-NOW rx #%d: src=" MACSTR " len=%d",
                     s_recv_count, MAC2STR(info->src_addr), len);
        } else {
            ESP_LOGI(TAG, "ESP-NOW rx #%d: src=NULL len=%d", s_recv_count, len);
        }
    }

    if (info == NULL || data == NULL || len <= 0 || len > 250) return;
    if (s_rx_queue == NULL) return;

    espnow_rx_item_t item;
    memcpy(item.src_mac, info->src_addr, 6);
    memcpy(item.data, data, (size_t)len);
    item.len = len;

    // Push to queue (non-blocking; drop if full).
    // espnow_recv_cb runs in Wi-Fi task context (NOT an ISR), so use the
    // normal xQueueSend (timeout=0) rather than the ISR variant.
    BaseType_t ok = xQueueSend(s_rx_queue, &item, 0);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, packet from " MACSTR " dropped",
                 MAC2STR(info->src_addr));
    }
}

// ----------------------------------------------------------------
// RX task  — processes one packet per iteration
// ----------------------------------------------------------------
// Diagnostic counters (rate-limited logs for debugging communication)
static int s_rx_packet_count  = 0;
static int s_ann_rx_count     = 0;

static void rx_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");

    while (1) {
        if (rx_process_one() < 0) {
            // Queue returned empty (shouldn't happen with portMAX_DELAY)
            taskYIELD();
        }
        s_rx_packet_count++;
        if (s_rx_packet_count % 20 == 0) {
            ESP_LOGI(TAG, "rx: %d packets received (%d announces)",
                     s_rx_packet_count, s_ann_rx_count);
        }
    }
}

// Process one received packet.  Blocks until a packet is available.
// Returns 0 on success, -1 on error.
static int rx_process_one(void)
{
    espnow_rx_item_t item;
    if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    MsgHeader hdr;
    if (!protocol_parse_header(item.data, item.len, &hdr)) {
        ESP_LOGW(TAG, "invalid packet from " MACSTR " (len=%d)",
                 MAC2STR(item.src_mac), item.len);
        return 0;
    }

    switch (hdr.msg_type) {

    case MSG_ANNOUNCE: {
#if CONFIG_DEVICE_ROLE_MASTER
        // Minimum: hdr(7) + name(16) = 23 bytes
        if (item.len < (int)(MSG_HEADER_SIZE + 16)) break;
        char ann_name[32];
        char ann_cap[CONFIG_MAX_CAPABILITY_LEN];
        protocol_parse_announce(item.data, item.len,
                                 ann_name, sizeof(ann_name),
                                 ann_cap, sizeof(ann_cap));
        peer_mgr_handle_announce(item.src_mac, ann_name, ann_cap);
        s_ann_rx_count++;
#endif
        break;
    }

    case MSG_DATA_RESP:
        espnow_comm_handle_resp(item.src_mac, item.data, item.len);
        break;

    case MSG_CMD:
    case MSG_DATA_REQ:
        if (s_recv_callback != NULL) {
            s_recv_callback(item.src_mac, hdr.msg_type, item.data, item.len);
        } else {
            ESP_LOGD(TAG, "unhandled 0x%02x (no callback registered)", hdr.msg_type);
        }
        break;

    case MSG_ACK:
        command_record_confirm(item.src_mac, hdr.seq_id);
        ESP_LOGD(TAG, "ACK seq=%u from " MACSTR, hdr.seq_id, MAC2STR(item.src_mac));
        break;

    default:
        ESP_LOGW(TAG, "unknown msg_type 0x%02x from " MACSTR,
                 hdr.msg_type, MAC2STR(item.src_mac));
        break;
    }

    return 0;
}

// ----------------------------------------------------------------
// Send announce (sensor → broadcast)
// ----------------------------------------------------------------
static int s_announce_sent_count = 0;

void espnow_comm_send_announce(void)
{
    uint8_t buf[250];
    size_t  len = 0;

    protocol_build_announce(buf, &len, g_espnow_module_name,
                             g_espnow_module_capability);

    esp_err_t ret = esp_now_send(s_broadcast_mac, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "announce send failed: %d", ret);
    } else {
        s_announce_sent_count++;
        // Log periodically (every 10 announces ≈ 30s) to confirm sensor is broadcasting
        if (s_announce_sent_count % 10 == 0) {
            ESP_LOGI(TAG, "announce sent x%d (name=%s, cap=%s)",
                     s_announce_sent_count, g_espnow_module_name,
                     g_espnow_module_capability);
        }
    }
}

// ----------------------------------------------------------------
// Send command (master → sensor) — fire-and-forget with single send
// ----------------------------------------------------------------
esp_err_t espnow_comm_send_cmd(uint8_t module_id, uint16_t cmd_id,
                                const uint8_t* payload, uint8_t payload_len)
{
    uint8_t buf[250];
    size_t  len = 0;

    const uint8_t seq_id = next_sequence_id();
    command_record_start(module_id, seq_id, cmd_id, payload, payload_len);
    protocol_build_cmd(buf, &len, module_id, seq_id,
                       cmd_id, payload, payload_len);

    // Find target peer
    PeerEntry* peer = peer_mgr_find_by_id(module_id, NULL);
    if (peer == NULL) {
        ESP_LOGW(TAG, "send_cmd: peer module_id=%u not found", module_id);
        command_record_fail(module_id, seq_id, ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }
    command_record_set_mac(module_id, seq_id, peer->mac);

    esp_err_t peer_ret = ensure_espnow_peer_registered(peer->mac);
    if (peer_ret != ESP_OK) {
        command_record_fail(module_id, seq_id, peer_ret);
        return peer_ret;
    }

    ESP_LOGI(TAG, "send_cmd module_id=%u cmd=0x%04x to " MACSTR " name=%s state=%d",
             module_id, (unsigned)cmd_id, MAC2STR(peer->mac),
             peer->name, (int)peer->state);

    esp_err_t ret = esp_now_send(peer->mac, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_cmd to module_id=%u failed: %d", module_id, ret);
        command_record_fail(module_id, seq_id, ret);
    }
    return ret;
}

bool espnow_comm_get_command_status(uint8_t module_id,
                                   espnow_command_status_t* out_status)
{
    if (out_status == NULL) {
        return false;
    }

    bool found = false;
    const uint32_t now_ms = command_now_ms();
    taskENTER_CRITICAL(&s_command_lock);
    for (int i = 0; i < CONFIG_MAX_PEERS; i++) {
        command_record_t* record = &s_command_records[i];
        if (!record->in_use || record->status.module_id != module_id) {
            continue;
        }
        if (record->status.state == ESPNOW_COMMAND_PENDING &&
            (uint32_t)(now_ms - record->status.sent_at_ms) >
                record->status.timeout_ms) {
            record->status.state = ESPNOW_COMMAND_TIMED_OUT;
            record->status.error = ESP_ERR_TIMEOUT;
        }
        *out_status = record->status;
        found = true;
        break;
    }
    taskEXIT_CRITICAL(&s_command_lock);
    if (!found) {
        memset(out_status, 0, sizeof(*out_status));
    }
    return found;
}

// ----------------------------------------------------------------
// Response handler — called from rx_task or externally
// ----------------------------------------------------------------
void espnow_comm_handle_resp(const uint8_t* src_mac,
                              const uint8_t* data, int len)
{
    if (src_mac == NULL || data == NULL) return;

    COMM_LOCK();

    if (!s_resp_pending) {
        COMM_UNLOCK();
        return;     // no pending request — ignore unsolicited response
    }

    // Validate source MAC
    if (memcmp(src_mac, s_resp_expected_mac, 6) != 0) {
        ESP_LOGW(TAG, "response mismatch: unexpected source " MACSTR,
                 MAC2STR(src_mac));
        COMM_UNLOCK();
        return;
    }

    // Extract and verify seq_id
    MsgHeader hdr;
    if (!protocol_parse_header(data, len, &hdr)) {
        COMM_UNLOCK();
        return;
    }

    if (hdr.seq_id != s_resp_expected_seq) {
        ESP_LOGW(TAG, "response seq mismatch: expected %u got %u",
                 s_resp_expected_seq, hdr.seq_id);
        COMM_UNLOCK();
        return;
    }

    // Extract values array from DATA_RESP payload
    int n = protocol_extract_values(data, len, s_resp_values, DATA_RESP_MAX_VALUES);
    if (n <= 0) {
        ESP_LOGW(TAG, "response: failed to extract values");
        COMM_UNLOCK();
        return;
    }
    s_resp_value_count = n;

    // Signal the waiting task
    xSemaphoreGive(s_resp_sem);
    COMM_UNLOCK();
}

// ----------------------------------------------------------------
// Synchronous read request  (master only)
// ----------------------------------------------------------------
int espnow_comm_request_read(uint8_t module_id, double* out_values, int max_values)
{
#if CONFIG_DEVICE_ROLE_MASTER

    // ---- Step 1: lock and check pending ----
    COMM_LOCK();
    if (s_resp_pending) {
        COMM_UNLOCK();
        ESP_LOGW(TAG, "request_read(%u): concurrent request rejected", module_id);
        return 0;
    }

    // ---- Step 2: find peer and set pending ----
    PeerEntry* peer = peer_mgr_find_by_id(module_id, NULL);
    if (peer == NULL) {
        COMM_UNLOCK();
        ESP_LOGW(TAG, "request_read(%u): peer not found", module_id);
        return 0;
    }

    // TOCTOU-safe: copy MAC inside lock
    uint8_t dst_mac[6];
    memcpy(dst_mac, peer->mac, 6);

    esp_err_t peer_ret = ensure_espnow_peer_registered(dst_mac);
    if (peer_ret != ESP_OK) {
        COMM_UNLOCK();
        ESP_LOGW(TAG, "request_read(%u): re-add peer failed: %d", module_id, peer_ret);
        return 0;
    }

    s_resp_pending = true;
    memcpy(s_resp_expected_mac, dst_mac, 6);
    COMM_UNLOCK();

    // ---- Step 3: retry loop (up to 3 attempts) ----
    int  result_count = 0;
    bool got_response = false;

    for (int retry = 0; retry < 3; retry++) {
        // Each retry gets a new seq_id (design.md §7.3)
        uint8_t seq_id = next_sequence_id();

        uint8_t buf[250];
        size_t  len = 0;
        protocol_build_data_req(buf, &len, module_id, seq_id);

        COMM_LOCK();
        s_resp_expected_seq = seq_id;
        // Reset the binary semaphore before sending (it may be in "given" state)
        xSemaphoreTake(s_resp_sem, 0);
        COMM_UNLOCK();

        esp_err_t send_ret = esp_now_send(dst_mac, buf, len);
        if (send_ret != ESP_OK) {
            ESP_LOGW(TAG, "request_read(%u): send failed on attempt %d",
                     module_id, retry + 1);
            if (retry < 2) continue;
            break;
        }

        // Wait for response
        if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(CONFIG_READ_TIMEOUT_MS)) == pdTRUE) {
            COMM_LOCK();
            int n = s_resp_value_count;
            if (n > max_values) n = max_values;
            for (int i = 0; i < n; i++) {
                out_values[i] = s_resp_values[i];
            }
            s_resp_pending = false;
            COMM_UNLOCK();
            result_count = n;
            got_response = true;
            break;
        }

        ESP_LOGW(TAG, "request_read(%u): timeout on attempt %d/%d",
                 module_id, retry + 1, 3);
    }

    if (!got_response) {
        COMM_LOCK();
        s_resp_pending = false;
        COMM_UNLOCK();
        ESP_LOGE(TAG, "request_read(%u): timeout after 3 attempts", module_id);
    }

    return result_count;

#else
    // Sensor mode — not supported
    ESP_LOGW(TAG, "request_read called on SENSOR build — ignored");
    (void)module_id;
    (void)out_values;
    (void)max_values;
    return 0;
#endif
}
