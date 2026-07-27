/*
 * ESP-LEGO Peer Manager Unit Tests (TC-P2.1 through TC-P2.4)
 * Tests insert/find, age timeout, full table, PENDING_CHANGE state machine
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "espnow_comm/peer_mgr.h"
#include <string.h>

static uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static uint8_t mac3[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
static uint8_t mac4[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04};

static void test_peer_insert_find(void) {
    TEST("TC-P2.1: Insert and find peers");

    peer_mgr_init();

    int idx = peer_mgr_insert(mac1, 1, "sensor_1");
    TEST_ASSERT(idx >= 0);

    PeerEntry* p = peer_mgr_find_by_mac(mac1, NULL);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(1, p->module_id);

    p = peer_mgr_find_by_id(1, NULL);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_STR_EQUAL("sensor_1", p->name);

    // Find non-existent
    p = peer_mgr_find_by_mac(mac2, NULL);
    TEST_ASSERT_NULL(p);

    TEST_PASS();
}

static void test_peer_insert_duplicate(void) {
    TEST("TC-P2.1 bonus: Duplicate insert refreshes last_seen");

    peer_mgr_init();
    peer_mgr_insert(mac1, 1, "sensor_1");
    int count_before = peer_mgr_total_count();

    // Insert same again — should refresh, not create new
    peer_mgr_insert(mac1, 1, "sensor_1");
    int count_after = peer_mgr_total_count();

    TEST_ASSERT_EQUAL_INT(count_before, count_after);

    TEST_PASS();
}

static void test_peer_timeout(void) {
    TEST("TC-P2.2: Peer timeout → OFFLINE");

    peer_mgr_init();

    // Insert with last_seen in the past
    peer_mgr_insert(mac1, 1, "sensor_1");
    PeerEntry* p = peer_mgr_find_by_id(1, NULL);
    TEST_ASSERT_NOT_NULL(p);
    // Set last_seen to old time (simulate last update was long ago)
    p->last_seen = 1; // very old tick
    p->state = PEER_ACTIVE;

    // Run age scan with current tick > last_seen + PEER_TIMEOUT
    TickType_t now = pdMS_TO_TICKS(CONFIG_PEER_TIMEOUT_MS) + 100;
    peer_mgr_age_scan(now);

    TEST_ASSERT_EQUAL_INT(PEER_OFFLINE, p->state);

    TEST_PASS();
}

static void test_peer_full_table(void) {
    TEST("TC-P2.3: Full table — 21st peer rejected");

    peer_mgr_init();

    // Fill all slots
    for (int i = 0; i < CONFIG_MAX_PEERS; i++) {
        uint8_t m[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, (uint8_t)i};
        char name[16];
        snprintf(name, sizeof(name), "s%d", i);
        int ret = peer_mgr_insert(m, (uint8_t)i, name);
        TEST_ASSERT(ret >= 0);
    }

    // Try to add one more
    TEST_ASSERT_EQUAL_INT(CONFIG_MAX_PEERS, peer_mgr_total_count());
    int ret = peer_mgr_insert(mac4, 99, "overflow");
    TEST_ASSERT(ret == -1);

    TEST_PASS();
}

static void test_peer_find_by_name(void) {
    TEST("TC-P2.1 bonus: Find by name");

    peer_mgr_init();
    peer_mgr_insert(mac1, 5, "kitchen_temp");

    PeerEntry* p = peer_mgr_find_by_name("kitchen_temp", NULL);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(5, p->module_id);

    p = peer_mgr_find_by_name("nonexistent", NULL);
    TEST_ASSERT_NULL(p);

    TEST_PASS();
}

static void test_peer_is_duplicate(void) {
    TEST("Bonus: dedup_seq ring bitmap");

    peer_mgr_init();
    peer_mgr_insert(mac1, 1, "sensor_1");
    PeerEntry* p = peer_mgr_find_by_id(1, NULL);
    TEST_ASSERT_NOT_NULL(p);

    // First seq_id — not duplicate
    TEST_ASSERT(!peer_mgr_is_duplicate(p, 0));
    TEST_ASSERT(!peer_mgr_is_duplicate(p, 1));
    TEST_ASSERT(!peer_mgr_is_duplicate(p, 7));

    // Same seq_id — duplicate (hash collision)
    TEST_ASSERT(peer_mgr_is_duplicate(p, 0));
    TEST_ASSERT(peer_mgr_is_duplicate(p, 1));

    // Different hash slot should still be fresh
    TEST_ASSERT(!peer_mgr_is_duplicate(p, 8)); // 8 % 8 = 0 — hash collision!

    TEST_PASS();
}

static void test_device_type_catalog_drives_resolution(void) {
    TEST("Device type catalog resolves actuator and light sensor aliases");

    size_t count = 0;
    const DeviceTypeSpec* specs = peer_mgr_device_type_specs(&count);
    TEST_ASSERT_NOT_NULL(specs);
    TEST_ASSERT(count >= 10);

    const DeviceTypeSpec* pump = NULL;
    const DeviceTypeSpec* light = NULL;
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(specs[i].canonical);
        for (size_t j = i + 1; j < count; j++) {
            TEST_ASSERT(strcmp(specs[i].canonical, specs[j].canonical) != 0);
        }
        if (strcmp(specs[i].canonical, "pump") == 0) {
            pump = &specs[i];
        } else if (strcmp(specs[i].canonical, "light") == 0) {
            light = &specs[i];
        }
    }

    TEST_ASSERT_NOT_NULL(pump);
    TEST_ASSERT_STR_EQUAL("pump", peer_mgr_type_from_query("请打开水泵"));

    PeerEntry entry = {};
    strcpy(entry.name, "garden_pump");
    strcpy(entry.capability, "Pump module: timed water pump control.");
    TEST_ASSERT(peer_mgr_matches_type(&entry, pump->canonical));

    TEST_ASSERT_NOT_NULL(light);
    TEST_ASSERT_STR_EQUAL("light", peer_mgr_type_from_query("读取光照强度"));
    PeerEntry light_entry = {};
    strcpy(light_entry.name, "living_light");
    strcpy(light_entry.capability,
           "BH1750 Light Sensor: Returns 1 value: [lux].");
    TEST_ASSERT(peer_mgr_matches_type(&light_entry, light->canonical));
    TEST_PASS();
}

void test_peer_mgr(void) {
    printf("\n[Peer Manager Tests]\n");
    test_peer_insert_find();
    test_peer_insert_duplicate();
    test_peer_timeout();
    test_peer_full_table();
    test_peer_find_by_name();
    test_peer_is_duplicate();
    test_device_type_catalog_drives_resolution();
}
