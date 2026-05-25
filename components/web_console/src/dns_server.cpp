/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Captive portal DNS server.
 *
 * Listens on UDP port 53 and responds to every DNS A query with
 * the SoftAP IP (192.168.4.1).  This makes the OS captive-portal
 * detection pop up the web console automatically after connecting
 * to the ESP-LEGO-Setup WiFi.
 *
 * No dynamic allocation, no std containers, no exceptions.
 * design.md §16.5
 */

#include "sdkconfig.h"
#include "web_console/dns_server.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "dns_server";

// SoftAP IP that all domains resolve to
#define DNS_TARGET_IP  "192.168.4.1"

// DNS buffer must hold: query + header + smallest-possible answer
// Max DNS name is 255 chars, plus label encoding overhead ≈ 2×
// We use 512 which covers the common EDNS0 case too.
#define DNS_BUF_SIZE   512

// ---- Task handle for graceful shutdown ----
static TaskHandle_t s_dns_task = NULL;

// ------------------------------------------------------------------
// Encode an IPv4 dotted-quad string into 4 raw bytes (network order)
// ------------------------------------------------------------------
static uint32_t ip_to_u32(const char* ip_str)
{
    uint32_t octets[4];
    sscanf(ip_str, "%lu.%lu.%lu.%lu", &octets[0], &octets[1],
           &octets[2], &octets[3]);
    uint32_t ip = (octets[0] << 24) | (octets[1] << 16) |
                  (octets[2] <<  8) | octets[3];
    return htonl(ip);  // DNS wire format = network byte order
}

// ------------------------------------------------------------------
// DNS question parsing — skip the encoded domain name in the query
// Returns the offset just past the QTYPE+QCLASS, or -1 on error.
// ------------------------------------------------------------------
static int skip_question(const uint8_t* buf, int len, int offset)
{
    if (offset >= len) return -1;

    // Walk through label sequence (ends with a 0-byte label)
    while (offset < len) {
        uint8_t label_len = buf[offset];
        if (label_len == 0) {
            offset++;  // skip the terminating zero
            break;
        }
        // DNS compression pointer (0xC0 0x??) — rare in queries,
        // but handle gracefully: jump wouldn't make sense here so treat as error
        if ((label_len & 0xC0) == 0xC0) {
            // Compression pointer found in question — malformed for our purposes
            return -1;
        }
        offset += 1 + label_len;  // skip length byte + label bytes
    }
    if (offset + 4 > len) return -1;  // need QTYPE(2) + QCLASS(2)
    return offset + 4;
}

// ------------------------------------------------------------------
// Build a DNS response for one A-record question.
// Returns the total response length, or -1 on error.
// ------------------------------------------------------------------
static int build_response(const uint8_t* query, int query_len,
                          uint8_t* resp, int resp_max)
{
    if (query_len < 12 || resp_max < query_len + 16) return -1;

    // ---- Copy DNS header, modify flags ----
    memcpy(resp, query, 12);
    // Set QR=1 (response), AA=1 (authoritative), RA=1
    resp[2] = 0x85;  // 1000 0101: QR=1, AA=1, RD=0, RA=1
    resp[3] = 0x80;  // 1000 0000: remaining flags + RCODE=0
    // QDCOUNT = 1 (from query — already set by client)
    // ANCOUNT = 1 (our answer)
    resp[6] = 0; resp[7] = 1;  // one answer

    // ---- Echo the question verbatim (starts at offset 12) ----
    int q_off = 12;
    int q_end = skip_question(query, query_len, q_off);
    if (q_end < 0) return -1;
    int q_size = q_end - q_off;
    memcpy(resp + 12, query + 12, q_size);
    int resp_len = 12 + q_size;

    // ---- Append answer  ----
    // NAME pointer (0xC0 0x0C) points back to the question in the packet
    resp[resp_len + 0] = 0xC0;
    resp[resp_len + 1] = 0x0C;

    // TYPE = A (1)
    resp[resp_len + 2] = 0; resp[resp_len + 3] = 1;
    // CLASS = IN (1)
    resp[resp_len + 4] = 0; resp[resp_len + 5] = 1;
    // TTL = 60 seconds
    resp[resp_len + 6] = 0; resp[resp_len + 7] = 0;
    resp[resp_len + 8] = 0; resp[resp_len + 9] = 60;
    // RDLENGTH = 4
    resp[resp_len + 10] = 0; resp[resp_len + 11] = 4;

    // RDATA = target IP
    uint32_t ip = ip_to_u32(DNS_TARGET_IP);
    memcpy(resp + resp_len + 12, &ip, 4);

    return resp_len + 16;  // answer section = 16 bytes
}

// ------------------------------------------------------------------
// DNS server task — listens on UDP port 53
// ------------------------------------------------------------------

static void dns_server_task(void* pv)
{
    (void)pv;
    int sock = -1;
    int opt = 1;
    uint8_t buf[DNS_BUF_SIZE];
    uint8_t resp[DNS_BUF_SIZE];

    // ---- Create UDP socket ----
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        goto cleanup;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() port 53 failed: errno %d", errno);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Captive DNS running on port 53 → %s", DNS_TARGET_IP);

    // ---- Main loop ----
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&from, &from_len);
        if (n < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            continue;
        }

        // Only handle standard queries (QR=0)
        if (n < 12 || (buf[2] & 0x80) != 0) continue;

        // Only handle A-record queries (QTYPE=1 in first question)
        // Quick check: offset after name + 2 bytes for QTYPE
        int qtype_off = 12;
        // Walk past the encoded domain name to reach QTYPE
        while (qtype_off < n) {
            uint8_t label = buf[qtype_off];
            if (label == 0) { qtype_off++; break; }
            if ((label & 0xC0) == 0xC0) { qtype_off += 2; break; }
            qtype_off += 1 + label;
        }
        if (qtype_off + 2 > n) continue;

        // Check QTYPE = 1 (A record)
        uint16_t qtype = (buf[qtype_off] << 8) | buf[qtype_off + 1];
        if (qtype != 1) continue;

        ESP_LOGD(TAG, "DNS query received");

        // Build response
        int resp_len = build_response(buf, n, resp, sizeof(resp));
        if (resp_len < 0) {
            ESP_LOGW(TAG, "Failed to build DNS response");
            continue;
        }

        // Send response back to the same client
        sendto(sock, resp, resp_len, 0,
               (struct sockaddr*)&from, from_len);
    }

cleanup:
    if (sock >= 0) closesocket(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------
// Public API — start the captive portal DNS server
// ------------------------------------------------------------------

void dns_server_start(void)
{
    if (s_dns_task != NULL) {
        ESP_LOGW(TAG, "DNS server already running");
        return;
    }

    BaseType_t t = xTaskCreate(dns_server_task, "dns", 3072, NULL, 3,
                                &s_dns_task);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        s_dns_task = NULL;
    }
}
