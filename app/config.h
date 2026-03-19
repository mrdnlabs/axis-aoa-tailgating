#pragma once

#include <stdbool.h>

/**
 * Initialize the configuration subsystem.
 * Must be called before any get/set operations.
 * Returns true on success.
 */
bool config_init(const char *app_name);

/**
 * Retrieve a string parameter value. Caller must free() the result.
 * Returns a copy of default_val if the parameter is not found.
 */
char *config_get_string(const char *name, const char *default_val);

/**
 * Retrieve an integer parameter value.
 * Returns default_val if the parameter is not found or not parseable.
 */
int config_get_int(const char *name, int default_val);

/**
 * Set a parameter value (as string).
 * Returns true on success.
 */
bool config_set(const char *name, const char *value);


/**
 * Clean up the configuration subsystem.
 */
void config_cleanup(void);
