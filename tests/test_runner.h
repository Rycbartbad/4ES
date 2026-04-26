#pragma once
/*
 * ESP-LEGO V1.0 — Unit Test Runner
 * Compiles on x86 with mock ESP-IDF headers.
 * Tests: lexer, parser, interpreter, peer_mgr, protocol, environment
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;
static const char* current_test = "";

#define TEST(name) \
    do { current_test = name; tests_run++; printf("  TEST: %s\n", name); } while(0)

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("    FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_INT(a, b) \
    do { \
        if ((int)(a) != (int)(b)) { \
            printf("    FAIL: expected %d, got %d\n", (int)(b), (int)(a)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_DOUBLE(a, b, eps) \
    do { \
        double diff = (a) - (b); \
        if (diff < 0) diff = -diff; \
        if (diff > (eps)) { \
            printf("    FAIL: expected %.6f, got %.6f\n", (double)(b), (double)(a)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_STR_EQUAL(a, b) \
    do { \
        if (!(a) || !(b) || strcmp((a),(b)) != 0) { \
            printf("    FAIL: expected '%s', got '%s'\n", (b)?(b):"NULL", (a)?(a):"NULL"); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(p) \
    do { \
        if ((p) != NULL) { \
            printf("    FAIL: expected NULL, got %p\n", (void*)(p)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(p) \
    do { \
        if ((p) == NULL) { \
            printf("    FAIL: expected non-NULL pointer\n"); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define TEST_PASS() tests_passed++

static int test_runner_summary(void) {
    printf("\n=== RESULTS: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed;
}
