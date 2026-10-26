#include "settings.h"
#include "config.h"
#include "validate.h"
#include "event_subscriber.h"
#include "input_trigger.h"

#include <axsdk/axparameter.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* AXParameter names (short form; the group is root.Antitailgate). */
#define P_TTL        "TokenExpirationSeconds"
#define P_AOA        "AoaScenarioId"
#define P_INPUT      "InputTriggerPort"
#define P_CLEAR      "AlarmClearSeconds"
#define P_TYPE       "AlarmActionType"
#define P_TLS        "AlarmActionTlsMode"
#define P_INSECURE   "AlarmActionInsecure"   /* legacy; read once for migration */
#define P_HOST       "AlarmActionHost"
#define P_PORT       "AlarmActionPort"
#define P_DURATION   "AlarmActionDuration"
#define P_USER       "AlarmActionUser"
#define P_PASS       "AlarmActionPass"
#define P_PIN        "AlarmActionPin"
#define P_URL        "AlarmActionUrl"
#define P_METHOD     "AlarmActionMethod"
#define P_PAYLOAD    "AlarmActionPayload"
#define P_HEADER     "AlarmActionHeader"

static Settings g_settings;
static GMutex   g_settings_lock;
static bool     g_ready = false;
static AXParameter *g_ax = NULL; /* borrowed from config.c for callbacks */

/* ------------------------------------------------------------------ */
/* Enum <-> string                                                     */
/* ------------------------------------------------------------------ */

const char *alarm_type_to_str(AlarmActionType t)
{
    switch (t) {
        case ALARM_VIRTUAL_INPUT: return "virtual_input";
        case ALARM_A9210_OUTPUT:  return "a9210_output";
        case ALARM_CUSTOM_HTTP:   return "custom_http";
        default:                  return "none";
    }
}

static bool alarm_type_from_str(const char *s, AlarmActionType *out)
{
    if (!s || !strcmp(s, "none"))          { *out = ALARM_NONE;          return true; }
    if (!strcmp(s, "virtual_input"))        { *out = ALARM_VIRTUAL_INPUT; return true; }
    if (!strcmp(s, "a9210_output"))         { *out = ALARM_A9210_OUTPUT;  return true; }
    if (!strcmp(s, "custom_http"))          { *out = ALARM_CUSTOM_HTTP;   return true; }
    return false;
}

const char *alarm_tls_to_str(AlarmTlsMode m)
{
    switch (m) {
        case TLS_PIN:      return "pin";
        case TLS_NOVERIFY: return "noverify";
        case TLS_HTTP:     return "http";
        default:           return "verify";
    }
}

