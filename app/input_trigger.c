#include "input_trigger.h"
#include "token_manager.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static AXEventHandler *g_event_handler   = NULL;
static guint           g_subscription_id = 0;
static char            g_port[16]        = "";

/* ------------------------------------------------------------------ */
/* Is the feature disabled?                                            */
/* ------------------------------------------------------------------ */

static bool is_disabled(const char *port)
{
    return (!port || !port[0] ||
            strcmp(port, "none") == 0 ||
            strcmp(port, "disabled") == 0 ||
            strcmp(port, "0") == 0);
}

/* ------------------------------------------------------------------ */
/* AXEvent callback                                                    */
/* ------------------------------------------------------------------ */

static void on_digital_input(guint subscription,
                              AXEvent *event,
                              gpointer user_data)
{
    (void)subscription;
    (void)user_data;

    AXEventKeyValueSet *kv =
        (AXEventKeyValueSet *)ax_event_get_key_value_set(event);
    if (!kv) {
        ax_event_free(event);
        return;
    }

    /* Filter by configured port number */
    gint port_val = 0;
    GError *err = NULL;
    if (ax_event_key_value_set_get_integer(kv, "port", NULL,
                                            &port_val, &err)) {
        int want = atoi(g_port);
        if (port_val != want) {
            if (err) g_error_free(err);
            ax_event_free(event);
            return;
        }
    }
    if (err) { g_error_free(err); err = NULL; }

    /* Only rising edge (LogicalState = true) */
    gboolean state = FALSE;
    if (ax_event_key_value_set_get_boolean(kv, "LogicalState", NULL,
                                            &state, &err)) {
        if (!state) {
            if (err) g_error_free(err);
            ax_event_free(event);
            return;
        }
    }
    if (err) g_error_free(err);

    syslog(LOG_INFO, "antitailgate: I/O port %s rising edge — creating token",
           g_port);

    char source[32];
    snprintf(source, sizeof(source), "IO Port %s", g_port);
    token_add("", source);

    ax_event_free(event);
}

/* ------------------------------------------------------------------ */
/* Build subscription key set                                          */
/* ------------------------------------------------------------------ */

static AXEventKeyValueSet *build_key_set(void)
{
    AXEventKeyValueSet *kv = ax_event_key_value_set_new();

    ax_event_key_value_set_add_key_value(kv,
        "topic0", "tns1", "Device",
        AX_VALUE_TYPE_STRING, NULL);

    ax_event_key_value_set_add_key_value(kv,
        "topic1", "tns1", "Trigger",
        AX_VALUE_TYPE_STRING, NULL);

    ax_event_key_value_set_add_key_value(kv,
        "topic2", "tns1", "DigitalInput",
        AX_VALUE_TYPE_STRING, NULL);

    return kv;
}

/* ------------------------------------------------------------------ */
/* Internal subscribe                                                  */
/* ------------------------------------------------------------------ */

static bool do_subscribe(void)
{
    GError *err = NULL;
    AXEventKeyValueSet *kv = build_key_set();

    if (!ax_event_handler_subscribe(
            g_event_handler, kv, &g_subscription_id,
            on_digital_input, NULL, &err)) {
        syslog(LOG_WARNING,
               "antitailgate: I/O input subscribe failed (%s)",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        ax_event_key_value_set_free(kv);
        return false;
    }

    ax_event_key_value_set_free(kv);
    syslog(LOG_INFO,
           "antitailgate: subscribed to I/O port %s digital input", g_port);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool input_trigger_init(const char *port)
{
    if (is_disabled(port))
        return true; /* disabled is not an error */

    strncpy(g_port, port, sizeof(g_port) - 1);
    g_port[sizeof(g_port) - 1] = '\0';

    g_event_handler = ax_event_handler_new();
    if (!g_event_handler) {
        syslog(LOG_WARNING,
               "antitailgate: ax_event_handler_new() failed for I/O input");
        return false;
    }

    if (!do_subscribe()) {
        ax_event_handler_free(g_event_handler);
        g_event_handler = NULL;
        return false;
    }
    return true;
}

bool input_trigger_resubscribe(const char *port)
{
    input_trigger_cleanup();
    return input_trigger_init(port);
}

void input_trigger_cleanup(void)
{
    if (g_event_handler) {
        if (g_subscription_id)
            ax_event_handler_unsubscribe(g_event_handler,
                                          g_subscription_id, NULL);
        ax_event_handler_free(g_event_handler);
        g_event_handler   = NULL;
        g_subscription_id = 0;
    }
    g_port[0] = '\0';
}
