#include "web_server.h"
#include "token_manager.h"
#include "alarm_handler.h"
#include "event_subscriber.h"
#include "input_trigger.h"
#include "config.h"
#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define APP_VERSION "1.0.0"

static struct mg_context *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static void send_json(struct mg_connection *conn,
                      int status_code, const char *json)
{
    mg_printf(conn,
              "HTTP/1.1 %d OK\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
              "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
              "Connection: close\r\n"
              "\r\n",
              status_code, (int)strlen(json));
    mg_write(conn, json, strlen(json));
}

static void send_error(struct mg_connection *conn,
                       int status_code, const char *message)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error\",\"message\":\"%s\"}", message);
    mg_printf(conn,
              "HTTP/1.1 %d Error\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Connection: close\r\n"
              "\r\n",
              status_code, (int)strlen(buf));
    mg_write(conn, buf, strlen(buf));
}

/* Global begin_request callback: intercept OPTIONS preflight before URI dispatch */
static int begin_request_callback(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (ri && strcmp(ri->request_method, "OPTIONS") == 0) {
        mg_printf(conn,
                  "HTTP/1.1 204 No Content\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                  "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                  "Content-Length: 0\r\n"
                  "Connection: close\r\n"
                  "\r\n");
        return 1; /* handled — do not pass to URI handlers */
    }
    return 0; /* not handled — pass through to URI handlers */
}

/* Format an ISO-8601 timestamp from time_t */
static void fmt_time(time_t t, char *out, size_t out_len)
{
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_val);
}

/* Escape a string for JSON (minimal: backslash and double-quote) */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_len; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            if (j + 3 < out_len)
                out[j++] = '\\';
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------ */
/* GET query parameter extraction                                      */
/* ------------------------------------------------------------------ */

static void get_query_param(struct mg_connection *conn,
                             const char *name,
                             char *out, size_t out_len)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    out[0] = '\0';
    if (!ri || !ri->query_string)
        return;
    mg_get_var(ri->query_string, strlen(ri->query_string),
               name, out, (int)out_len);
}

/* Read the request body into a malloc'd buffer. Caller must free(). */
static char *read_body(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    long long clen = 0;
    if (ri->content_length > 0)
        clen = ri->content_length;
    if (clen <= 0 || clen > 65536)
        return NULL;
    char *buf = malloc((size_t)clen + 1);
    if (!buf)
        return NULL;
    int n = mg_read(conn, buf, (size_t)clen);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON parser: extract string/int field from a flat JSON obj  */
/* ------------------------------------------------------------------ */

static bool json_get_string(const char *json,
                             const char *key,
                             char *out, size_t out_len)
{
    /* Looks for "key":"value" */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p)
        return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len)
        out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p)
        return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9')
        return false;
    *out = atoi(p);
    return true;
}

/* ------------------------------------------------------------------ */
/* Handler: /badge-read                                                */
/* ------------------------------------------------------------------ */

static int handler_badge_read(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    /* Defaults: query string badge_id, source "http" */
    char badge_id[64] = "";
    char source[64]   = "http";
    get_query_param(conn, "badge_id", badge_id, sizeof(badge_id));

    /* Try to parse JSON body for door controller events.
     * Expected: {"door":"Front Door","card":"12345"}
     * Falls back to query-string / defaults if body is absent or unparseable. */
    char *body = read_body(conn);
    if (body) {
        char door[64] = "";
        char card[64] = "";
        json_get_string(body, "door", door, sizeof(door));
        json_get_string(body, "card", card, sizeof(card));
        if (door[0])
            strncpy(source,   door, sizeof(source)   - 1);
        if (card[0])
            strncpy(badge_id, card, sizeof(badge_id) - 1);
        free(body);
    }

    if (!badge_id[0])
        strncpy(badge_id, "unknown", sizeof(badge_id) - 1);

    int count = token_add(badge_id, source);

    char resp[256];
    char safe_badge[128], safe_source[128];
    json_escape(badge_id, safe_badge,  sizeof(safe_badge));
    json_escape(source,   safe_source, sizeof(safe_source));
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"message\":\"Token created\","
             "\"token_count\":%d,\"badge_id\":\"%s\",\"source\":\"%s\"}",
             count, safe_badge, safe_source);
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /threshold-crossing                                        */
/* ------------------------------------------------------------------ */

