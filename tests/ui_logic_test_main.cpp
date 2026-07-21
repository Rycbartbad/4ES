#include "test_runner.h"

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

extern void test_touch_logic(void);
extern void test_ui_sensor_model(void);

int main(void)
{
    printf("ESP-LEGO UI Logic Tests\n=======================\n\n");
    test_touch_logic();
    test_ui_sensor_model();
    return test_runner_summary();
}
