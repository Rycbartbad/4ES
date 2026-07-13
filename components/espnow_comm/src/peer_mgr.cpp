/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Peer manager.
 *
 * Static object-pool-based peer table with semaphore-guarded access.
 * No dynamic allocation, no C++ std containers, no exceptions.
 *
 * Lock discipline (design.md §4.4):
 *   - All public API functions lock the mutex at entry, unlock at exit.
 *   - age_scan locks per-entry (not whole scan) to minimise hold time.
 *   - TOCTOU: callers copy needed fields (e.g. mac[6]) inside the lock
 *     and use the copy after unlock.
 */

#include "sdkconfig.h"
#include "espnow_comm/peer_mgr.h"
#include "esp_now.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "peer_mgr";

// ------------------------------------------------------------------
// Static pool
// ------------------------------------------------------------------
static PeerEntry        s_peers[MAX_PEERS];
static int              s_peer_count = 0;
static SemaphoreHandle_t s_peer_mutex = NULL;

// Active-list scratch buffer (returned by peer_mgr_list)
static PeerEntry*       s_active_list[MAX_PEERS];

// ------------------------------------------------------------------
// Lock helpers
// ------------------------------------------------------------------
#define PEER_LOCK()   xSemaphoreTake(s_peer_mutex, portMAX_DELAY)
#define PEER_UNLOCK() xSemaphoreGive(s_peer_mutex)

// Decide whether a slot is occupied (mac is non-zero).
// ESP32 MACs never start with 0x00, so this is a safe "empty" sentinel.
#define ENTRY_IS_EMPTY(e) \
    ((e)->mac[0] == 0 && (e)->mac[1] == 0 && (e)->mac[2] == 0 && \
     (e)->mac[3] == 0 && (e)->mac[4] == 0 && (e)->mac[5] == 0)

// ------------------------------------------------------------------
// Init / teardown
// ------------------------------------------------------------------
void peer_mgr_init(void)
{
    if (s_peer_mutex == NULL) {
        s_peer_mutex = xSemaphoreCreateMutex();
    }
    PEER_LOCK();
    memset(s_peers, 0, sizeof(s_peers));
    s_peer_count = 0;
    PEER_UNLOCK();
    ESP_LOGI(TAG, "peer_mgr initialized (%d slots)", MAX_PEERS);
}

// ------------------------------------------------------------------
// Handle announce from sensor — auto-assigns module_id if new MAC
// ------------------------------------------------------------------
uint8_t peer_mgr_handle_announce(const uint8_t* mac, const char* name, const char* capability)
{
    if (mac == NULL || name == NULL) return 0;

    PEER_LOCK();

    // 1. Try to find existing entry by MAC
    int existing_idx = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx >= 0) {
        // Found — update name, capability, refresh state
        PeerEntry* e = &s_peers[existing_idx];
        size_t nlen = strlen(name);
        if (nlen > 16) nlen = 16;
        memcpy(e->name, name, nlen);
        e->name[nlen] = '\0';

        // Update capability if provided
        if (capability != NULL) {
            size_t clen = strlen(capability);
            if (clen >= sizeof(e->capability)) clen = sizeof(e->capability) - 1;
            memcpy(e->capability, capability, clen);
            e->capability[clen] = '\0';
        }

        e->state     = PEER_ACTIVE;
        e->last_seen = xTaskGetTickCount();
        uint8_t id   = e->module_id;
        PEER_UNLOCK();
        return id;
    }

    // 2. Not found — allocate next sequential module_id
    uint8_t max_id = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id > max_id) {
            max_id = s_peers[i].module_id;
        }
    }
    uint8_t new_id = max_id + 1;
    if (new_id == 0) new_id = 1;  // wrap: 255→1

    // 3. Find empty slot
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) {
            slot = i;
            break;
        }
        if (s_peers[i].state == PEER_OFFLINE && slot == -1) {
            slot = i;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "peer table full — cannot register new sensor");
        PEER_UNLOCK();
        return 0;
    }

    // 4. Register MAC in ESP-NOW underlying layer to permit unicast messages
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 0;
        peerInfo.ifidx   = WIFI_IF_STA;
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, mac, 6);
        esp_now_add_peer(&peerInfo);
    }

    // 5. Fill entry
    PeerEntry* e = &s_peers[slot];
    memset(e, 0, sizeof(PeerEntry));
    memcpy(e->mac, mac, 6);
    e->module_id = new_id;

    snprintf(e->peer_id, PEER_ID_STR_LEN,
             "%02x:%02x:%02x:%02x:%02x:%02x:%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)new_id);

    size_t nlen = strlen(name);
    if (nlen > 16) nlen = 16;
    memcpy(e->name, name, nlen);
    e->name[nlen] = '\0';

    // Copy capability descriptor
    if (capability != NULL) {
        size_t clen = strlen(capability);
        if (clen >= sizeof(e->capability)) clen = sizeof(e->capability) - 1;
        memcpy(e->capability, capability, clen);
        e->capability[clen] = '\0';
    } else {
        e->capability[0] = '\0';
    }

    memset(e->dedup_seq, 0xFF, sizeof(e->dedup_seq));
    e->dedup_count = 0;

    e->state     = PEER_ACTIVE;
    e->last_seen = xTaskGetTickCount();
    s_peer_count++;

    ESP_LOGI(TAG, "peer announced (auto-id): %s \"%s\" (slot %d)",
             e->peer_id, e->name, slot);
    PEER_UNLOCK();
    return new_id;
}

