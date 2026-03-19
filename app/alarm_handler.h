#pragma once

#include <stdbool.h>

/**
 * Initialize the alarm handler (curl_global_init).
 */
void alarm_handler_init(void);

/**
 * Fire the configured alarm action in a background thread.
 * is_test: if true, bypasses cooldown and marks as test in logs.
 */
void alarm_handler_notify(bool is_test);

/**
 * Clean up the alarm handler (curl_global_cleanup).
 */
void alarm_handler_cleanup(void);
