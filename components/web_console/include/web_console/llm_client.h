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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Function-calling result — hybrid tool-call path (design: local clarify)
//
// llm_client_call_ex() compiles the model's tool_calls into a DSL script.
// When a device reference matches several online peers of the same type, it
// cannot be resolved on the master without input, so the call returns a
// CLARIFY result: the generated script contains a placeholder token where the
// device id belongs, plus the list of candidate devices for the UI to offer.
// The UI substitutes the chosen id into the placeholder and injects locally
// via /api/script — no second LLM round-trip.
// ---------------------------------------------------------------------------

#define LLM_CLARIFY_MAX_OPTIONS 8

typedef enum {
    LLM_RESULT_SCRIPT  = 0,   // script_out is ready to inject as-is
    LLM_RESULT_CLARIFY = 1,   // script_out has a placeholder; ask the user
} LlmResultKind;

typedef struct {
    uint8_t id;
    char    name[17];
} LlmClarifyOption;

typedef struct {
    int  kind;                 // LlmResultKind
    char question[128];        // human prompt, e.g. "Multiple servo devices..."
    char placeholder[24];      // token in script_out to replace, e.g. "__DEVICE__"
    int  option_count;
    LlmClarifyOption options[LLM_CLARIFY_MAX_OPTIONS];
} LlmClarify;

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
 * @brief Like llm_client_call(), but uses DeepSeek function-calling and can
 *        return a CLARIFY result when a device reference is ambiguous.
 *
 * On return 0, inspect clarify_out->kind:
 *   - LLM_RESULT_SCRIPT : script_out is ready to inject.
 *   - LLM_RESULT_CLARIFY: script_out contains clarify_out->placeholder where a
 *                         device id must go; present clarify_out->options to
 *                         the user and substitute the chosen id.
 *
 * @param clarify_out Out param (must be non-NULL) for the clarify result.
 * @return 0 on success, -1 on error.
 */
int llm_client_call_ex(const char* ssid, const char* pass,
                       const char* llm_url, const char* llm_key,
                       const char* llm_model, const char* user_prompt,
                       char* script_out, int max_len,
                       LlmClarify* clarify_out);

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
