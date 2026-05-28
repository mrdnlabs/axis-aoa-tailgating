#pragma once

#include <stdbool.h>

/**
 * Initialize and start the embedded CivetWeb HTTP server on 127.0.0.1:port.
 * Returns true on success.
 */
bool web_server_init(int port);

/**
 * Stop and clean up the web server.
 */
void web_server_cleanup(void);
