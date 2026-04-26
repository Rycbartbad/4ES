#pragma once
// Convenience header: includes all path-based FreeRTOS mocks.
// Component sources include via `#include "freertos/FreeRTOS.h"` etc.,
// resolved by the tests/mocks/freertos/ subdirectory.
// Test files can include this single header for simplicity.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
