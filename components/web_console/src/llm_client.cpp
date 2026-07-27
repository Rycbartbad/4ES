/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Web Console: LLM HTTP client.
 *
 * Implements:
 *   - WiFi mode switch: SoftAP → STA → LLM call → ESP-NOW channel (§16.5)
 *   - System prompt construction with BNF + device list + builtins
 *   - HTTP POST to OpenAI-compatible API
 *   - Response parsing with cJSON
 *   - Script extraction (strip markdown code fences)
 *
 * design.md §16.5, §16.6
 */

#include "sdkconfig.h"

#include "web_console/llm_client.h"
#include "web_console/script_inject.h"
#include "web_console/script_normalizer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "cJSON.h"

#include "espnow_comm/peer_mgr.h"
#include "espnow_comm/comm.h"
#include "interpreter/builtins.h"
#include "esp_now.h"

// ====================================================================
// Log tag
// ====================================================================
static const char* TAG = "llm_client";

// ====================================================================
// Event group bits for Wi-Fi connection
// ====================================================================
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;

// STA connection state — updated by wifi_event_handler and wifi_connect_sta
static bool s_sta_connected = false;

// ====================================================================
// ESP event handler for Wi-Fi connection
// ====================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // WiFi association succeeded — update state but do NOT signal
        // the event group yet.  We wait for IP_EVENT_STA_GOT_IP (DHCP)
        // so that the HTTP client has a valid IP to bind to.
        s_sta_connected = true;
        wifi_event_sta_connected_t* info = (wifi_event_sta_connected_t*)event_data;
        (void)info;
        ESP_LOGI(TAG, "STA Connected event: channel=%d", info->channel);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        // Route outbound DNS and HTTP through the STA interface.
        esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_set_default_netif(sta_netif);
        }

        // Preserve DNS supplied by the network's DHCP server. Campus and
        // corporate networks often block direct access to public resolvers,
        // so unconditionally replacing it with 8.8.8.8 breaks an otherwise
        // valid Wi-Fi connection. Use a public resolver only if DHCP supplied
        // no usable primary DNS at all.
        const ip_addr_t* primary_dns = dns_getserver(0);
        if (primary_dns == NULL || ip_addr_isany(primary_dns)) {
            ip_addr_t fallback_dns;
            IP_ADDR4(&fallback_dns, 8, 8, 8, 8);
            dns_setserver(0, &fallback_dns);
            ESP_LOGW(TAG, "DHCP supplied no DNS; using fallback 8.8.8.8");
        } else {
            ESP_LOGI(TAG, "Using DHCP DNS: " IPSTR,
                     IP2STR(&primary_dns->u_addr.ip4));
        }
        ESP_LOGI(TAG, "Default netif set to STA");
    }
}

// ====================================================================
// Public STA connection state
// ====================================================================

bool wifi_sta_is_connected(void)
{
    return s_sta_connected;
}

bool llm_client_uses_local_proxy(const char* llm_url)
{
    if (llm_url == NULL || strncmp(llm_url, "http://", 7) != 0) {
        return false;
    }

    const char* host = llm_url + 7;
    const char* end = host;
    while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#') {
        end++;
    }

    const char softap_prefix[] = "192.168.4.";
    const size_t prefix_len = sizeof(softap_prefix) - 1;
    return (size_t)(end - host) > prefix_len &&
           strncmp(host, softap_prefix, prefix_len) == 0;
}

static void set_softap_default_netif(void)
{
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_set_default_netif(ap_netif);
    } else {
        ESP_LOGW(TAG, "SoftAP netif handle unavailable");
    }
}

static bool restore_espnow_channel(void)
{
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    const uint8_t channel = CONFIG_SOFTAP_CHANNEL;
#else
    const uint8_t channel = 1;
#endif

    // 1. Disconnect STA. Keep the AP interface alive while restoring the
    // ESP-NOW channel: stopping Wi-Fi tears down the SoftAP entirely and
    // makes phones abandon ESP-LEGO-Setup for another saved network.
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "STA disconnect before channel restore failed: %d", ret);
    }
    s_sta_connected = false;

    // Let the STA disconnect event settle before changing the radio channel.
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Fast path: switch back without stopping Wi-Fi or reinitialising
    // ESP-NOW. Peers follow the current radio channel, so this retains the
    // SoftAP while the browser remains connected.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // If STA and SoftAP were already on the configured ESP-NOW channel,
    // setting that same channel is unnecessary. The driver rejects even a
    // no-op set_channel() while a phone is associated with the SoftAP, which
    // previously sent us into the destructive Wi-Fi restart fallback.
    uint8_t current_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    ret = esp_wifi_get_channel(&current_channel, &second_channel);
    if (ret == ESP_OK && current_channel == channel) {
        peer_mgr_espnow_readd_all();
        ESP_LOGI(TAG, "WiFi kept running; already on ESP-NOW channel %u", channel);
        return true;
    }

    ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ret == ESP_OK) {
        peer_mgr_espnow_readd_all();
        ESP_LOGI(TAG, "WiFi kept running; restored ESP-NOW channel %u", channel);
        return true;
    }

    // 3. Some driver states reject a channel change immediately after STA
    // disconnect. Retain the old full restart only as a recovery fallback.
    ESP_LOGW(TAG, "Fast channel restore to %u failed: %d; retrying with WiFi restart",
             channel, ret);
    esp_now_deinit();

    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %d", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %d", ret);
        return false;
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set channel %u after WiFi restart failed: %d", channel, ret);
        return false;
    }

    // 4. Re-init ESP-NOW (callbacks + broadcast peer) after the fallback.
    ret = espnow_comm_reinit_espnow();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "espnow_comm_reinit_espnow failed: %d", ret);
        return false;
    }

    // 5. Re-add all active peers to the ESP-NOW table.
    peer_mgr_espnow_readd_all();

    ESP_LOGI(TAG, "WiFi restarted on channel %u, ESP-NOW re-initialised", channel);
    return true;
}

