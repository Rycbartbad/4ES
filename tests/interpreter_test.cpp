/*
 * ESP-LEGO Interpreter Unit Tests
 *
 * Tests full pipeline: lex → parse → execute.
 * Uses the same initialization as the script_runner.
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "interpreter/intern.h"
#include "interpreter/ast.h"
#include "interpreter/lexer.h"
#include "interpreter/parser.h"
#include "interpreter/environment.h"
#include "interpreter/interpreter.h"
#include "interpreter/builtins.h"
#include <string.h>
#include <stdio.h>

static Environment      s_env;
static ExecutionContext s_ctx;
volatile bool s_script_timeout = false;
volatile bool s_script_abort_requested = false;

static char s_output[4096];
static int  s_out_len = 0;

static void cap(const char* s, int l) {
    if (s_out_len + l < (int)sizeof(s_output) - 1) {
        memcpy(s_output + s_out_len, s, (size_t)l);
        s_out_len += l; s_output[s_out_len] = 0;
    }
}

static void init(void) {
    ast_pool_init(); env_init(&s_env, NULL);
    register_builtins(&s_env); env_snapshot(&s_env);
    ctx_init(&s_ctx); g_print_callback = cap;
}

static bool run(const char* src) {
    ast_pool_reset(); intern_reset(); env_restore_pristine(&s_env);
    ctx_reset(&s_ctx); s_script_timeout = false;
    s_script_abort_requested = false;
    s_ctx.s_script_timeout_ptr = &s_script_timeout;
    s_output[0] = 0; s_out_len = 0;

    Lexer l; lexer_init(&l, src);
    Parser p; parser_init(&p, &l);
    ASTNode* ast = parser_parse(&p);
    if (p.had_error) return false;

    ResourceReport rpt = validate_resources(ast, &s_env);
    if (!rpt.passed) return false;

    execute(ast, &s_env, &s_ctx);
    return !s_ctx.constraint_violated;
}

static void test_arith(void) {
    TEST("Interp: 1+2*3=7"); TEST_ASSERT(run("print(1+2*3);"));
    TEST_ASSERT(strstr(s_output, "7")); TEST_PASS();
}

static void test_var(void) {
    TEST("Interp: var x=42; print(x)"); TEST_ASSERT(run("var x=42;print(x);"));
    TEST_ASSERT(strstr(s_output, "42")); TEST_PASS();
}

static void test_string(void) {
    TEST("Interp: print string"); TEST_ASSERT(run("print(\"hello\");"));
    TEST_ASSERT(strstr(s_output, "hello")); TEST_PASS();
}

static void test_if_true(void) {
    TEST("Interp: if(1) print(10)");
    TEST_ASSERT(run("if(1){print(10);}else{print(20);}"));
    TEST_ASSERT(strstr(s_output, "10")); TEST_PASS();
}

static void test_if_false(void) {
    TEST("Interp: if(0) else print(20)");
    TEST_ASSERT(run("if(0){print(10);}else{print(20);}"));
    TEST_ASSERT(strstr(s_output, "20")); TEST_PASS();
}

static void test_while(void) {
    TEST("Interp: while i<3");
    TEST_ASSERT(run("var i=0;while(i<3){print(i);i=i+1;}"));
    TEST_ASSERT(strstr(s_output, "0") && strstr(s_output, "2")); TEST_PASS();
}

static void test_func(void) {
    TEST("Interp: func add(a,b){return a+b;} print(add(3,4))");
    TEST_ASSERT(run("func add(a,b){return a+b;} var c=add(3,4); print(c);"));
    TEST_ASSERT(strstr(s_output, "7")); TEST_PASS();
}

static void test_func_nest(void) {
    TEST("Interp: func dbl(x){return x*2;} print(dbl(dbl(5)))");
    TEST_ASSERT(run("func dbl(x){return x*2;} var r=dbl(dbl(5));print(r);"));
    TEST_ASSERT(strstr(s_output, "20")); TEST_PASS();
}

static void test_assign(void) {
    TEST("Interp: x=99 changes var");
    TEST_ASSERT(run("var x=0; x=99; print(x);"));
    TEST_ASSERT(strstr(s_output, "99")); TEST_PASS();
}

static void test_compare(void) {
    TEST("Interp: comparison ops");
    TEST_ASSERT(run("print(10>5);print(3<1);print(5==5);print(4!=7);"));
    TEST_ASSERT(strstr(s_output, "true")); TEST_PASS();
}

static void test_logic(void) {
    TEST("Interp: logic ops && || !");
    TEST_ASSERT(run("print(true&&true);print(false||true);print(!false);"));
    TEST_ASSERT(strstr(s_output, "true")); TEST_PASS();
}

static void test_multi_stmt(void) {
    TEST("Interp: multiple stmts");
    TEST_ASSERT(run("var a=1;var b=2;var c=a+b;print(c);"));
    TEST_ASSERT(strstr(s_output, "3")); TEST_PASS();
}

static void test_redecl_builtin(void) {
    TEST("Interp: can't redefine builtin func name");
    run("func print(){}");
    TEST_PASS();
}

static void test_script_isolate(void) {
    TEST("Interp: isolation between scripts");
    run("var x=42;"); run("print(x);");
    TEST_PASS();
}

void test_interpreter(void) {
    static bool once = false; if (!once) { init(); once = true; }
    printf("\n[Interpreter Tests]\n");
    test_arith(); test_var(); test_string(); test_if_true(); test_if_false();
    test_while(); test_func(); test_func_nest(); test_assign();
    test_compare(); test_logic(); test_multi_stmt(); test_redecl_builtin();
    test_script_isolate();
}
