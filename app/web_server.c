#include "web_server.h"
#include "token_manager.h"
#include "alarm_handler.h"
#include "event_publisher.h"
#include "settings.h"
#include "json_util.h"
#include "civetweb.h"

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define APP_VERSION       "1.0.0"
#define ADMIN_PROXY_PFX   "/local/antitailgate/admin"
#define INGEST_PROXY_PFX  "/local/antitailgate/ingest"
#define CSRF_HEADER_NAME  "X-Requested-With"
#define CSRF_HEADER_VALUE "antitailgate"
#define MAX_BODY_BYTES    65536

static struct mg_context *g_ctx = NULL;

static const char *http_reason(int code)
{
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        default:  return (code >= 200 && code < 300) ? "OK" : "Error";
    }
}

static void send_json(struct mg_connection *conn, int code, const char *json)
{
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "Cache-Control: no-store, no-cache, must-revalidate, private\r\n"
              "Pragma: no-cache\r\n"
              "X-Content-Type-Options: nosniff\r\n"
              "X-Frame-Options: DENY\r\n"
              "Connection: close\r\n"
              "\r\n",
              code, http_reason(code), (int)strlen(json));
    mg_write(conn, json, strlen(json));
}

static void send_error(struct mg_connection *conn, int code, const char *message)
{
    GString *b = g_string_new("{\"status\":\"error\",\"message\":\"");
    json_escape_append(b, message ? message : "error");
    g_string_append(b, "\"}");
    send_json(conn, code, b->str);
    g_string_free(b, TRUE);
}

static void audit_log(struct mg_connection *conn, const char *action)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *xff  = mg_get_header(conn, "X-Forwarded-For");
    syslog(LOG_NOTICE, "antitailgate: audit action=%s remote=%s xff=%s",
           action,
           (ri && ri->remote_addr[0]) ? ri->remote_addr : "?",
           xff ? xff : "-");
}

/* Reject a request whose decoded path differs from its cleaned path, i.e.
 * one that used %2e%2e / backslash / encoded-slash tricks to reach a
 * handler Apache would not have authorized for this access level.  Apache
 * authorizes on the raw proxy path; CivetWeb dispatches on the cleaned
 * path; this makes the two agree. */
static int begin_request_callback(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri)
        return 0;

    if (strcmp(ri->request_method, "OPTIONS") == 0) {
        /* No CORS allow-headers: cross-origin preflights fail by omission. */
        mg_printf(conn, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
        return 1;
    }

    const char *raw = ri->local_uri_raw ? ri->local_uri_raw : "";
    const char *clean = ri->local_uri ? ri->local_uri : "";
    bool bad = (strcmp(raw, clean) != 0);
    /* A backslash in the request target is never legitimate here and is the
     * classic separator-confusion vector. */
    if (ri->request_uri && strchr(ri->request_uri, '\\'))
        bad = true;
    if (bad) {
        syslog(LOG_WARNING, "antitailgate: rejected non-canonical path '%s'",
               ri->request_uri ? ri->request_uri : "?");
        send_error(conn, 400, "Bad request path");
        return 1;
    }
    return 0;
}

/* POST + application/json (+ optional CSRF header for admin). */
static bool require_mutating_request(struct mg_connection *conn, bool require_xrw)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri) { send_error(conn, 400, "Bad request"); return false; }
    if (strcmp(ri->request_method, "POST") != 0) {
        send_error(conn, 405, "Method not allowed; POST required");
        return false;
    }
    const char *ct = mg_get_header(conn, "Content-Type");
    if (!ct || strncasecmp(ct, "application/json", 16) != 0) {
        send_error(conn, 415, "Unsupported Media Type; application/json required");
        return false;
    }
    if (require_xrw) {
        const char *xrw = mg_get_header(conn, CSRF_HEADER_NAME);
        if (!xrw || strcmp(xrw, CSRF_HEADER_VALUE) != 0) {
            send_error(conn, 403, "Missing or invalid " CSRF_HEADER_NAME " header");
            return false;
        }
    }
    return true;
}

typedef enum {
    BODY_OK, BODY_NONE, BODY_TOO_LARGE, BODY_LEN_REQUIRED, BODY_READ_ERROR
} BodyStatus;

