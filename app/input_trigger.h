#pragma once

#include <stdbool.h>

/**
 * Initialize digital I/O input trigger subscription.
 * port: "1", "2", etc. or "none"/"disabled" to skip.
 * Returns true on success (including when disabled).
 */
bool input_trigger_init(const char *port);

/**
 * Switch to a different I/O port without restarting.
 * Pass "none" to disable.
 */
bool input_trigger_resubscribe(const char *port);

/**
 * Clean up the I/O input subscription.
 */
void input_trigger_cleanup(void);
