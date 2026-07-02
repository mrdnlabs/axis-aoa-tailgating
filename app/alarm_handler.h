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
 * Wait (up to timeout_ms) for all in-flight alarm workers to finish.
 * Call this in the shutdown sequence BEFORE tearing down the modules the
 * workers depend on (config, token_manager, curl globals).
 */
void alarm_handler_drain(int timeout_ms);

/**
 * Clean up the alarm handler.  Drains workers with a 5-second default,
 * then curl_global_cleanup.  Callers that need a longer bound should
 * call alarm_handler_drain() explicitly first.
 */
void alarm_handler_cleanup(void);
