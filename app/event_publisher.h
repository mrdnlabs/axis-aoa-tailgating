#pragma once

#include <stdbool.h>

/**
 * Declare the tailgating alarm event with the Axis event system.
 * Must be called from the GLib main loop thread before any send.
 * Returns true on success.
 */
bool event_publisher_init(void);

/**
 * Send a tailgating alarm event.
 * active=true  → alarm raised
 * active=false → alarm cleared
 * Thread-safe: schedules the send on the GLib main loop if needed.
 */
void event_publisher_send_alarm(bool active);

/**
 * Clean up the event publisher.
 */
void event_publisher_cleanup(void);
