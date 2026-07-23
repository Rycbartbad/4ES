#pragma once

/**
 * Normalize an LLM response or manual script payload into raw ESP-LEGO DSL.
 *
 * Accepted forms:
 *   - raw DSL source
 *   - a Markdown fenced block
 *   - a JSON object whose `script` field is a string
 *   - a fenced JSON object
 *
 * The function performs no allocation and always null-terminates `out` when
 * `out_len` is positive.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCRIPT_NORMALIZE_OK = 0,
    SCRIPT_NORMALIZE_EMPTY = -1,
    SCRIPT_NORMALIZE_INVALID_JSON = -2,
    SCRIPT_NORMALIZE_MISSING_SCRIPT = -3,
    SCRIPT_NORMALIZE_TOO_LONG = -4,
    SCRIPT_NORMALIZE_UNSAFE_LOOP = -5,
    SCRIPT_NORMALIZE_INVALID_ARGUMENT = -6,
} ScriptNormalizeResult;

int script_normalize_response(const char* input, char* out, int out_len);
const char* script_normalize_error(int result);

#ifdef __cplusplus
}
#endif
