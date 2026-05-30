/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Sensor-side TCP client
 *
 * Connects to master's SoftAP + TCP server, sends IDENTIFY_ACK,
 * and dispatches DATA_REQ / CMD messages to the app callback.
 * Reconnects with exponential backoff on disconnect.
 */

#include "sdkconfig.h"
#include "tcp_comm/tcp_comm.h"
#include "tcp_comm/tcp_protocol.h"
#include "espnow_comm/protocol.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

/*
 * WiFi defaults — can be overridden via Kconfig.  To add proper
 * Kconfig entries in tcp_comm/Kconfig:
 *
 *   config TCP_WIFI_SSID
 *       string "Master SoftAP SSID"
 *       default "ESP_LEGO_MASTER"
 *
 *   config TCP_WIFI_PASSWORD
 *       string "Master SoftAP Password"
 *       default ""
 *
 *   config TCP_WIFI_CONNECT_TIMEOUT_MS
 *       int "WiFi connect timeout (ms)"
 *       default 10000
 */
#ifndef CONFIG_TCP_WIFI_SSID
#define CONFIG_TCP_WIFI_SSID        "ESP_LEGO_MASTER"
#endif
#ifndef CONFIG_TCP_WIFI_PASSWORD
#define CONFIG_TCP_WIFI_PASSWORD    ""
#endif
#ifndef CONFIG_TCP_WIFI_CONNECT_TIMEOUT_MS
#define CONFIG_TCP_WIFI_CONNECT_TIMEOUT_MS  10000
#endif

static const char* TAG = "tcp_cli";

// ====================================================================
// Sensor globals
// ====================================================================
static int               s_sock              = -1;
static SemaphoreHandle_t s_comm_mutex        = NULL;
static TaskHandle_t      s_client_task_handle = NULL;
static tcp_recv_callback_t s_recv_callback   = NULL;
static bool              s_connected         = false;
static int               s_reconnect_delay_ms = CONFIG_TCP_RECONNECT_BASE_MS;

#define COMM_LOCK()   xSemaphoreTake(s_comm_mutex, portMAX_DELAY)
#define COMM_UNLOCK() xSemaphoreGive(s_comm_mutex)

// ====================================================================
// Forward declarations
// ====================================================================
static void tcp_client_task(void* arg);
static int  try_connect(void);
static bool send_identify_ack(int sock, uint8_t seq_id);

// ====================================================================
// Init — creates the TCP client task
// ====================================================================
esp_err_t tcp_comm_init(void)
{
    s_comm_mutex = xSemaphoreCreateMutex();
    if (s_comm_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(tcp_client_task, "tcp_cli",
                                      CONFIG_TCP_COMM_STACK_SIZE,
                                      NULL, 5, &s_client_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create client task");
        vSemaphoreDelete(s_comm_mutex);
        s_comm_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP client initialized");
    return ESP_OK;
}

// ====================================================================
// Deinit — tear down task, socket, mutex
// ====================================================================
void tcp_comm_deinit(void)
{
    if (s_client_task_handle != NULL) {
        vTaskDelete(s_client_task_handle);
        s_client_task_handle = NULL;
    }

    COMM_LOCK();
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    s_connected = false;
    COMM_UNLOCK();

    if (s_comm_mutex != NULL) {
        vSemaphoreDelete(s_comm_mutex);
        s_comm_mutex = NULL;
    }
    s_recv_callback = NULL;

    ESP_LOGI(TAG, "TCP client deinitialized");
}

// ====================================================================
// Register receive callback
// ====================================================================
void tcp_comm_register_recv_callback(tcp_recv_callback_t cb)
{
    s_recv_callback = cb;
}

// ====================================================================
// WiFi STA connection helper (idempotent)
// ====================================================================
static void wifi_sta_init(void)
{
    static bool s_wifi_inited = false;
    if (s_wifi_inited) {
        return;
    }

    ESP_LOGI(TAG, "WiFi STA init, connecting to '%s'", CONFIG_TCP_WIFI_SSID);

    /* These are safe to call multiple times (later calls are no-ops
     * or return ESP_ERR_INVALID_STATE which we tolerate). */
    esp_netif_init();
    esp_event_loop_create_default();

    /* Only create default STA netif if one doesn't already exist */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_init: %d (ignored)", ret);
    }

    wifi_config_t wifi_config = { 0 };
    strlcpy((char*)wifi_config.sta.ssid,     CONFIG_TCP_WIFI_SSID,
             sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, CONFIG_TCP_WIFI_PASSWORD,
             sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) ESP_LOGW(TAG, "set_mode: %d", ret);

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) ESP_LOGW(TAG, "set_config: %d", ret);

    ret = esp_wifi_start();
    if (ret != ESP_OK) ESP_LOGW(TAG, "start: %d", ret);

    ret = esp_wifi_connect();
    if (ret != ESP_OK) ESP_LOGW(TAG, "connect: %d", ret);

    ESP_LOGI(TAG, "WiFi STA connecting...");
    s_wifi_inited = true;
}