// ------------------------------------------------------------------
// Insert
// ------------------------------------------------------------------
int peer_mgr_insert(const uint8_t* mac, uint8_t module_id, const char* name)
{
    if (mac == NULL || name == NULL) return -1;

    PEER_LOCK();

    // 1. Check if entry already exists (by composite key)
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id == module_id &&
            memcmp(s_peers[i].mac, mac, 6) == 0) {
            // Already tracked — refresh last_seen and return index
            s_peers[i].last_seen = xTaskGetTickCount();
            s_peers[i].state     = PEER_ACTIVE;
            PEER_UNLOCK();
            return i;
        }
    }

    // 2. Find empty slot (first empty, or first OFFLINE as fallback)
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) {
            slot = i;
            break;
        }
        if (s_peers[i].state == PEER_OFFLINE && slot == -1) {
            slot = i;   // prefer empty, but accept OFFLINE as fallback
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "peer table full (%d/%d active)", s_peer_count, MAX_PEERS);
        PEER_UNLOCK();
        return -1;
    }

    // 3. Register MAC in ESP-NOW underlying layer for unicast send
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 0;
        peerInfo.ifidx   = WIFI_IF_STA;
        peerInfo.encrypt = false;
        memcpy(peerInfo.peer_addr, mac, 6);
        esp_err_t add_ret = esp_now_add_peer(&peerInfo);
        if (add_ret != ESP_OK && add_ret != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "esp_now_add_peer failed for %02x:%02x:%02x:%02x:%02x:%02x: %d",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], add_ret);
        }
    }

    // 4. Fill entry
    PeerEntry* e = &s_peers[slot];
    memset(e, 0, sizeof(PeerEntry));
    memcpy(e->mac, mac, 6);
    e->module_id = module_id;

    // Build composite peer ID: "aa:bb:cc:dd:ee:ff:module_id\0"
    snprintf(e->peer_id, PEER_ID_STR_LEN,
             "%02x:%02x:%02x:%02x:%02x:%02x:%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)module_id);

    // Copy name (max 16 chars + null)
    size_t nlen = strlen(name);
    if (nlen > 16) nlen = 16;
    memcpy(e->name, name, nlen);
    e->name[nlen] = '\0';

    // Init dedup slots to 0xFF so seq_id=0 doesn't match on first call
    memset(e->dedup_seq, 0xFF, sizeof(e->dedup_seq));
    e->dedup_count = 0;

    e->state    = PEER_ACTIVE;
    e->last_seen = xTaskGetTickCount();
    s_peer_count++;

    ESP_LOGI(TAG, "peer inserted: %s \"%s\" (slot %d)", e->peer_id, e->name, slot);
    PEER_UNLOCK();
    return slot;
}

