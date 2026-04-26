#include "interpreter/intern.h"
#include <string.h>

// Fixed intern table — no dynamic allocation
static struct {
    char  str[INTERN_ENTRY_LEN];
    bool  used;
} s_intern_table[CONFIG_INTERN_TABLE_SIZE];

static int s_intern_count = 0;

const char* intern_string(const char* start, int len)
{
    if (!start || len <= 0) return NULL;

    // Truncate if too long
    if (len >= INTERN_ENTRY_LEN) len = INTERN_ENTRY_LEN - 1;

    // Check if already interned
    for (int i = 0; i < s_intern_count; i++) {
        if (s_intern_table[i].used &&
            (int)strlen(s_intern_table[i].str) == len &&
            memcmp(s_intern_table[i].str, start, len) == 0) {
            return s_intern_table[i].str;
        }
    }

    // Allocate new entry
    if (s_intern_count >= CONFIG_INTERN_TABLE_SIZE) return NULL;

    memcpy(s_intern_table[s_intern_count].str, start, len);
    s_intern_table[s_intern_count].str[len] = '\0';
    s_intern_table[s_intern_count].used = true;
    return s_intern_table[s_intern_count++].str;
}

void intern_reset(void)
{
    for (int i = 0; i < s_intern_count; i++) {
        s_intern_table[i].used = false;
    }
    s_intern_count = 0;
}

int intern_count(void)
{
    return s_intern_count;
}