/* Wait for WiFi to have L2 association + a valid IP address.
 * Returns ESP_OK once ready, ESP_ERR_TIMEOUT if the deadline passes. */
static esp_err_t wifi_wait_connected(void)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return ESP_ERR_NOT_FOUND;

    int waited = 0;
    while (waited < CONFIG_TCP_WIFI_CONNECT_TIMEOUT_MS) {
        /* Layer 2: associated? */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
            continue;
        }

        /* Layer 3: valid IP? */
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK ||
            ip.ip.addr == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
            continue;
        }

        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ip.ip));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "WiFi connect timeout (%d ms)",
             CONFIG_TCP_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

// ====================================================================
// Non-blocking connect with caller-specified timeout
// ====================================================================
static int connect_with_timeout(int fd, const struct sockaddr* addr,
                                 socklen_t addrlen, int timeout_ms)
{
    /* 1. Switch to non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 2. Initiate connection */
    int ret = connect(fd, addr, addrlen);
    if (ret == 0) {
        /* Connected immediately (unlikely for TCP, but handle it) */
        fcntl(fd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        /* Real error */
        return -1;
    }

    /* 3. Wait for completion with select() */
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(fd + 1, NULL, &write_fds, NULL, &tv);
    if (ret <= 0) {
        /* Timeout (0) or error (< 0) */
        return -1;
    }

    /* 4. Check socket error status */
    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
    if (so_error != 0) {
        errno = so_error;
        return -1;
    }

    /* 5. Restore blocking mode */
    fcntl(fd, F_SETFL, flags);
    return 0;
}

// ====================================================================
// Try TCP connect to master (returns fd on success, -1 on failure)
// ====================================================================
static int try_connect(void)
{
    /* --- Ensure WiFi is up first --- */
    if (wifi_wait_connected() != ESP_OK) {
        /* WiFi not ready — caller will retry with backoff */
        return -1;
    }

    /* --- Create socket --- */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        return -1;
    }

    /* --- TCP keepalive (detect dead connection) --- */
    int ka = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    int idle  = CONFIG_TCP_KEEPALIVE_IDLE_S;
    int intvl = CONFIG_TCP_KEEPALIVE_INTERVAL_S;
    int cnt   = CONFIG_TCP_KEEPALIVE_COUNT;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    /* --- Disable Nagle — small messages must go immediately --- */
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* --- Resolve and connect --- */
    struct sockaddr_in addr = { 0 };
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("192.168.4.1");
    addr.sin_port        = htons(CONFIG_TCP_PORT);

    ESP_LOGI(TAG, "connecting to 192.168.4.1:%d...", CONFIG_TCP_PORT);

    /* Use short timeout (5 s) so we don't block too long if the
     * master is unreachable — the retry loop will come back. */
    if (connect_with_timeout(sock, (struct sockaddr*)&addr,
                              sizeof(addr), 5000) < 0) {
        ESP_LOGW(TAG, "connect failed: %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "connected to master");
    return sock;
}

// ====================================================================
// Build and send IDENTIFY_ACK with module name + capability
// ====================================================================
static bool send_identify_ack(int sock, uint8_t seq_id)
{
    /* Payload layout: [MsgHeader][16B name][1B cap_len][cap_len B cap] */
    uint8_t pay[MSG_HEADER_SIZE + 16 + 1 + CONFIG_MAX_CAPABILITY_LEN];
    size_t  pay_len = 0;

    MsgHeader* hdr = (MsgHeader*)pay;
    hdr->version   = MSG_VERSION;
    hdr->msg_type  = MSG_IDENTIFY_ACK;
    hdr->target_id = 0xFF;
    hdr->seq_id    = seq_id;
    hdr->cmd_id    = 0;

    /* Name (16 bytes, zero-padded) */
    uint8_t* p = pay + MSG_HEADER_SIZE;
    size_t nlen = strlen(g_tcp_module_name);
    if (nlen > 16) nlen = 16;
    memcpy(p, g_tcp_module_name, nlen);
    if (nlen < 16) memset(p + nlen, 0, 16 - nlen);
    p += 16;

    /* Capability (length-prefixed) */
    size_t clen = strlen(g_tcp_module_capability);
    if (clen > CONFIG_MAX_CAPABILITY_LEN - 1) clen = CONFIG_MAX_CAPABILITY_LEN - 1;
    p[0] = (uint8_t)clen;
    memcpy(p + 1, g_tcp_module_capability, clen);

    hdr->payload_len = 16 + 1 + (uint16_t)clen;
    pay_len = MSG_HEADER_SIZE + 16 + 1 + clen;

    /* Frame with 2-byte length prefix and send */
    uint8_t  frame_buf[TCP_MAX_FRAME_SIZE];
    uint8_t* frame = NULL;
    int frame_len = tcp_protocol_frame(frame_buf, sizeof(frame_buf),
                                       pay, (int)pay_len, &frame);
    if (frame_len <= 0) return false;

    ssize_t sent = write(sock, frame, (size_t)frame_len);
    if (sent <= 0) return false;

    ESP_LOGI(TAG, "IDENTIFY_ACK sent: name='%s', cap_len=%d",
             g_tcp_module_name, (int)clen);
    return true;
}

// ====================================================================
// Main client task — connects, authenticates, dispatches, reconnects
// ====================================================================
static void tcp_client_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TCP client task started");

    /* One-time WiFi STA initialization */
    wifi_sta_init();

    while (1) {
        /* ============================================================
         * Phase 1 — connect (with exponential backoff)
         * ============================================================ */
        if (!s_connected) {
            int sock = try_connect();
            if (sock < 0) {
                ESP_LOGI(TAG, "retry in %d ms", s_reconnect_delay_ms);
                vTaskDelay(pdMS_TO_TICKS(s_reconnect_delay_ms));

                /* Exponential backoff: 1s → 2s → 4s → ... → max */
                s_reconnect_delay_ms =
                    (s_reconnect_delay_ms * 2 > CONFIG_TCP_RECONNECT_MAX_MS)
                        ? CONFIG_TCP_RECONNECT_MAX_MS
                        : s_reconnect_delay_ms * 2;
                continue;
            }

            COMM_LOCK();
            s_sock     = sock;
            s_connected = true;
            s_reconnect_delay_ms = CONFIG_TCP_RECONNECT_BASE_MS;
            COMM_UNLOCK();

            /* Send identity */
            if (!send_identify_ack(sock, 0)) {
                ESP_LOGW(TAG, "IDENTIFY_ACK send failed, disconnecting");
                COMM_LOCK();
                close(s_sock);
                s_sock = -1;
                s_connected = false;
                COMM_UNLOCK();
                continue;
            }
        }

        /* ============================================================
         * Phase 2 — connected: select() loop for incoming messages
         * ============================================================ */
        COMM_LOCK();
        int  sock      = s_sock;
        bool connected = s_connected;
        COMM_UNLOCK();

        if (!connected || sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);

        struct timeval tv;
        tv.tv_sec  = CONFIG_TCP_SELECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (CONFIG_TCP_SELECT_TIMEOUT_MS % 1000) * 1000;

        int activity = select(sock + 1, &read_fds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select error: %d", errno);
            goto disconnect;
        }
        if (activity == 0) {
            /* Timeout — loop back to re-check connected flag etc. */
            continue;
        }

        /* --- Read and dispatch messages (in own scope for goto safety) --- */
        {
        uint8_t frame_buf[TCP_MAX_FRAME_SIZE];
        int ret = tcp_protocol_read_frame(sock, frame_buf,
                                           sizeof(frame_buf),
                                           CONFIG_TCP_SELECT_TIMEOUT_MS);
        if (ret <= 0) {
            if (ret == 0) continue;  /* select() said ready but frame not
                                      * yet complete — retry loop */
            ESP_LOGW(TAG, "read error or peer closed connection");
            goto disconnect;
        }

        const uint8_t* msg     = frame_buf + TCP_FRAME_PREFIX_SIZE;
        int            msg_len = ret - TCP_FRAME_PREFIX_SIZE;

        MsgHeader hdr;
        if (!protocol_parse_header(msg, msg_len, &hdr)) {
            ESP_LOGW(TAG, "invalid message header");
            continue;
        }

        switch (hdr.msg_type) {

        case MSG_IDENTIFY:
            ESP_LOGI(TAG, "MSG_IDENTIFY from master, re-sending ACK");
            send_identify_ack(sock, hdr.seq_id);
            break;

        case MSG_DATA_REQ: {
            ESP_LOGI(TAG, "MSG_DATA_REQ (seq=%u)", hdr.seq_id);
            if (s_recv_callback != NULL) {
                s_recv_callback(NULL, hdr.msg_type, msg, msg_len);
            }
            break;
        }

        case MSG_CMD: {
            ESP_LOGI(TAG, "MSG_CMD (cmd_id=0x%04x, seq=%u)",
                     hdr.cmd_id, hdr.seq_id);
            if (s_recv_callback != NULL) {
                s_recv_callback(NULL, hdr.msg_type, msg, msg_len);
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "unknown msg_type 0x%02x, ignoring",
                     hdr.msg_type);
            break;
        }
        }
        continue;

    disconnect:
        COMM_LOCK();
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        s_connected = false;
        COMM_UNLOCK();
        ESP_LOGW(TAG, "disconnected — entering reconnect loop");
    }
}

// ====================================================================
// Public API — sensors don't initiate reads or send commands
// ====================================================================

/* Sensor never initiates a read — always responds to master's DATA_REQ */
int tcp_comm_request_read(uint8_t module_id, double* out_values,
                           int max_values)
{
    (void)module_id;
    (void)out_values;
    (void)max_values;
    return 0;
}

/* Sensor never sends commands to the master */
esp_err_t tcp_comm_send_cmd(uint8_t module_id, uint16_t cmd_id,
                             const uint8_t* payload, uint16_t payload_len)
{
    (void)module_id;
    (void)cmd_id;
    (void)payload;
    (void)payload_len;
    return ESP_ERR_NOT_SUPPORTED;
}

/* Send raw framed data over the current connection (used by app callback
 * to reply with DATA_RESP or ACK frames). */
int tcp_comm_send_raw(const uint8_t* data, int len)
{
    COMM_LOCK();
    int  sock      = s_sock;
    bool connected = s_connected;
    COMM_UNLOCK();

    if (!connected || sock < 0) return -1;

    uint8_t  frame_buf[TCP_MAX_FRAME_SIZE];
    uint8_t* frame = NULL;
    int frame_len = tcp_protocol_frame(frame_buf, sizeof(frame_buf),
                                       data, len, &frame);
    if (frame_len <= 0) return -1;

    ssize_t sent = write(sock, frame, (size_t)frame_len);
    return (sent > 0) ? (int)sent : -1;
}

// ====================================================================
// Module globals — set by app_main before tcp_comm_init()
// ====================================================================
char g_tcp_module_name[17]               = "sensor";
char g_tcp_module_capability[CONFIG_MAX_CAPABILITY_LEN] = "";