// ------------------------------------------------------------------
// Update — state machine (design.md §5.2)
// ------------------------------------------------------------------
int peer_mgr_update(const uint8_t* mac, uint8_t module_id, const char* name)
{
    if (mac == NULL || name == NULL) return -1;

    PEER_LOCK();

    // --- step 1: try exact (mac + module_id) match ---
    PeerEntry* exact = NULL;
    int exact_idx = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id == module_id &&
            memcmp(s_peers[i].mac, mac, 6) == 0) {
            exact    = &s_peers[i];
            exact_idx = i;
            break;
        }
    }

    if (exact != NULL) {
        TickType_t now = xTaskGetTickCount();
        switch (exact->state) {
            case PEER_PENDING_CHANGE:
                // Values already match (exact match) → confirm
                exact->state = PEER_ACTIVE;
                // Update name in case it changed
                {
                    size_t nlen = strlen(name);
                    if (nlen > 16) nlen = 16;
                    memcpy(exact->name, name, nlen);
                    exact->name[nlen] = '\0';
                }
                // Also check timeout for safety
                if (now - exact->pending_change_tick >
                    pdMS_TO_TICKS(CONFIG_PEER_PENDING_TIMEOUT_MS)) {
                    exact->state = PEER_ACTIVE;
                }
                exact->last_seen = now;
                ESP_LOGD(TAG, "peer PENDING_CHANGE confirmed → ACTIVE (idx %d)", exact_idx);
                break;

            case PEER_ACTIVE:
                // Same device, same IDs — update name if different, refresh
                {
                    size_t nlen = strlen(name);
                    if (nlen > 16) nlen = 16;
                    if (strncmp(exact->name, name, 16) != 0) {
                        // Name changed → enter PENDING_CHANGE
                        exact->state = PEER_PENDING_CHANGE;
                        exact->pending_change_tick = now;
                        ESP_LOGD(TAG, "peer name changed → PENDING_CHANGE (idx %d)", exact_idx);
                    }
                    memcpy(exact->name, name, nlen);
                    exact->name[nlen] = '\0';
                }
                exact->last_seen = now;
                break;

            case PEER_NEW:
            case PEER_OFFLINE:
                exact->state = PEER_ACTIVE;
                exact->last_seen = now;
                ESP_LOGD(TAG, "peer revived %s → ACTIVE (idx %d)", exact->peer_id, exact_idx);
                break;
        }
        PEER_UNLOCK();
        return exact_idx;
    }

    // --- step 2: same MAC, different module_id ---
    PeerEntry* same_mac = NULL;
    int same_mac_idx = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (memcmp(s_peers[i].mac, mac, 6) == 0 &&
            s_peers[i].module_id != module_id) {
            same_mac    = &s_peers[i];
            same_mac_idx = i;
            break;
        }
    }

    if (same_mac != NULL) {
        TickType_t now = xTaskGetTickCount();
        bool changed = (same_mac->module_id != module_id) ||
                       (strncmp(same_mac->name, name, 16) != 0);

        if (same_mac->state == PEER_PENDING_CHANGE) {
            // Already pending — check if this new announce matches the pending values
            // The entry already has the updated values from the first change
            if (!changed) {
                // Same values as before → confirm the change
                same_mac->state = PEER_ACTIVE;
            }
            // Timeout revert
            if (now - same_mac->pending_change_tick >
                pdMS_TO_TICKS(CONFIG_PEER_PENDING_TIMEOUT_MS)) {
                same_mac->state = PEER_ACTIVE;
            }
        } else if (same_mac->state == PEER_ACTIVE) {
            if (changed) {
                // First detection of change → PENDING_CHANGE
                same_mac->state = PEER_PENDING_CHANGE;
                same_mac->pending_change_tick = now;
                ESP_LOGD(TAG, "peer %s module_id changed → PENDING_CHANGE (idx %d)",
                         same_mac->peer_id, same_mac_idx);
            }
        } else {
            // NEW / OFFLINE → revive as ACTIVE
            same_mac->state = PEER_ACTIVE;
        }

        // Update values
        size_t nlen = strlen(name);
        if (nlen > 16) nlen = 16;
        memcpy(same_mac->name, name, nlen);
        same_mac->name[nlen] = '\0';
        same_mac->module_id = module_id;
        memcpy(same_mac->mac, mac, 6);   // mac could have changed too
        // Rebuild peer_id
        snprintf(same_mac->peer_id, PEER_ID_STR_LEN,
                 "%02x:%02x:%02x:%02x:%02x:%02x:%u",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned)module_id);
        same_mac->last_seen = now;

        PEER_UNLOCK();
        return same_mac_idx;
    }

    // --- step 3: same module_id, different MAC (ID conflict) ---
    PeerEntry* same_id = NULL;
    int same_id_idx = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id == module_id &&
            memcmp(s_peers[i].mac, mac, 6) != 0) {
            same_id    = &s_peers[i];
            same_id_idx = i;
            break;
        }
    }

    if (same_id != NULL) {
        TickType_t now = xTaskGetTickCount();
        bool changed = (memcmp(same_id->mac, mac, 6) != 0) ||
                       (strncmp(same_id->name, name, 16) != 0);

        if (same_id->state == PEER_PENDING_CHANGE) {
            if (!changed) {
                same_id->state = PEER_ACTIVE;
            }
            if (now - same_id->pending_change_tick >
                pdMS_TO_TICKS(CONFIG_PEER_PENDING_TIMEOUT_MS)) {
                same_id->state = PEER_ACTIVE;
            }
        } else if (same_id->state == PEER_ACTIVE) {
            if (changed) {
                same_id->state = PEER_PENDING_CHANGE;
                same_id->pending_change_tick = now;
                ESP_LOGD(TAG, "peer %s MAC changed → PENDING_CHANGE (idx %d)",
                         same_id->peer_id, same_id_idx);
            }
        } else {
            same_id->state = PEER_ACTIVE;
        }

        size_t nlen = strlen(name);
        if (nlen > 16) nlen = 16;
        memcpy(same_id->name, name, nlen);
        same_id->name[nlen] = '\0';
        same_id->module_id = module_id;
        memcpy(same_id->mac, mac, 6);
        snprintf(same_id->peer_id, PEER_ID_STR_LEN,
                 "%02x:%02x:%02x:%02x:%02x:%02x:%u",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned)module_id);
        same_id->last_seen = now;

        PEER_UNLOCK();
        return same_id_idx;
    }

    PEER_UNLOCK();

    // --- step 4: not found → delegate to insert ---
    return peer_mgr_insert(mac, module_id, name);
}

