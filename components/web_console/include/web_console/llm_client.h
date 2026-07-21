#pragma once

/**
 * @file llm_client.h
 * @brief LLM API HTTP client — design.md §16.5, §16.6
 *
 * Provides functions to:
 * - wifi_connect_sta()   — connect to Wi-Fi as STA (after saving credentials)
 * - wifi_sta_is_connected() — check if STA has an IP
 * - llm_client_call()    — call an OpenAI-compatible LLM API
 */

#include "sdkconfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to configured Wi-Fi as STA.
 *
 * Switches from SoftAP to STA and connects to the given network.
 * Use this after saving WiFi credentials to establish connectivity
 * before making LLM requests.
 *
 * @param ssid   Target Wi-Fi SSID
 * @param pass   Target Wi-Fi password (may be empty string)
 * @return ESP_OK on success, ESP_FAIL on timeout (15 s)
 */
esp_err_t wifi_connect_sta(const char* ssid, const char* pass);

/**
 * @brief Check if STA interface is currently connected to an AP.
 * @return true if connected (has IP), false otherwise.
 */
bool wifi_sta_is_connected(void);

/**
 * @brief Return true when the LLM URL points at a PC-side proxy on the
 *        ESP SoftAP subnet, e.g. http://192.168.4.2:18082/v1.
 *
 * In this mode the ESP keeps STA disconnected and sends HTTP over its AP
 * interface, avoiding channel conflicts with ESP-NOW peers.
 */
bool llm_client_uses_local_proxy(const char* llm_url);

/**
 * @brief Build the dynamic ESP-LEGO system prompt for a remote LLM caller.
 *
 * Used both by the on-device HTTP client and by the phone-mobile-data path.
 * The prompt includes the language grammar, builtins and current peer list.
 *
 * @return Number of bytes that would be written, excluding the terminator.
 */
int llm_client_build_system_prompt(char* buf, int max_len);

/**
 * @brief Call LLM API to generate a script from natural language.
 *
 * Performs the full round-trip:
 * 1. Builds system prompt (BNF + devices + builtins + constraints)
 * 2. Checks STA connection — if down, reconnects to the configured AP
 * 3. Sends HTTP POST to the LLM API
 * 4. Parses JSON response, extracts script content
 * 5. (Does NOT tear down STA — stays connected for next request)
 *
 * @param ssid       Target Wi-Fi SSID
 * @param pass       Target Wi-Fi password (may be empty)
 * @param llm_url    LLM API base URL (e.g. "https://api.openai.com/v1")
 * @param llm_key    LLM API key (may be empty)
 * @param llm_model  Model name (e.g. "gpt-4o-mini")
 * @param user_prompt User's natural language instruction
 * @param script_out Buffer for the extracted script
 * @param max_len    Size of script_out buffer
 * @return 0 on success, -1 on error (wifi/timeout/parse).
 */
int llm_client_call(const char* ssid, const char* pass,
                     const char* llm_url, const char* llm_key,
                     const char* llm_model, const char* user_prompt,
                     char* script_out, int max_len);

/**
 * @brief Restore ESP-NOW channel and resume suspended tasks after LLM call.
 *
 * MUST be called after llm_client_call() returns (success or error), and
 * MUST be called AFTER the HTTP response has been sent to avoid killing
 * the SoftAP while the browser is still waiting.
 *
 * @param local_proxy_mode  true if the LLM URL points at a local proxy
 */
void llm_client_finish_network(bool local_proxy_mode);

#ifdef __cplusplus
}
#endif