static int handler_threshold_crossing(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    bool authorized = token_consume("http");

    if (!authorized)
        alarm_handler_notify(false);

    int remaining = token_count();
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"outcome\":\"%s\",\"tokens_remaining\":%d}",
             authorized ? "authorized" : "alarm",
             remaining);
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /status                                                    */
/* ------------------------------------------------------------------ */

static int handler_status(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    HistoryEvent events[MAX_HISTORY];
    AlarmRecord  alarms[MAX_HISTORY];
    int alarm_count = 0;

    int ev_count = history_snapshot(events, MAX_HISTORY,
                                    alarms, MAX_HISTORY, &alarm_count);
    int tc = token_count();

    /* Build JSON response */
    char *buf = malloc(65536);
    if (!buf) {
        send_error(conn, 500, "Out of memory");
        return 500;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 65536 - pos,
                    "{\"token_count\":%d,\"events\":[", tc);

    for (int i = 0; i < ev_count; i++) {
        char ts[32];
        fmt_time(events[i].timestamp, ts, sizeof(ts));

        const char *type_str =
            events[i].type == EVENT_BADGE_READ ? "badge_read" : "line_crossing";
        const char *outcome_str;
        switch (events[i].outcome) {
            case OUTCOME_TOKEN_CREATED: outcome_str = "token_created"; break;
            case OUTCOME_AUTHORIZED:    outcome_str = "authorized";    break;
            case OUTCOME_ALARM:         outcome_str = "alarm";         break;
            default:                    outcome_str = "expired";       break;
        }

        char safe_badge[128], safe_source[32];
        json_escape(events[i].badge_id, safe_badge, sizeof(safe_badge));
        json_escape(events[i].source,   safe_source, sizeof(safe_source));

        pos += snprintf(buf + pos, 65536 - pos,
                        "%s{\"timestamp\":\"%s\",\"type\":\"%s\","
                        "\"source\":\"%s\",\"outcome\":\"%s\","
                        "\"badge_id\":\"%s\"}",
                        i > 0 ? "," : "",
                        ts, type_str, safe_source, outcome_str, safe_badge);
    }

    pos += snprintf(buf + pos, 65536 - pos, "],\"alarms\":[");

    for (int i = 0; i < alarm_count; i++) {
        char ts[32];
        fmt_time(alarms[i].timestamp, ts, sizeof(ts));
        pos += snprintf(buf + pos, 65536 - pos,
                        "%s{\"timestamp\":\"%s\",\"notification_sent\":%s}",
                        i > 0 ? "," : "",
                        ts,
                        alarms[i].notification_sent ? "true" : "false");
    }

    pos += snprintf(buf + pos, 65536 - pos, "]}");

    send_json(conn, 200, buf);
    free(buf);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /config GET                                                */
/* ------------------------------------------------------------------ */

static int handler_config_get(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char *ttl = config_get_string("TokenExpirationSeconds", "7");
    char *aoa = config_get_string("AoaScenarioId", "1");
    char *iop = config_get_string("InputTriggerPort", "none");

    char *aa_type     = config_get_string("AlarmActionType",     "none");
    char *aa_host     = config_get_string("AlarmActionHost",     "");
    char *aa_port     = config_get_string("AlarmActionPort",     "1");
    char *aa_duration = config_get_string("AlarmActionDuration", "5");
    char *aa_user     = config_get_string("AlarmActionUser",     "");
    char *aa_pass     = config_get_string("AlarmActionPass",     "");
    char *aa_url      = config_get_string("AlarmActionUrl",      "");
    char *aa_method   = config_get_string("AlarmActionMethod",   "GET");
    char *aa_payload  = config_get_string("AlarmActionPayload",  "");
    char *aa_header   = config_get_string("AlarmActionHeader",   "");

    char safe_aoa[128], safe_iop[32];
    json_escape(aoa, safe_aoa, sizeof(safe_aoa));
    json_escape(iop, safe_iop, sizeof(safe_iop));

    char s_type[64], s_host[128], s_port[16], s_dur[16];
    char s_user[128], s_pass[128], s_url[512], s_method[16];
    char s_payload[512], s_header[256];
    json_escape(aa_type,     s_type,    sizeof(s_type));
    json_escape(aa_host,     s_host,    sizeof(s_host));
    json_escape(aa_port,     s_port,    sizeof(s_port));
    json_escape(aa_duration, s_dur,     sizeof(s_dur));
    json_escape(aa_user,     s_user,    sizeof(s_user));
    json_escape(aa_pass,     s_pass,    sizeof(s_pass));
    json_escape(aa_url,      s_url,     sizeof(s_url));
    json_escape(aa_method,   s_method,  sizeof(s_method));
    json_escape(aa_payload,  s_payload, sizeof(s_payload));
    json_escape(aa_header,   s_header,  sizeof(s_header));

    char resp[4096];
    snprintf(resp, sizeof(resp),
             "{\"TokenExpirationSeconds\":%s,"
             "\"AoaScenarioId\":\"%s\","
             "\"InputTriggerPort\":\"%s\","
             "\"AlarmActionType\":\"%s\","
             "\"AlarmActionHost\":\"%s\","
             "\"AlarmActionPort\":\"%s\","
             "\"AlarmActionDuration\":\"%s\","
             "\"AlarmActionUser\":\"%s\","
             "\"AlarmActionPass\":\"%s\","
             "\"AlarmActionUrl\":\"%s\","
             "\"AlarmActionMethod\":\"%s\","
             "\"AlarmActionPayload\":\"%s\","
             "\"AlarmActionHeader\":\"%s\"}",
             ttl, safe_aoa, safe_iop,
             s_type, s_host, s_port, s_dur,
             s_user, s_pass, s_url, s_method,
             s_payload, s_header);

    free(ttl); free(aoa); free(iop);
    free(aa_type); free(aa_host); free(aa_port); free(aa_duration);
    free(aa_user); free(aa_pass); free(aa_url); free(aa_method);
    free(aa_payload); free(aa_header);

    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /config POST                                               */
/* ------------------------------------------------------------------ */

static int handler_config_post(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char *body = read_body(conn);
    if (!body) {
        send_error(conn, 400, "Missing or oversized request body");
        return 400;
    }

    /* Collect which keys were updated */
    char updated[512] = "";
    int  updated_count = 0;

#define TRY_STR(field) do { \
    char val[256]; \
    if (json_get_string(body, field, val, sizeof(val))) { \
        config_set(field, val); \
        if (updated_count > 0) strncat(updated, ",", sizeof(updated) - strlen(updated) - 1); \
        strncat(updated, "\"" field "\"", sizeof(updated) - strlen(updated) - 1); \
        updated_count++; \
    } \
} while (0)

#define TRY_INT(field) do { \
    int val; \
    if (json_get_int(body, field, &val)) { \
        char sval[32]; \
        snprintf(sval, sizeof(sval), "%d", val); \
        config_set(field, sval); \
        if (updated_count > 0) strncat(updated, ",", sizeof(updated) - strlen(updated) - 1); \
        strncat(updated, "\"" field "\"", sizeof(updated) - strlen(updated) - 1); \
        updated_count++; \
        if (strcmp(field, "TokenExpirationSeconds") == 0) \
            token_set_expiration(val); \
    } \
} while (0)

    TRY_INT("TokenExpirationSeconds");

    /* AoaScenarioId is a string — store and resubscribe without restart */
    {
        char val[256];
        if (json_get_string(body, "AoaScenarioId", val, sizeof(val))) {
            config_set("AoaScenarioId", val);
            event_subscriber_resubscribe(val);
            if (updated_count > 0)
                strncat(updated, ",", sizeof(updated) - strlen(updated) - 1);
            strncat(updated, "\"AoaScenarioId\"",
                    sizeof(updated) - strlen(updated) - 1);
            updated_count++;
        }
    }

    /* InputTriggerPort — resubscribe live */
    {
        char val[32];
        if (json_get_string(body, "InputTriggerPort", val, sizeof(val))) {
            config_set("InputTriggerPort", val);
            input_trigger_resubscribe(val);
            if (updated_count > 0)
                strncat(updated, ",", sizeof(updated) - strlen(updated) - 1);
            strncat(updated, "\"InputTriggerPort\"",
                    sizeof(updated) - strlen(updated) - 1);
            updated_count++;
        }
    }

    /* Alarm action params */
    TRY_STR("AlarmActionType");
    TRY_STR("AlarmActionHost");
    TRY_STR("AlarmActionPort");
    TRY_STR("AlarmActionDuration");
    TRY_STR("AlarmActionUser");
    TRY_STR("AlarmActionPass");
    TRY_STR("AlarmActionUrl");
    TRY_STR("AlarmActionMethod");
    TRY_STR("AlarmActionPayload");
    TRY_STR("AlarmActionHeader");

#undef TRY_STR
#undef TRY_INT

    free(body);

    char resp[640];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"updated\":[%s]}", updated);
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /config (dispatch GET/POST)                               */
/* ------------------------------------------------------------------ */