/* Read the request body into *out (NUL-terminated, caller frees).  Returns
 * a status so handlers can distinguish empty / too-large / chunked. */
static BodyStatus read_body(struct mg_connection *conn, char **out, size_t *out_len)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    *out = NULL;
    *out_len = 0;
    if (!ri)
        return BODY_READ_ERROR;
    if (ri->content_length == 0)
        return BODY_NONE;
    if (ri->content_length > MAX_BODY_BYTES)
        return BODY_TOO_LARGE;

    /* Read the (de-chunked) body up to the cap.  content_length may be -1
     * for a chunked or length-less request; mg_read still returns the
     * decoded bytes, so we grow until EOF rather than trusting the header. */
    size_t cap = (ri->content_length > 0) ? (size_t)ri->content_length : 4096;
    size_t got = 0;
    char *buf = malloc(cap + 1);
    if (!buf)
        return BODY_READ_ERROR;
    for (;;) {
        if (got == cap) {
            if (cap >= MAX_BODY_BYTES) { free(buf); return BODY_TOO_LARGE; }
            cap = cap * 2 > MAX_BODY_BYTES ? MAX_BODY_BYTES : cap * 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); return BODY_READ_ERROR; }
            buf = nb;
        }
        int n = mg_read(conn, buf + got, cap - got);
        if (n < 0) { free(buf); return BODY_READ_ERROR; }
        if (n == 0) break;
        got += (size_t)n;
    }
    if (got == 0) { free(buf); return BODY_NONE; }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return BODY_OK;
}

static bool send_body_error(struct mg_connection *conn, BodyStatus st)
{
    switch (st) {
        case BODY_TOO_LARGE:    send_error(conn, 413, "Request body too large"); return true;
        case BODY_LEN_REQUIRED: send_error(conn, 411, "Length required (chunked not supported)"); return true;
        case BODY_READ_ERROR:   send_error(conn, 400, "Could not read request body"); return true;
        default: return false;
    }
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static int handler_badge_read(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri || strcmp(ri->request_method, "POST") != 0) {
        send_error(conn, 405, "Method not allowed; POST required");
        return 405;
    }

    char badge_id[64] = "";
    char source[64]   = "http";
    char err[256]     = "";

    char *body = NULL;
    size_t blen = 0;
    BodyStatus st = read_body(conn, &body, &blen);
    if (send_body_error(conn, st))
        return 400;

    if (st == BODY_OK) {
        /* A body must be JSON; a bodyless trigger is fine without a type. */
        const char *ct = mg_get_header(conn, "Content-Type");
        if (!ct || strncasecmp(ct, "application/json", 16) != 0) {
            free(body);
            send_error(conn, 415, "Unsupported Media Type; application/json required");
            return 415;
        }
        JsonObject *o = json_object_parse(body, blen, err, sizeof err);
        free(body);
        if (!o) { send_error(conn, 400, err); return 400; }

        const char *allowed[] = { "door", "card", "badge_id", NULL };
        if (!json_object_only_keys(o, allowed, err, sizeof err)) {
            json_object_free(o);
            send_error(conn, 400, err);
            return 400;
        }
        const char *v = NULL;
        int r = json_get_scalar_str(o, "door", &v, err, sizeof err);
        if (r < 0) { json_object_free(o); send_error(conn, 400, err); return 400; }
        if (r == 1 && v[0]) g_strlcpy(source, v, sizeof source);

        /* Accept "card" or legacy "badge_id". */
        r = json_get_scalar_str(o, "card", &v, err, sizeof err);
        if (r == 0) r = json_get_scalar_str(o, "badge_id", &v, err, sizeof err);
        if (r < 0) { json_object_free(o); send_error(conn, 400, err); return 400; }
        if (r == 1 && v[0]) g_strlcpy(badge_id, v, sizeof badge_id);
        json_object_free(o);
    }

    if (!badge_id[0])
        g_strlcpy(badge_id, "unknown", sizeof badge_id);

    int count = token_add(badge_id, source);

    GString *b = g_string_new("{\"status\":\"ok\",\"message\":\"Token created\",\"token_count\":");
    g_string_append_printf(b, "%d,\"badge_id\":\"", count);
    json_escape_append(b, badge_id);
    g_string_append(b, "\",\"source\":\"");
    json_escape_append(b, source);
    g_string_append(b, "\"}");
    send_json(conn, 200, b->str);
    g_string_free(b, TRUE);
    return 200;
}