// ------------------------------------------------------------------
// Remove
// ------------------------------------------------------------------
void peer_mgr_remove(const uint8_t* mac)
{
    if (mac == NULL) return;

    PEER_LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            ESP_LOGI(TAG, "removing peer %s", s_peers[i].peer_id);
            memset(&s_peers[i], 0, sizeof(PeerEntry));
            s_peer_count--;
            break;
        }
    }
    PEER_UNLOCK();
}

// ------------------------------------------------------------------
// Find helpers  (linear search, return pointer or NULL)
// ------------------------------------------------------------------
PeerEntry* peer_mgr_find_by_mac(const uint8_t* mac, bool* out_conflict)
{
    if (mac == NULL) {
        if (out_conflict) *out_conflict = false;
        return NULL;
    }
    if (out_conflict) *out_conflict = false;

    PEER_LOCK();
    PeerEntry* first = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            if (!first) {
                first = &s_peers[i];
            } else if (out_conflict) {
                *out_conflict = true;
            }
        }
    }
    PEER_UNLOCK();
    return first;
}

PeerEntry* peer_mgr_find_by_id(uint8_t module_id, bool* out_conflict)
{
    if (out_conflict) *out_conflict = false;

    PEER_LOCK();
    int match_count = 0;
    PeerEntry* first = NULL;
    PeerEntry* first_active = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id == module_id) {
            match_count++;
            if (first == NULL) first = &s_peers[i];
            if (s_peers[i].state == PEER_ACTIVE && first_active == NULL) {
                first_active = &s_peers[i];
            }
        }
    }
    if (out_conflict) *out_conflict = (match_count > 1);
    PEER_UNLOCK();
    return first_active ? first_active : first;
}

