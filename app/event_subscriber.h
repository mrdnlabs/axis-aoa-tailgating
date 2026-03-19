#pragma once

#include <stdbool.h>

/**
 * Initialize the AXEvent subscription for AOA events.
 * scenario_id: AOA scenario identifier string (e.g. "1", "3", or a GUID-style ID).
 * Returns true if subscription was set up, false if AOA unavailable.
 */
bool event_subscriber_init(const char *scenario_id);

/**
 * Switch to a different AOA scenario without restarting the app.
 * Tears down the existing subscription and creates a new one.
 * Returns true on success.
 */
bool event_subscriber_resubscribe(const char *scenario_id);

/**
 * Clean up the AXEvent subscription.
 */
void event_subscriber_cleanup(void);
