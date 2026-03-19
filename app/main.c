#include "config.h"
#include "token_manager.h"
#include "event_subscriber.h"
#include "event_publisher.h"
#include "input_trigger.h"
#include "web_server.h"
#include "alarm_handler.h"

#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <syslog.h>

#define APP_NAME      "antitailgate"
#define WEB_PORT      8080

static GMainLoop *g_loop = NULL;

/* ------------------------------------------------------------------ */
/* GLib timer: expire tokens every second                              */
/* ------------------------------------------------------------------ */

static gboolean on_expire_timer(gpointer user_data)
{
    (void)user_data;
    token_expire_tick();
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Signal handlers                                                     */
/* ------------------------------------------------------------------ */

static gboolean on_signal(gpointer user_data)
{
    (void)user_data;
    syslog(LOG_INFO, "antitailgate: signal received, shutting down");
    if (g_loop)
        g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "antitailgate: starting v1.0.0");

    /* 1. Configuration */
    if (!config_init(APP_NAME)) {
        syslog(LOG_ERR, "antitailgate: config_init failed");
        return 1;
    }

    int   ttl      = config_get_int("TokenExpirationSeconds", 7);
    char *scenario = config_get_string("AoaScenarioId", "1");
    char *io_port  = config_get_string("InputTriggerPort", "none");


    /* 2. Token manager */
    token_manager_init(ttl);

    /* 3. Alarm handler (libcurl) */
    alarm_handler_init();

    /* 4a. Event publisher (declare outbound Axis event) */
    event_publisher_init();

    /* 4. Web server */
    if (!web_server_init(WEB_PORT)) {
        syslog(LOG_ERR, "antitailgate: web_server_init failed");
        alarm_handler_cleanup();
        token_manager_cleanup();
        config_cleanup();
        return 1;
    }

    /* 5. GLib main loop */
    g_loop = g_main_loop_new(NULL, FALSE);

    /* Register signal handlers via GLib for clean shutdown */
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT,  on_signal, NULL);

    /* Token expiry timer: fires every 1 second */
    g_timeout_add_seconds(1, on_expire_timer, NULL);

    /* 6. AOA event subscription (non-fatal if AOA not installed) */
    bool aoa_ok = event_subscriber_init(scenario);
    if (!aoa_ok) {
        syslog(LOG_WARNING,
               "antitailgate: AOA subscription failed; "
               "HTTP fallback (/api/threshold-crossing) remains available");
    }

    /* 7. I/O input trigger (non-fatal) */
    bool io_ok = input_trigger_init(io_port);
    if (!io_ok) {
        syslog(LOG_WARNING,
               "antitailgate: I/O input trigger failed for port '%s'",
               io_port);
    }

    syslog(LOG_INFO,
           "antitailgate: running (port=%d, TTL=%ds, scenario=%s, aoa=%s, io=%s)",
           WEB_PORT, ttl, scenario, aoa_ok ? "yes" : "no",
           io_ok ? io_port : "failed");
    free(scenario);
    free(io_port);

    /* 8. Block until signal */
    g_main_loop_run(g_loop);

    /* 9. Cleanup */
    syslog(LOG_INFO, "antitailgate: cleaning up");
    input_trigger_cleanup();
    event_subscriber_cleanup();
    event_publisher_cleanup();
    web_server_cleanup();
    alarm_handler_cleanup();
    token_manager_cleanup();
    config_cleanup();

    g_main_loop_unref(g_loop);
    closelog();
    return 0;
}
