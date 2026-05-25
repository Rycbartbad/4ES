#include "sdkconfig.h"
#include "interpreter/lexer.h"
#include "interpreter/intern.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

// ---------- private helpers ----------

static char peek(const Lexer* lex)
{
    return lex->source[lex->pos];
}

static char advance(Lexer* lex)
{
    char c = lex->source[lex->pos];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    lex->pos++;
    return c;
}

// Skip whitespace and //-style line comments.
static void skip_whitespace(Lexer* lex)
{
    for (;;) {
        char c = peek(lex);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                advance(lex);
                break;
            case '/':
                if (lex->source[lex->pos + 1] == '/') {
                    // line comment until end of line or end of source
                    while (peek(lex) != '\0' && peek(lex) != '\n') {
                        advance(lex);
                    }
                    if (peek(lex) == '\n') {
                        advance(lex);
                    }
                } else {
                    return; // division operator, not a comment
                }
                break;
            default:
                return;
        }
    }
}

// Keyword table
typedef struct {
    const char* word;
    TokenType   type;
} KeywordEntry;

static const KeywordEntry s_keywords[] = {
    {"var",    TOKEN_VAR},
    {"if",     TOKEN_IF},
    {"else",   TOKEN_ELSE},
    {"while",  TOKEN_WHILE},
    {"true",   TOKEN_TRUE},
    {"false",  TOKEN_FALSE},
    {"func",   TOKEN_FUNC},
    {"return", TOKEN_RETURN},
};

static const int s_num_keywords = (int)(sizeof(s_keywords) / sizeof(s_keywords[0]));