PeerEntry* peer_mgr_find_by_name(const char* name, bool* out_conflict)
{
    if (name == NULL) {
        if (out_conflict) *out_conflict = false;
        return NULL;
    }
    if (out_conflict) *out_conflict = false;

    PEER_LOCK();
    int match_count = 0;
    PeerEntry* first = NULL;
    PeerEntry* first_active = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (strncmp(s_peers[i].name, name, 16) == 0) {
            match_count++;
            if (first == NULL) first = &s_peers[i];
            if (s_peers[i].state == PEER_ACTIVE && first_active == NULL) {
                first_active = &s_peers[i];
            }
        }
    }
    if (out_conflict) *out_conflict = (match_count > 1);
    PEER_UNLOCK();
    return first_active ? first_active : first;
}

// ------------------------------------------------------------------
// Canonical device-type resolution (fuzzy addressing helpers)
// ------------------------------------------------------------------

// Case-insensitive substring test (ASCII case folding only; multi-byte
// UTF-8 sequences such as Chinese are compared byte-wise, which is exactly
// what we want for literal synonym matching).
static bool str_icontains(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char* p = hay; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

bool peer_mgr_matches_type(const PeerEntry* entry, const char* type)
{
    if (!entry || !type) return false;

    // Search both the human name and the capability descriptor.
    #define PEER_HAS(s) (str_icontains(entry->name, (s)) || \
                         str_icontains(entry->capability, (s)))

    // Actuators
    if (strcmp(type, "servo") == 0) {
        return PEER_HAS("servo");
    }
    if (strcmp(type, "buzzer") == 0) {
        return PEER_HAS("buzzer") || PEER_HAS("doorbell") || PEER_HAS("beep");
    }

    // Sensor sub-types (specific) — let the model/interpreter pick the right
    // sensor when several are online (e.g. "温度" vs "光照").
    if (strcmp(type, "temperature") == 0) {
        return PEER_HAS("temp");
    }
    if (strcmp(type, "humidity") == 0) {
        return PEER_HAS("humid");
    }
    if (strcmp(type, "light") == 0) {
        return PEER_HAS("light") || PEER_HAS("lux") || PEER_HAS("bh1750");
    }
    if (strcmp(type, "gas") == 0) {
        return PEER_HAS("co2") || PEER_HAS("tvoc") || PEER_HAS("ch2o") ||
               PEER_HAS("gas") || PEER_HAS("air")  || PEER_HAS("jw01");
    }
    if (strcmp(type, "rain") == 0) {
        return PEER_HAS("rain");
    }
    if (strcmp(type, "vibration") == 0) {
        return PEER_HAS("vibrat");
    }

    // Generic sensor (broad — matches any readable module)
    if (strcmp(type, "sensor") == 0) {
        return PEER_HAS("sensor") || PEER_HAS("remote_read") || PEER_HAS("adc");
    }

    #undef PEER_HAS
    return false;
}

