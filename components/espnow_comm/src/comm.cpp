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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// MAC address formatting (esp_mac.h may not always be includable)
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

// ----------------------------------------------------------------
// Module name (set by sensor before send_announce)
// ----------------------------------------------------------------
char    g_espnow_module_name[17] = "sensor";

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

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %d", ret);
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

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %d", ret);
        return ret;
    }

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

    // ---- Add broadcast peer (for announce) ----
    esp_now_peer_info_t peer = {};
    peer.channel = channel;
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

    // ---- Initialize peer manager (must be done before any age_scan) ----
    peer_mgr_init();

    ESP_LOGI(TAG, "ESP-NOW comm initialized (channel %d)", channel);
    return ESP_OK;
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
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "send status: %d", (int)status);
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t* info,
                           const uint8_t* data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > 250) return;
    if (s_rx_queue == NULL) return;

    espnow_rx_item_t item;
    memcpy(item.src_mac, info->src_addr, 6);
    memcpy(item.data, data, (size_t)len);
    item.len = len;

    // Push to queue (non-blocking; drop if full)
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(s_rx_queue, &item, &woke);
    // No yield needed — our task is same or lower priority
    (void)woke;
}

// ----------------------------------------------------------------
// RX task  — processes one packet per iteration
// ----------------------------------------------------------------
static void rx_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");

    while (1) {
        if (rx_process_one() < 0) {
            // Queue returned empty (shouldn't happen with portMAX_DELAY)
            taskYIELD();
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
        // Payload: [16B name (zero-padded)] — no module_id (master assigns it)
#if CONFIG_DEVICE_ROLE_MASTER
        if (item.len < (int)(MSG_HEADER_SIZE + 1)) break;
        const char* ann_name = (const char*)(item.data + MSG_HEADER_SIZE);
        peer_mgr_handle_announce(item.src_mac, ann_name);
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
void espnow_comm_send_announce(void)
{
    uint8_t buf[250];
    size_t  len = 0;

    protocol_build_announce(buf, &len, g_espnow_module_name);

    esp_err_t ret = esp_now_send(s_broadcast_mac, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "announce send failed: %d", ret);
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

    s_current_seq_id++;
    protocol_build_cmd(buf, &len, module_id, s_current_seq_id,
                       cmd_id, payload, payload_len);

    // Find target peer
    PeerEntry* peer = peer_mgr_find_by_id(module_id, NULL);
    if (peer == NULL) {
        ESP_LOGW(TAG, "send_cmd: peer module_id=%u not found", module_id);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = esp_now_send(peer->mac, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_cmd to module_id=%u failed: %d", module_id, ret);
    }
    return ret;
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

    s_resp_pending = true;
    memcpy(s_resp_expected_mac, dst_mac, 6);
    COMM_UNLOCK();

    // ---- Step 3: retry loop (up to 3 attempts) ----
    int  result_count = 0;
    bool got_response = false;

    for (int retry = 0; retry < 3; retry++) {
        // Each retry gets a new seq_id (design.md §7.3)
        s_current_seq_id++;
        uint8_t seq_id = s_current_seq_id;

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
