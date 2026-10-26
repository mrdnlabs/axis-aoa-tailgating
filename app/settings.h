#pragma once

#include "json_util.h"

#include <glib.h>
#include <stdbool.h>

/*
 * Validated, in-memory configuration snapshot.
 *
 * Every reader (HTTP handlers, the alarm worker, the event publisher) takes
 * a cheap lock-protected copy via settings_get() instead of doing a D-Bus
 * round trip per value.  The snapshot is populated once at startup and then
 * kept current by AXParameter change callbacks, so values written through
 * the app's own API, through param.cgi, or through the device's native
 * app-settings dialog all converge to the same validated state.
 *
 * All strings are bounded and known-valid once they are in the snapshot:
 * anything that fails validation on the way in keeps the previous value.
 */

typedef enum {
    ALARM_NONE,
    ALARM_VIRTUAL_INPUT,
    ALARM_A9210_OUTPUT,
    ALARM_CUSTOM_HTTP,
} AlarmActionType;

typedef enum {
    TLS_VERIFY,    /* https, verify peer against the system CA store      */
    TLS_PIN,       /* https, verify against AlarmActionPin (pinned key)   */
    TLS_NOVERIFY,  /* https, no verification (self-signed peripherals)    */
    TLS_HTTP,      /* plain http (cleartext; last resort)                 */
} AlarmTlsMode;

typedef struct {
    int             token_ttl;             /* 1..3600 */
    char            aoa_scenario_id[65];
    char            input_port[16];        /* "none" or decimal */
    int             alarm_clear_seconds;   /* 1..60 */

    AlarmActionType alarm_type;
    AlarmTlsMode    alarm_tls;
    char            alarm_host[256];
    char            alarm_port[16];
    char            alarm_duration[16];    /* seconds, 1..3600 */
    char            alarm_user[128];
    char            alarm_pass[128];       /* secret; never serialized */
    char            alarm_pin[1024];       /* sha256//... pinned key */
    char            alarm_url[1024];
    char            alarm_method[8];       /* GET|POST|PUT */
    char            alarm_payload[1024];
    char            alarm_header[256];     /* secret-ish; never serialized */
} Settings;

/* Called once on the main loop at startup, after config_init().  Reads and
 * validates every parameter, then registers change callbacks. */
void settings_init(void);

/* Copy the current snapshot.  Cheap, lock-protected, no IPC; safe from any
 * thread. */
void settings_get(Settings *out);

/* Serialize the snapshot as the JSON object returned by GET /admin/config.
 * Secrets are emitted as "<name>Configured": bool, never as values. */
void settings_to_json(GString *out);

/* Two-phase apply of a settings POST.
 *   Phase 1: validate every present field; on any error write nothing,
 *            fill err, return false (caller sends 400 err).
 *   Phase 2: persist changed fields, refresh the snapshot, and (on the main
 *            loop) resubscribe AOA / input only if those values changed.
 * On success, 'updated' is filled with a JSON array body of changed names.
 * Runs on a CivetWeb worker thread. */
bool settings_apply(const JsonObject *req, GString *updated,
                    char *err, size_t err_len);

/* Reset everything to defaults (used by /admin/reset-defaults). */
void settings_reset_defaults(void);

/* Build the outbound alarm URL(s) from the snapshot for the current type.
 * Returns false if the type is misconfigured (reason in err). */
bool settings_build_alarm_urls(const Settings *s,
                               char *url, size_t url_len,
                               char *deactivate_url, size_t deact_len,
                               char *err, size_t err_len);

const char *alarm_type_to_str(AlarmActionType t);
const char *alarm_tls_to_str(AlarmTlsMode m);