static int handler_threshold_crossing(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    if (!require_mutating_request(conn, true))
        return 400;
    audit_log(conn, "threshold-crossing");

    bool authorized = token_consume("http");
    if (!authorized)
        alarm_handler_notify(false);

    GString *b = g_string_new(NULL);
    g_string_append_printf(b,
        "{\"status\":\"ok\",\"outcome\":\"%s\",\"tokens_remaining\":%d}",
        authorized ? "authorized" : "alarm", token_count());
    send_json(conn, 200, b->str);
    g_string_free(b, TRUE);
    return 200;
}

static int handler_status(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    HistoryEvent events[MAX_HISTORY];
    AlarmRecord  alarms[MAX_HISTORY];
    int alarm_count = 0;
    int ev_count = history_snapshot(events, MAX_HISTORY, alarms, MAX_HISTORY, &alarm_count);
    TokenStats stats;
    token_get_stats(&stats);

    GString *buf = g_string_new(NULL);
    g_string_append_printf(buf,
        "{\"token_count\":%d,"
        "\"stats\":{\"badge_reads\":%" G_GUINT64_FORMAT
        ",\"authorized\":%" G_GUINT64_FORMAT
        ",\"alarms\":%" G_GUINT64_FORMAT "},\"events\":[",
        stats.token_count, stats.badge_reads, stats.authorized, stats.alarms);

    for (int i = 0; i < ev_count; i++) {
        char ts[32];
        const char *type_str =
            events[i].type == EVENT_BADGE_READ ? "badge_read" : "line_crossing";
        const char *oc;
        switch (events[i].outcome) {
            case OUTCOME_TOKEN_CREATED: oc = "token_created"; break;
            case OUTCOME_AUTHORIZED:    oc = "authorized";    break;
            case OUTCOME_ALARM:         oc = "alarm";         break;
            case OUTCOME_EXPIRED:       oc = "expired";       break;
            default:                    oc = "unknown";       break;
        }
        struct tm tm_val;
        gmtime_r(&events[i].timestamp, &tm_val);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_val);

        g_string_append_printf(buf, "%s{\"timestamp\":\"%s\",\"type\":\"%s\",\"source\":\"",
                               i ? "," : "", ts, type_str);
        json_escape_append(buf, events[i].source);
        g_string_append(buf, "\",\"outcome\":\"");
        g_string_append(buf, oc);
        g_string_append(buf, "\",\"badge_id\":\"");
        json_escape_append(buf, events[i].badge_id);
        g_string_append(buf, "\"}");
    }
    g_string_append(buf, "],\"alarms\":[");
    for (int i = 0; i < alarm_count; i++) {
        char ts[32];
        struct tm tm_val;
        gmtime_r(&alarms[i].timestamp, &tm_val);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_val);
        g_string_append_printf(buf, "%s{\"timestamp\":\"%s\",\"action_status\":\"", i ? "," : "", ts);
        json_escape_append(buf, alarms[i].action_status);
        g_string_append(buf, "\"}");
    }
    g_string_append(buf, "]}");
    send_json(conn, 200, buf->str);
    g_string_free(buf, TRUE);
    return 200;
}

static int handler_config_get(struct mg_connection *conn)
{
    GString *b = g_string_new(NULL);
    settings_to_json(b);
    send_json(conn, 200, b->str);
    g_string_free(b, TRUE);
    return 200;
}

static int handler_config_post(struct mg_connection *conn)
{
    if (!require_mutating_request(conn, true))
        return 400;
    audit_log(conn, "config-post");

    char *body = NULL;
    size_t blen = 0;
    BodyStatus st = read_body(conn, &body, &blen);
    if (send_body_error(conn, st))
        return 400;
    if (st == BODY_NONE) { send_error(conn, 400, "Empty request body"); return 400; }

    char err[256] = "";
    JsonObject *o = json_object_parse(body, blen, err, sizeof err);
    free(body);
    if (!o) { send_error(conn, 400, err); return 400; }

    GString *updated = g_string_new(NULL);
    bool ok = settings_apply(o, updated, err, sizeof err);
    json_object_free(o);

    if (!ok) {
        g_string_free(updated, TRUE);
        send_error(conn, 400, err);
        return 400;
    }
    GString *resp = g_string_new("{\"status\":\"ok\",\"updated\":[");
    g_string_append(resp, updated->str);
    g_string_append(resp, "]}");
    send_json(conn, 200, resp->str);
    g_string_free(resp, TRUE);
    g_string_free(updated, TRUE);
    return 200;
}

