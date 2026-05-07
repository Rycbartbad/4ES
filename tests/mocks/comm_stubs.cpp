/*
 * ESP-LEGO V1.0 — Linker stubs for host-side testing.
 *
 * Provides minimal implementations of functions that are normally
 * defined in app_main.cpp or access real ESP-NOW hardware.
 * C linkage required to match extern "C" declarations in component headers.
 */

#include <stdint.h>

// track_output_pin is Non-C-linkage (declared outside extern "C" in interpreter.h)
// Defined in app_main.cpp — tracks output pins for hardware safety reset
void track_output_pin(uint8_t pin) {
    (void)pin;
}

// ESP-NOW functions use C linkage (declared inside extern "C" in comm.h)
#ifdef __cplusplus
extern "C" {
#endif

int espnow_comm_request_read(uint8_t module_id, double* out_values, int max_values) {
    (void)module_id;
    (void)out_values;
    (void)max_values;
    return 0;
}

int espnow_comm_send_cmd(uint8_t module_id, uint16_t cmd_id,
                         const uint8_t* payload, uint8_t payload_len) {
    (void)module_id;
    (void)cmd_id;
    (void)payload;
    (void)payload_len;
    return 0;
}

#ifdef __cplusplus
}
#endif
