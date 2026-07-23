#include "test_runner.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

// Test counters — defined here, declared extern in test_runner.h for cross-TU sharing
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

// Test suites — implementations in separate test files
extern void test_lexer(void);
extern void test_parser(void);
extern void test_interpreter(void);
extern void test_peer_mgr(void);
extern void test_protocol(void);
extern void test_environment(void);
extern void test_builtins(void);
extern void test_touch_logic(void);
extern void test_ui_sensor_model(void);
extern void test_mic_level(void);
extern void test_script_normalizer(void);

int main(void) {
    printf("ESP-LEGO Unit Tests\n==================\n\n");
    test_lexer();
    test_protocol();
    test_environment();
    test_peer_mgr();
    test_parser();
    test_interpreter();
    test_builtins();
    test_touch_logic();
    test_ui_sensor_model();
    test_mic_level();
    test_script_normalizer();
    return test_runner_summary();
}
