#include "test_runner.h"
#include "hw_drivers/pump_control.h"

typedef struct {
    bool output;
    bool arm_result;
    int set_calls;
    int arm_calls;
    int cancel_calls;
    uint32_t last_ticks;
    bool set_result;
} PumpMock;

static bool mock_set_output(void* context, bool active)
{
    PumpMock* mock = (PumpMock*)context;
    mock->set_calls++;
    if (mock->set_result) {
        mock->output = active;
    }
    return mock->set_result;
}

static bool mock_arm_timer(void* context, uint32_t ticks)
{
    PumpMock* mock = (PumpMock*)context;
    mock->arm_calls++;
    mock->last_ticks = ticks;
    return mock->arm_result;
}

static void mock_cancel_timer(void* context)
{
    PumpMock* mock = (PumpMock*)context;
    mock->cancel_calls++;
}

static void test_pump_stays_off_when_timer_arm_fails(void)
{
    TEST("Pump control: timer arm failure is fail-safe OFF");
    PumpMock mock = {};
    mock.set_result = true;
    PumpControlBackend backend = {
        &mock, mock_set_output, mock_arm_timer, mock_cancel_timer
    };
    PumpControl control = {};

    TEST_ASSERT(pump_control_init(&control, &backend, 10, 30000));
    mock.arm_result = false;
    TEST_ASSERT(!pump_control_apply(&control, 5000));
    TEST_ASSERT(!mock.output);
    TEST_ASSERT(!pump_control_is_active(&control));
    TEST_ASSERT_EQUAL_INT(1, mock.arm_calls);
    TEST_PASS();
}

static void test_pump_rounds_up_to_one_tick_and_enforces_max(void)
{
    TEST("Pump control: finite duration rounds up and respects max");
    PumpMock mock = {};
    mock.arm_result = true;
    mock.set_result = true;
    PumpControlBackend backend = {
        &mock, mock_set_output, mock_arm_timer, mock_cancel_timer
    };
    PumpControl control = {};

    TEST_ASSERT(pump_control_init(&control, &backend, 10, 30000));
    TEST_ASSERT(pump_control_apply(&control, 1));
    TEST_ASSERT_EQUAL_INT(1, mock.last_ticks);
    TEST_ASSERT(mock.output);

    TEST_ASSERT(!pump_control_apply(&control, 30001));
    TEST_ASSERT(!mock.output);
    TEST_ASSERT(!pump_control_is_active(&control));
    TEST_PASS();
}

static void test_pump_off_and_output_failure_are_fail_safe(void)
{
    TEST("Pump control: output failure and stop command remain OFF");
    PumpMock mock = {};
    mock.arm_result = true;
    mock.set_result = true;
    PumpControlBackend backend = {
        &mock, mock_set_output, mock_arm_timer, mock_cancel_timer
    };
    PumpControl control = {};

    TEST_ASSERT(pump_control_init(&control, &backend, 10, 30000));
    mock.set_result = false;
    TEST_ASSERT(!pump_control_apply(&control, 1000));
    TEST_ASSERT(!mock.output);
    TEST_ASSERT(!pump_control_is_active(&control));
    TEST_ASSERT_EQUAL_INT(1, mock.cancel_calls);

    mock.set_result = true;
    TEST_ASSERT(pump_control_apply(&control, 5000));
    TEST_ASSERT(pump_control_apply(&control, 2000));
    TEST_ASSERT_EQUAL_INT(200, mock.last_ticks);
    TEST_ASSERT(pump_control_apply(&control, 0));
    TEST_ASSERT(!mock.output);
    TEST_ASSERT(!pump_control_is_active(&control));
    TEST_ASSERT(mock.cancel_calls >= 2);
    TEST_PASS();
}

void test_pump_control(void)
{
    printf("\n[Pump Control Tests]\n");
    test_pump_stays_off_when_timer_arm_fails();
    test_pump_rounds_up_to_one_tick_and_enforces_max();
    test_pump_off_and_output_failure_are_fail_safe();
}
