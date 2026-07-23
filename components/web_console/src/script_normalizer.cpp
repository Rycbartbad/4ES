#include "web_console/script_normalizer.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

struct TextRange {
    const char* begin;
    const char* end;
};

static TextRange trim_range(TextRange range)
{
    while (range.begin < range.end &&
           isspace(static_cast<unsigned char>(*range.begin))) {
        range.begin++;
    }
    while (range.end > range.begin &&
           isspace(static_cast<unsigned char>(range.end[-1]))) {
        range.end--;
    }
    return range;
}

static int copy_range(TextRange range, char* out, int out_len)
{
    range = trim_range(range);
    const ptrdiff_t length = range.end - range.begin;
    if (length <= 0) {
        out[0] = '\0';
        return SCRIPT_NORMALIZE_EMPTY;
    }
    if (length >= out_len) {
        out[0] = '\0';
        return SCRIPT_NORMALIZE_TOO_LONG;
    }
    memmove(out, range.begin, static_cast<size_t>(length));
    out[length] = '\0';
    return SCRIPT_NORMALIZE_OK;
}

static const char* find_bytes(const char* begin, const char* end,
                              const char* needle)
{
    const size_t needle_len = strlen(needle);
    if (needle_len == 0) return begin;
    for (const char* p = begin; p + needle_len <= end; p++) {
        if (memcmp(p, needle, needle_len) == 0) return p;
    }
    return NULL;
}

static bool fenced_payload(TextRange input, TextRange* payload)
{
    const char* opening = find_bytes(input.begin, input.end, "```");
    if (opening == NULL) return false;

    const char* content = opening + 3;
    while (content < input.end && *content != '\r' && *content != '\n') {
        content++;
    }
    if (content < input.end && *content == '\r') content++;
    if (content < input.end && *content == '\n') content++;

    const char* closing = find_bytes(content, input.end, "```");
    if (closing == NULL) return false;
    *payload = trim_range({content, closing});
    return true;
}

static void skip_ws(const char*& p, const char* end)
{
    while (p < end && isspace(static_cast<unsigned char>(*p))) p++;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool append_byte(char value, char* out, int out_len, int& written)
{
    if (out == NULL) return true;
    if (written >= out_len - 1) return false;
    out[written++] = value;
    return true;
}

static bool append_utf8(uint32_t codepoint, char* out, int out_len,
                        int& written)
{
    if (codepoint <= 0x7f) {
        return append_byte(static_cast<char>(codepoint), out, out_len, written);
    }
    if (codepoint <= 0x7ff) {
        return append_byte(static_cast<char>(0xc0 | (codepoint >> 6)), out,
                           out_len, written) &&
               append_byte(static_cast<char>(0x80 | (codepoint & 0x3f)), out,
                           out_len, written);
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff) codepoint = 0xfffd;
    return append_byte(static_cast<char>(0xe0 | (codepoint >> 12)), out,
                       out_len, written) &&
           append_byte(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)), out,
                       out_len, written) &&
           append_byte(static_cast<char>(0x80 | (codepoint & 0x3f)), out,
                       out_len, written);
}

static int parse_json_string(const char*& p, const char* end, char* out,
                             int out_len)
{
    if (p >= end || *p != '"') return SCRIPT_NORMALIZE_INVALID_JSON;
    p++;
    int written = 0;
    while (p < end) {
        char c = *p++;
        if (c == '"') {
            if (out != NULL) out[written] = '\0';
            return written;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return SCRIPT_NORMALIZE_INVALID_JSON;
        }
        if (c != '\\') {
            if (!append_byte(c, out, out_len, written)) {
                return SCRIPT_NORMALIZE_TOO_LONG;
            }
            continue;
        }
        if (p >= end) return SCRIPT_NORMALIZE_INVALID_JSON;
        const char escaped = *p++;
        char decoded = '\0';
        switch (escaped) {
        case '"': decoded = '"'; break;
        case '\\': decoded = '\\'; break;
        case '/': decoded = '/'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case 'n': decoded = '\n'; break;
        case 'r': decoded = '\r'; break;
        case 't': decoded = '\t'; break;
        case 'u': {
            if (end - p < 4) return SCRIPT_NORMALIZE_INVALID_JSON;
            uint32_t codepoint = 0;
            for (int i = 0; i < 4; i++) {
                const int value = hex_value(*p++);
                if (value < 0) return SCRIPT_NORMALIZE_INVALID_JSON;
                codepoint = (codepoint << 4) | static_cast<uint32_t>(value);
            }
            if (!append_utf8(codepoint, out, out_len, written)) {
                return SCRIPT_NORMALIZE_TOO_LONG;
            }
            continue;
        }
        default:
            return SCRIPT_NORMALIZE_INVALID_JSON;
        }
        if (!append_byte(decoded, out, out_len, written)) {
            return SCRIPT_NORMALIZE_TOO_LONG;
        }
    }
    return SCRIPT_NORMALIZE_INVALID_JSON;
}

static bool skip_json_value(const char*& p, const char* end)
{
    skip_ws(p, end);
    if (p >= end) return false;
    if (*p == '"') return parse_json_string(p, end, NULL, 0) >= 0;

    if (*p == '{' || *p == '[') {
        const char opening = *p++;
        const char closing = opening == '{' ? '}' : ']';
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"') {
                if (parse_json_string(p, end, NULL, 0) < 0) return false;
            } else {
                if (*p == opening) depth++;
                if (*p == closing) depth--;
                p++;
            }
        }
        return depth == 0;
    }

    const char* start = p;
    while (p < end && *p != ',' && *p != '}') p++;
    return trim_range({start, p}).begin < trim_range({start, p}).end;
}

