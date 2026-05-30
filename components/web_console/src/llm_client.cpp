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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

        // ---- Set public DNS servers as fallback ----
        // lwIP's global DNS table may be empty if AP was default netif when
        // STA DHCP completed. Set 8.8.8.8 and 8.8.4.4 to guarantee DNS works.
        ip_addr_t dns_primary, dns_secondary;
        IP_ADDR4(&dns_primary, 8, 8, 8, 8);
        IP_ADDR4(&dns_secondary, 8, 8, 4, 4);
        dns_setserver(0, &dns_primary);
        dns_setserver(1, &dns_secondary);

        // Also set STA as default netif
        esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            esp_netif_set_default_netif(sta_netif);
        }

        ESP_LOGI(TAG, "DNS fixed: 8.8.8.8 / 8.8.4.4, default netif=STA");
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

static void restore_espnow_channel(void)
{
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
    const uint8_t channel = CONFIG_SOFTAP_CHANNEL;
#else
    const uint8_t channel = 1;
#endif

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "STA disconnect before ESP-NOW restore failed: %d", ret);
    }
    s_sta_connected = false;
    vTaskDelay(pdMS_TO_TICKS(300));

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "restore APSTA mode failed: %d", ret);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "restore ESP-NOW channel %u failed: %d", channel, ret);
    } else {
        ESP_LOGI(TAG, "restored ESP-NOW channel %u after LLM call", channel);
    }
}