static int handler_config(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri) return 400;
    if (strcmp(ri->request_method, "GET") == 0)
        return handler_config_get(conn);
    if (strcmp(ri->request_method, "POST") == 0)
        return handler_config_post(conn);
    send_error(conn, 405, "Method not allowed");
    return 405;
}

static int handler_test(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    send_json(conn, 200,
              "{\"status\":\"ok\",\"app\":\"antitailgate\",\"version\":\"" APP_VERSION "\"}");
    return 200;
}

static int handler_clear_history(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    if (!require_mutating_request(conn, true))
        return 400;
    audit_log(conn, "clear-history");
    history_clear();
    token_clear_all();
    event_publisher_send_alarm(false);
    send_json(conn, 200, "{\"status\":\"ok\",\"message\":\"History and tokens cleared\"}");
    return 200;
}

static int handler_reset_defaults(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    if (!require_mutating_request(conn, true))
        return 400;
    audit_log(conn, "reset-defaults");
    settings_reset_defaults();
    token_set_expiration(7);
    token_clear_all();
    history_clear();
    event_publisher_send_alarm(false);
    send_json(conn, 200, "{\"status\":\"ok\",\"message\":\"Reset to defaults\"}");
    return 200;
}

static int handler_test_alarm_action(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    if (!require_mutating_request(conn, true))
        return 400;
    audit_log(conn, "test-alarm-action");

    char status[160] = "";
    bool ok = alarm_handler_test(status, sizeof status);

    GString *b = g_string_new(NULL);
    g_string_append_printf(b, "{\"status\":\"%s\",\"result\":\"", ok ? "ok" : "error");
    json_escape_append(b, status);
    g_string_append(b, "\"}");
    send_json(conn, ok ? 200 : 502, b->str);
    g_string_free(b, TRUE);
    return ok ? 200 : 502;
}

static void register_handler(const char *path, mg_request_handler h)
{
    mg_set_request_handler(g_ctx, path, h, NULL);
}

bool web_server_init(int port)
{
    (void)port;
    const char *options[] = {
        "listening_ports",       "127.0.0.1:8080",
        "num_threads",           "30",
        "request_timeout_ms",    "3000",
        "keep_alive_timeout_ms", "200",
        "tcp_nodelay",           "1",
        NULL
    };
    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof callbacks);
    callbacks.begin_request = begin_request_callback;

    g_ctx = mg_start(&callbacks, NULL, options);
    if (!g_ctx) {
        syslog(LOG_ERR, "antitailgate: CivetWeb failed to start on 127.0.0.1:8080");
        return false;
    }

    /* Only the proxy-prefixed paths are registered; the Axis reverse proxy
     * targets exactly these, and begin_request_callback rejects any path
     * that normalized to a different shape. */
    register_handler(INGEST_PROXY_PFX "/badge-read",         handler_badge_read);
    register_handler(ADMIN_PROXY_PFX  "/test",               handler_test);
    register_handler(ADMIN_PROXY_PFX  "/status",             handler_status);
    register_handler(ADMIN_PROXY_PFX  "/config",             handler_config);
    register_handler(ADMIN_PROXY_PFX  "/threshold-crossing", handler_threshold_crossing);
    register_handler(ADMIN_PROXY_PFX  "/clear-history",      handler_clear_history);
    register_handler(ADMIN_PROXY_PFX  "/reset-defaults",     handler_reset_defaults);
    register_handler(ADMIN_PROXY_PFX  "/test-alarm-action",  handler_test_alarm_action);

    syslog(LOG_INFO, "antitailgate: CivetWeb started on 127.0.0.1:8080");
    return true;
}

void web_server_cleanup(void)
{
    if (g_ctx) {
        mg_stop(g_ctx);
        g_ctx = NULL;
    }
}