static int extract_json_script(TextRange json, char* out, int out_len)
{
    const char* p = json.begin;
    bool found_script = false;
    skip_ws(p, json.end);
    if (p >= json.end || *p++ != '{') return SCRIPT_NORMALIZE_INVALID_JSON;

    while (p < json.end) {
        skip_ws(p, json.end);
        if (p < json.end && *p == '}') {
            p++;
            skip_ws(p, json.end);
            if (p != json.end) return SCRIPT_NORMALIZE_INVALID_JSON;
            return found_script ? SCRIPT_NORMALIZE_OK
                                : SCRIPT_NORMALIZE_MISSING_SCRIPT;
        }

        char key[32] = {};
        const int key_len = parse_json_string(p, json.end, key, sizeof(key));
        if (key_len < 0) return key_len;
        skip_ws(p, json.end);
        if (p >= json.end || *p++ != ':') return SCRIPT_NORMALIZE_INVALID_JSON;
        skip_ws(p, json.end);

        if (strcmp(key, "script") == 0) {
            const int result = parse_json_string(p, json.end, out, out_len);
            if (result < 0) return result;
            if (result == 0) return SCRIPT_NORMALIZE_EMPTY;
            found_script = true;
        } else if (!skip_json_value(p, json.end)) {
            return SCRIPT_NORMALIZE_INVALID_JSON;
        }
        skip_ws(p, json.end);
        if (p < json.end && *p == ',') {
            p++;
            continue;
        }
        if (p < json.end && *p == '}') continue;
        return SCRIPT_NORMALIZE_INVALID_JSON;
    }
    return SCRIPT_NORMALIZE_INVALID_JSON;
}

static void trim_in_place(char* text)
{
    TextRange range = trim_range({text, text + strlen(text)});
    const size_t length = static_cast<size_t>(range.end - range.begin);
    memmove(text, range.begin, length);
    text[length] = '\0';
}

static bool contains_ignoring_space(const char* text, const char* pattern)
{
    for (const char* start = text; *start; start++) {
        const char* p = start;
        const char* q = pattern;
        while (*p && *q) {
            if (isspace(static_cast<unsigned char>(*p))) {
                p++;
                continue;
            }
            if (tolower(static_cast<unsigned char>(*p)) !=
                tolower(static_cast<unsigned char>(*q))) {
                break;
            }
            p++;
            q++;
        }
        if (*q == '\0') return true;
    }
    return false;
}

static int finalize_script(char* out, int out_len)
{
    trim_in_place(out);
    if (out[0] == '\0') return SCRIPT_NORMALIZE_EMPTY;

    TextRange range = {out, out + strlen(out)};
    TextRange fenced = {};
    if (fenced_payload(range, &fenced)) {
        const int result = copy_range(fenced, out, out_len);
        if (result != SCRIPT_NORMALIZE_OK) return result;
        trim_in_place(out);
    }

    if (out[0] == '{' || out[0] == '[' || strstr(out, "```") != NULL) {
        return SCRIPT_NORMALIZE_INVALID_JSON;
    }
    if (contains_ignoring_space(out, "while(true)") ||
        contains_ignoring_space(out, "while(1)")) {
        return SCRIPT_NORMALIZE_UNSAFE_LOOP;
    }
    return SCRIPT_NORMALIZE_OK;
}

}  // namespace

int script_normalize_response(const char* input, char* out, int out_len)
{
    if (out == NULL || out_len <= 0) return SCRIPT_NORMALIZE_INVALID_ARGUMENT;
    out[0] = '\0';
    if (input == NULL) return SCRIPT_NORMALIZE_INVALID_ARGUMENT;

    TextRange candidate = trim_range({input, input + strlen(input)});
    if (candidate.begin == candidate.end) return SCRIPT_NORMALIZE_EMPTY;

    TextRange fenced = {};
    if (fenced_payload(candidate, &fenced)) candidate = fenced;
    candidate = trim_range(candidate);

    int result = SCRIPT_NORMALIZE_OK;
    if (candidate.begin < candidate.end && *candidate.begin == '{') {
        result = extract_json_script(candidate, out, out_len);
    } else {
        result = copy_range(candidate, out, out_len);
    }
    if (result != SCRIPT_NORMALIZE_OK) return result;
    return finalize_script(out, out_len);
}

const char* script_normalize_error(int result)
{
    switch (result) {
    case SCRIPT_NORMALIZE_OK: return "ok";
    case SCRIPT_NORMALIZE_EMPTY: return "AI returned an empty script";
    case SCRIPT_NORMALIZE_INVALID_JSON: return "AI returned malformed script JSON";
    case SCRIPT_NORMALIZE_MISSING_SCRIPT: return "AI JSON response has no script field";
    case SCRIPT_NORMALIZE_TOO_LONG: return "Generated script is too long";
    case SCRIPT_NORMALIZE_UNSAFE_LOOP:
        return "Infinite loops are not allowed; use a bounded counter loop";
    default: return "Invalid script response";
    }
}
