/*
 * ESP-LEGO Environment Unit Tests
 * Tests scope chain, snapshot/restore, pool allocation
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "interpreter/environment.h"
#include "interpreter/value.h"
#include <string.h>

static Value num_val(double n) { Value v; v.type = VAL_NUM; v.num = n; return v; }
static Value str_val(const char* s) { Value v; v.type = VAL_STR; v.str = s; return v; }

static void test_env_basic_define_get(void) {
    TEST("Environment: Basic define and get");

    Environment env;
    env_init(&env, NULL);
    TEST_ASSERT_EQUAL_INT(0, env.count);

    env_define(&env, "x", num_val(42.0));
    Value v = env_get(&env, "x");
    TEST_ASSERT_EQUAL_INT(VAL_NUM, v.type);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, v.num, 0.001);

    TEST_PASS();
}

static void test_env_set_undefined(void) {
    TEST("Environment: Set undefined variable");

    Environment env;
    env_init(&env, NULL);
    int ret = env_set(&env, "x", num_val(5.0));
    TEST_ASSERT(ret == -1); // Should fail — not defined

    TEST_PASS();
}

static void test_env_parent_scope(void) {
    TEST("Environment: Parent scope lookup");

    Environment parent;
    env_init(&parent, NULL);
    env_define(&parent, "a", num_val(1.0));

    Environment child;
    env_init(&child, &parent);
    env_define(&child, "b", num_val(2.0));

    // Child can see its own
    TEST_ASSERT_EQUAL_DOUBLE(2.0, env_get(&child, "b").num, 0.001);
    // Child can see parent's
    TEST_ASSERT_EQUAL_DOUBLE(1.0, env_get(&child, "a").num, 0.001);
    // Parent can't see child's
    Value v = env_get(&parent, "b");
    TEST_ASSERT_EQUAL_INT(VAL_UNDEFINED, v.type);

    TEST_PASS();
}

static void test_env_snapshot_restore(void) {
    TEST("Environment: Snapshot and restore");

    Environment env;
    env_init(&env, NULL);
    env_define(&env, "x", num_val(10.0));

    // Snapshot
    TEST_ASSERT_EQUAL_INT(0, env_snapshot(&env));

    // Modify
    env_define(&env, "y", num_val(20.0));
    TEST_ASSERT_EQUAL_INT(2, env.count);

    // Restore — y should be gone
    env_restore_pristine(&env);
    TEST_ASSERT_EQUAL_INT(1, env.count);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, env_get(&env, "x").num, 0.001);

    TEST_PASS();
}

static void test_env_pool_alloc_free(void) {
    TEST("Environment: Pool alloc and free");

    // Allocate from pool
    Environment* e1 = env_alloc(NULL);
    TEST_ASSERT_NOT_NULL(e1);

    Environment* e2 = env_alloc(NULL);
    TEST_ASSERT_NOT_NULL(e2);

    // Free and re-allocate
    env_free(e1);
    Environment* e3 = env_alloc(NULL);
    TEST_ASSERT_NOT_NULL(e3);
    TEST_ASSERT(e3 == e1); // Should reuse freed slot

    // Clean up so pool exhaustion test has full pool
    env_free(e3);
    env_free(e2);

    TEST_PASS();
}

static void test_env_pool_exhaustion(void) {
    TEST("Environment: Pool exhaustion");

    // Allocate all slots
    Environment* envs[CONFIG_ENV_POOL_SIZE];
    for (int i = 0; i < CONFIG_ENV_POOL_SIZE; i++) {
        envs[i] = env_alloc(NULL);
        TEST_ASSERT_NOT_NULL(envs[i]);
    }

    // Next should fail
    Environment* full = env_alloc(NULL);
    TEST_ASSERT_NULL(full);

    TEST_PASS();
}

static void test_env_bindings_full(void) {
    TEST("Environment: Bindings full");

    Environment env;
    env_init(&env, NULL);
    char names[CONFIG_MAX_BINDINGS][16];

    // Environment bindings retain identifier pointers, so each test name must
    // have stable storage just like interned identifiers in the interpreter.
    for (int i = 0; i < CONFIG_MAX_BINDINGS; i++) {
        snprintf(names[i], sizeof(names[i]), "v%d", i);
        int ret = env_define(&env, names[i], num_val(i));
        TEST_ASSERT_EQUAL_INT(i, ret);
    }

    // Next should fail
    int ret = env_define(&env, "overflow", num_val(999));
    TEST_ASSERT(ret == -1);

    TEST_PASS();
}

void test_environment(void) {
    printf("\n[Environment Tests]\n");
    test_env_basic_define_get();
    test_env_set_undefined();
    test_env_parent_scope();
    test_env_snapshot_restore();
    test_env_pool_alloc_free();
    test_env_pool_exhaustion();
    test_env_bindings_full();
}