static void finish_llm_network(bool local_proxy_mode)
{
    if (local_proxy_mode) {
        set_softap_default_netif();
        return;
    }
    restore_espnow_channel();
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
        .written = 0
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

    if (status_code != 200) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ====================================================================
// Build system prompt — design.md §16.6
// ====================================================================

static int build_system_prompt(char* buf, int max_len)
{
    int pos = 0;

    // Header
    pos += snprintf(buf + pos, (size_t)(max_len - pos),
        "You are an ESP-LEGO device running a lightweight script interpreter.\n\n");

    // BNF Grammar (design.md §6.2)
    pos += snprintf(buf + pos, (size_t)(max_len - pos),
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
    pos += snprintf(buf + pos, (size_t)(max_len - pos),
        "Builtin functions:\n"
        "- digital_read(pin) -> number   0 (LOW) or 1 (HIGH), reads GPIO voltage level\n"
        "- digital_write(pin,val)-> void set GPIO: 0=LOW/off, 1=HIGH/on\n"
        "- analog_read(pin) -> number   read ADC pin, returns 0-4095 (10-bit scaled)\n"
        "- analog_write(pin,val)-> void PWM output, val: 0(off) to 1023(max)\n"
        "- sleep(ms) -> void            pause script for N milliseconds\n"
        "- print(val) -> void           print value to web console execution log\n"
        "- list_peers() -> string       list all online devices as formatted text\n"
        "- peer_count() -> number       number of currently online peer devices\n"
        "- peer_online(id) -> bool      check if peer id/name is online\n"
        "- remote_read(id)->num|list  read sensor data from remote module (id=number or name; returns single number for 1-sensor modules, or a list for multi-sensor modules; submodule firmware decides which pins to read)\n"
        "- espnow_send(id,cmd,pl...)->void  send custom command to remote module (cmd=0x0001-0xFFFF, payload=bytes; reading sensors uses remote_read() not espnow_send)\n"
        "- list_new(size) -> list       create a list with N slots (pool allocated)\n"
        "- list_get(lst,i) -> number    get element at index i (0-based) from list\n"
        "- list_set(lst,i,v) -> void    set element at index i in list to value v\n"
        "- list_len(lst) -> number      number of elements in list\n"
        "- remote_read_avg(ids)->num    read multiple remote sensors, return average (ids=comma-separated string like \"1,2,3\")\n"
        "- remote_read_max(ids)->num    read multiple remotes, return max value\n"
        "- remote_read_min(ids)->num    read multiple remotes, return min value\n"
        "- read_sensor(pin) -> number   read LOCAL analog sensor (ADC pin), returns 0-4095\n"
        "- send_motor(pin,speed)->void  DC motor via PWM: speed 0(stop) to 100(full)\n"
        "- mic_level() -> number        local INMP441 microphone level, 0-100\n"
        "- buzzer_beep(id,count)->num   make remote buzzer beep count times; id can be number or peer name\n"
        "- buzzer_note(id,note,dur)->num play one remote buzzer note; note 0=C4, 12=C5, 19=G5, 24=C6, 36=rest; dur in ms\n"
        "- buzzer_song(id,song)->num    play preset remote buzzer song: 0=twinkle, 1=birthday, 2=jingle\n"
        "- servo_write(id,angle)->num   set remote servo angle, angle is 0-180 degrees\n"
        "- servo_sweep(id,from,to,step,delay)->num sweep remote servo; delay is ms between steps\n\n"
        "Actuator intent rules:\n"
        "- If the user asks a buzzer, doorbell, speaker, or beeper to beep, ring, buzz, sound, or chime, use buzzer_* functions only.\n"
        "- Do NOT use remote_read() for buzzer/doorbell/servo actuator requests.\n"
        "- If the user asks a servo to turn, rotate, move, or set an angle, use servo_write() or servo_sweep().\n"
        "- For combined actuator requests, keep every requested actuator action. Do not drop buzzer actions when the prompt also mentions servo.\n"
        "- For repeated timed actions, emit an explicit sequence using sleep(ms); the script runtime is sequential, not concurrent.\n"
        "- If the user gives a finite servo angle sequence, do not wrap it in while unless they explicitly ask to repeat or loop.\n"
        "- Chinese '蜂鸣器每隔一秒叫一声' means add buzzer_beep(...,1) at t=0 and then every 1000ms during the finite action sequence.\n"
        "- Prefer peer names such as \"servo\" and \"doorbell\" for combined actuator scripts when those names are listed online.\n"
        "- If only one online peer is listed, use id=1 for buzzer or servo requests.\n"
        "- For 'make the buzzer beep twice', '蜂鸣器叫两声', or 'doorbell ring twice', output exactly: buzzer_beep(1,2);\n\n"
        "Buzzer examples:\n"
        "- User asks 'make the buzzer beep twice' or '蜂鸣器叫两声': buzzer_beep(1,2);\n"
        "- If a listed online device has Buzzer capability, use that device id/name.\n"
        "- Do not use raw hex command IDs like 0x0013; the script language only accepts decimal numbers.\n\n"
        "Servo examples:\n"
        "- User asks 'turn the servo to 90 degrees': servo_write(1,90);\n"
        "- User asks 'sweep the servo': servo_sweep(1,0,180,15,200);\n"
        "- User asks 'move servo 90,75,105,90 every 500ms and beep every second': print(servo_write(\"servo\",90)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",75)); sleep(500); print(servo_write(\"servo\",105)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",90));\n"
        "- User asks '让 servo 舵机先转到 90 度，然后转到 75 度，再转到 105 度，最后回到 90 度，每一步间隔 500 毫秒。同时让蜂鸣器每隔一秒叫一声': print(servo_write(\"servo\",90)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",75)); sleep(500); print(servo_write(\"servo\",105)); print(buzzer_beep(\"doorbell\",1)); sleep(500); print(servo_write(\"servo\",90));\n\n");

    // Online devices
    pos += snprintf(buf + pos, (size_t)(max_len - pos),
        "Online devices:\n");

    int peer_count = 0;
    PeerEntry** peers = peer_mgr_list(&peer_count);
    for (int i = 0; i < peer_count && pos < max_len - 160; i++) {
        const char* cap = peers[i]->capability;
        if (cap && cap[0]) {
            pos += snprintf(buf + pos, (size_t)(max_len - pos),
                "- id=%u, name=%s, capabilities: %s\n",
                peers[i]->module_id, peers[i]->name, cap);
        } else {
            pos += snprintf(buf + pos, (size_t)(max_len - pos),
                "- id=%u, name=%s\n",
                peers[i]->module_id, peers[i]->name);
        }
    }
    pos += snprintf(buf + pos, (size_t)(max_len - pos), "\n");

    // Resource limits
    pos += snprintf(buf + pos, (size_t)(max_len - pos),
        "Resource limits:\n"
        "- Max 20 remote_read() calls per script\n"
        "- Max 10000 loop iterations\n"
        "- Max 50000 statements\n"
        "- Generate ONLY valid script code, no explanations.\n"
        "- Do NOT use markdown code fences (no ```).\n"
        "- Output ONLY the raw script source code.\n");

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
    const char* start = response;
    const char* end   = NULL;

    // Find ``` marker (if present)
    const char* fence = strstr(response, "```");
    if (fence != NULL) {
        // Skip past ``` and optional language tag
        start = fence + 3;
        while (*start == ' ' || *start == '\t') start++;
        // Skip language identifier (e.g. "javascript", "js", "c")
        while (*start && *start != '\n' && *start != '\r') start++;
        // Skip the newline
        if (*start == '\r') start++;
        if (*start == '\n') start++;

        // Find closing fence
        const char* end_fence = strstr(start, "```");
        if (end_fence != NULL) {
            end = end_fence;
        } else {
            end = start + strlen(start);
        }
    } else {
        // No fences — use entire response as script
        end = response + strlen(response);
    }

    // Copy into output buffer
    int len = (int)(end - start);
    if (len > max_len - 1) len = max_len - 1;
    strncpy(script_out, start, (size_t)len);
    script_out[len] = '\0';
}

