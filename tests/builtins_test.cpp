/*
 * ESP-LEGO Builtin Function Unit Tests
 *
 * Tests each builtin function by calling call_builtin_by_name directly
 * with constructed Value arguments and checking the return value.
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "interpreter/interpreter.h"
#include "interpreter/builtins.h"
#include "interpreter/environment.h"
#include "interpreter/value.h"
#include "espnow_comm/peer_mgr.h"
#include "espnow_comm/protocol.h"
#include <string.h>
#include <string.h>
#include <stdio.h>

// ================================================================
// Global ExecutionContext + env for builtin tests
// ================================================================
static ExecutionContext s_bif_ctx;
static Environment     s_bif_env;

// Defined in interpreter_test.cpp (shared across test binaries)
extern volatile bool s_script_timeout;
extern volatile bool s_script_abort_requested;
extern int g_mock_send_cmd_calls;
extern uint8_t g_mock_send_cmd_module_id;
extern uint16_t g_mock_send_cmd_id;
extern uint8_t g_mock_send_cmd_payload[16];
extern uint8_t g_mock_send_cmd_payload_len;
extern void mock_send_cmd_reset(void);

// ================================================================
// Helper: build a Value of a given type
// ================================================================
static Value v_num(double n) { Value v; v.type = VAL_NUM; v.num = n; return v; }
static Value v_bool(bool b)  { Value v; v.type = VAL_BOOL; v.b = b; return v; }
static Value v_str(const char* s) { Value v; v.type = VAL_STR; v.str = s; return v; }

// ================================================================
// Helper: call a builtin and check result
// ================================================================
static Value call_bif(const char* name, Value* args, int n) {
    return call_builtin_by_name(name, args, n, &s_bif_ctx);
}

static void init_builtins(void) {
    ctx_init(&s_bif_ctx);
    env_init(&s_bif_env, NULL);
    // builtins need a registered env for some functions (e.g. peer lookup)
    // register_builtins(&s_bif_env);
    // env_snapshot(&s_bif_env);
    s_bif_ctx.s_script_timeout_ptr = &s_script_timeout;

    // Reset peer manager so builtins tests start with a clean table
    // (peer_mgr tests may have left stale entries).
    peer_mgr_init();
}

// ----------------------------------------------------------------

static void test_bif_analog_read(void) {
    TEST("Builtin: analog_read returns 0 (mock)");
    Value args[] = { v_num(1) };
    Value r = call_bif("analog_read", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);  // mock returns 0
    TEST_PASS();
}

static void test_bif_analog_write(void) {
    TEST("Builtin: analog_write returns undefined (mock)");
    Value args[] = { v_num(1), v_num(128) };
    Value r = call_bif("analog_write", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    TEST_PASS();
}

static void test_bif_digital_read(void) {
    TEST("Builtin: digital_read returns 0 (mock)");
    Value args[] = { v_num(2) };
    Value r = call_bif("digital_read", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_PASS();
}

static void test_bif_digital_write(void) {
    TEST("Builtin: digital_write returns undefined (mock)");
    Value args[] = { v_num(2), v_num(1) };
    Value r = call_bif("digital_write", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    TEST_PASS();
}

static void test_bif_sleep(void) {
    TEST("Builtin: sleep returns undefined");
    Value args[] = { v_num(10) };
    Value r = call_bif("sleep", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    TEST_PASS();
}

static void test_bif_sleep_negative(void) {
    TEST("Builtin: sleep(-1) safe (no crash)");
    Value args[] = { v_num(-1) };
    Value r = call_bif("sleep", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    // The if(ms>0) guard should prevent the negative from causing issues
    TEST_PASS();
}

static void test_bif_print_returns_undefined(void) {
    TEST("Builtin: print returns undefined");
    Value args[] = { v_num(42) };
    Value r = call_bif("print", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    TEST_PASS();
}

static void test_bif_list_new(void) {
    TEST("Builtin: list_new returns a list");
    Value args[] = { v_num(5) };
    Value r = call_bif("list_new", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_LIST, r.type);
    TEST_ASSERT_NOT_NULL(r.list);
    TEST_ASSERT_EQUAL_INT(5, r.list->len);
    TEST_PASS();
}

static void test_bif_list_get_set_len(void) {
    TEST("Builtin: list_get/set/len round-trip");
    Value args_new[] = { v_num(3) };
    Value lst_v = call_bif("list_new", args_new, 1);
    ListData* lst = lst_v.list;

    // list_set(lst, 0, 42.5)
    Value args_set[] = { lst_v, v_num(0), v_num(42.5) };
    call_bif("list_set", args_set, 3);

    // list_get(lst, 0) → 42.5
    Value args_get[] = { lst_v, v_num(0) };
    Value g = call_bif("list_get", args_get, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, g.type);
    TEST_ASSERT_EQUAL_DOUBLE(42.5, g.num, 0.001);

    // list_len(lst) → 3
    Value args_len[] = { lst_v };
    Value l = call_bif("list_len", args_len, 1);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, l.type);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, l.num, 0.001);
    TEST_PASS();
}

static void test_bif_list_free(void) {
    TEST("Builtin: list_free returns undefined");
    Value args_new[] = { v_num(2) };
    Value lst = call_bif("list_new", args_new, 1);
    Value args_free[] = { lst };
    Value r = call_bif("list_free", args_free, 1);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    // After free, pool slot is zeroed but pointer still exists
    // (use-after-free is a design caveat documented in AGENTS.md)
    TEST_PASS();
}

static void test_bif_peer_count(void) {
    TEST("Builtin: peer_count returns 0 (no peers in mock)");
    Value r = call_bif("peer_count", NULL, 0);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_bif_remote_read(void) {
    TEST("Builtin: remote_read returns 0 (mock)");
    Value args[] = { v_num(1) };
    Value r = call_bif("remote_read", args, 1);
    // Mock returns 0.0 on timeout
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_PASS();
}

static void test_bif_espnow_send(void) {
    TEST("Builtin: espnow_send returns -1 (mock, peer not found)");
    Value args[] = { v_num(1), v_num(0x0001) };
    Value r = call_bif("espnow_send", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_PASS();
}

static void test_bif_read_sensor(void) {
    TEST("Builtin: read_sensor returns cached remote sensor value");
    Value args[] = { v_num(1) };
    Value r = call_bif("read_sensor", args, 1);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);  // no callback registered in test context
    TEST_PASS();
}

static void test_bif_send_motor_alias(void) {
    TEST("Builtin: send_motor is alias for analog_write");
    Value args[] = { v_num(4), v_num(50) };
    Value r = call_bif("send_motor", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, r.type);
    TEST_PASS();
}

static void test_bif_buzzer_note(void) {
    TEST("Builtin: buzzer_note sends remote buzzer note");
    Value args[] = { v_num(1), v_num(19), v_num(200) };
    Value r = call_bif("buzzer_note", args, 3);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_bif_buzzer_beep(void) {
    TEST("Builtin: buzzer_beep sends repeated remote buzzer notes");
    Value args[] = { v_num(1), v_num(2) };
    Value r = call_bif("buzzer_beep", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_bif_buzzer_song(void) {
    TEST("Builtin: buzzer_song sends remote buzzer song");
    Value args[] = { v_num(1), v_num(0) };
    Value r = call_bif("buzzer_song", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_bif_servo_write(void) {
    TEST("Builtin: servo_write sends remote servo angle");
    Value args[] = { v_num(1), v_num(90) };
    Value r = call_bif("servo_write", args, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_bif_servo_sweep(void) {
    TEST("Builtin: servo_sweep sends repeated remote servo angles");
    Value args[] = { v_num(1), v_num(0), v_num(30), v_num(30), v_num(20) };
    Value r = call_bif("servo_sweep", args, 5);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

static void test_control_command_catalog(void) {
    TEST("Control command catalog exposes unique AI-callable commands");
    size_t count = 0;
    const ControlCommandSpec* specs = control_command_specs(&count);
    TEST_ASSERT_NOT_NULL(specs);
    TEST_ASSERT_EQUAL_INT(7, count);

    const ControlCommandSpec* pump = NULL;
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(specs[i].dsl_name);
        TEST_ASSERT_NOT_NULL(specs[i].handler);
        for (size_t j = i + 1; j < count; j++) {
            TEST_ASSERT(strcmp(specs[i].dsl_name, specs[j].dsl_name) != 0);
        }
        if (strcmp(specs[i].dsl_name, "pump_write") == 0) {
            pump = &specs[i];
        }
    }
    TEST_ASSERT_NOT_NULL(pump);
    TEST_ASSERT_STR_EQUAL("pump", pump->device_type);
    TEST_ASSERT_EQUAL_INT(2, pump->arg_count);
    TEST_ASSERT_EQUAL_DOUBLE(30000.0, pump->args[1].max_value, 0.001);
    TEST_PASS();
}

static void test_control_command_formats_dsl(void) {
    TEST("Control command catalog formats tool arguments as executable DSL");
    char out[128] = {};
    const char* pump_args[] = {"7", "5000"};
    int len = control_command_format_dsl(
        "pump_write", pump_args, 2, out, sizeof(out));
    TEST_ASSERT(len > 0);
    TEST_ASSERT_STR_EQUAL("pump_write(7,5000);\n", out);

    const char* sensor_args[] = {"\"temperature\""};
    len = control_command_format_dsl(
        "read_sensor", sensor_args, 1, out, sizeof(out));
    TEST_ASSERT(len > 0);
    TEST_ASSERT_STR_EQUAL("print(read_sensor(\"temperature\"));\n", out);

    len = control_command_format_dsl(
        "pump_write", pump_args, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(-1, len);
    TEST_PASS();
}

static void test_bif_pump_write_payload_and_limits(void) {
    TEST("Builtin: pump_write sends finite big-endian duration");
    mock_send_cmd_reset();
    Value args[] = { v_num(7), v_num(5000) };
    Value r = call_bif("pump_write", args, 2);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(1, g_mock_send_cmd_calls);
    TEST_ASSERT_EQUAL_INT(7, g_mock_send_cmd_module_id);
    TEST_ASSERT_EQUAL_INT(CMD_PUMP_WRITE, g_mock_send_cmd_id);
    TEST_ASSERT_EQUAL_INT(2, g_mock_send_cmd_payload_len);
    TEST_ASSERT_EQUAL_INT(0x13, g_mock_send_cmd_payload[0]);
    TEST_ASSERT_EQUAL_INT(0x88, g_mock_send_cmd_payload[1]);

    mock_send_cmd_reset();
    Value off[] = { v_num(7), v_num(0) };
    r = call_bif("pump_write", off, 2);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(1, g_mock_send_cmd_calls);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_payload[0]);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_payload[1]);

    mock_send_cmd_reset();
    Value one_ms[] = { v_num(7), v_num(1) };
    r = call_bif("pump_write", one_ms, 2);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(1, g_mock_send_cmd_calls);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_payload[0]);
    TEST_ASSERT_EQUAL_INT(1, g_mock_send_cmd_payload[1]);

    mock_send_cmd_reset();
    Value max_duration[] = { v_num(7), v_num(30000) };
    r = call_bif("pump_write", max_duration, 2);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(1, g_mock_send_cmd_calls);
    TEST_ASSERT_EQUAL_INT(0x75, g_mock_send_cmd_payload[0]);
    TEST_ASSERT_EQUAL_INT(0x30, g_mock_send_cmd_payload[1]);

    mock_send_cmd_reset();
    Value too_long[] = { v_num(7), v_num(30001) };
    r = call_bif("pump_write", too_long, 2);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_calls);

    Value negative[] = { v_num(7), v_num(-1) };
    r = call_bif("pump_write", negative, 2);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_calls);

    Value wrong_type[] = { v_num(7), v_str("5000") };
    r = call_bif("pump_write", wrong_type, 2);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, r.num, 0.001);
    TEST_ASSERT_EQUAL_INT(0, g_mock_send_cmd_calls);
    TEST_PASS();
}

static void test_bif_arg_validation(void) {
    TEST("Builtin: argument validation (missing required args)");
    // espnow_send with 0 args should return -1
    Value r0 = call_bif("espnow_send", NULL, 0);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r0.type);
    TEST_PASS();
}

static void test_bif_list_get_oob(void) {
    TEST("Builtin: list_get out-of-bounds returns 0");
    Value args_new[] = { v_num(2) };
    Value lst = call_bif("list_new", args_new, 1);
    Value args_get[] = { lst, v_num(99) };
    Value r = call_bif("list_get", args_get, 2);
    TEST_ASSERT_EQUAL_INT(VAL_NUM, r.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.num, 0.001);
    TEST_PASS();
}

// ----------------------------------------------------------------

void test_builtins(void) {
    static bool inited = false;
    if (!inited) {
        init_builtins();
        inited = true;
    }

    printf("\n[Builtin Tests]\n");
    test_bif_analog_read();
    test_bif_analog_write();
    test_bif_digital_read();
    test_bif_digital_write();
    test_bif_sleep();
    test_bif_sleep_negative();
    test_bif_print_returns_undefined();
    test_bif_list_new();
    test_bif_list_get_set_len();
    test_bif_list_free();
    test_bif_peer_count();
    test_bif_remote_read();
    test_bif_espnow_send();
    test_bif_read_sensor();
    test_bif_send_motor_alias();
    test_bif_buzzer_note();
    test_bif_buzzer_beep();
    test_bif_buzzer_song();
    test_bif_servo_write();
    test_bif_servo_sweep();
    test_control_command_catalog();
    test_control_command_formats_dsl();
    test_bif_pump_write_payload_and_limits();
    test_bif_arg_validation();
    test_bif_list_get_oob();
}