// -------------------------------------------------------------------------
// Reference-word -> canonical-type synonym table.
//
// This is the ONE place to extend spoken / dialect / multilingual synonyms:
// just add a { "word", "type" } row.  Matching is case-insensitive substring,
// so "温度传感器现在多少" already contains "温度".
//
// ORDER MATTERS: more specific words (sensor sub-types) must appear BEFORE
// the generic "sensor" / "传感器" row, so "温度传感器" resolves to
// temperature rather than the generic sensor bucket.
// -------------------------------------------------------------------------
typedef struct {
    const char* word;   // reference word (English or Chinese)
    const char* type;   // canonical type tag
} SynonymEntry;

static const SynonymEntry kSynonyms[] = {
    // actuators
    { "servo", "servo" }, { "舵机", "servo" }, { "伺服", "servo" },
    { "buzzer", "buzzer" }, { "doorbell", "buzzer" }, { "beeper", "buzzer" },
    { "蜂鸣器", "buzzer" }, { "门铃", "buzzer" }, { "喇叭", "buzzer" },
    // sensor sub-types (specific — keep before the generic sensor rows)
    { "温度", "temperature" }, { "temperature", "temperature" }, { "temp", "temperature" },
    { "湿度", "humidity" }, { "humidity", "humidity" }, { "humid", "humidity" },
    { "光照", "light" }, { "亮度", "light" }, { "light", "light" }, { "lux", "light" },
    { "气体", "gas" }, { "空气", "gas" }, { "gas", "gas" }, { "co2", "gas" },
    { "雨", "rain" }, { "rain", "rain" },
    { "振动", "vibration" }, { "vibration", "vibration" },
    // generic sensor (keep last so specific types win)
    { "传感器", "sensor" }, { "sensor", "sensor" },
};

const char* peer_mgr_type_from_query(const char* query)
{
    if (!query) return NULL;
    for (size_t i = 0; i < sizeof(kSynonyms) / sizeof(kSynonyms[0]); i++) {
        if (str_icontains(query, kSynonyms[i].word)) {
            return kSynonyms[i].type;
        }
    }
    return NULL;
}

void peer_mgr_type_tags(const PeerEntry* entry, char* out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!entry) return;

    // Broad type(s) first, then any specific sensor sub-types the device
    // supports, so the model can match "温度" to a multi-sensor module.
    static const char* kTags[] = {
        "servo", "buzzer", "sensor",
        "temperature", "humidity", "light", "gas", "rain", "vibration"
    };
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++) {
        if (!peer_mgr_matches_type(entry, kTags[i])) continue;
        int w = snprintf(out + pos, out_len - pos, "%s%s",
                         pos ? "," : "", kTags[i]);
        if (w < 0) break;
        pos += (size_t)w;
        if (pos >= out_len) { out[out_len - 1] = '\0'; break; }
    }
}

PeerEntry* peer_mgr_find_by_type(const char* type, int* out_count)
{
    if (out_count) *out_count = 0;
    if (!type) return NULL;

    PEER_LOCK();
    int match_count = 0;
    PeerEntry* first = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].state != PEER_ACTIVE) continue;
        if (peer_mgr_matches_type(&s_peers[i], type)) {
            match_count++;
            if (first == NULL) first = &s_peers[i];
        }
    }
    if (out_count) *out_count = match_count;
    PEER_UNLOCK();
    return first;
}

PeerEntry* peer_mgr_find_by_mac_and_id(const uint8_t* mac, uint8_t module_id)
{
    if (mac == NULL) return NULL;
    PEER_LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].module_id == module_id &&
            memcmp(s_peers[i].mac, mac, 6) == 0) {
            PEER_UNLOCK();
            return &s_peers[i];
        }
    }
    PEER_UNLOCK();
    return NULL;
}

// ------------------------------------------------------------------
// Count helpers
// ------------------------------------------------------------------
int peer_mgr_active_count(void)
{
    int cnt = 0;
    PEER_LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].state == PEER_ACTIVE) cnt++;
    }
    PEER_UNLOCK();
    return cnt;
}

int peer_mgr_total_count(void)
{
    int cnt = 0;
    PEER_LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!ENTRY_IS_EMPTY(&s_peers[i])) cnt++;
    }
    PEER_UNLOCK();
    return cnt;
}