// Returns TOKEN_IDENTIFIER if not a keyword.
static TokenType keyword_type(const char* start, int len)
{
    for (int i = 0; i < s_num_keywords; i++) {
        if ((int)strlen(s_keywords[i].word) == len &&
            memcmp(start, s_keywords[i].word, len) == 0) {
            return s_keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

// ---------- public API ----------

void lexer_init(Lexer* lex, const char* source)
{
    lex->source = source ? source : "";
    lex->pos    = 0;
    lex->line   = 1;
    lex->col    = 1;
}

Token lexer_next(Lexer* lex)
{
    Token t;
    t.line = lex->line;
    t.col  = lex->col;
    t.num  = 0.0;
    t.str  = NULL;

    skip_whitespace(lex);

    char c = peek(lex);

    // End of input
    if (c == '\0') {
        t.type = TOKEN_END;
        return t;
    }

    // ---------- Number ----------
    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)lex->source[lex->pos + 1])))
    {
        int start_pos = lex->pos;

        if (c == '.') {
            advance(lex); // consume leading dot
        }

        while (isdigit((unsigned char)peek(lex))) {
            advance(lex);
        }

        if (peek(lex) == '.') {
            advance(lex); // consume decimal dot
            while (isdigit((unsigned char)peek(lex))) {
                advance(lex);
            }
        }

        int len = lex->pos - start_pos;
        char buf[64];                   // larger than any practical script number
        if (len >= 64) len = 63;
        memcpy(buf, lex->source + start_pos, len);
        buf[len] = '\0';

        t.type = TOKEN_NUMBER;
        t.num  = strtod(buf, NULL);
        return t;
    }

    // ---------- String ----------
    if (c == '"') {
        advance(lex); // consume opening "

        char buf[INTERN_ENTRY_LEN];     // 64 bytes, matches intern table slot
        int  buf_len = 0;
        bool truncated = false;

        for (;;) {
            char ch = peek(lex);
            if (ch == '\0') {
                t.type = TOKEN_ERROR;
                t.str  = intern_string("Unterminated string", 19);
                return t;
            }
            if (ch == '"') {
                advance(lex); // consume closing "
                break;
            }

            if (ch == '\\') {
                advance(lex); // consume backslash
                char esc = peek(lex);
                if (esc == '\0') {
                    t.type = TOKEN_ERROR;
                    t.str  = intern_string("Unterminated string escape", 26);
                    return t;
                }
                char replace;
                switch (esc) {
                    case '"':  replace = '"';  break;
                    case '\\': replace = '\\'; break;
                    case 'n':  replace = '\n'; break;
                    case 't':  replace = '\t'; break;
                    case 'r':  replace = '\r'; break;
                    default:
                        t.type = TOKEN_ERROR;
                        t.str  = intern_string("Invalid string escape", 21);
                        return t;
                }
                if (buf_len < INTERN_ENTRY_LEN - 1) {
                    buf[buf_len++] = replace;
                } else {
                    truncated = true;
                }
                advance(lex); // consume escaped character
            } else {
                if (buf_len < INTERN_ENTRY_LEN - 1) {
                    buf[buf_len++] = ch;
                } else {
                    truncated = true;
                }
                advance(lex);
            }
        }

        if (truncated) {
            ESP_LOGW("lexer", "String literal truncated at %d chars", INTERN_ENTRY_LEN - 1);
        }

        buf[buf_len] = '\0';
        t.type = TOKEN_STRING;
        t.str  = intern_string(buf, buf_len);
        return t;
    }

    // ---------- Identifier / keyword ----------
    if (isalpha((unsigned char)c) || c == '_') {
        int start_pos = lex->pos;
        advance(lex);
        while (isalnum((unsigned char)peek(lex)) || peek(lex) == '_') {
            advance(lex);
        }
        int len = lex->pos - start_pos;
        const char* start_ptr = lex->source + start_pos;

        TokenType kw = keyword_type(start_ptr, len);
        t.type = kw;
        if (kw == TOKEN_IDENTIFIER) {
            t.str = intern_string(start_ptr, len);
        }
        return t;
    }

    // ---------- Operators & delimiters ----------
    switch (c) {
        // Two-character operators (greedy longest match)
        case '=':
            advance(lex);
            if (peek(lex) == '=') { advance(lex); t.type = TOKEN_EQ; }
            else                   { t.type = TOKEN_ASSIGN; }
            return t;
        case '!':
            advance(lex);
            if (peek(lex) == '=') { advance(lex); t.type = TOKEN_NEQ; }
            else                   { t.type = TOKEN_NOT; }
            return t;
        case '<':
            advance(lex);
            if (peek(lex) == '=') { advance(lex); t.type = TOKEN_LE; }
            else                   { t.type = TOKEN_LT; }
            return t;
        case '>':
            advance(lex);
            if (peek(lex) == '=') { advance(lex); t.type = TOKEN_GE; }
            else                   { t.type = TOKEN_GT; }
            return t;
        case '&':
            advance(lex);
            if (peek(lex) == '&') { advance(lex); t.type = TOKEN_AND; }
            else                   { t.type = TOKEN_ERROR; t.str = intern_string("Unexpected '&'", 14); }
            return t;
        case '|':
            advance(lex);
            if (peek(lex) == '|') { advance(lex); t.type = TOKEN_OR; }
            else                   { t.type = TOKEN_ERROR; t.str = intern_string("Unexpected '|'", 14); }
            return t;
        // Single-character operators
        case '+': advance(lex); t.type = TOKEN_PLUS;   return t;
        case '-': advance(lex); t.type = TOKEN_MINUS;  return t;
        case '*': advance(lex); t.type = TOKEN_STAR;   return t;
        case '/': advance(lex); t.type = TOKEN_SLASH;  return t;
        // Delimiters
        case '(': advance(lex); t.type = TOKEN_LPAREN;  return t;
        case ')': advance(lex); t.type = TOKEN_RPAREN;  return t;
        case '{': advance(lex); t.type = TOKEN_LBRACE;  return t;
        case '}': advance(lex); t.type = TOKEN_RBRACE;  return t;
        case ';': advance(lex); t.type = TOKEN_SEMICOLON; return t;
        case ',': advance(lex); t.type = TOKEN_COMMA;   return t;
        // ----- Unexpected character -----
        default:
            advance(lex); // consume so the next call can make progress
            t.type = TOKEN_ERROR;
            {
                char err[64];
                int n = snprintf(err, sizeof(err), "Unexpected character: '%c'", c);
                if (n < 0) n = 0;
                if (n >= (int)sizeof(err)) n = (int)sizeof(err) - 1;
                t.str = intern_string(err, n);
            }
            return t;
    }
}
