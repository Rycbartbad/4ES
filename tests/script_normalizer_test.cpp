#include "test_runner.h"
#include "web_console/script_normalizer.h"

static void expect_normalized(const char* input, const char* expected)
{
    char output[512] = {};
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response(input, output, sizeof(output)),
        SCRIPT_NORMALIZE_OK);
    TEST_ASSERT_STR_EQUAL(output, expected);
}

static void test_script_normalizer_formats(void)
{
    TEST("Script normalizer: raw, Markdown and JSON responses");
    expect_normalized("  print(1);  ", "print(1);");
    expect_normalized("```javascript\nprint(2);\n```", "print(2);");
    expect_normalized(
        "{\"script\":\"var humidity = read_sensor(\\\"humidity\\\");\\nprint(humidity);\"}",
        "var humidity = read_sensor(\"humidity\");\nprint(humidity);");
    expect_normalized(
        "```json\n{\"status\":\"ok\",\"script\":\"print(3);\"}\n```",
        "print(3);");
    TEST_PASS();
}

static void test_script_normalizer_rejects_bad_wrappers(void)
{
    TEST("Script normalizer: malformed wrappers are never injected");
    char output[128] = {};
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response("{\"code\":\"print(1);\"}", output,
                                  sizeof(output)),
        SCRIPT_NORMALIZE_MISSING_SCRIPT);
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response("{\"script\":123}", output,
                                  sizeof(output)),
        SCRIPT_NORMALIZE_INVALID_JSON);
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response("{\"script\":\"print(1);\" trailing}", output,
                                  sizeof(output)),
        SCRIPT_NORMALIZE_INVALID_JSON);
    TEST_PASS();
}

static void test_script_normalizer_loop_safety(void)
{
    TEST("Script normalizer: unyielding infinite loops are rejected");
    char output[256] = {};
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response("while (true) { print(1); }", output,
                                  sizeof(output)),
        SCRIPT_NORMALIZE_UNSAFE_LOOP);
    TEST_ASSERT_EQUAL_INT(
        script_normalize_response(
            "while (true) { print(1); sleep(1000); }", output,
            sizeof(output)),
        SCRIPT_NORMALIZE_UNSAFE_LOOP);
    expect_normalized(
        "var i=0; while (i<20) { print(1); sleep(1000); i=i+1; }",
        "var i=0; while (i<20) { print(1); sleep(1000); i=i+1; }");
    TEST_PASS();
}

void test_script_normalizer(void)
{
    test_script_normalizer_formats();
    test_script_normalizer_rejects_bad_wrappers();
    test_script_normalizer_loop_safety();
}