// ------------------------------------------------------------------
// List active entries (returns pointer to internal scratch array)
// ------------------------------------------------------------------
PeerEntry** peer_mgr_list(int* count)
{
    if (count == NULL) return NULL;

    PEER_LOCK();
    int idx = 0;
    for (int i = 0; i < MAX_PEERS && idx < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        if (s_peers[i].state == PEER_ACTIVE) {
            s_active_list[idx++] = &s_peers[i];
        }
    }
    *count = idx;
    PEER_UNLOCK();
    return s_active_list;
}

// ------------------------------------------------------------------
// Age scan — lock per entry, not whole scan (design.md §4.4)
// ------------------------------------------------------------------
void peer_mgr_age_scan(TickType_t now)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        PEER_LOCK();
        if (ENTRY_IS_EMPTY(&s_peers[i])) {
            PEER_UNLOCK();
            continue;
        }
        PeerEntry* e = &s_peers[i];
        if ((e->state == PEER_ACTIVE || e->state == PEER_PENDING_CHANGE) &&
            (now - e->last_seen > pdMS_TO_TICKS(CONFIG_PEER_TIMEOUT_MS))) {
            e->state = PEER_OFFLINE;
            ESP_LOGI(TAG, "peer %s timed out → OFFLINE", e->peer_id);

            // Remove from ESP-NOW internal peer table so the slot
            // can be reused.  Stale entries prevent esp_now_add_peer
            // from succeeding for new sensors.
            esp_now_del_peer(e->mac);
        }
        PEER_UNLOCK();
    }
}

// ------------------------------------------------------------------
// State helpers
// ------------------------------------------------------------------
void peer_mgr_set_pending(PeerEntry* entry)
{
    if (entry == NULL) return;
    PEER_LOCK();
    entry->state = PEER_PENDING_CHANGE;
    entry->pending_change_tick = xTaskGetTickCount();
    PEER_UNLOCK();
}

bool peer_mgr_is_duplicate(PeerEntry* entry, uint8_t seq_id)
{
    if (entry == NULL) return false;

    // Ring bitmap: dedup_seq[seq_id % 8] stores the last seq_id seen in this slot
    uint8_t slot = seq_id % 8;
    PEER_LOCK();
    if (entry->dedup_seq[slot] == seq_id) {
        PEER_UNLOCK();
        return true;    // duplicate
    }
    entry->dedup_seq[slot] = seq_id;
    entry->dedup_count++;
    PEER_UNLOCK();
    return false;
}

// ------------------------------------------------------------------
// ESP-NOW peer re-add (after WiFi stop/start cycle)
// ------------------------------------------------------------------
void peer_mgr_espnow_readd_all(void)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        PEER_LOCK();
        if (ENTRY_IS_EMPTY(&s_peers[i])) {
            PEER_UNLOCK();
            continue;
        }
        PeerEntry* e = &s_peers[i];
        if (e->state != PEER_ACTIVE) {
            PEER_UNLOCK();
            continue;
        }

        // Re-add to ESP-NOW internal table
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, e->mac, 6);
#if defined(CONFIG_SOFTAP_CHANNEL) && CONFIG_SOFTAP_CHANNEL > 0
        peer.channel = CONFIG_SOFTAP_CHANNEL;
#else
        peer.channel = 1;
#endif
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;

        esp_err_t ret = esp_now_add_peer(&peer);
        if (ret == ESP_OK) {
            // Reset last_seen to now so the peer doesn't immediately
            // timeout when aging resumes after WiFi restart.
            e->last_seen = xTaskGetTickCount();
            ESP_LOGI(TAG, "re-added ESP-NOW peer %s channel %u",
                     e->peer_id, (unsigned)peer.channel);
        } else if (ret != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "re-add peer %s failed: %d", e->peer_id, ret);
        }
        PEER_UNLOCK();
    }

    ESP_LOGI(TAG, "ESP-NOW peer re-add complete, %d active", peer_mgr_active_count());
}
