#pragma once

#include "sdkconfig.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Intern table size from Kconfig
#ifndef CONFIG_INTERN_TABLE_SIZE
#define CONFIG_INTERN_TABLE_SIZE 128
#endif

#define INTERN_ENTRY_LEN 64

// Intern a string (start, len) → pointer to persistent pool.
// Returns nullptr if table is full (caller must handle error).
const char* intern_string(const char* start, int len);

// Reset intern table (called between scripts)
void intern_reset(void);

// Current count of interned strings
int intern_count(void);

#ifdef __cplusplus
}
#endif
