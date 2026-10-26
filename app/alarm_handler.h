#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Initialize the alarm handler (curl_global_init). */
void alarm_handler_init(void);

/**
 * Fire the configured alarm action.
 * is_test=false: publishes the stateful TailgatingAlarm event and, if
 *   configured, dispatches the outbound HTTP action on a background thread.
 * is_test=true: never used here for the UI test (see alarm_handler_test);
 *   kept for internal callers that want a non-test dispatch path.
 */
void alarm_handler_notify(bool is_test);

/**
 * Run the configured action once, synchronously, for the settings "Test"
 * button.  Does NOT publish the TailgatingAlarm event.  Writes a short
 * outcome string (e.g. "HTTP 200" or a validation reason) into status.
 * Returns true on a 2xx response.
 */
bool alarm_handler_test(char *status, size_t status_len);

/** Signal in-flight workers to abort (used early in shutdown). */
void alarm_handler_begin_shutdown(void);

/** Wait up to timeout_ms for in-flight alarm workers to finish. */
void alarm_handler_drain(int timeout_ms);

/** Clean up the alarm handler (drains workers, then curl_global_cleanup). */
void alarm_handler_cleanup(void);
