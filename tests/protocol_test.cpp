/*
 * ESP-LEGO Protocol Packet Unit Tests
 * Tests packet builders and parsers for all message types
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "espnow_comm/protocol.h"
#include <string.h>

static void test_build_announce(void) {
    TEST("Protocol: Build announce packet");

    uint8_t buf[128];
    size_t len = 0;
    protocol_build_announce(buf, &len, 5, "sensor_5");

    TEST_ASSERT(len >= MSG_HEADER_SIZE + 1); // header + at least module_id
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_VERSION, hdr.version);
    TEST_ASSERT_EQUAL_INT(MSG_ANNOUNCE, hdr.msg_type);

    TEST_PASS();
}

static void test_build_data_req(void) {
    TEST("Protocol: Build DATA_REQ");

    uint8_t buf[128];
    size_t len = 0;
    protocol_build_data_req(buf, &len, 3, 42, 7);

    TEST_ASSERT(len == MSG_HEADER_SIZE + DATA_REQ_PIN_SIZE);
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_REQ, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(3, hdr.target_id);
    TEST_ASSERT_EQUAL_INT(42, hdr.seq_id);

    TEST_PASS();
}

static void test_build_data_resp(void) {
    TEST("Protocol: Build DATA_RESP");

    uint8_t buf[128];
    size_t len = 0;
    double value = 3.14159;
    protocol_build_data_resp(buf, &len, 1, 99, value);

    TEST_ASSERT(len == MSG_HEADER_SIZE + DATA_RESP_VAL_SIZE);
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_RESP, hdr.msg_type);

    double extracted;
    TEST_ASSERT(protocol_extract_double(buf, (int)len, &extracted));
    TEST_ASSERT_EQUAL_DOUBLE(value, extracted, 0.0001);

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

    // Announce
    protocol_build_announce(buf, &len, 10, "test");
    MsgHeader hdr;
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_ANNOUNCE, hdr.msg_type);

    // DATA_REQ
    protocol_build_data_req(buf, &len, 5, 100, 3);
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_REQ, hdr.msg_type);
    TEST_ASSERT_EQUAL_INT(100, hdr.seq_id);

    // DATA_RESP with double
    protocol_build_data_resp(buf, &len, 1, 42, 3.14);
    TEST_ASSERT(protocol_parse_header(buf, (int)len, &hdr));
    TEST_ASSERT_EQUAL_INT(MSG_DATA_RESP, hdr.msg_type);
    double v;
    TEST_ASSERT(protocol_extract_double(buf, (int)len, &v));
    TEST_ASSERT_EQUAL_DOUBLE(3.14, v, 0.0001);

    TEST_PASS();
}

void test_protocol(void) {
    printf("\n[Protocol Tests]\n");
    test_build_announce();
    test_build_data_req();
    test_build_data_resp();
    test_build_ack();
    test_parse_header_invalid();
    test_roundtrip_all_types();
}