static int handler_config(struct mg_connection *conn, void *cbdata)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri)
        return 400;

    if (strcmp(ri->request_method, "GET") == 0)
        return handler_config_get(conn, cbdata);
    if (strcmp(ri->request_method, "POST") == 0)
        return handler_config_post(conn, cbdata);

    send_error(conn, 405, "Method not allowed");
    return 405;
}

/* ------------------------------------------------------------------ */
/* Handler: /test                                                      */
/* ------------------------------------------------------------------ */

static int handler_test(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const char *resp =
        "{\"status\":\"ok\",\"app\":\"antitailgate\",\"version\":\"" APP_VERSION "\"}";
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /clear-history                                             */
/* ------------------------------------------------------------------ */

static int handler_clear_history(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    history_clear();
    const char *resp = "{\"status\":\"ok\",\"message\":\"History cleared\"}";
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /reset-defaults                                            */
/* ------------------------------------------------------------------ */

static int handler_reset_defaults(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    config_set("TokenExpirationSeconds", "7");
    config_set("AoaScenarioId",          "1");
    config_set("InputTriggerPort",       "none");
    config_set("AlarmActionType",        "none");
    config_set("AlarmActionHost",        "");
    config_set("AlarmActionPort",        "1");
    config_set("AlarmActionDuration",    "5");
    config_set("AlarmActionUser",        "");
    config_set("AlarmActionPass",        "");
    config_set("AlarmActionUrl",         "");
    config_set("AlarmActionMethod",      "GET");
    config_set("AlarmActionPayload",     "");
    config_set("AlarmActionHeader",      "");
    token_set_expiration(7);
    event_subscriber_resubscribe("1");
    input_trigger_resubscribe("none");

    const char *resp = "{\"status\":\"ok\",\"message\":\"Reset to defaults\"}";
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Handler: /test-alarm-action                                         */
/* ------------------------------------------------------------------ */

static int handler_test_alarm_action(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char *type = config_get_string("AlarmActionType", "none");
    if (!type || strcmp(type, "none") == 0) {
        free(type);
        send_error(conn, 400, "No alarm action configured");
        return 400;
    }
    free(type);

    alarm_handler_notify(true);

    const char *resp = "{\"status\":\"ok\",\"message\":\"Alarm action test fired\"}";
    send_json(conn, 200, resp);
    return 200;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool web_server_init(int port)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    const char *options[] = {
        "listening_ports",    port_str,
        "num_threads",        "10",
        "request_timeout_ms", "10000",
        NULL
    };

    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = begin_request_callback;

    g_ctx = mg_start(&callbacks, NULL, options);
    if (!g_ctx) {
        syslog(LOG_ERR, "antitailgate: CivetWeb failed to start on port %d", port);
        return false;
    }

    /* Register handlers at root paths (direct port 8080 access) */
    mg_set_request_handler(g_ctx, "/badge-read",         handler_badge_read,         NULL);
    mg_set_request_handler(g_ctx, "/threshold-crossing", handler_threshold_crossing, NULL);
    mg_set_request_handler(g_ctx, "/status",             handler_status,             NULL);
    mg_set_request_handler(g_ctx, "/config",             handler_config,             NULL);
    mg_set_request_handler(g_ctx, "/test$",              handler_test,               NULL);
    mg_set_request_handler(g_ctx, "/clear-history",      handler_clear_history,      NULL);
    mg_set_request_handler(g_ctx, "/reset-defaults",       handler_reset_defaults,      NULL);
    mg_set_request_handler(g_ctx, "/test-alarm-action",  handler_test_alarm_action,   NULL);

    /* Register at full proxy path (Apache forwards /local/<appName>/api/... unchanged) */
    #define PROXY_PFX "/local/antitailgate/api"
    mg_set_request_handler(g_ctx, PROXY_PFX "/badge-read",         handler_badge_read,         NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/threshold-crossing", handler_threshold_crossing, NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/status",             handler_status,             NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/config",             handler_config,             NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/test$",              handler_test,               NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/clear-history",      handler_clear_history,      NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/reset-defaults",     handler_reset_defaults,     NULL);
    mg_set_request_handler(g_ctx, PROXY_PFX "/test-alarm-action",  handler_test_alarm_action,  NULL);

    syslog(LOG_INFO, "antitailgate: CivetWeb started on %s", port_str);
    return true;
}

void web_server_cleanup(void)
{
    if (g_ctx) {
        mg_stop(g_ctx);
        g_ctx = NULL;
    }
}