void llm_client_finish_network(bool local_proxy_mode)
{
    if (local_proxy_mode) {
        set_softap_default_netif();
        return;
    }
    bool channel_ok = restore_espnow_channel();

    // Resume ESP-NOW rx and peer aging — suspended before channel switch.
    // Always resume even if channel restore failed — otherwise the tasks
    // stay suspended forever.  If the channel is wrong, peers will time
    // out and re-announce when the channel eventually recovers.
    {
        volatile TaskHandle_t* h = script_inject_get_timeout_task_handle();
        TaskHandle_t timeout_task = (h != NULL) ? *h : NULL;
        if (timeout_task != NULL) {
            vTaskResume(timeout_task);
        }
    }
    espnow_comm_resume_rx();

    // Give announces time to arrive and refresh last_seen before the
    // caller injects the generated script.  Without this, the first
    // espnow_comm_send_cmd() after restart may race against rx_task
    // and send to a peer that was just re-added but hasn't been seen yet.
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!channel_ok) {
        ESP_LOGE(TAG, "ESP-NOW channel NOT restored — sensor commands will fail");
    }
}

// ====================================================================
// Internal STA connection (called by wifi_connect_sta and llm_client_call)
// Registers its own event handlers, performs the connect, returns.
// Caller must ensure APSTA mode is set before calling.
// ====================================================================

static esp_err_t do_sta_connect(const char* ssid, const char* pass,
                               int timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_FAIL;
        }
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    // Register event handlers
    esp_event_handler_instance_t wifi_handler, ip_handler;
    esp_event_handler_instance_register(WIFI_EVENT,
                        ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_handler);
    esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &ip_handler);

    // Configure STA
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (pass && strlen(pass) > 0) {
        strncpy((char*)wifi_config.sta.password, pass,
                sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Trigger connection
    esp_wifi_connect();

    // Wait for result
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(timeout_ms));

    // Clean up handlers immediately
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);

    if (bits & WIFI_CONNECTED_BIT) {
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected to %s", ssid);
        return ESP_OK;
    }

    s_sta_connected = false;
    ESP_LOGE(TAG, "STA connect failed for %s (%d ms)", ssid, timeout_ms);
    return ESP_FAIL;
}

// ====================================================================
// Public: connect to Wi-Fi as STA while keeping SoftAP alive.
// Call this after saving WiFi credentials (e.g. from /api/wifi/connect).
// Skips the connection attempt if STA is already up — no-op.
// ====================================================================

esp_err_t wifi_connect_sta(const char* ssid, const char* pass)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Use esp_wifi_sta_get_obtained_ip to check real connection state
    // without relying on our internal s_sta_connected flag (which may
    // be stale after an undetected auto-reconnect).
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA already connected (ssid=%s, rssi=%d) — skipping",
                 ap_info.ssid, ap_info.rssi);
        return ESP_OK;
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    return do_sta_connect(ssid, pass, 15000);
}

// ====================================================================
// Refresh internal STA state from the WiFi driver.
// Call this before llm_client_call to detect auto-reconnections.
// ====================================================================

static void refresh_sta_state(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_sta_connected = true;
    } else {
        s_sta_connected = false;
    }
}



// ====================================================================
// Direct DNS probe using lwIP getaddrinfo — diagnostic helper.
// Returns 0 on success, -1 on failure.
// ====================================================================

static int probe_dns(const char* hostname)
{
    struct addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = NULL;
    int ret = getaddrinfo(hostname, NULL, &hints, &result);
    if (ret != 0) {
        ESP_LOGW(TAG, "DNS probe FAILED for %s: ret=%d errno=%d", hostname, ret, errno);
        return -1;
    }
    if (result == NULL) {
        ESP_LOGW(TAG, "DNS probe returned NULL result for %s", hostname);
        return -1;
    }

    char addr_str[46];
    struct sockaddr_in* sa = (struct sockaddr_in*)result->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "DNS probe OK: %s -> %s", hostname, addr_str);
    freeaddrinfo(result);
    return 0;
}

// Context passed to http_event_handler for collecting response data
typedef struct {
    char* buf;
    int   max_len;
    int   written;
    bool  truncated;
} http_body_ctx_t;

// Event handler — captures body chunks during esp_http_client_perform().
// This is the *only* reliable way to read chunked transfer-encoded
// responses in ESP-IDF v5.2 (esp_http_client_read() after perform()
// returns 0 because the internal buffer was already consumed).
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    http_body_ctx_t* ctx = (http_body_ctx_t*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (ctx->written + copy > ctx->max_len - 1) {
            copy = ctx->max_len - 1 - ctx->written;
            ctx->truncated = true;
        }
        if (copy > 0 && evt->data) {
            memcpy(ctx->buf + ctx->written, evt->data, (size_t)copy);
            ctx->written += copy;
        }
    }
    return ESP_OK;
}

// ====================================================================
// HTTP client — send POST to LLM API, receive response
// ====================================================================

