#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ESP-NOW peer info
typedef struct {
    uint8_t peer_addr[6];
    uint8_t channel;
    uint8_t ifidx;
    bool encrypt;
    void* priv;
} esp_now_peer_info_t;

// ESP-NOW recv info (the IDF v5.x version has src_addr + dst_addr + rx_ctrl)
typedef struct {
    const uint8_t* src_addr;
    const uint8_t* dst_addr;
    void* rx_ctrl;
} esp_now_recv_info_t;

typedef int esp_now_send_status_t;
#define ESP_NOW_SEND_SUCCESS    0
#define ESP_NOW_SEND_FAIL       1
#define ESP_NOW_ETH_ALEN        6
#ifndef WIFI_IF_STA
#define WIFI_IF_STA             0
#endif

// Stub functions
static inline esp_err_t esp_now_init(void) { return ESP_OK; }
static inline esp_err_t esp_now_deinit(void) { return ESP_OK; }
static inline esp_err_t esp_now_send(const uint8_t* addr, const uint8_t* data, int len) {
    (void)addr; (void)data; (void)len; return ESP_OK;
}
static inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t* peer) {
    (void)peer; return ESP_OK;
}
static inline esp_err_t esp_now_del_peer(const uint8_t* addr) {
    (void)addr; return ESP_OK;
}
static inline bool esp_now_is_peer_exist(const uint8_t* addr) {
    (void)addr; return false;
}
static inline esp_err_t esp_now_register_send_cb(void* cb) { (void)cb; return ESP_OK; }
static inline esp_err_t esp_now_register_recv_cb(void* cb) { (void)cb; return ESP_OK; }
