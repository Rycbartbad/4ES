/*
 * ESP-LEGO Parser Unit Tests
 *
 * Tests AST construction from source code via lexer → parser pipeline.
 */
#include "sdkconfig.h"
#include "test_runner.h"
#include "interpreter/lexer.h"
#include "interpreter/parser.h"
#include "interpreter/ast.h"
#include <string.h>

static ASTNode* parse_source(Parser* p, Lexer* l, const char* src) {
    lexer_init(l, src);
    parser_init(p, l);
    return parser_parse(p);
}

static void test_empty(void) {
    TEST("Parser: Empty program"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "");
    TEST_ASSERT_EQUAL_INT(NODE_PROGRAM, ast->type);
    TEST_ASSERT_EQUAL_INT(0, ast->stmt_count); TEST_PASS();
}

static void test_var_decl(void) {
    TEST("Parser: Variable declaration"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "var x = 42;");
    ASTNode* s = ast->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_VAR_DECL, s->type);
    TEST_ASSERT_STR_EQUAL("x", s->func_name);
    TEST_ASSERT_EQUAL_INT(NODE_LITERAL_NUM, s->func_body->type);
    TEST_ASSERT_EQUAL_DOUBLE(42, s->func_body->num_val, 0.001); TEST_PASS();
}

static void test_if_else(void) {
    TEST("Parser: If-else"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "if(1){print(1);}else{print(2);}");
    ASTNode* s = ast->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_IF, s->type);
    TEST_ASSERT_NOT_NULL(s->condition);
    TEST_ASSERT_NOT_NULL(s->if_body);
    TEST_ASSERT_NOT_NULL(s->else_body); TEST_PASS();
}

static void test_while(void) {
    TEST("Parser: While loop"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "while(x<10){x=x+1;}");
    TEST_ASSERT_EQUAL_INT(NODE_WHILE, ast->stmts[0]->type); TEST_PASS();
}

static void test_func_def(void) {
    TEST("Parser: Function definition"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "func add(a,b){return a+b;}");
    ASTNode* f = ast->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_FUNC_DEF, f->type);
    TEST_ASSERT_STR_EQUAL("add", f->func_name);
    TEST_ASSERT_EQUAL_INT(2, f->param_count); TEST_PASS();
}

static void test_func_call(void) {
    TEST("Parser: Function call"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "print(1+2);");
    ASTNode* c = ast->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_FUNC_CALL, c->type);
    TEST_ASSERT_STR_EQUAL("print", c->name);
    TEST_ASSERT_EQUAL_INT(1, c->arg_count); TEST_PASS();
}

static void test_return(void) {
    TEST("Parser: Return statement"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "func f(){return 42;}");
    ASTNode* r = ast->stmts[0]->func_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_RETURN_STMT, r->type); TEST_PASS();
}

static void test_precedence(void) {
    TEST("Parser: Precedence 1+2*3"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "1+2*3;");
    ASTNode* e = ast->stmts[0];
    TEST_ASSERT_EQUAL_INT(NODE_BINARY_OP, e->type);
    TEST_ASSERT_EQUAL_INT(TOKEN_PLUS, e->op);
    TEST_ASSERT_EQUAL_INT(NODE_BINARY_OP, e->right->type);
    TEST_ASSERT_EQUAL_INT(TOKEN_STAR, e->right->op); TEST_PASS();
}

static void test_bool_literals(void) {
    TEST("Parser: Bool literals"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "var a=true; var b=false;");
    TEST_ASSERT(ast->stmts[0]->func_body->bool_val);
    TEST_ASSERT(!ast->stmts[1]->func_body->bool_val); TEST_PASS();
}

static void test_string(void) {
    TEST("Parser: String literal"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "print(\"hi\");");
    TEST_ASSERT_STR_EQUAL("hi", ast->stmts[0]->args[0]->str_val); TEST_PASS();
}

static void test_unary(void) {
    TEST("Parser: Unary NOT"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "var a=!true;");
    TEST_ASSERT_EQUAL_INT(NODE_UNARY_OP, ast->stmts[0]->func_body->type);
    TEST_ASSERT_EQUAL_INT(TOKEN_NOT, ast->stmts[0]->func_body->op); TEST_PASS();
}

static void test_block(void) {
    TEST("Parser: Empty block"); Lexer l; Parser p;
    ASTNode* ast = parse_source(&p, &l, "{}");
    TEST_ASSERT_EQUAL_INT(NODE_BLOCK, ast->stmts[0]->type);
    TEST_ASSERT_EQUAL_INT(0, ast->stmts[0]->stmt_count); TEST_PASS();
}

static void test_error_syntax(void) {
    TEST("Parser: Error recovery"); Lexer l; Parser p;
    parse_source(&p, &l, "var x = 1  var y = 2");
    TEST_ASSERT(p.had_error); TEST_PASS();
}

static void test_generated_monitoring_script(void) {
    TEST("Parser: generated nested monitoring script fits pointer pool");
    Lexer l; Parser p;
    const char* source =
        "var count = 0;\n"
        "while (count < 5) {\n"
        "  var humidity = read_sensor(\"humidity\");\n"
        "  print(\"Humidity: \" + humidity);\n"
        "  if (humidity > 20) {\n"
        "    print(\"Humidity > 20, turning on water pump\");\n"
        "    send_motor(\"pump\", 100);\n"
        "  } else {\n"
        "    send_motor(\"pump\", 0);\n"
        "  }\n"
        "  sleep(5000);\n"
        "  count = count + 1;\n"
        "}\n"
        "send_motor(\"pump\", 0);\n"
        "print(\"Monitoring complete\");";
    ASTNode* ast = parse_source(&p, &l, source);
    TEST_ASSERT(!p.had_error);
    TEST_ASSERT_NOT_NULL(ast);
    TEST_ASSERT_EQUAL_INT(4, ast->stmt_count);
    TEST_PASS();
}

void test_parser(void) {
    printf("\n[Parser Tests]\n");
    test_empty(); test_var_decl(); test_if_else(); test_while();
    test_func_def(); test_func_call(); test_return(); test_precedence();
    test_bool_literals(); test_string(); test_unary(); test_block();
    test_error_syntax(); test_generated_monitoring_script();
}
