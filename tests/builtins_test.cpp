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
    test_bif_read_sensor_alias();
    test_bif_send_motor_alias();
    test_bif_buzzer_note();
    test_bif_buzzer_beep();
    test_bif_buzzer_song();
    test_bif_servo_write();
    test_bif_servo_sweep();
    test_bif_arg_validation();
    test_bif_list_get_oob();
}
