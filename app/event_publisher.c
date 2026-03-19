#include "event_publisher.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdbool.h>
#include <syslog.h>

static AXEventHandler *g_pub_handler   = NULL;
static guint           g_declaration_id = 0;
static bool            g_initialized    = false;

/* ------------------------------------------------------------------ */
/* Init: declare the stateful alarm event                             */
/* ------------------------------------------------------------------ */

bool event_publisher_init(void)
{
    GError *err = NULL;

    g_pub_handler = ax_event_handler_new();
    if (!g_pub_handler) {
        syslog(LOG_WARNING, "antitailgate: event_publisher: ax_event_handler_new() failed");
        return false;
    }

    /* Build the event key-value schema.
     * Topic path:
     *   tnsaxis:CameraApplicationPlatform/antitailgate/TailgatingAlarm
     * Data key: active (bool, stateful)
     */
    AXEventKeyValueSet *kv = ax_event_key_value_set_new();

    ax_event_key_value_set_add_key_value(kv,
        "topic0", "tnsaxis", "CameraApplicationPlatform",
        AX_VALUE_TYPE_STRING, NULL);

    ax_event_key_value_set_add_key_value(kv,
        "topic1", "tnsaxis", "antitailgate",
        AX_VALUE_TYPE_STRING, NULL);

    ax_event_key_value_set_add_key_value(kv,
        "topic2", "tnsaxis", "TailgatingAlarm",
        AX_VALUE_TYPE_STRING, NULL);

    /* Mark topic1 as the application data namespace */
    ax_event_key_value_set_mark_as_user_defined(kv,
        "topic1", "tnsaxis", "isApplicationData", NULL);

    /* The stateful data key: active (initially false) */
    ax_event_key_value_set_add_key_value(kv,
        "active", NULL, "0",
        AX_VALUE_TYPE_BOOL, NULL);

    ax_event_key_value_set_mark_as_data(kv, "active", NULL, NULL);

    /* Declare as a stateful (property) event */
    if (!ax_event_handler_declare(g_pub_handler,
                                  kv,
                                  TRUE,   /* stateful */
                                  &g_declaration_id,
                                  NULL,   /* no send callback needed */
                                  NULL,
                                  &err)) {
        syslog(LOG_WARNING,
               "antitailgate: event_publisher: declare failed: %s",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        ax_event_key_value_set_free(kv);
        ax_event_handler_free(g_pub_handler);
        g_pub_handler = NULL;
        return false;
    }

    ax_event_key_value_set_free(kv);
    g_initialized = true;
    syslog(LOG_INFO,
           "antitailgate: event_publisher: declared TailgatingAlarm event (id=%u)",
           g_declaration_id);
    return true;
}

/* ------------------------------------------------------------------ */
/* Send: fire the alarm event on the main loop                        */
/* ------------------------------------------------------------------ */

/* Idle callback payload */
typedef struct { bool active; } SendPayload;

static gboolean do_send_event(gpointer data)
{
    SendPayload *p = (SendPayload *)data;

    if (!g_initialized) {
        g_free(p);
        return G_SOURCE_REMOVE;
    }

    AXEventKeyValueSet *kv = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_value(kv,
        "active", NULL, p->active ? "1" : "0",
        AX_VALUE_TYPE_BOOL, NULL);

    AXEvent *event = ax_event_new2(kv, NULL);
    ax_event_key_value_set_free(kv);

    if (!ax_event_handler_send_event(g_pub_handler, g_declaration_id, event, NULL)) {
        syslog(LOG_WARNING,
               "antitailgate: event_publisher: send_event failed (active=%d)", p->active);
    } else {
        syslog(LOG_INFO,
               "antitailgate: TailgatingAlarm event sent (active=%d)", p->active);
    }

    ax_event_free(event);
    g_free(p);
    return G_SOURCE_REMOVE;
}

void event_publisher_send_alarm(bool active)
{
    if (!g_initialized)
        return;

    /* Schedule on the GLib main loop so axevent stays single-threaded */
    SendPayload *p = g_new0(SendPayload, 1);
    p->active = active;
    g_idle_add(do_send_event, p);
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                            */
/* ------------------------------------------------------------------ */

void event_publisher_cleanup(void)
{
    if (g_pub_handler) {
        if (g_declaration_id)
            ax_event_handler_undeclare(g_pub_handler, g_declaration_id, NULL);
        ax_event_handler_free(g_pub_handler);
        g_pub_handler = NULL;
        g_declaration_id = 0;
    }
    g_initialized = false;
}
