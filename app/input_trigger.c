#include "input_trigger.h"
#include "token_manager.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/*
 * Digital input trigger.
 *
 * Subscribes to the device's input-port state topic:
 *   tns1:Device / tnsaxis:IO / Port
 *     source key "port"  (xsd:int, 0-based: 0 = "Port 1", -1 = "Any")
 *     data   key "state" (xsd:boolean, property state)
 *
 * The configured port value is the DEVICE-NATIVE 0-based index, matching
 * both this event's "port" key and the root.IOPort.I<n> parameters.  The
 * settings UI shows a friendly "Port <n+1>" label but stores <n>.
 */

static AXEventHandler *g_event_handler   = NULL;
static guint           g_subscription_id = 0;
static int             g_current_port    = -2;   /* -2 = unset */
static gboolean        g_baseline_pending = FALSE;

/* "none"/"disabled"/empty = off.  A bare "0" is a valid port (Port 1). */
static bool is_disabled(const char *port)
{
    return (!port || !port[0] ||
            strcmp(port, "none") == 0 ||
            strcmp(port, "disabled") == 0);
}

static void on_digital_input(guint subscription, AXEvent *event, gpointer user_data)
{
    (void)subscription;
    (void)user_data;

    AXEventKeyValueSet *kv = (AXEventKeyValueSet *)ax_event_get_key_value_set(event);
    if (!kv) { ax_event_free(event); return; }

    /* Match the configured port (the subscription also filters, but the
     * "Any" (-1) declaration means we re-check here). */
    gint port_val = 0;
    GError *err = NULL;
    if (ax_event_key_value_set_get_integer(kv, "port", NULL, &port_val, &err)) {
        if (port_val != g_current_port) {
            if (err) g_error_free(err);
            ax_event_free(event);
            return;
        }
    }
    if (err) { g_error_free(err); err = NULL; }

    gboolean state = FALSE;
    bool have_state = ax_event_key_value_set_get_boolean(kv, "state", NULL, &state, &err);
    if (err) { g_error_free(err); err = NULL; }

    /* First delivery after subscribe is the current state, not a new edge. */
    if (g_baseline_pending) {
        g_baseline_pending = FALSE;
        ax_event_free(event);
        return;
    }

    if (have_state && !state) {   /* falling edge — ignore */
        ax_event_free(event);
        return;
    }

    syslog(LOG_INFO, "antitailgate: I/O port %d rising edge — creating token",
           g_current_port);
    char source[32];
    snprintf(source, sizeof source, "IO Port %d", g_current_port + 1);
    token_add("", source);

    ax_event_free(event);
}

static AXEventKeyValueSet *build_key_set(int port)
{
    AXEventKeyValueSet *kv = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_value(kv, "topic0", "tns1", "Device",
                                         AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(kv, "topic1", "tnsaxis", "IO",
                                         AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(kv, "topic2", "tnsaxis", "Port",
                                         AX_VALUE_TYPE_STRING, NULL);
    /* Filter on the port source key so we only wake for the chosen input.
     * Including the key with a value in a subscription set matches only
     * that value. */
    gint p = port;
    ax_event_key_value_set_add_key_value(kv, "port", NULL, &p,
                                         AX_VALUE_TYPE_INT, NULL);
    return kv;
}

static bool do_subscribe(int port)
{
    GError *err = NULL;
    AXEventKeyValueSet *kv = build_key_set(port);
    if (!ax_event_handler_subscribe(g_event_handler, kv, &g_subscription_id,
                                    on_digital_input, NULL, &err)) {
        syslog(LOG_WARNING, "antitailgate: I/O input subscribe failed (%s)",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        ax_event_key_value_set_free(kv);
        return false;
    }
    ax_event_key_value_set_free(kv);
    g_current_port = port;
    g_baseline_pending = TRUE;
    syslog(LOG_INFO, "antitailgate: subscribed to I/O port %d (shown as Port %d)",
           port, port + 1);
    return true;
}

static void teardown(void)
{
    if (g_event_handler) {
        if (g_subscription_id)
            ax_event_handler_unsubscribe(g_event_handler, g_subscription_id, NULL);
        ax_event_handler_free(g_event_handler);
        g_event_handler = NULL;
        g_subscription_id = 0;
    }
    g_current_port = -2;
}

/* ------------------------------------------------------------------ */
/* Public API — all calls run on the GLib main loop                    */
/* ------------------------------------------------------------------ */

bool input_trigger_init(const char *port)
{
    if (is_disabled(port))
        return true; /* disabled is not an error */

    char *end = NULL;
    long p = strtol(port, &end, 10);
    if (!end || *end != '\0' || p < 0 || p > 65535) {
        syslog(LOG_WARNING, "antitailgate: invalid I/O port '%s'", port);
        return false;
    }

    g_event_handler = ax_event_handler_new();
    if (!g_event_handler) {
        syslog(LOG_WARNING, "antitailgate: ax_event_handler_new() failed for I/O input");
        return false;
    }
    if (!do_subscribe((int)p)) {
        ax_event_handler_free(g_event_handler);
        g_event_handler = NULL;
        return false;
    }
    return true;
}

bool input_trigger_resubscribe(const char *port)
{
    bool want_off = is_disabled(port);
    long p = want_off ? -2 : strtol(port, NULL, 10);

    /* Idempotent: unchanged port keeps the existing subscription. */
    if (!want_off && g_event_handler && g_subscription_id && (int)p == g_current_port)
        return true;
    if (want_off && !g_event_handler)
        return true;

    teardown();
    if (want_off)
        return true;
    return input_trigger_init(port);
}

void input_trigger_cleanup(void)
{
    teardown();
}
