/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Captive portal DNS server.
 * design.md §16.5
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the captive portal DNS server.
 *
 * Listens on UDP port 53 and responds to every A-record query with the
 * SoftAP IP (192.168.4.1).  This triggers the OS captive-portal assistant
 * to pop up the web console automatically after connecting to the
 * ESP-LEGO-Setup network.
 *
 * Safe to call multiple times (idempotent).
 */
void dns_server_start(void);

#ifdef __cplusplus
}
#endif