static esp_err_t http_post_json(const char* url, const char* auth_header,
                                 const char* json_body,
                                 char* response_buf, int response_max)
{
    // Prepare response collector
    http_body_ctx_t body_ctx = {
        .buf     = response_buf,
        .max_len = response_max,
        .written = 0,
        .truncated = false
    };

    esp_http_client_config_t config = {};
    config.url               = url;
    config.method            = HTTP_METHOD_POST;
    config.timeout_ms         = 30000;
    config.buffer_size       = 4096;
    config.buffer_size_tx    = 2048;
    config.crt_bundle_attach  = esp_crt_bundle_attach;
    config.event_handler      = http_event_handler;
    config.user_data          = &body_ctx;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    // Set headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (auth_header) {
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    // Set body
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    // Perform request (body_ctx.buf is filled by event_handler during this call)
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %d %s", err, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Read response
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status_code);

    response_buf[body_ctx.written] = '\0';
    ESP_LOGD(TAG, "Response body: %d bytes received", body_ctx.written);

    esp_http_client_cleanup(client);

    if (body_ctx.truncated) {
        ESP_LOGE(TAG, "LLM response exceeded %d-byte buffer", response_max - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    if (status_code != 200) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ====================================================================
// Build system prompt — design.md §16.6
// ====================================================================

// Bounds-safe formatted append.  snprintf(buf+pos, max_len-pos, ...) is
// unsafe once pos >= max_len because (max_len - pos) is negative and, cast
// to size_t, becomes a huge value that lets snprintf write past the buffer.
// This helper clamps pos so appends can never overflow and later text is
// simply truncated instead of corrupting the heap.
static void prompt_append(char* buf, int max_len, int* pos, const char* fmt, ...)
{
    if (buf == NULL || pos == NULL || *pos >= max_len - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *pos, (size_t)(max_len - *pos), fmt, ap);
    va_end(ap);
    if (w < 0) {
        return;
    }
    *pos += w;
    if (*pos >= max_len) {
        // vsnprintf already null-terminated within the buffer; clamp so the
        // next call is a no-op instead of computing a negative size.
        *pos = max_len - 1;
    }
}

int llm_client_build_system_prompt(char* buf, int max_len)
{
    int pos = 0;

    // Header
    prompt_append(buf, max_len, &pos,
        "You are an ESP-LEGO device running a lightweight script interpreter.\n\n");

    // BNF Grammar (design.md §6.2)
    prompt_append(buf, max_len, &pos,
        "BNF Grammar:\n"
        "program      = statement*\n"
        "statement    = var_decl | if_stmt | while_stmt | block | expr_stmt\n"
        "             | func_decl | return_stmt\n"
        "func_decl    = \"func\" IDENTIFIER \"(\" [IDENTIFIER (\",\" IDENTIFIER)*] \")\" block\n"
        "return_stmt  = \"return\" expression \";\"\n"
        "var_decl     = \"var\" IDENTIFIER \"=\" expression \";\"\n"
        "if_stmt      = \"if\" \"(\" expression \")\" statement (\"else\" statement)?\n"
        "while_stmt   = \"while\" \"(\" expression \")\" statement\n"
        "block        = \"{\" statement* \"}\"\n"
        "expression   = assignment\n"
        "assignment   = IDENTIFIER \"=\" expression | logic_or\n"
        "logic_or     = logic_and (\"||\" logic_and)*\n"
        "logic_and    = equality (\"&&\" equality)*\n"
        "equality     = comparison ((\"==\" | \"!=\") comparison)*\n"
        "comparison   = term ((\"<\" | \">\" | \"<=\" | \">=\") term)*\n"
        "term         = factor ((\"+\" | \"-\") factor)*\n"
        "factor       = unary ((\"*\" | \"/\") unary)*\n"
        "unary        = (\"!\" | \"-\") unary | call\n"
        "call         = IDENTIFIER \"(\" [expression (\",\" expression)*] \")\" | primary\n"
        "primary      = NUMBER | STRING | \"true\" | \"false\" | IDENTIFIER | \"(\" expression \")\"\n\n");

    // Builtin functions (design.md §6.10)
    // Each entry: signature -> return_type   short description
    prompt_append(buf, max_len, &pos,
        "Builtin functions:\n"
        "- digital_read(pin) -> number   0 (LOW) or 1 (HIGH), reads GPIO voltage level\n"
        "- digital_write(pin,val)-> void set GPIO: 0=LOW/off, 1=HIGH/on\n"
        "- analog_read(pin) -> number   read ADC pin, returns 0-4095 (10-bit scaled)\n"
        "- analog_write(pin,val)-> void PWM output, val: 0(off) to 1023(max)\n"
        "- sleep(ms) -> void            pause script for N milliseconds\n"
        "- print(val) -> void           print value to web console execution log\n"
        "- The + operator concatenates strings with numbers/booleans, e.g. print(\"Temperature: \" + 25);\n"
        "- list_peers() -> string       list all online devices as formatted text\n"
        "- peer_count() -> number       number of currently online peer devices\n"
        "- peer_online(id) -> bool      check if peer id/name is online\n"
        "- remote_read(device)->num|list  read sensor data using a stable quoted name/type (legacy numeric IDs are accepted by the runtime but must not be generated)\n"
        "- remote_read_avg(ids)->num    read multiple remote sensors, return average (ids=comma-separated string like \"1,2,3\")\n"
        "- remote_read_max(ids)->num    read multiple remotes, return max value\n"
        "- remote_read_min(ids)->num    read multiple remotes, return min value\n"
        "- list_get(lst,i) -> number    get element at index i (0-based) from list\n"
        "- list_len(lst) -> number      number of elements in list\n\n"
        "CRITICAL — sensor data handling:\n"
        "- remote_read() may return a LIST (multiple sensor values) or a single NUMBER. Use list_get() for multi-value sensors.\n"
        "- read_sensor() ALWAYS returns a single NUMBER — use directly, no list_get needed. It reads the LCD-cached value.\n"
        "- For single-value sensors: you can use read_sensor(id) directly, e.g. var t = read_sensor(1); if (t > 30) { ... }\n"
        "- Example correct with read_sensor: var val = read_sensor(\"sensor\"); if (val == 1) { buzzer_beep(\"doorbell\", 3); }\n"
        "CRITICAL — polling loops and execution time:\n"
        "- Every script has a hard 30-second safety timeout. Generate finite logic that completes within 25 seconds.\n"
        "- NEVER generate while(true) or while(1). For monitoring, use a counter-bounded loop.\n"
        "- WRONG: while (rain == 1) { ... }  ← loop may never start, and the sensor is not refreshed\n"
        "- CORRECT: var sample = 0; while (sample < 20) { var rain = read_sensor(\"sensor\"); if (rain == 1) { buzzer_beep(\"doorbell\", 1); } sleep(1000); sample = sample + 1; }\n"
        "- The condition check goes inside the bounded polling body; always include sleep() to avoid flooding ESP-NOW.\n"
        "- If the user asks to monitor forever or continuously, generate the longest safe bounded run under 25 seconds and print(\"Monitoring window complete\") at the end.\n\n"
        "- send_motor(pin,speed)->void  DC motor via PWM: speed 0(stop) to 100(full)\n"
        "- mic_level() -> number        local INMP441 microphone level, 0-100\n");

    size_t command_count = 0;
    const ControlCommandSpec* commands =
        control_command_specs(&command_count);
    for (size_t i = 0; i < command_count; i++) {
        prompt_append(buf, max_len, &pos, "- %s(",
                      commands[i].dsl_name);
        for (uint8_t j = 0; j < commands[i].arg_count; j++) {
            prompt_append(buf, max_len, &pos, "%s%s",
                          j ? "," : "", commands[i].args[j].name);
        }
        prompt_append(buf, max_len, &pos, ")->number  %s\n",
                      commands[i].description);
    }

    prompt_append(buf, max_len, &pos,
        "\n"
        "Actuator intent rules:\n"
        "- If the user asks a buzzer, doorbell, speaker, or beeper to beep, ring, buzz, sound, or chime, use buzzer_* functions only.\n"
        "- Do NOT use remote_read() for buzzer/doorbell/servo/pump actuator requests.\n"
        "- If the user asks a servo to turn, rotate, move, or set an angle, use servo_write() or servo_sweep().\n"
        "- If the user asks to run a pump, use pump_write(device,duration_ms) with a finite duration no greater than 30000 ms. Use duration_ms=0 only to turn it off.\n"
        "- For combined actuator requests, keep every requested actuator action. Do not drop buzzer actions when the prompt also mentions servo.\n"
        "- For repeated timed actions, emit an explicit sequence using sleep(ms); the script runtime is sequential, not concurrent.\n"
        "- If the user gives a finite servo angle sequence, do not wrap it in while unless they explicitly ask to repeat or loop.\n"
        "- Chinese '蜂鸣器每隔一秒叫一声' means add buzzer_beep(...,1) at t=0 and then every 1000ms during the finite action sequence.\n"
        "- Always address every remote device by its stable quoted peer name or type, never by a numeric module ID.\n"
        "- For 'make the buzzer beep twice', '蜂鸣器叫两声', or 'doorbell ring twice', output exactly: buzzer_beep(\"buzzer\",2);\n\n"
        "Buzzer examples:\n"
        "- User asks 'make the buzzer beep twice' or '蜂鸣器叫两声': buzzer_beep(\"buzzer\",2);\n"
        "- If a listed online device has Buzzer capability, use its quoted name.\n"
        "- Do not use raw hex command IDs like 0x0013; the script language only accepts decimal numbers.\n\n"
        "Servo examples:\n"
        "- User asks 'turn the servo to 90 degrees': servo_write(\"servo\",90);\n"
        "- User asks 'sweep the servo': servo_sweep(\"servo\",0,180,15,200);\n"
        "- User asks 'move servo 90,75,105,90 every 500ms and beep every second': print(servo_write(\"servo\",90)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",75)); sleep(500); print(servo_write(\"servo\",105)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",90));\n"
        "- User asks '让 servo 舵机先转到 90 度，然后转到 75 度，再转到 105 度，最后回到 90 度，每一步间隔 500 毫秒。同时让蜂鸣器每隔一秒叫一声': print(servo_write(\"servo\",90)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",75)); sleep(500); print(servo_write(\"servo\",105)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",90));\n\n"
        "Pump examples:\n"
        "- User asks '打开水泵5秒': pump_write(\"pump\",5000);\n"
        "- User asks '关闭水泵': pump_write(\"pump\",0);\n\n");

    // Device resolution — teach the model to map vague/fuzzy references
    // (a device type or function word, with no id) to a concrete online peer.
    // This is what lets "舵机怎么样" / "让蜂鸣器响一下" work without the user
    // ever spelling out "id 为几的".
    prompt_append(buf, max_len, &pos,
        "Device resolution (IMPORTANT — resolve vague references yourself):\n"
        "- Users usually name a device only by its TYPE or FUNCTION and give NO id. This is normal. NEVER ask the user for an id and NEVER refuse because an id is missing.\n"
        "- Each online device below is tagged with canonical types. Match the user's word by type FIRST, then by name, then by capabilities text; call the builtin with its quoted stable name.\n"
        "- If exactly ONE online device matches the type, use it even when the request is vague ('舵机转一下', '蜂鸣器响两声', '看看传感器').\n"
        "- If SEVERAL devices match the same type, pick the one whose name the user mentioned; otherwise the FIRST matching device below, and add print(\"using <name>\"); so the user sees which device was chosen. Never ask to clarify.\n"
        "- If NO device list is available, use the quoted canonical type word, never a guessed numeric ID.\n"
        "- Vague sensor requests mean read and print it, e.g. print(read_sensor(\"sensor\")). Vague actuator tests mean ONE short safe demo: servo_sweep(\"servo\",0,180,15,200), buzzer_beep(\"buzzer\",2), or pump_write(\"pump\",2000).\n"
        "- Numeric module IDs are transient discovery data. Never put them in any newly generated script.\n\n");

    size_t type_count = 0;
    const DeviceTypeSpec* type_specs =
        peer_mgr_device_type_specs(&type_count);
    prompt_append(buf, max_len, &pos, "Canonical device types and aliases:\n");
    for (size_t i = 0; i < type_count; i++) {
        prompt_append(buf, max_len, &pos, "- %s: ",
                      type_specs[i].canonical);
        for (size_t j = 0; j < type_specs[i].alias_count; j++) {
            prompt_append(buf, max_len, &pos, "%s%s",
                          j ? "/" : "", type_specs[i].aliases[j]);
        }
        prompt_append(buf, max_len, &pos, "\n");
    }
    prompt_append(buf, max_len, &pos, "\n");

    // Online devices — each tagged with a canonical type= so the model can
    // ground vague references (grounding step). peer_mgr_type_tags() derives
    // the tag from the peer's name + capability so submodule firmware need
    // not change.
    prompt_append(buf, max_len, &pos, "Online devices:\n");

    int peer_count = 0;
    PeerEntry** peers = peer_mgr_list(&peer_count);
    for (int i = 0; i < peer_count; i++) {
        char type_tags[96];
        peer_mgr_type_tags(peers[i], type_tags, sizeof(type_tags));
        const char* cap = peers[i]->capability;
        if (cap && cap[0]) {
            prompt_append(buf, max_len, &pos,
                "- id=%u, name=%s, type=%s, capabilities: %s\n",
                peers[i]->module_id, peers[i]->name,
                type_tags[0] ? type_tags : "unknown", cap);
        } else {
            prompt_append(buf, max_len, &pos,
                "- id=%u, name=%s, type=%s\n",
                peers[i]->module_id, peers[i]->name,
                type_tags[0] ? type_tags : "unknown");
        }
    }
    prompt_append(buf, max_len, &pos, "\n");

    // Resource limits
    prompt_append(buf, max_len, &pos,
        "Resource limits:\n"
        "- Max %d sensor reads per script; keep bounded monitoring at or below this count\n"
        "- Max %d loop iterations\n"
        "- Max %d executed statements\n"
        "- Generate ONLY valid script code, no explanations.\n"
        "- Do NOT use markdown code fences (no ```).\n"
        "- Do NOT wrap the script in JSON or a script field.\n"
        "- Output ONLY the raw script source code.\n",
        CONFIG_MAX_SENSOR_CALLS_PER_SCRIPT,
        CONFIG_MAX_LOOP_ITERATIONS,
        CONFIG_MAX_EXEC_STATEMENTS);

    if (pos >= max_len) {
        buf[max_len - 1] = '\0';
    }

    return pos;
}

