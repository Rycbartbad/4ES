/*
 * ESP-LEGO Lexer Unit Tests (TC-P3.1 through TC-P3.9)
 * Tests tokenizer output for numbers, keywords, identifiers, strings, comments, errors
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "interpreter/lexer.h"
#include "interpreter/token.h"
#include "interpreter/intern.h"
#include <string.h>

static void test_lexer_number_literals(void) {
    TEST("TC-P3.1: Number literals");

    Lexer lex;
    lexer_init(&lex, "42");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_NUMBER, t.type);
    TEST_ASSERT_EQUAL_DOUBLE(42.0, t.num, 0.001);

    lexer_init(&lex, "3.14");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_NUMBER, t.type);
    TEST_ASSERT_EQUAL_DOUBLE(3.14, t.num, 0.001);

    lexer_init(&lex, "0");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_NUMBER, t.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, t.num, 0.001);

    TEST_PASS();
}

static void test_lexer_keywords(void) {
    TEST("TC-P3.2: All keywords");

    const char* keywords[] = {"var","if","else","while","true","false","func","return"};
    TokenType expected[] = {TOKEN_VAR,TOKEN_IF,TOKEN_ELSE,TOKEN_WHILE,TOKEN_TRUE,TOKEN_FALSE,TOKEN_FUNC,TOKEN_RETURN};

    for (int i = 0; i < 8; i++) {
        Lexer lex;
        lexer_init(&lex, keywords[i]);
        Token t = lexer_next(&lex);
        TEST_ASSERT_EQUAL_INT(expected[i], t.type);
    }
    TEST_PASS();
}

static void test_lexer_identifiers(void) {
    TEST("TC-P3.3: Identifiers");

    Lexer lex;
    lexer_init(&lex, "foobar");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t.type);
    TEST_ASSERT_NOT_NULL(t.str);
    TEST_ASSERT_STR_EQUAL("foobar", t.str);

    lexer_init(&lex, "_temp");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t.type);
    TEST_ASSERT_STR_EQUAL("_temp", t.str);

    lexer_init(&lex, "var1");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t.type);
    TEST_ASSERT_STR_EQUAL("var1", t.str);

    TEST_PASS();
}

static void test_lexer_operators(void) {
    TEST("TC-P3.4: Double-character operators");

    Lexer lex;
    lexer_init(&lex, "==");
    TEST_ASSERT_EQUAL_INT(TOKEN_EQ, lexer_next(&lex).type);

    lexer_init(&lex, "!=");
    TEST_ASSERT_EQUAL_INT(TOKEN_NEQ, lexer_next(&lex).type);

    lexer_init(&lex, "<=");
    TEST_ASSERT_EQUAL_INT(TOKEN_LE, lexer_next(&lex).type);

    lexer_init(&lex, ">=");
    TEST_ASSERT_EQUAL_INT(TOKEN_GE, lexer_next(&lex).type);

    lexer_init(&lex, "&&");
    TEST_ASSERT_EQUAL_INT(TOKEN_AND, lexer_next(&lex).type);

    lexer_init(&lex, "||");
    TEST_ASSERT_EQUAL_INT(TOKEN_OR, lexer_next(&lex).type);

    // Single operators
    lexer_init(&lex, "+");
    TEST_ASSERT_EQUAL_INT(TOKEN_PLUS, lexer_next(&lex).type);
    lexer_init(&lex, "-");
    TEST_ASSERT_EQUAL_INT(TOKEN_MINUS, lexer_next(&lex).type);
    lexer_init(&lex, "*");
    TEST_ASSERT_EQUAL_INT(TOKEN_STAR, lexer_next(&lex).type);
    lexer_init(&lex, "/");
    TEST_ASSERT_EQUAL_INT(TOKEN_SLASH, lexer_next(&lex).type);

    TEST_PASS();
}

static void test_lexer_string_literals(void) {
    TEST("TC-P3.5: String literals");

    Lexer lex;
    lexer_init(&lex, "\"hello world\"");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_STRING, t.type);
    TEST_ASSERT_STR_EQUAL("hello world", t.str);

    // Empty string
    lexer_init(&lex, "\"\"");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_STRING, t.type);

    // Escape sequences
    lexer_init(&lex, "\"a\\nb\\tc\\\"d\"");
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_STRING, t.type);
    TEST_ASSERT_STR_EQUAL("a\nb\tc\"d", t.str);

    TEST_PASS();
}

static void test_lexer_comments(void) {
    TEST("TC-P3.6: Comments");

    Lexer lex;
    lexer_init(&lex, "var x = 1; // this is a comment\nprint(x);");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_VAR, t.type);
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t.type);
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_ASSIGN, t.type);
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_NUMBER, t.type);
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_SEMICOLON, t.type);
    // After comment, next token is "print" (TOKEN_IDENTIFIER)
    // But "print" might be a keyword... check if it's reserved
    // Actually "print" is NOT a keyword in V1.0, it's a builtin identifier
    t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t.type);

    TEST_PASS();
}

static void test_lexer_illegal_char(void) {
    TEST("TC-P3.7: Illegal character");

    Lexer lex;
    lexer_init(&lex, "var x = @;");
    Token t = lexer_next(&lex); // var
    (void)t;
    t = lexer_next(&lex); // x
    (void)t;
    t = lexer_next(&lex); // =
    (void)t;
    t = lexer_next(&lex); // @ - ERROR
    TEST_ASSERT_EQUAL_INT(TOKEN_ERROR, t.type);
    TEST_ASSERT_NOT_NULL(t.str);

    TEST_PASS();
}

static void test_lexer_eof(void) {
    TEST("TC-P3.8: EOF / empty source");

    Lexer lex;
    lexer_init(&lex, "");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_END, t.type);

    TEST_PASS();
}

static void test_lexer_intern_consistency(void) {
    TEST("TC-P3.9: String interning consistency");

    Lexer lex;
    lexer_init(&lex, "counter counter");
    Token t1 = lexer_next(&lex);
    Token t2 = lexer_next(&lex);

    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t1.type);
    TEST_ASSERT_EQUAL_INT(TOKEN_IDENTIFIER, t2.type);
    // Both should point to the same interned string
    TEST_ASSERT(t1.str == t2.str);

    TEST_PASS();
}

static void test_lexer_leading_dot_number(void) {
    TEST("Bonus: Leading dot number");

    Lexer lex;
    lexer_init(&lex, ".5");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_NUMBER, t.type);
    TEST_ASSERT_EQUAL_DOUBLE(0.5, t.num, 0.001);

    TEST_PASS();
}

static void test_lexer_unterminated_string(void) {
    TEST("Bonus: Unterminated string");

    Lexer lex;
    lexer_init(&lex, "\"unterminated");
    Token t = lexer_next(&lex);
    TEST_ASSERT_EQUAL_INT(TOKEN_ERROR, t.type);

    TEST_PASS();
}

void test_lexer(void) {
    printf("\n[Lexer Tests]\n");
    test_lexer_number_literals();
    test_lexer_keywords();
    test_lexer_identifiers();
    test_lexer_operators();
    test_lexer_string_literals();
    test_lexer_comments();
    test_lexer_illegal_char();
    test_lexer_eof();
    test_lexer_intern_consistency();
    test_lexer_leading_dot_number();
    test_lexer_unterminated_string();
}
