#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
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

// ------------------------------------------------------------------
// Canonical device-type resolution (fuzzy addressing helpers)
//
// A user/LLM often refers to a device by its function word ("舵机",
// "buzzer") with no id.  These helpers map such vague references to a
// concrete peer, based on the peer's name + capability descriptor.
// Canonical type tags are the literal strings "servo", "buzzer",
// "sensor".
// ------------------------------------------------------------------

// True if `entry` can act as the given canonical type ("servo"/"buzzer"/
// "sensor"), judged from its name and capability text.
bool peer_mgr_matches_type(const PeerEntry* entry, const char* type);

// Map a reference word (English or Chinese synonym, e.g. "舵机", "servo",
// "门铃") to a canonical type tag, or NULL if it does not look like a type.
const char* peer_mgr_type_from_query(const char* query);

// Write a comma-separated list of the canonical types this peer supports
// (e.g. "servo,sensor") into `out`. Always null-terminates.
void peer_mgr_type_tags(const PeerEntry* entry, char* out, size_t out_len);

// Find the first ACTIVE peer matching a canonical `type`. Sets *out_count
// (may be NULL) to the number of ACTIVE peers of that type so callers can
// detect ambiguity. Returns NULL if none match.
PeerEntry* peer_mgr_find_by_type(const char* type, int* out_count);

int         peer_mgr_active_count(void);
int         peer_mgr_total_count(void);
PeerEntry** peer_mgr_list(int* count);

void peer_mgr_age_scan(TickType_t now);
void peer_mgr_set_pending(PeerEntry* entry);
bool peer_mgr_is_duplicate(PeerEntry* entry, uint8_t seq_id);

// Re-add all ACTIVE peers to the ESP-NOW peer table.
// Call after esp_wifi_stop()/esp_wifi_start() cycle which clears the
// ESP-NOW internal peer list.
void peer_mgr_espnow_readd_all(void);

#ifdef __cplusplus
}
#endif