// ====================================================================
// Extract script from LLM response — strip markdown code fences
// ====================================================================

static void extract_script_from_response(const char* response, char* script_out, int max_len)
{
    const int result = script_normalize_response(response, script_out, max_len);
    if (result != SCRIPT_NORMALIZE_OK) {
        script_out[0] = '\0';
        ESP_LOGW(TAG, "Rejected LLM script response: %s",
                 script_normalize_error(result));
    }
}

// ====================================================================
// Function-calling (tools) support — hybrid path
// ====================================================================

static void llm_add_tools(cJSON* root)
{
    cJSON* tools = cJSON_CreateArray();
    if (tools == NULL) return;

    size_t count = 0;
    const ControlCommandSpec* specs = control_command_specs(&count);
    for (size_t i = 0; i < count; i++) {
        cJSON* tool = cJSON_CreateObject();
        cJSON* function = cJSON_CreateObject();
        cJSON* parameters = cJSON_CreateObject();
        cJSON* properties = cJSON_CreateObject();
        cJSON* required = cJSON_CreateArray();
        if (!tool || !function || !parameters || !properties || !required) {
            cJSON_Delete(tool);
            cJSON_Delete(function);
            cJSON_Delete(parameters);
            cJSON_Delete(properties);
            cJSON_Delete(required);
            cJSON_Delete(tools);
            ESP_LOGW(TAG, "Failed to allocate LLM tool schema");
            return;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(function, "name", specs[i].dsl_name);
        cJSON_AddStringToObject(function, "description",
                                specs[i].description);
        cJSON_AddStringToObject(parameters, "type", "object");
        for (uint8_t j = 0; j < specs[i].arg_count; j++) {
            const ControlArgSpec* arg = &specs[i].args[j];
            cJSON* property = cJSON_CreateObject();
            if (property == NULL) continue;
            if (arg->kind == CONTROL_ARG_DEVICE) {
                cJSON_AddStringToObject(property, "type", "string");
            } else {
                cJSON_AddStringToObject(property, "type", "number");
                cJSON_AddNumberToObject(property, "minimum",
                                        arg->min_value);
                cJSON_AddNumberToObject(property, "maximum",
                                        arg->max_value);
            }
            cJSON_AddStringToObject(property, "description",
                                    arg->description);
            cJSON_AddItemToObject(properties, arg->name, property);
            cJSON_AddItemToArray(required, cJSON_CreateString(arg->name));
        }
        cJSON_AddItemToObject(parameters, "properties", properties);
        cJSON_AddItemToObject(parameters, "required", required);
        cJSON_AddItemToObject(function, "parameters", parameters);
        cJSON_AddItemToObject(tool, "function", function);
        cJSON_AddItemToArray(tools, tool);
    }

    cJSON* run_tool = cJSON_CreateObject();
    cJSON* run_function = cJSON_CreateObject();
    cJSON* run_parameters = cJSON_CreateObject();
    cJSON* run_properties = cJSON_CreateObject();
    cJSON* run_script = cJSON_CreateObject();
    cJSON* run_required = cJSON_CreateArray();
    if (!run_tool || !run_function || !run_parameters || !run_properties ||
        !run_script || !run_required) {
        cJSON_Delete(run_tool);
        cJSON_Delete(run_function);
        cJSON_Delete(run_parameters);
        cJSON_Delete(run_properties);
        cJSON_Delete(run_script);
        cJSON_Delete(run_required);
        cJSON_Delete(tools);
        ESP_LOGW(TAG, "Failed to allocate run_script tool schema");
        return;
    }
    cJSON_AddStringToObject(run_tool, "type", "function");
    cJSON_AddStringToObject(run_function, "name", "run_script");
    cJSON_AddStringToObject(
        run_function, "description",
        "Run a full ESP-LEGO DSL script for complex or multi-step logic.");
    cJSON_AddStringToObject(run_parameters, "type", "object");
    cJSON_AddStringToObject(run_script, "type", "string");
    cJSON_AddItemToObject(run_properties, "script", run_script);
    cJSON_AddItemToArray(run_required, cJSON_CreateString("script"));
    cJSON_AddItemToObject(run_parameters, "properties", run_properties);
    cJSON_AddItemToObject(run_parameters, "required", run_required);
    cJSON_AddItemToObject(run_function, "parameters", run_parameters);
    cJSON_AddItemToObject(run_tool, "function", run_function);
    cJSON_AddItemToArray(tools, run_tool);

    cJSON_AddItemToObject(root, "tools", tools);
    cJSON_AddStringToObject(root, "tool_choice", "auto");
}

// In-place replace every occurrence of `from` with `to` in a bounded buffer.
static void str_replace_all(char* buf, int buf_size, const char* from, const char* to)
{
    int flen = (int)strlen(from);
    int tlen = (int)strlen(to);
    if (flen == 0) return;
    char* p;
    while ((p = strstr(buf, from)) != NULL) {
        int cur  = (int)strlen(buf);
        int tail = (int)strlen(p + flen);
        if (cur - flen + tlen >= buf_size) break;   // no room — stop
        memmove(p + tlen, p + flen, (size_t)tail + 1);
        memcpy(p, to, (size_t)tlen);
    }
}

// Format a numeric tool argument as a script literal ("90", "0.5").
static bool num_arg_literal(cJSON* args, const ControlArgSpec* spec,
                            char* out, int out_len)
{
    cJSON* v = args ? cJSON_GetObjectItem(args, spec->name) : NULL;
    if (!v || !cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < spec->min_value ||
        v->valuedouble > spec->max_value) {
        return false;
    }
    double d = v->valuedouble;
    snprintf(out, out_len, "%g", d);
    return true;
}

// Record every ACTIVE peer matching `type` as a clarify option.
static void fill_clarify_options(LlmClarify* clarify, const char* type)
{
    clarify->option_count = 0;
    int count = 0;
    PeerEntry** peers = peer_mgr_list(&count);
    for (int i = 0; i < count && clarify->option_count < LLM_CLARIFY_MAX_OPTIONS; i++) {
        if (!peer_mgr_matches_type(peers[i], type)) continue;
        LlmClarifyOption* o = &clarify->options[clarify->option_count++];
        o->id = peers[i]->module_id;
        strncpy(o->name, peers[i]->name, sizeof(o->name) - 1);
        o->name[sizeof(o->name) - 1] = '\0';
    }
}

static void string_arg_literal(const char* value, char* out, int out_len)
{
    if (!out || out_len <= 0) return;
    if (out_len < 3) {
        out[0] = '\0';
        return;
    }
    int pos = 0;
    out[pos++] = '"';
    for (const char* p = value ? value : "";
         *p && pos < out_len - 2; p++) {
        if ((*p == '"' || *p == '\\') && pos < out_len - 3) {
            out[pos++] = '\\';
        }
        out[pos++] = *p;
    }
    out[pos++] = '"';
    out[pos] = '\0';
}

// Resolve a tool_call 'device' argument into a stable script literal.
// Legacy numeric IDs are accepted only when they currently belong to the
// expected device type, then immediately canonicalized to the peer name.
static void resolve_device_literal(cJSON* device, LlmClarify* clarify,
                                   char clarify_type[16],
                                   const char* expected_type,
                                   char* out, int out_len)
{
    const char* s = NULL;
    if (device && cJSON_IsNumber(device)) {
        double raw_id = device->valuedouble;
        if (isfinite(raw_id) && raw_id >= 0 && raw_id <= 255 &&
            floor(raw_id) == raw_id) {
            bool conflict = false;
            PeerEntry* numbered =
                peer_mgr_find_by_id((uint8_t)raw_id, &conflict);
            if (numbered && !conflict &&
                peer_mgr_matches_type(numbered, expected_type)) {
                string_arg_literal(numbered->name, out, out_len);
                return;
            }
        }
        s = expected_type;
    }

    if (!s && device && cJSON_IsString(device)) {
        s = device->valuestring;
    }
    if (!s || !s[0]) s = expected_type;
    if (!s || !s[0]) {
        string_arg_literal("", out, out_len);
        return;
    }

    bool conflict = false;
    PeerEntry* p = peer_mgr_find_by_name(s, &conflict);
    if (p && !conflict && peer_mgr_matches_type(p, expected_type)) {
        string_arg_literal(p->name, out, out_len);
        return;
    }
    if (p && !peer_mgr_matches_type(p, expected_type)) {
        s = expected_type;
    }

    const char* type = peer_mgr_type_from_query(s);
    if (type) {
        int count = 0;
        PeerEntry* tp = peer_mgr_find_by_type(type, &count);
        if (count == 1 && tp) {
            string_arg_literal(tp->name, out, out_len);
            return;
        }
        if (count > 1) {
            if (clarify->kind != LLM_RESULT_CLARIFY) {
                // Activate clarification for this device type.
                clarify->kind = LLM_RESULT_CLARIFY;
                strncpy(clarify_type, type, 15);
                clarify_type[15] = '\0';
                strncpy(clarify->placeholder, "__DEVICE__",
                        sizeof(clarify->placeholder) - 1);
                clarify->placeholder[sizeof(clarify->placeholder) - 1] = '\0';
                snprintf(clarify->question, sizeof(clarify->question),
                         "Multiple %s devices are online - which one?", type);
                fill_clarify_options(clarify, type);
                snprintf(out, out_len, "%s", clarify->placeholder);
                return;
            }
            if (strcmp(clarify_type, type) == 0) {
                snprintf(out, out_len, "%s", clarify->placeholder);
                return;
            }
            // A second, different ambiguous type in the same request: degrade
            // to the type word so the interpreter picks the first + logs it.
            string_arg_literal(s, out, out_len);
            return;
        }
        // count == 0 -> no matching device online; keep the raw reference
    }

    string_arg_literal(s, out, out_len);
}

// Compile one tool_call into a DSL statement appended to `script`.
static void compile_tool_call(const char* fname, cJSON* args,
                              char* script, int max_len, int* pos,
                              LlmClarify* clarify, char clarify_type[16])
{
    if (strcmp(fname, "run_script") == 0) {
        cJSON* s = args ? cJSON_GetObjectItem(args, "script") : NULL;
        if (s && s->valuestring) {
            prompt_append(script, max_len, pos, "%s\n", s->valuestring);
        }
        return;
    }

    const ControlCommandSpec* spec = control_command_find(fname);
    if (spec == NULL || args == NULL ||
        spec->arg_count > CONTROL_COMMAND_MAX_ARGS) {
        return;
    }

    char literals[CONTROL_COMMAND_MAX_ARGS][32] = {};
    const char* literal_ptrs[CONTROL_COMMAND_MAX_ARGS] = {};
    for (uint8_t i = 0; i < spec->arg_count; i++) {
        literal_ptrs[i] = literals[i];
        if (spec->args[i].kind == CONTROL_ARG_DEVICE) {
            cJSON* device = cJSON_GetObjectItem(args, spec->args[i].name);
            resolve_device_literal(device, clarify, clarify_type,
                                   spec->device_type,
                                   literals[i], sizeof(literals[i]));
        } else if (!num_arg_literal(args, &spec->args[i], literals[i],
                                    sizeof(literals[i]))) {
            ESP_LOGW(TAG, "Invalid tool argument %s.%s",
                     fname, spec->args[i].name);
            return;
        }
    }

    char statement[192];
    int written = control_command_format_dsl(
        fname, literal_ptrs, spec->arg_count, statement, sizeof(statement));
    if (written > 0) {
        prompt_append(script, max_len, pos, "%s", statement);
    }
}

// ====================================================================
// llm_client_call_ex — LLM round trip with function-calling
//
// 1. Build system prompt (BNF + devices + builtins + constraints)
// 2. Ensure STA is connected (connect if not)
// 3. POST to LLM API with a tools schema (skipped in local-proxy mode)
// 4. Parse response: compile tool_calls -> DSL, else fall back to content
// 5. Detect ambiguous device references -> CLARIFY result
//
// Returns 0 on success, -1 on error.
// ====================================================================

int llm_client_call_ex(const char* ssid, const char* pass,
                        const char* llm_url, const char* llm_key,
                        const char* llm_model, const char* user_prompt,
                        char* script_out, int max_len,
                        LlmClarify* clarify_out)
{
    if (!ssid || !llm_url || !llm_model || !user_prompt ||
        !script_out || max_len <= 0) {
        return -1;
    }

    // Use the caller's clarify struct, or a local throwaway if not provided.
    LlmClarify local_clarify;
    LlmClarify* clarify = clarify_out ? clarify_out : &local_clarify;
    clarify->kind         = LLM_RESULT_SCRIPT;
    clarify->question[0]  = '\0';
    clarify->placeholder[0] = '\0';
    clarify->option_count = 0;

    const bool local_proxy_mode = llm_client_uses_local_proxy(llm_url);

    // ---- 1. Ensure the right network path is active ----
    if (local_proxy_mode) {
        ESP_LOGI(TAG, "Using local LLM proxy on SoftAP subnet; STA stays disconnected");
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "STA disconnect before local proxy call failed: %d", disconnect_ret);
        }
        s_sta_connected = false;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_ps(WIFI_PS_NONE);
        set_softap_default_netif();
    } else {
    // Suspend ESP-NOW rx and peer aging before channel switches to STA.
    // The single radio moves off the ESP-NOW SoftAP channel during the
    // LLM call; suspending the timeout task prevents peers from being
    // falsely aged to OFFLINE while announces cannot be received.
    espnow_comm_suspend_rx();
    {
        volatile TaskHandle_t* h = script_inject_get_timeout_task_handle();
        TaskHandle_t timeout_task = (h != NULL) ? *h : NULL;
        if (timeout_task != NULL) {
            vTaskSuspend(timeout_task);
        }
    }

    // Ensure STA is connected.
    // Probe the real driver state — this catches auto-reconnects that
    // happened outside our code (e.g. after a temporary signal drop).
    refresh_sta_state();

    if (!s_sta_connected) {
        ESP_LOGI(TAG, "STA not connected — connecting to %s", ssid);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (do_sta_connect(ssid, pass, 15000) != ESP_OK) {
            /* network teardown moved to llm_client_finish_network() called by caller */
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGI(TAG, "STA already connected — reusing connection");
    }

    // CRITICAL: Force STA as default netif on EVERY call.
    // esp_netif_set_default_netif() is only called inside do_sta_connect()'s
    // GOT_IP handler — which runs ONCE. On subsequent calls (STA already
    // connected), do_sta_connect() is skipped, so the default netif can drift
    // to AP after any WiFi traffic. Without this, getaddrinfo() routes through
    // the wrong interface and returns EAI_FAIL errno=202.
    // Use esp_netif_get_handle_from_ifkey() instead of netif_find("st0") —
    // raw lwIP netif_find() breaks after mode switches increment interface
    // number (confirmed ESP-IDF bug #16742).
    {
        esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_set_default_netif(sta_netif);
        } else {
            ESP_LOGW(TAG, "STA netif handle unavailable");
        }
    }
    }

    // ---- 2. Build system prompt ----
    // Heap-allocate a generous buffer (design.md's "no dynamic allocation"
    // rule does not apply here — the prompt contains BNF + builtins + device
    // list which can grow, and heap is already used extensively elsewhere).
    const int sys_prompt_len = 8192;
    char* sys_prompt = (char*)malloc(sys_prompt_len);
    if (sys_prompt == NULL) {
        ESP_LOGE(TAG, "Failed to allocate system prompt buffer");
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }
    llm_client_build_system_prompt(sys_prompt, sys_prompt_len);

    // ---- 3. Build HTTP JSON body ----
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        free(sys_prompt);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    cJSON_AddStringToObject(root, "model", llm_model);

    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    if (messages) {
        cJSON* sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", sys_prompt);
        cJSON_AddItemToArray(messages, sys_msg);

        cJSON* user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_prompt);
        cJSON_AddItemToArray(messages, user_msg);
    }

    // Free prompt buffer — cJSON already copied its content internally
    free(sys_prompt);
    sys_prompt = NULL;

    cJSON_AddNumberToObject(root, "temperature", 0);
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    if (strstr(llm_model, "deepseek-v4") != NULL) {
        cJSON* thinking = cJSON_AddObjectToObject(root, "thinking");
        if (thinking) {
            cJSON_AddStringToObject(thinking, "type", "disabled");
        }
    }

    // Function-calling: offer device tools so the model emits structured
    // calls we can validate and disambiguate locally. Skipped for local
    // proxies, which may not support the OpenAI tools field.
    if (!local_proxy_mode) {
        llm_add_tools(root);
    }

    char* json_body = cJSON_PrintUnformatted(root);
    if (json_body == NULL) {
        cJSON_Delete(root);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }
    cJSON_Delete(root);

    // Auth header: "Bearer {key}"
    char auth_header[256];
    if (llm_key && strlen(llm_key) > 0) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", llm_key);
    } else {
        auth_header[0] = '\0';
    }

    // Build full URL
    char full_url[512];
    if (strstr(llm_url, "/chat/completions") != NULL) {
        strncpy(full_url, llm_url, sizeof(full_url) - 1);
        full_url[sizeof(full_url) - 1] = '\0';
    } else {
        snprintf(full_url, sizeof(full_url), "%s/chat/completions", llm_url);
    }

    // ---- 4. HTTP POST ----
    if (local_proxy_mode) {
        set_softap_default_netif();
    } else {
    // HTTP POST over STA.
    // Log current DNS (diagnostic only — DNS comes from GOT_IP handler via DHCP).
    ESP_LOGI(TAG, "STA DNS: " IPSTR,
             IP2STR(&dns_getserver(0)->u_addr.ip4));

    // Force STA as default netif before DNS probe and HTTP request.
    // GOT_IP handler only runs once on first connection. On subsequent calls,
    // do_sta_connect() is skipped and the default netif can drift to AP,
    // causing getaddrinfo() to route through the wrong interface → EAI_FAIL.
    {
        esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_set_default_netif(sta_netif);
        }
    }

    // Probe DNS for the target hostname before attempting HTTP
    // Extract hostname from full URL for the probe
    const char* url_for_probe = full_url;
    if (strncmp(url_for_probe, "https://", 8) == 0) {
        url_for_probe += 8;
    } else if (strncmp(url_for_probe, "http://", 7) == 0) {
        url_for_probe += 7;
    }
    // Find first '/' after hostname
    const char* slash = strchr(url_for_probe, '/');
    int hostname_len = slash ? (slash - url_for_probe) : (int)strlen(url_for_probe);
    char hostname_buf[256];
    if (hostname_len >= (int)sizeof(hostname_buf)) hostname_len = sizeof(hostname_buf) - 1;
    strncpy(hostname_buf, url_for_probe, (size_t)hostname_len);
    hostname_buf[hostname_len] = '\0';
    ESP_LOGI(TAG, "Probing DNS for: %s", hostname_buf);
    probe_dns(hostname_buf);
    }

    char* response = (char*)malloc(4096);
    if (response == NULL) {
        free(json_body);
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    esp_err_t http_err = http_post_json(full_url,
                           auth_header[0] ? auth_header : NULL,
                           json_body, response, 4096);

    free(json_body);

    if (http_err != ESP_OK) {
        free(response);
        ESP_LOGE(TAG, "HTTP request failed");
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    // ---- 5. Parse JSON response ----
    ESP_LOGD(TAG, "Raw response (%d bytes): %s", (int)strlen(response), response);
    cJSON* resp_root = cJSON_Parse(response);
    if (resp_root == NULL) {
        ESP_LOGE(TAG, "Failed to parse LLM response JSON (%d bytes received)",
                 (int)strlen(response));
        free(response);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }
    free(response);
    response = NULL;

    cJSON* choices = cJSON_GetObjectItem(resp_root, "choices");
    if (choices == NULL || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(resp_root);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    cJSON* choice0 = cJSON_GetArrayItem(choices, 0);
    if (choice0 == NULL) {
        cJSON_Delete(resp_root);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    cJSON* message = cJSON_GetObjectItem(choice0, "message");
    if (message == NULL) {
        cJSON_Delete(resp_root);
        /* network teardown moved to llm_client_finish_network() called by caller */
        return -1;
    }

    // ---- 6. Extract script: prefer tool_calls, fall back to content ----
    bool have_script = false;
    char clarify_type[16] = "";

    cJSON* tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        int spos = 0;
        script_out[0] = '\0';
        int n = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < n; i++) {
            cJSON* tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON* fn = cJSON_GetObjectItem(tc, "function");
            if (fn == NULL) continue;
            cJSON* nm = cJSON_GetObjectItem(fn, "name");
            cJSON* ar = cJSON_GetObjectItem(fn, "arguments");
            if (nm == NULL || nm->valuestring == NULL) continue;
            // 'arguments' is a JSON-encoded string per the OpenAI/DeepSeek spec.
            cJSON* call_args = (ar && ar->valuestring)
                                   ? cJSON_Parse(ar->valuestring) : NULL;
            compile_tool_call(nm->valuestring, call_args, script_out, max_len,
                              &spos, clarify, clarify_type);
            if (call_args) cJSON_Delete(call_args);
        }
        have_script = (spos > 0);
    }

    if (!have_script) {
        // No usable tool_calls — use plain content (strips markdown fences).
        // Covers local-proxy / non-tools models.
        cJSON* content = cJSON_GetObjectItem(message, "content");
        if (content && content->valuestring && content->valuestring[0]) {
            extract_script_from_response(content->valuestring, script_out, max_len);
            have_script = (script_out[0] != '\0');
        }
    }

    cJSON_Delete(resp_root);
    /* network teardown moved to llm_client_finish_network() called by caller */

    if (!have_script) {
        ESP_LOGE(TAG, "LLM response had neither usable tool_calls nor content");
        return -1;
    }

    ESP_LOGI(TAG, "Generated script (%d bytes)%s: %s",
             (int)strlen(script_out),
             clarify->kind == LLM_RESULT_CLARIFY ? " [needs clarify]" : "",
             script_out);

    // STA is disconnected above so ESP-NOW returns to the SoftAP channel
    // before /api/ai injects the generated script.
    return 0;
}

// ====================================================================
// llm_client_call — backward-compatible wrapper (no clarify capability)
//
// Legacy callers that cannot ask the user still get a runnable script: any
// ambiguous device placeholder is bound to the first candidate.
// ====================================================================

int llm_client_call(const char* ssid, const char* pass,
                     const char* llm_url, const char* llm_key,
                     const char* llm_model, const char* user_prompt,
                     char* script_out, int max_len)
{
    LlmClarify clarify;
    int r = llm_client_call_ex(ssid, pass, llm_url, llm_key, llm_model,
                               user_prompt, script_out, max_len, &clarify);
    if (r == 0 && clarify.kind == LLM_RESULT_CLARIFY &&
        clarify.option_count > 0 && clarify.placeholder[0]) {
        char idbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)clarify.options[0].id);
        str_replace_all(script_out, max_len, clarify.placeholder, idbuf);
    }
    return r;
}
