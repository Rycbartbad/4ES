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
static PeerEntry*       s_all_list[MAX_PEERS];

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

static const char* const kServoAliases[] = {"servo", "舵机", "伺服"};
static const char* const kServoTokens[] = {"servo"};
static const char* const kBuzzerAliases[] = {
    "buzzer", "doorbell", "beeper", "蜂鸣器", "门铃", "喇叭"
};
static const char* const kBuzzerTokens[] = {"buzzer", "doorbell", "beep"};
static const char* const kPumpAliases[] = {"pump", "水泵", "抽水", "泵"};
static const char* const kPumpTokens[] = {"pump"};
static const char* const kSensorAliases[] = {"传感器", "sensor"};
static const char* const kSensorTokens[] = {"sensor", "remote_read", "adc"};
static const char* const kTemperatureAliases[] = {"温度", "temperature", "temp"};
static const char* const kTemperatureTokens[] = {"temp"};
static const char* const kHumidityAliases[] = {"湿度", "humidity", "humid"};
static const char* const kHumidityTokens[] = {"humid"};
static const char* const kLightAliases[] = {"光照", "亮度", "light", "lux"};
static const char* const kLightTokens[] = {"light", "lux", "bh1750"};
static const char* const kGasAliases[] = {"气体", "空气", "gas", "co2"};
static const char* const kGasTokens[] = {
    "co2", "tvoc", "ch2o", "gas", "air", "jw01"
};
static const char* const kRainAliases[] = {"雨", "rain"};
static const char* const kRainTokens[] = {"rain"};
static const char* const kVibrationAliases[] = {"振动", "vibration"};
static const char* const kVibrationTokens[] = {"vibrat"};

#define DEVICE_TYPE_ROW(type_name, alias_array, token_array) \
    { type_name, alias_array, sizeof(alias_array) / sizeof(alias_array[0]), \
      token_array, sizeof(token_array) / sizeof(token_array[0]) }

// Tag order intentionally keeps broad actuator/sensor types before sensor
// sub-types. Query resolution handles the broad "sensor" row last so
// "温度传感器" resolves to temperature rather than sensor.
static const DeviceTypeSpec kDeviceTypeSpecs[] = {
    DEVICE_TYPE_ROW("servo",       kServoAliases,       kServoTokens),
    DEVICE_TYPE_ROW("buzzer",      kBuzzerAliases,      kBuzzerTokens),
    DEVICE_TYPE_ROW("pump",        kPumpAliases,        kPumpTokens),
    DEVICE_TYPE_ROW("sensor",      kSensorAliases,      kSensorTokens),
    DEVICE_TYPE_ROW("temperature", kTemperatureAliases, kTemperatureTokens),
    DEVICE_TYPE_ROW("humidity",    kHumidityAliases,    kHumidityTokens),
    DEVICE_TYPE_ROW("light",       kLightAliases,       kLightTokens),
    DEVICE_TYPE_ROW("gas",         kGasAliases,         kGasTokens),
    DEVICE_TYPE_ROW("rain",        kRainAliases,        kRainTokens),
    DEVICE_TYPE_ROW("vibration",   kVibrationAliases,   kVibrationTokens),
};

#undef DEVICE_TYPE_ROW

const DeviceTypeSpec* peer_mgr_device_type_specs(size_t* out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(kDeviceTypeSpecs) / sizeof(kDeviceTypeSpecs[0]);
    }
    return kDeviceTypeSpecs;
}

static const DeviceTypeSpec* find_device_type_spec(const char* type)
{
    if (type == NULL) return NULL;
    size_t count = 0;
    const DeviceTypeSpec* specs = peer_mgr_device_type_specs(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(specs[i].canonical, type) == 0) {
            return &specs[i];
        }
    }
    return NULL;
}

bool peer_mgr_matches_type(const PeerEntry* entry, const char* type)
{
    if (!entry || !type) return false;

    const DeviceTypeSpec* spec = find_device_type_spec(type);
    if (spec == NULL) return false;
    for (size_t i = 0; i < spec->match_token_count; i++) {
        const char* token = spec->match_tokens[i];
        if (str_icontains(entry->name, token) ||
            str_icontains(entry->capability, token)) {
            return true;
        }
    }
    return false;
}

const char* peer_mgr_type_from_query(const char* query)
{
    if (!query) return NULL;
    size_t count = 0;
    const DeviceTypeSpec* specs = peer_mgr_device_type_specs(&count);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            const bool generic_sensor =
                strcmp(specs[i].canonical, "sensor") == 0;
            if ((pass == 0 && generic_sensor) ||
                (pass == 1 && !generic_sensor)) {
                continue;
            }
            for (size_t j = 0; j < specs[i].alias_count; j++) {
                if (str_icontains(query, specs[i].aliases[j])) {
                    return specs[i].canonical;
                }
            }
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
    size_t count = 0;
    const DeviceTypeSpec* specs = peer_mgr_device_type_specs(&count);
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (!peer_mgr_matches_type(entry, specs[i].canonical)) continue;
        int w = snprintf(out + pos, out_len - pos, "%s%s",
                         pos ? "," : "", specs[i].canonical);
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

PeerEntry** peer_mgr_list_all(int* count)
{
    if (count == NULL) return NULL;

    PEER_LOCK();
    int idx = 0;
    for (int i = 0; i < MAX_PEERS && idx < MAX_PEERS; i++) {
        if (ENTRY_IS_EMPTY(&s_peers[i])) continue;
        s_all_list[idx++] = &s_peers[i];
    }
    *count = idx;
    PEER_UNLOCK();
    return s_all_list;
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
