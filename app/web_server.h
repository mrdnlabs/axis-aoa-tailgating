#pragma once

#include <stdbool.h>

/**
 * Initialize and start the embedded CivetWeb HTTP server on localhost:port.
 * Returns true on success.
 */
bool web_server_init(int port);

/**
 * Stop and clean up the web server.
 */
void web_server_cleanup(void);
