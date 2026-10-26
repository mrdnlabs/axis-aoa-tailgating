#include "event_subscriber.h"
#include "token_manager.h"
#include "alarm_handler.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

static AXEventHandler *g_event_handler   = NULL;
static guint           g_subscription_id = 0;
static char            g_current_id[65]  = "";

/* A stateful AOA topic redelivers its current value right after subscribe.
 * Treat the first delivery after a (re)subscribe as a baseline to sync
 * against, not as a fresh crossing, so enabling the app while someone is
 * mid-zone doesn't spuriously consume a token or raise an alarm. */
static gboolean        g_baseline_pending = FALSE;

/* ------------------------------------------------------------------ */
/* AXEvent callback                                                    */
/* ------------------------------------------------------------------ */

static void on_line_crossing(guint subscription, AXEvent *event, gpointer user_data)
{
    (void)subscription;
    (void)user_data;

    /* For fence/crossline (non-counting) scenarios, the event fires on
     * both rising edge (active=true, person enters zone) and falling edge
     * (active=false, person leaves). We only want the rising edge.
     *
     * For CrosslineCounting scenarios the event has no 'active' key at all —
     * it fires once per detected crossing with count data. We always process it.
     *
     * Strategy: try to read 'active'; if it exists and is FALSE, skip.
     */
    bool has_active = false;
    AXEventKeyValueSet *kv = (AXEventKeyValueSet *)ax_event_get_key_value_set(event);
    if (kv) {
        gboolean active_val = FALSE;
        GError  *err = NULL;
        if (ax_event_key_value_set_get_boolean(kv, "active", NULL,
                                               &active_val, &err)) {
            has_active = true;
            if (!active_val) {
                /* Falling edge of a fence event — ignore */
                if (err) g_error_free(err);
                ax_event_free(event);
                return;
            }
        }
        if (err) g_error_free(err);
    }

    /* Swallow the first stateful delivery after (re)subscribe: it is the
     * current property value, not a new crossing.  Counting scenarios have
     * no 'active' key and fire once per crossing, so never baseline those. */
    if (g_baseline_pending && has_active) {
        g_baseline_pending = FALSE;
        syslog(LOG_DEBUG, "antitailgate: AOA baseline delivery ignored");
        ax_event_free(event);
        return;
    }
    g_baseline_pending = FALSE;

    syslog(LOG_INFO, "antitailgate: AOA crossing event received");

    bool authorized = token_consume("aoa");
    if (!authorized)
        alarm_handler_notify(false);

    ax_event_free(event);
}

/* ------------------------------------------------------------------ */
/* Build the event key filter for a given scenario                     */
/*                                                                     */
/* Topic path:                                                         */
/*   tnsaxis:CameraApplicationPlatform/ObjectAnalytics/Device1ScenarioX */
/*                                                                     */
/* No 'active' filter — we handle fence vs. counting in the callback. */
/* ------------------------------------------------------------------ */

static AXEventKeyValueSet *build_key_set(const char *scenario_id)
{
    AXEventKeyValueSet *kv = ax_event_key_value_set_new();

    ax_event_key_value_set_add_key_value(kv,
        "topic0", "tnsaxis", "CameraApplicationPlatform",
        AX_VALUE_TYPE_STRING, NULL);

    ax_event_key_value_set_add_key_value(kv,
        "topic1", "tnsaxis", "ObjectAnalytics",
        AX_VALUE_TYPE_STRING, NULL);

    /* Build "Device1Scenario<id>" — id may be an integer string or GUID */
    char scenario_topic[256];
    snprintf(scenario_topic, sizeof(scenario_topic),
             "Device1Scenario%s", scenario_id);
    ax_event_key_value_set_add_key_value(kv,
        "topic2", "tnsaxis", scenario_topic,
        AX_VALUE_TYPE_STRING, NULL);

    return kv;
}

/* ------------------------------------------------------------------ */
/* Internal: subscribe (handler must already exist)                   */
/* ------------------------------------------------------------------ */

static bool do_subscribe(const char *scenario_id)
{
    GError *err = NULL;
    AXEventKeyValueSet *kv = build_key_set(scenario_id);

    if (!ax_event_handler_subscribe(
            g_event_handler,
            kv,
            &g_subscription_id,
            on_line_crossing,
            NULL,
            &err)) {
        syslog(LOG_WARNING,
               "antitailgate: AXEvent subscribe failed (%s) — AOA events disabled",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        ax_event_key_value_set_free(kv);
        return false;
    }

    ax_event_key_value_set_free(kv);
    g_strlcpy(g_current_id, scenario_id, sizeof g_current_id);
    g_baseline_pending = TRUE;
    syslog(LOG_INFO,
           "antitailgate: subscribed to AOA scenario '%s'", scenario_id);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API — all calls run on the GLib main loop                    */
/* ------------------------------------------------------------------ */

bool event_subscriber_init(const char *scenario_id)
{
    g_event_handler = ax_event_handler_new();
    if (!g_event_handler) {
        syslog(LOG_WARNING,
               "antitailgate: ax_event_handler_new() failed — AOA events disabled");
        return false;
    }

    if (!do_subscribe(scenario_id)) {
        ax_event_handler_free(g_event_handler);
        g_event_handler = NULL;
        g_current_id[0] = '\0';
        return false;
    }
    return true;
}

bool event_subscriber_resubscribe(const char *scenario_id)
{
    if (!scenario_id || !scenario_id[0])
        return false;

    /* Idempotent: a save that didn't change the scenario (or a duplicate
     * marshalled call) must not drop and recreate the subscription, which
     * would miss crossings during the gap. */
    if (g_event_handler && g_subscription_id &&
        strcmp(scenario_id, g_current_id) == 0)
        return true;

    if (!g_event_handler)
        return event_subscriber_init(scenario_id);

    if (g_subscription_id) {
        ax_event_handler_unsubscribe(g_event_handler, g_subscription_id, NULL);
        g_subscription_id = 0;
    }
    g_current_id[0] = '\0';
    return do_subscribe(scenario_id);
}

void event_subscriber_cleanup(void)
{
    if (g_event_handler) {
        if (g_subscription_id)
            ax_event_handler_unsubscribe(g_event_handler,
                                         g_subscription_id, NULL);
        ax_event_handler_free(g_event_handler);
        g_event_handler = NULL;
        g_subscription_id = 0;
    }
    g_current_id[0] = '\0';
}