static bool alarm_tls_from_str(const char *s, AlarmTlsMode *out)
{
    if (!s || !s[0] || !strcmp(s, "verify")) { *out = TLS_VERIFY;   return true; }
    if (!strcmp(s, "pin"))                    { *out = TLS_PIN;      return true; }
    if (!strcmp(s, "noverify"))               { *out = TLS_NOVERIFY; return true; }
    if (!strcmp(s, "http"))                   { *out = TLS_HTTP;     return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* Defaults + load                                                     */
/* ------------------------------------------------------------------ */

static void fill_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->token_ttl = 7;
    g_strlcpy(s->aoa_scenario_id, "1", sizeof s->aoa_scenario_id);
    g_strlcpy(s->input_port, "none", sizeof s->input_port);
    s->alarm_clear_seconds = 2;
    s->alarm_type = ALARM_NONE;
    s->alarm_tls = TLS_VERIFY;
    g_strlcpy(s->alarm_port, "1", sizeof s->alarm_port);
    g_strlcpy(s->alarm_duration, "5", sizeof s->alarm_duration);
    g_strlcpy(s->alarm_method, "GET", sizeof s->alarm_method);
}

/* Read a raw string param into dst if valid; otherwise leave dst unchanged. */
static void load_str(const char *name, const char *dflt,
                     char *dst, size_t dst_len)
{
    char *v = config_get_string(name, dflt);
    g_strlcpy(dst, v ? v : "", dst_len);
    free(v);
}

static void load_from_store(Settings *s)
{
    fill_defaults(s);

    s->token_ttl = config_get_int(P_TTL, 7);
    if (s->token_ttl < 1 || s->token_ttl > 3600) s->token_ttl = 7;

    s->alarm_clear_seconds = config_get_int(P_CLEAR, 2);
    if (s->alarm_clear_seconds < 1 || s->alarm_clear_seconds > 60)
        s->alarm_clear_seconds = 2;

    char tmp[1024];
    char err[128];

    load_str(P_AOA, "1", tmp, sizeof tmp);
    if (validate_scenario_id(tmp, P_AOA, err, sizeof err))
        g_strlcpy(s->aoa_scenario_id, tmp, sizeof s->aoa_scenario_id);

    load_str(P_INPUT, "none", tmp, sizeof tmp);
    if (!tmp[0]) g_strlcpy(tmp, "none", sizeof tmp);
    g_strlcpy(s->input_port, tmp, sizeof s->input_port);

    load_str(P_TYPE, "none", tmp, sizeof tmp);
    alarm_type_from_str(tmp, &s->alarm_type);

    /* TLS mode, with migration from the legacy AlarmActionInsecure bool. */
    load_str(P_TLS, "", tmp, sizeof tmp);
    if (tmp[0]) {
        alarm_tls_from_str(tmp, &s->alarm_tls);
    } else {
        char *legacy = config_get_string(P_INSECURE, "false");
        s->alarm_tls = (legacy && !strcmp(legacy, "true")) ? TLS_HTTP : TLS_VERIFY;
        free(legacy);
    }

    load_str(P_HOST, "", tmp, sizeof tmp);
    if (validate_host(tmp, P_HOST, err, sizeof err))
        g_strlcpy(s->alarm_host, tmp, sizeof s->alarm_host);

    load_str(P_PORT, "1", tmp, sizeof tmp);
    if (validate_numeric_str(tmp, 1, 65535, P_PORT, err, sizeof err) && tmp[0])
        g_strlcpy(s->alarm_port, tmp, sizeof s->alarm_port);

    load_str(P_DURATION, "5", tmp, sizeof tmp);
    if (validate_numeric_str(tmp, 1, 3600, P_DURATION, err, sizeof err) && tmp[0])
        g_strlcpy(s->alarm_duration, tmp, sizeof s->alarm_duration);

    load_str(P_USER, "", tmp, sizeof tmp);
    if (validate_text(tmp, 127, P_USER, err, sizeof err))
        g_strlcpy(s->alarm_user, tmp, sizeof s->alarm_user);

    load_str(P_PASS, "", s->alarm_pass, sizeof s->alarm_pass);

    load_str(P_PIN, "", tmp, sizeof tmp);
    if (validate_pinned_key(tmp, P_PIN, err, sizeof err))
        g_strlcpy(s->alarm_pin, tmp, sizeof s->alarm_pin);

    load_str(P_URL, "", tmp, sizeof tmp);
    if (validate_url(tmp, P_URL, err, sizeof err))
        g_strlcpy(s->alarm_url, tmp, sizeof s->alarm_url);

    load_str(P_METHOD, "GET", tmp, sizeof tmp);
    if (!strcmp(tmp, "GET") || !strcmp(tmp, "POST") || !strcmp(tmp, "PUT"))
        g_strlcpy(s->alarm_method, tmp, sizeof s->alarm_method);

    load_str(P_PAYLOAD, "", tmp, sizeof tmp);
    if (validate_text(tmp, 1023, P_PAYLOAD, err, sizeof err))
        g_strlcpy(s->alarm_payload, tmp, sizeof s->alarm_payload);

    load_str(P_HEADER, "", tmp, sizeof tmp);
    if (validate_header(tmp, P_HEADER, err, sizeof err))
        g_strlcpy(s->alarm_header, tmp, sizeof s->alarm_header);
}

/* ------------------------------------------------------------------ */
/* Resubscribe marshalling (always run on the main loop)              */
/* ------------------------------------------------------------------ */

static gboolean resub_aoa_idle(gpointer data)
{
    char *id = data;
    event_subscriber_resubscribe(id);  /* idempotent; no-op if unchanged */
    g_free(id);
    return G_SOURCE_REMOVE;
}

static gboolean resub_input_idle(gpointer data)
{
    char *port = data;
    input_trigger_resubscribe(port);   /* idempotent; no-op if unchanged */
    g_free(port);
    return G_SOURCE_REMOVE;
}

/* Called while holding no lock.  Schedules the resubscribe on the main loop
 * so AXEvent handlers are only ever touched from the dispatch thread. */
static void marshal_resub_aoa(const char *id)
{
    g_idle_add(resub_aoa_idle, g_strdup(id));
}
static void marshal_resub_input(const char *port)
{
    g_idle_add(resub_input_idle, g_strdup(port));
}

/* ------------------------------------------------------------------ */
/* Change callback (runs on the main loop; never calls ax_parameter_get) */
/* ------------------------------------------------------------------ */

static const char *short_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static void on_param_changed(const gchar *name, const gchar *value, gpointer data)
{
    (void)data;
    const char *n = short_name(name);
    const char *v = value ? value : "";
    char err[128];
    bool resub_aoa = false, resub_input = false;
    char new_aoa[65] = "", new_port[16] = "";

    g_mutex_lock(&g_settings_lock);
    if (!strcmp(n, P_TTL)) {
        long t = g_ascii_strtoll(v, NULL, 10);
        if (t >= 1 && t <= 3600) g_settings.token_ttl = (int)t;
    } else if (!strcmp(n, P_CLEAR)) {
        long t = g_ascii_strtoll(v, NULL, 10);
        if (t >= 1 && t <= 60) g_settings.alarm_clear_seconds = (int)t;
    } else if (!strcmp(n, P_AOA)) {
        if (validate_scenario_id(v, n, err, sizeof err) &&
            strcmp(v, g_settings.aoa_scenario_id) != 0) {
            g_strlcpy(g_settings.aoa_scenario_id, v, sizeof g_settings.aoa_scenario_id);
            g_strlcpy(new_aoa, g_settings.aoa_scenario_id, sizeof new_aoa);
            resub_aoa = true;
        }
    } else if (!strcmp(n, P_INPUT)) {
        const char *pv = v[0] ? v : "none";
        if (strcmp(pv, g_settings.input_port) != 0) {
            g_strlcpy(g_settings.input_port, pv, sizeof g_settings.input_port);
            g_strlcpy(new_port, g_settings.input_port, sizeof new_port);
            resub_input = true;
        }
    } else if (!strcmp(n, P_TYPE)) {
        alarm_type_from_str(v, &g_settings.alarm_type);
    } else if (!strcmp(n, P_TLS)) {
        alarm_tls_from_str(v, &g_settings.alarm_tls);
    } else if (!strcmp(n, P_HOST)) {
        if (validate_host(v, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_host, v, sizeof g_settings.alarm_host);
    } else if (!strcmp(n, P_PORT)) {
        if (v[0] && validate_numeric_str(v, 1, 65535, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_port, v, sizeof g_settings.alarm_port);
    } else if (!strcmp(n, P_DURATION)) {
        if (v[0] && validate_numeric_str(v, 1, 3600, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_duration, v, sizeof g_settings.alarm_duration);
    } else if (!strcmp(n, P_USER)) {
        if (validate_text(v, 127, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_user, v, sizeof g_settings.alarm_user);
    } else if (!strcmp(n, P_PASS)) {
        g_strlcpy(g_settings.alarm_pass, v, sizeof g_settings.alarm_pass);
    } else if (!strcmp(n, P_PIN)) {
        if (validate_pinned_key(v, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_pin, v, sizeof g_settings.alarm_pin);
    } else if (!strcmp(n, P_URL)) {
        if (validate_url(v, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_url, v, sizeof g_settings.alarm_url);
    } else if (!strcmp(n, P_METHOD)) {
        if (!strcmp(v, "GET") || !strcmp(v, "POST") || !strcmp(v, "PUT"))
            g_strlcpy(g_settings.alarm_method, v, sizeof g_settings.alarm_method);
    } else if (!strcmp(n, P_PAYLOAD)) {
        if (validate_text(v, 1023, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_payload, v, sizeof g_settings.alarm_payload);
    } else if (!strcmp(n, P_HEADER)) {
        if (validate_header(v, n, err, sizeof err))
            g_strlcpy(g_settings.alarm_header, v, sizeof g_settings.alarm_header);
    }
    g_mutex_unlock(&g_settings_lock);

    /* Already on the main loop here, but marshalling keeps one code path
     * and dedupes against an in-flight idle. */
    if (resub_aoa)   marshal_resub_aoa(new_aoa);
    if (resub_input) marshal_resub_input(new_port);
}

static void register_callbacks(void)
{
    if (!g_ax)
        return;
    const char *names[] = {
        P_TTL, P_AOA, P_INPUT, P_CLEAR, P_TYPE, P_TLS, P_HOST, P_PORT,
        P_DURATION, P_USER, P_PASS, P_PIN, P_URL, P_METHOD, P_PAYLOAD,
        P_HEADER, NULL
    };
    for (const char **p = names; *p; p++) {
        GError *err = NULL;
        if (!ax_parameter_register_callback(g_ax, *p, on_param_changed, NULL, &err)) {
            syslog(LOG_WARNING, "antitailgate: register_callback(%s) failed: %s",
                   *p, err ? err->message : "unknown");
            if (err) g_error_free(err);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void settings_init(void)
{
    g_mutex_init(&g_settings_lock);
    g_ax = config_ax_parameter();
    g_mutex_lock(&g_settings_lock);
    load_from_store(&g_settings);
    g_ready = true;
    g_mutex_unlock(&g_settings_lock);
    register_callbacks();
    syslog(LOG_INFO,
           "antitailgate: settings loaded (TTL=%d, scenario=%s, input=%s, "
           "alarm=%s, tls=%s)",
           g_settings.token_ttl, g_settings.aoa_scenario_id,
           g_settings.input_port, alarm_type_to_str(g_settings.alarm_type),
           alarm_tls_to_str(g_settings.alarm_tls));
}

void settings_get(Settings *out)
{
    g_mutex_lock(&g_settings_lock);
    if (g_ready) {
        *out = g_settings;
    } else {
        fill_defaults(out);
    }
    g_mutex_unlock(&g_settings_lock);
}

void settings_to_json(GString *out)
{
    Settings s;
    settings_get(&s);
    g_string_append_c(out, '{');
    g_string_append_printf(out, "\"TokenExpirationSeconds\":%d,", s.token_ttl);
    g_string_append(out, "\"AoaScenarioId\":\"");
    json_escape_append(out, s.aoa_scenario_id);
    g_string_append(out, "\",\"InputTriggerPort\":\"");
    json_escape_append(out, s.input_port);
    g_string_append_printf(out, "\",\"AlarmClearSeconds\":%d,", s.alarm_clear_seconds);
    g_string_append_printf(out, "\"AlarmActionType\":\"%s\",", alarm_type_to_str(s.alarm_type));
    g_string_append_printf(out, "\"AlarmActionTlsMode\":\"%s\",", alarm_tls_to_str(s.alarm_tls));
    g_string_append(out, "\"AlarmActionHost\":\"");
    json_escape_append(out, s.alarm_host);
    g_string_append(out, "\",\"AlarmActionPort\":\"");
    json_escape_append(out, s.alarm_port);
    g_string_append(out, "\",\"AlarmActionDuration\":\"");
    json_escape_append(out, s.alarm_duration);
    g_string_append(out, "\",\"AlarmActionUser\":\"");
    json_escape_append(out, s.alarm_user);
    g_string_append(out, "\",\"AlarmActionUrl\":\"");
    json_escape_append(out, s.alarm_url);
    g_string_append(out, "\",\"AlarmActionMethod\":\"");
    json_escape_append(out, s.alarm_method);
    g_string_append(out, "\",\"AlarmActionPayload\":\"");
    json_escape_append(out, s.alarm_payload);
    /* Secrets: expose only a configured-boolean, never the value. */
    g_string_append_printf(out, "\",\"AlarmActionPassConfigured\":%s,",
                           s.alarm_pass[0] ? "true" : "false");
    g_string_append_printf(out, "\"AlarmActionHeaderConfigured\":%s,",
                           s.alarm_header[0] ? "true" : "false");
    g_string_append_printf(out, "\"AlarmActionPinConfigured\":%s}",
                           s.alarm_pin[0] ? "true" : "false");
}

/* Validate one present field into tmp; return 1 ok, 0 absent, -1 error. */
static int apply_str(const JsonObject *req, const char *key,
                     bool (*vfn)(const char *, const char *, char *, size_t),
                     char *dst, size_t dst_len, char *err, size_t err_len)
{
    const char *v = NULL;
    int r = json_get_scalar_str(req, key, &v, err, err_len);
    if (r <= 0) return r;
    if (vfn && !vfn(v, key, err, err_len)) return -1;
    g_strlcpy(dst, v, dst_len);
    return 1;
}

bool settings_apply(const JsonObject *req, GString *updated,
                    char *err, size_t err_len)
{
    static const char *const allowed[] = {
        P_TTL, P_AOA, P_INPUT, P_CLEAR, P_TYPE, P_TLS, P_HOST, P_PORT,
        P_DURATION, P_USER, P_PASS, P_PIN, P_URL, P_METHOD, P_PAYLOAD,
        P_HEADER, "AlarmActionClearPass", "AlarmActionClearHeader", NULL
    };
    if (!json_object_only_keys(req, allowed, err, err_len))
        return false;

    Settings cur;
    settings_get(&cur);
    Settings nw = cur;
    bool clear_pass = false;

    /* ---- Phase 1: validate everything into nw; persist nothing yet ---- */
    int r;

    r = json_get_int(req, P_TTL, 1, 3600, &nw.token_ttl, err, err_len);
    if (r < 0) return false;

    r = json_get_int(req, P_CLEAR, 1, 60, &nw.alarm_clear_seconds, err, err_len);
    if (r < 0) return false;

    r = apply_str(req, P_AOA, validate_scenario_id,
                  nw.aoa_scenario_id, sizeof nw.aoa_scenario_id, err, err_len);
    if (r < 0) return false;

    /* input port: "none" or a decimal; empty becomes "none" */
    {
        const char *v = NULL;
        int rr = json_get_scalar_str(req, P_INPUT, &v, err, err_len);
        if (rr < 0) return false;
        if (rr == 1) {
            if (!v[0] || !strcmp(v, "none")) {
                g_strlcpy(nw.input_port, "none", sizeof nw.input_port);
            } else if (validate_numeric_str(v, 0, 65535, P_INPUT, err, err_len)) {
                g_strlcpy(nw.input_port, v, sizeof nw.input_port);
            } else {
                return false;
            }
        }
    }

    {
        const char *v = NULL;
        int rr = json_get_scalar_str(req, P_TYPE, &v, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && !alarm_type_from_str(v, &nw.alarm_type)) {
            snprintf(err, err_len, "Field '%s' must be none, virtual_input, "
                     "a9210_output, or custom_http", P_TYPE);
            return false;
        }
    }
    {
        const char *v = NULL;
        int rr = json_get_scalar_str(req, P_TLS, &v, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && !alarm_tls_from_str(v, &nw.alarm_tls)) {
            snprintf(err, err_len, "Field '%s' must be verify, pin, noverify, "
                     "or http", P_TLS);
            return false;
        }
    }

    r = apply_str(req, P_HOST, validate_host, nw.alarm_host, sizeof nw.alarm_host, err, err_len);
    if (r < 0) return false;

    { const char *v=NULL; int rr=json_get_scalar_str(req,P_PORT,&v,err,err_len);
      if (rr<0) return false;
      if (rr==1){ if(!validate_numeric_str(v,1,65535,P_PORT,err,err_len)) return false;
                  if(v[0]) g_strlcpy(nw.alarm_port,v,sizeof nw.alarm_port);} }

    { const char *v=NULL; int rr=json_get_scalar_str(req,P_DURATION,&v,err,err_len);
      if (rr<0) return false;
      if (rr==1){ if(!validate_numeric_str(v,1,3600,P_DURATION,err,err_len)) return false;
                  if(v[0]) g_strlcpy(nw.alarm_duration,v,sizeof nw.alarm_duration);} }

    r = apply_str(req, P_USER, NULL, nw.alarm_user, sizeof nw.alarm_user, err, err_len);
    if (r < 0) return false;
    if (json_object_get(req, P_USER) &&
        !validate_text(nw.alarm_user, 127, P_USER, err, err_len)) return false;

    r = apply_str(req, P_PIN, validate_pinned_key, nw.alarm_pin, sizeof nw.alarm_pin, err, err_len);
    if (r < 0) return false;

    r = apply_str(req, P_URL, validate_url, nw.alarm_url, sizeof nw.alarm_url, err, err_len);
    if (r < 0) return false;

    { const char *v=NULL; int rr=json_get_scalar_str(req,P_METHOD,&v,err,err_len);
      if (rr<0) return false;
      if (rr==1){ if(strcmp(v,"GET")&&strcmp(v,"POST")&&strcmp(v,"PUT")){
                    snprintf(err,err_len,"Field '%s' must be GET, POST, or PUT",P_METHOD); return false;}
                  g_strlcpy(nw.alarm_method,v,sizeof nw.alarm_method);} }

    r = apply_str(req, P_PAYLOAD, NULL, nw.alarm_payload, sizeof nw.alarm_payload, err, err_len);
    if (r < 0) return false;
    if (json_object_get(req, P_PAYLOAD) &&
        !validate_text(nw.alarm_payload, 1023, P_PAYLOAD, err, err_len)) return false;

    /* Secrets (password, header): keep-if-blank.  Only overwrite when a
     * non-empty value is supplied; an explicit Clear flag wipes.  This is
     * why a plain Save with blank secret fields never erases them. */
    {
        const char *v = NULL;
        int rr = json_get_str(req, P_PASS, &v, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && v[0]) {
            if (!validate_text(v, 127, P_PASS, err, err_len)) return false;
            g_strlcpy(nw.alarm_pass, v, sizeof nw.alarm_pass);
        }
        bool want_clear = false;
        rr = json_get_bool(req, "AlarmActionClearPass", &want_clear, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && want_clear) { nw.alarm_pass[0] = '\0'; clear_pass = true; }

        v = NULL;
        rr = json_get_str(req, P_HEADER, &v, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && v[0]) {
            if (!validate_header(v, P_HEADER, err, err_len)) return false;
            g_strlcpy(nw.alarm_header, v, sizeof nw.alarm_header);
        }
        want_clear = false;
        rr = json_get_bool(req, "AlarmActionClearHeader", &want_clear, err, err_len);
        if (rr < 0) return false;
        if (rr == 1 && want_clear) nw.alarm_header[0] = '\0';
    }

    /* ---- Phase 2: persist only what changed, refresh snapshot ---- */
#define CHANGED_STR(field, pname) \
    (strcmp(cur.field, nw.field) != 0)
#define PERSIST(cond, pname, val) do { \
        if (cond) { \
            if (config_set(pname, val)) \
                g_string_append_printf(updated, "%s\"%s\"", updated->len ? "," : "", pname); \
            else \
                snprintf(err, err_len, "Failed to persist %s", pname); \
        } \
    } while (0)

    char ibuf[16];
    if (cur.token_ttl != nw.token_ttl) {
        snprintf(ibuf, sizeof ibuf, "%d", nw.token_ttl);
        PERSIST(true, P_TTL, ibuf);
    }
    if (cur.alarm_clear_seconds != nw.alarm_clear_seconds) {
        snprintf(ibuf, sizeof ibuf, "%d", nw.alarm_clear_seconds);
        PERSIST(true, P_CLEAR, ibuf);
    }
    PERSIST(CHANGED_STR(aoa_scenario_id,P_AOA), P_AOA, nw.aoa_scenario_id);
    PERSIST(CHANGED_STR(input_port,P_INPUT), P_INPUT, nw.input_port);
    if (cur.alarm_type != nw.alarm_type)
        PERSIST(true, P_TYPE, alarm_type_to_str(nw.alarm_type));
    if (cur.alarm_tls != nw.alarm_tls)
        PERSIST(true, P_TLS, alarm_tls_to_str(nw.alarm_tls));
    PERSIST(CHANGED_STR(alarm_host,P_HOST), P_HOST, nw.alarm_host);
    PERSIST(CHANGED_STR(alarm_port,P_PORT), P_PORT, nw.alarm_port);
    PERSIST(CHANGED_STR(alarm_duration,P_DURATION), P_DURATION, nw.alarm_duration);
    PERSIST(CHANGED_STR(alarm_user,P_USER), P_USER, nw.alarm_user);
    PERSIST(CHANGED_STR(alarm_pin,P_PIN), P_PIN, nw.alarm_pin);
    PERSIST(CHANGED_STR(alarm_url,P_URL), P_URL, nw.alarm_url);
    PERSIST(CHANGED_STR(alarm_method,P_METHOD), P_METHOD, nw.alarm_method);
    PERSIST(CHANGED_STR(alarm_payload,P_PAYLOAD), P_PAYLOAD, nw.alarm_payload);
    PERSIST(CHANGED_STR(alarm_header,P_HEADER), P_HEADER, nw.alarm_header);
    if (strcmp(cur.alarm_pass, nw.alarm_pass) != 0) {
        PERSIST(true, P_PASS, nw.alarm_pass);
        if (clear_pass)
            g_string_append_printf(updated, "%s\"AlarmActionClearPass\"",
                                   updated->len ? "," : "");
    }
#undef PERSIST
#undef CHANGED_STR

    /* Refresh the snapshot directly so a GET right after this POST is
     * current, without waiting for the deferred change callback. */
    g_mutex_lock(&g_settings_lock);
    bool aoa_changed   = strcmp(g_settings.aoa_scenario_id, nw.aoa_scenario_id) != 0;
    bool input_changed = strcmp(g_settings.input_port, nw.input_port) != 0;
    g_settings = nw;
    g_mutex_unlock(&g_settings_lock);

    if (aoa_changed)   marshal_resub_aoa(nw.aoa_scenario_id);
    if (input_changed) marshal_resub_input(nw.input_port);

    /* Zero the stack copies that held the password. */
    explicit_bzero(cur.alarm_pass, sizeof cur.alarm_pass);
    explicit_bzero(nw.alarm_pass, sizeof nw.alarm_pass);
    return true;
}

void settings_reset_defaults(void)
{
    Settings d;
    fill_defaults(&d);

    config_set(P_TTL, "7");
    config_set(P_AOA, "1");
    config_set(P_INPUT, "none");
    config_set(P_CLEAR, "2");
    config_set(P_TYPE, "none");
    config_set(P_TLS, "verify");
    config_set(P_HOST, "");
    config_set(P_PORT, "1");
    config_set(P_DURATION, "5");
    config_set(P_USER, "");
    config_set(P_PASS, "");
    config_set(P_PIN, "");
    config_set(P_URL, "");
    config_set(P_METHOD, "GET");
    config_set(P_PAYLOAD, "");
    config_set(P_HEADER, "");

    g_mutex_lock(&g_settings_lock);
    g_settings = d;
    g_mutex_unlock(&g_settings_lock);

    marshal_resub_aoa("1");
    marshal_resub_input("none");
}

bool settings_build_alarm_urls(const Settings *s,
                               char *url, size_t url_len,
                               char *deactivate_url, size_t deact_len,
                               char *err, size_t err_len)
{
    url[0] = '\0';
    if (deactivate_url) deactivate_url[0] = '\0';
    const char *scheme = (s->alarm_tls == TLS_HTTP) ? "http" : "https";

    if (s->alarm_type == ALARM_VIRTUAL_INPUT) {
        if (!s->alarm_host[0]) { snprintf(err, err_len, "virtual_input requires a host"); return false; }
        snprintf(url, url_len,
                 "%s://%s/axis-cgi/virtualinput/activate.cgi"
                 "?schemaversion=1&port=%s&duration=%s",
                 scheme, s->alarm_host, s->alarm_port, s->alarm_duration);
        return true;
    }
    if (s->alarm_type == ALARM_A9210_OUTPUT) {
        if (!s->alarm_host[0]) { snprintf(err, err_len, "a9210_output requires a host"); return false; }
        /* One request with a device-side timed pulse: action=<port>:/<ms>\
         * so the relay drops even if we crash or shut down mid-pulse. */
        long secs = g_ascii_strtoll(s->alarm_duration[0] ? s->alarm_duration : "5", NULL, 10);
        long ms = secs * 1000;
        if (ms < 1) ms = 1000;
        if (ms > 3600000) ms = 3600000;
        snprintf(url, url_len,
                 "%s://%s/axis-cgi/io/port.cgi?action=%s%%3A%%2F%ld%%5C",
                 scheme, s->alarm_host, s->alarm_port, ms);
        return true;
    }
    if (s->alarm_type == ALARM_CUSTOM_HTTP) {
        if (!s->alarm_url[0]) { snprintf(err, err_len, "custom_http requires a URL"); return false; }
        if (s->alarm_tls != TLS_HTTP && strncmp(s->alarm_url, "http://", 7) == 0) {
            snprintf(err, err_len, "URL is http:// but TLS mode is not 'http'");
            return false;
        }
        g_strlcpy(url, s->alarm_url, url_len);
        return true;
    }
    snprintf(err, err_len, "no alarm action configured");
    return false;
}