// ====================================================================
// llm_client_call — LLM round trip
//
// 1. Build system prompt (BNF + devices + builtins + constraints)
// 2. Ensure STA is connected (connect if not)
// 3. POST to LLM API (connection is reused)
// 4. Parse response, extract script
// 5. Disconnect STA and restore ESP-NOW channel before script injection
//
// Returns 0 on success, -1 on error.
// ====================================================================

int llm_client_call(const char* ssid, const char* pass,
                     const char* llm_url, const char* llm_key,
                     const char* llm_model, const char* user_prompt,
                     char* script_out, int max_len)
{
    if (!ssid || !llm_url || !llm_model || !user_prompt ||
        !script_out || max_len <= 0) {
        return -1;
    }

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
    // Ensure STA is connected.
    // Probe the real driver state — this catches auto-reconnects that
    // happened outside our code (e.g. after a temporary signal drop).
    refresh_sta_state();

    if (!s_sta_connected) {
        ESP_LOGI(TAG, "STA not connected — connecting to %s", ssid);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (do_sta_connect(ssid, pass, 15000) != ESP_OK) {
            finish_llm_network(local_proxy_mode);
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
    const int sys_prompt_len = 6144;
    char* sys_prompt = (char*)malloc(sys_prompt_len);
    if (sys_prompt == NULL) {
        ESP_LOGE(TAG, "Failed to allocate system prompt buffer");
        finish_llm_network(local_proxy_mode);
        return -1;
    }
    build_system_prompt(sys_prompt, sys_prompt_len);

    // ---- 3. Build HTTP JSON body ----
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        free(sys_prompt);
        finish_llm_network(local_proxy_mode);
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

    char* json_body = cJSON_PrintUnformatted(root);
    if (json_body == NULL) {
        cJSON_Delete(root);
        finish_llm_network(local_proxy_mode);
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
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    esp_err_t http_err = http_post_json(full_url,
                           auth_header[0] ? auth_header : NULL,
                           json_body, response, 4096);

    free(json_body);

    if (http_err != ESP_OK) {
        free(response);
        ESP_LOGE(TAG, "HTTP request failed");
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    // ---- 5. Parse JSON response ----
    ESP_LOGD(TAG, "Raw response (%d bytes): %s", (int)strlen(response), response);
    cJSON* resp_root = cJSON_Parse(response);
    if (resp_root == NULL) {
        ESP_LOGE(TAG, "Failed to parse LLM response JSON (%d bytes received)",
                 (int)strlen(response));
        free(response);
        finish_llm_network(local_proxy_mode);
        return -1;
    }
    free(response);
    response = NULL;

    cJSON* choices = cJSON_GetObjectItem(resp_root, "choices");
    if (choices == NULL || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(resp_root);
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    cJSON* choice0 = cJSON_GetArrayItem(choices, 0);
    if (choice0 == NULL) {
        cJSON_Delete(resp_root);
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    cJSON* message = cJSON_GetObjectItem(choice0, "message");
    if (message == NULL) {
        cJSON_Delete(resp_root);
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    cJSON* content = cJSON_GetObjectItem(message, "content");
    if (content == NULL || content->valuestring == NULL) {
        cJSON_Delete(resp_root);
        finish_llm_network(local_proxy_mode);
        return -1;
    }

    // ---- 6. Extract script (strip markdown fences) ----
    extract_script_from_response(content->valuestring, script_out, max_len);
    cJSON_Delete(resp_root);
    finish_llm_network(local_proxy_mode);

    ESP_LOGI(TAG, "Extracted script (%d bytes): %s", (int)strlen(script_out), script_out);

    // STA is disconnected above so ESP-NOW returns to the SoftAP channel
    // before /api/ai injects the generated script.
    return 0;
}
