/*
 * ESP-LEGO V1.0 — Windows script runner (REPL)
 *
 * Compiles with MinGW g++ using the same mock infrastructure as the
 * unit tests.  Runs the ESP-LEGO interpreter on Windows WITHOUT any
 * ESP32 hardware — perfect for testing script syntax and semantics.
 *
 * Usage:
 *   script_runner.exe "var x = 1 + 2; print(x);"
 *   echo "print(3+4);" | script_runner.exe
 *   script_runner.exe my_script.txt
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

#include "interpreter/intern.h"
#include "interpreter/ast.h"
#include "interpreter/lexer.h"
#include "interpreter/parser.h"
#include "interpreter/environment.h"
#include "interpreter/interpreter.h"
#include "interpreter/builtins.h"

// ====================================================================
// Global interpreter state (same layout as app_main.cpp)
// ====================================================================
static Environment s_global_env;
static ExecutionContext s_ctx;

// Stub for the global timeout/abort flags (defined in app_main.cpp on real HW)
volatile bool s_script_timeout = false;
volatile bool s_script_abort_requested = false;

// ====================================================================
// Read entire file or stdin into a buffer
// ====================================================================
static char* read_input(const char* path) {
    FILE* fp;
    if (path) {
        fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "Error: cannot open '%s'\n", path);
            return NULL;
        }
    } else {
        fp = stdin;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        // stdin or empty — use a fixed buffer
        if (fp != stdin) fclose(fp);
        return NULL;
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fprintf(stderr, "Error: out of memory\n");
        if (fp != stdin) fclose(fp);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    buf[read] = '\0';

    if (fp != stdin) fclose(fp);
    return buf;
}

// ====================================================================
// Run one script
// ====================================================================
static int run_script(const char* source) {
    if (!source || strlen(source) == 0) {
        fprintf(stderr, "Error: empty script\n");
        return -1;
    }

    int len = (int)strlen(source);

    // Reset pools for a fresh script
    ast_pool_reset();
    intern_reset();
    env_restore_pristine(&s_global_env);
    ctx_reset(&s_ctx);

    s_script_timeout = false;
    s_script_abort_requested = false;
    s_ctx.s_script_timeout_ptr = &s_script_timeout;

    // Lex
    Lexer lexer;
    lexer_init(&lexer, source);

    // Parse
    Parser parser;
    parser_init(&parser, &lexer);
    ASTNode* ast = parser_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "Parse error L%d:%d: %s\n",
                parser.error_line, parser.error_col,
                parser.error_msg ? parser.error_msg : "unknown");
        return -1;
    }

    if (!ast) {
        fprintf(stderr, "Error: parser returned NULL AST\n");
        return -1;
    }

    // Validate resources
    ResourceReport rpt = validate_resources(ast, &s_global_env);
    if (!rpt.passed) {
        fprintf(stderr, "Resource validation failed: %s\n",
                rpt.fail_reason ? rpt.fail_reason : "unknown");
        return -1;
    }

    // Execute
    execute(ast, &s_global_env, &s_ctx);

    // Report runtime errors
    if (s_ctx.constraint_violated) {
        fprintf(stderr, "Runtime error: %s\n",
                s_ctx.violation_msg ? s_ctx.violation_msg : "constraint violated");
        return -1;
    }

    return 0;
}

// ====================================================================
// main
// ====================================================================
int main(int argc, char** argv) {
    // Initialise interpreter state
    ast_pool_init();
    env_init(&s_global_env, NULL);
    register_builtins(&s_global_env);
    env_snapshot(&s_global_env);   // save pristine state (builtins only)
    ctx_init(&s_ctx);

    if (argc >= 2) {
        // Argument mode: script source or file path
        const char* arg = argv[1];

        if (arg[0] == '@') {
            // Read from file: "@filename"
            char* source = read_input(arg + 1);
            if (!source) return 1;
            int ret = run_script(source);
            free(source);
            return ret;
        }

        // Treat argument as inline script source
        return run_script(arg);
    }

    // REPL mode: read from stdin until EOF
    printf("ESP-LEGO V1.0 Script Runner (stdin mode)\n");
    printf("Enter script, Ctrl+Z then Enter to execute:\n");

    char* source = read_input(NULL);
    if (!source) return 0;

    int ret = run_script(source);
    free(source);
    return ret;
}
