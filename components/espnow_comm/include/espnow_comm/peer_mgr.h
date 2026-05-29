#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// Peer states — design.md §5.2
typedef enum {
    PEER_NEW,
    PEER_ACTIVE,
    PEER_PENDING_CHANGE,
    PEER_OFFLINE,
} PeerState;

#define PEER_ID_STR_LEN 22   // "aa:bb:cc:dd:ee:ff:255\0" = 21 + 1
#define PEER_LAST_VALUE_MAX 4

#ifndef CONFIG_MAX_CAPABILITY_LEN
#define CONFIG_MAX_CAPABILITY_LEN 128
#endif

typedef struct {
    uint8_t  mac[6];
    uint8_t  module_id;
    char     name[17];        // max 16 chars + null
    char     capability[CONFIG_MAX_CAPABILITY_LEN]; // sensor descriptor
    PeerState state;
    TickType_t last_seen;
    TickType_t pending_change_tick;

    // Composite peer ID "aa:bb:cc:dd:ee:ff:module_id\0"
    char peer_id[PEER_ID_STR_LEN];

    double last_values[PEER_LAST_VALUE_MAX];
    uint8_t last_value_count;
    TickType_t last_data_tick;

    // Dedup sliding window — design.md §5.1
    uint8_t  dedup_seq[8];
    int      dedup_count;
} PeerEntry;

#define MAX_PEERS CONFIG_MAX_PEERS

// New announce handler — auto-assigns module_id if MAC is new
// Returns the assigned module_id, or 0 on failure (table full).
// Master-side: called from rx_process_one() on MSG_ANNOUNCE.
uint8_t peer_mgr_handle_announce(const uint8_t* mac, const char* name, const char* capability);

void peer_mgr_init(void);
int  peer_mgr_insert(const uint8_t* mac, uint8_t module_id, const char* name);
int  peer_mgr_update(const uint8_t* mac, uint8_t module_id, const char* name);
void peer_mgr_remove(const uint8_t* mac);

PeerEntry* peer_mgr_find_by_mac(const uint8_t* mac, bool* out_conflict);
PeerEntry* peer_mgr_find_by_id(uint8_t module_id, bool* out_conflict);
PeerEntry* peer_mgr_find_by_name(const char* name, bool* out_conflict);
PeerEntry* peer_mgr_find_by_mac_and_id(const uint8_t* mac, uint8_t module_id);

int         peer_mgr_active_count(void);
int         peer_mgr_total_count(void);
PeerEntry** peer_mgr_list(int* count);
int         peer_mgr_copy_active(PeerEntry* out, int max_entries);
int         peer_mgr_copy_snapshot(PeerEntry* out, int max_entries);
void        peer_mgr_update_data_by_mac(const uint8_t* mac,
                                        const double* values,
                                        int value_count);

void peer_mgr_age_scan(TickType_t now);
void peer_mgr_set_pending(PeerEntry* entry);
bool peer_mgr_is_duplicate(PeerEntry* entry, uint8_t seq_id);

#ifdef __cplusplus
}
#endif
