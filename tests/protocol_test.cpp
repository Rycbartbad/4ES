/*
 * ESP-LEGO Protocol Packet Unit Tests
 * Tests packet builders and parsers for all message types
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "espnow_comm/protocol.h"
#include <string.h>
#include <math.h>

static void test_build_announce(void) {
    TEST("Protocol: Build announce packet");

    uint8_t buf[128];
    size_t len = 0;
    protocol_build_announce(buf, &len, "sensor_5", "");

    TEST_ASSERT(len == MSG_HEADER_SIZE + 17); // header + name + cap_len
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_VERSION, hdr.version);
    TEST_ASSERT_EQUAL_INT(MSG_ANNOUNCE, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(17, hdr.payload_len);

    TEST_PASS();
}

static void test_build_data_req(void) {
    TEST("Protocol: Build DATA_REQ (no payload)");

    uint8_t buf[128];
    size_t len = 0;
    protocol_build_data_req(buf, &len, 3, 42);

    TEST_ASSERT(len == MSG_HEADER_SIZE);  // header only, no payload
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_REQ, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(3, hdr.target_id);
    TEST_ASSERT_EQUAL_INT(42, hdr.seq_id);
    TEST_ASSERT_EQUAL_INT(0, hdr.payload_len);  // no payload
    TEST_ASSERT_EQUAL_INT(0, hdr.cmd_id);        // reserved

    TEST_PASS();
}

static void test_build_data_resp_single(void) {
    TEST("Protocol: Build DATA_RESP (single value)");

    uint8_t buf[128];
    size_t len = 0;
    double val = 3.14159;
    protocol_build_data_resp(buf, &len, 1, 99, &val, 1);

    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_RESP, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(1, hdr.target_id);
    TEST_ASSERT_EQUAL_INT(99, hdr.seq_id);

    // Parse back
    double extracted[4];
    int n = protocol_extract_values(buf, (int)len, extracted, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_DOUBLE(val, extracted[0], 0.0001);

    TEST_PASS();
}

static void test_build_data_resp_multi(void) {
    TEST("Protocol: Build DATA_RESP (multiple values)");

    uint8_t buf[128];
    size_t len = 0;
    double vals[3] = {1.1, 2.2, 3.3};
    protocol_build_data_resp(buf, &len, 1, 99, vals, 3);

    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_RESP, hdr.msg_type);
    // payload = 1B count + 3 × 8B = 25
    TEST_ASSERT_EQUAL_INT(25, hdr.payload_len);

    // Parse back
    double extracted[8];
    int n = protocol_extract_values(buf, (int)len, extracted, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_DOUBLE(1.1, extracted[0], 0.0001);
    TEST_ASSERT_EQUAL_DOUBLE(2.2, extracted[1], 0.0001);
    TEST_ASSERT_EQUAL_DOUBLE(3.3, extracted[2], 0.0001);

    TEST_PASS();
}

static void test_build_data_resp_clamp(void) {
    TEST("Protocol: DATA_RESP extract clamps to caller buffer");

    uint8_t buf[128];
    size_t len = 0;
    double vals[3] = {10.0, 20.0, 30.0};
    protocol_build_data_resp(buf, &len, 1, 0, vals, 3);

    // Parse with smaller buffer
    double extracted[2];
    int n = protocol_extract_values(buf, (int)len, extracted, 2);
    TEST_ASSERT_EQUAL_INT(2, n);   // clamped to caller's max
    TEST_ASSERT_EQUAL_DOUBLE(10.0, extracted[0], 0.0001);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, extracted[1], 0.0001);

    TEST_PASS();
}

static void test_build_ack(void) {
    TEST("Protocol: Build ACK");

    uint8_t buf[128];
    size_t len = 0;
    protocol_build_ack(buf, &len, 2, 55);

    TEST_ASSERT(len == MSG_HEADER_SIZE);
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_ACK, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(55, hdr.seq_id);

    TEST_PASS();
}

static void test_parse_header_invalid(void) {
    TEST("Protocol: Parse invalid header");

    // Too short
    uint8_t small[3] = {0x01, 0x10, 0x00};
    MsgHeader hdr;
    TEST_ASSERT(!protocol_parse_header(small, 3, &hdr));

    // Version mismatch
    uint8_t bad_ver[MSG_HEADER_SIZE];
    memset(bad_ver, 0, MSG_HEADER_SIZE);
    bad_ver[0] = 0xFF; // wrong version
    TEST_ASSERT(!protocol_parse_header(bad_ver, MSG_HEADER_SIZE, &hdr));

    TEST_PASS();
}

static void test_roundtrip_all_types(void) {
    TEST("Protocol: Round-trip all types");

    uint8_t buf[128];
    size_t len;

    // Announce (no module_id — master assigns it dynamically)
    protocol_build_announce(buf, &len, "test", "");
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_ANNOUNCE, hdr.msg_type);

    // DATA_REQ (no payload)
    protocol_build_data_req(buf, &len, 5, 100);
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_REQ, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(100, hdr.seq_id);
    TEST_ASSERT_EQUAL_INT(0, hdr.payload_len);

    // DATA_RESP with multiple values
    double vals[2] = {2.718, 1.414};
    protocol_build_data_resp(buf, &len, 1, 42, vals, 2);
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_RESP, hdr.msg_type);
    double extracted[4];
    int n = protocol_extract_values(buf, (int)len, extracted, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_DOUBLE(2.718, extracted[0], 0.0001);
    TEST_ASSERT_EQUAL_DOUBLE(1.414, extracted[1], 0.0001);

    TEST_PASS();
}

void test_protocol(void) {
    printf("\n[Protocol Tests]\n");
    test_build_announce();
    test_build_data_req();
    test_build_data_resp_single();
    test_build_data_resp_multi();
    test_build_data_resp_clamp();
    test_build_ack();
    test_parse_header_invalid();
    test_roundtrip_all_types();
}
