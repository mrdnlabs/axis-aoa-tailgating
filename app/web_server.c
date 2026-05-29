#include "web_server.h"
#include "token_manager.h"
#include "alarm_handler.h"
#include "event_subscriber.h"
#include "input_trigger.h"
#include "config.h"
#include "civetweb.h"

#include <glib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define APP_VERSION      "1.0.0"
#define ADMIN_PREFIX     "/admin"
#define INGEST_PREFIX    "/ingest"
#define ADMIN_PROXY_PFX  "/local/antitailgate/admin"
#define INGEST_PROXY_PFX "/local/antitailgate/ingest"

static struct mg_context *g_ctx = NULL;

static const char *http_reason(int status_code)
{
    switch (status_code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

static void send_json(struct mg_connection *conn,
                      int status_code, const char *json)
{
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "Connection: close\r\n"
              "\r\n",
              status_code, http_reason(status_code), (int)strlen(json));
    mg_write(conn, json, strlen(json));
}

static void send_error(struct mg_connection *conn,
                       int status_code, const char *message)
{
    char safe_message[256];
    size_t j = 0;

    for (size_t i = 0; message && message[i] && j + 2 < sizeof(safe_message); i++) {
        if (message[i] == '"' || message[i] == '\\')
            safe_message[j++] = '\\';
        safe_message[j++] = message[i];
    }
    safe_message[j] = '\0';

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error\",\"message\":\"%s\"}", safe_message);
    send_json(conn, status_code, buf);
}

static int begin_request_callback(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (ri && strcmp(ri->request_method, "OPTIONS") == 0) {
        mg_printf(conn,
                  "HTTP/1.1 204 No Content\r\n"
                  "Content-Length: 0\r\n"
                  "Connection: close\r\n"
                  "\r\n");
        return 1;
    }
    return 0;
}

static void fmt_time(time_t t, char *out, size_t out_len)
{
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_val);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!in || out_len == 0) {
        if (out_len > 0)
            out[0] = '\0';
        return;
    }

    for (size_t i = 0; in[i] && j + 2 < out_len; i++) {
        if (in[i] == '"' || in[i] == '\\')
            out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = '\0';
}

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

static const char *skip_ws(const char *p)
{
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static bool parse_json_string(const char *p, char *out, size_t out_len,
                              const char **out_next,
                              char *err, size_t err_len)
{
    size_t j = 0;

    if (!p || *p != '"') {
        snprintf(err, err_len, "Expected JSON string");
        return false;
    }
    p++;

    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\') {
            if (!*p) {
                snprintf(err, err_len, "Invalid JSON escape");
                return false;
            }
            switch (*p) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default:
                    snprintf(err, err_len, "Unsupported JSON escape");
                    return false;
            }
            p++;
        }
        if (j + 1 >= out_len) {
            snprintf(err, err_len, "JSON string too long");
            return false;
        }
        out[j++] = ch;
    }

    if (*p != '"') {
        snprintf(err, err_len, "Unterminated JSON string");
        return false;
    }
    out[j] = '\0';
    *out_next = p + 1;
    return true;
}

static bool skip_json_value(const char *p, const char **out_next,
                            char *err, size_t err_len)
{
    p = skip_ws(p);
    if (!p || !*p) {
        snprintf(err, err_len, "Expected JSON value");
        return false;
    }

    if (*p == '"') {
        char scratch[1024];
        return parse_json_string(p, scratch, sizeof(scratch), out_next, err, err_len);
    }

    if (*p == '-' || isdigit((unsigned char)*p)) {
        p++;
        while (*p && isdigit((unsigned char)*p))
            p++;
        *out_next = p;
        return true;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out_next = p + 4;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_next = p + 5;
        return true;
    }
    if (strncmp(p, "null", 4) == 0) {
        *out_next = p + 4;
        return true;
    }

    snprintf(err, err_len, "Unsupported JSON value");
    return false;
}

static bool validate_flat_json_object(const char *json,
                                      char *err, size_t err_len)
{
    const char *p = skip_ws(json);
    char key[128];

    if (!p || *p != '{') {
        snprintf(err, err_len, "JSON body must be an object");
        return false;
    }
    p = skip_ws(p + 1);
    if (*p == '}')
        return true;

    while (*p) {
        if (!parse_json_string(p, key, sizeof(key), &p, err, err_len))
            return false;
        p = skip_ws(p);
        if (*p != ':') {
            snprintf(err, err_len, "Expected ':' after key");
            return false;
        }
        p = skip_ws(p + 1);
        if (!skip_json_value(p, &p, err, err_len))
            return false;
        p = skip_ws(p);
        if (*p == '}')
            return true;
        if (*p != ',') {
            snprintf(err, err_len, "Expected ',' between fields");
            return false;
        }
        p = skip_ws(p + 1);
    }

    snprintf(err, err_len, "Unterminated JSON object");
    return false;
}

static bool json_find_value(const char *json, const char *target_key,
                            const char **value_start,
                            char *err, size_t err_len)
{
    const char *p = skip_ws(json);
    char key[128];

    if (!p || *p != '{') {
        snprintf(err, err_len, "JSON body must be an object");
        return false;
    }
    p = skip_ws(p + 1);
    if (*p == '}')
        return false;

    while (*p) {
        if (!parse_json_string(p, key, sizeof(key), &p, err, err_len))
            return false;
        p = skip_ws(p);
        if (*p != ':') {
            snprintf(err, err_len, "Expected ':' after key");
            return false;
        }
        p = skip_ws(p + 1);
        if (strcmp(key, target_key) == 0) {
            *value_start = p;
            return true;
        }
        if (!skip_json_value(p, &p, err, err_len))
            return false;
        p = skip_ws(p);
        if (*p == '}')
            return false;
        if (*p != ',') {
            snprintf(err, err_len, "Expected ',' between fields");
            return false;
        }
        p = skip_ws(p + 1);
    }

    snprintf(err, err_len, "Unterminated JSON object");
    return false;
}

static bool json_get_string(const char *json, const char *key,
                            char *out, size_t out_len,
                            bool *present, char *err, size_t err_len)
{
    const char *value = NULL;
    *present = false;
    err[0] = '\0';

    if (!json_find_value(json, key, &value, err, err_len))
        return (err[0] == '\0');

    *present = true;
    return parse_json_string(skip_ws(value), out, out_len, &value, err, err_len);
}

static bool json_get_int(const char *json, const char *key,
                         int *out, bool *present,
                         char *err, size_t err_len)
{
    const char *value = NULL;
    char *end = NULL;
    long parsed;

    *present = false;
    err[0] = '\0';
    if (!json_find_value(json, key, &value, err, err_len))
        return (err[0] == '\0');

    value = skip_ws(value);
    if (!value || (!isdigit((unsigned char)*value) && *value != '-')) {
        snprintf(err, err_len, "Field '%s' must be an integer", key);
        return false;
    }

    parsed = strtol(value, &end, 10);
    if (!end || end == value) {
        snprintf(err, err_len, "Field '%s' must be an integer", key);
        return false;
    }
    end = (char *)skip_ws(end);
    if (*end != '\0' && *end != ',' && *end != '}') {
        snprintf(err, err_len, "Field '%s' must be an integer", key);
        return false;
    }

    *out = (int)parsed;
    *present = true;
    return true;
}

static bool json_get_bool(const char *json, const char *key,
                          bool *out, bool *present,
                          char *err, size_t err_len)
{
    const char *value = NULL;
    *present = false;
    err[0] = '\0';

    if (!json_find_value(json, key, &value, err, err_len))
        return (err[0] == '\0');

    value = skip_ws(value);
    if (strncmp(value, "true", 4) == 0 &&
        (*skip_ws(value + 4) == '\0' || *skip_ws(value + 4) == ',' || *skip_ws(value + 4) == '}')) {
        *out = true;
        *present = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0 &&
        (*skip_ws(value + 5) == '\0' || *skip_ws(value + 5) == ',' || *skip_ws(value + 5) == '}')) {
        *out = false;
        *present = true;
        return true;
    }

    snprintf(err, err_len, "Field '%s' must be a boolean", key);
    return false;
}

static bool validate_string(const char *value, size_t max_len,
                            const char *field, char *err, size_t err_len)
{
    if (!value)
        return true;
    if (strlen(value) > max_len) {
        snprintf(err, err_len, "Field '%s' exceeds %zu characters",
                 field, max_len);
        return false;
    }
    return true;
}

static bool validate_int_range(int value, int min_val, int max_val,
                               const char *field, char *err, size_t err_len)
{
    if (value < min_val || value > max_val) {
        snprintf(err, err_len, "Field '%s' must be between %d and %d",
                 field, min_val, max_val);
        return false;
    }
    return true;
}

/* Allow empty (= "unset"), or a non-empty numeric string within [min,max]. */
static bool validate_numeric_str(const char *value, int min_val, int max_val,
                                 const char *field, char *err, size_t err_len)
{
    if (!value || !value[0])
        return true;
    for (size_t i = 0; value[i]; i++) {
        if (!isdigit((unsigned char)value[i])) {
            snprintf(err, err_len, "Field '%s' must be numeric", field);
            return false;
        }
    }
    long v = strtol(value, NULL, 10);
    if (v < min_val || v > max_val) {
        snprintf(err, err_len, "Field '%s' must be between %d and %d",
                 field, min_val, max_val);
        return false;
    }
    return true;
}

/* Allow empty, or a hostname/IP: [A-Za-z0-9.-_] only. No URLs, no schemes. */
static bool validate_host(const char *value, const char *field,
                          char *err, size_t err_len)
{
    if (!value || !value[0])
        return true;
    for (size_t i = 0; value[i]; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) {
            snprintf(err, err_len,
                     "Field '%s' must be a hostname or IP "
                     "(letters, digits, '.', '-', '_')", field);
            return false;
        }
    }
    return true;
}

static bool config_set_checked(const char *name, const char *value,
                               GString *updated, GString *errors)
{
    if (!config_set(name, value)) {
        g_string_append_printf(errors, "%s\"%s persistence failed\"",
                               errors->len > 0 ? "," : "", name);
        return false;
    }

    g_string_append_printf(updated, "%s\"%s\"",
                           updated->len > 0 ? "," : "", name);
    return true;
}

static int handler_badge_read(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char badge_id[64] = "";
    char source[64]   = "http";
    char err[256] = "";
    char door[64];
    char card[64];
    bool present = false;
    char *body = NULL;

    get_query_param(conn, "badge_id", badge_id, sizeof(badge_id));

    body = read_body(conn);
    if (body) {
        if (!validate_flat_json_object(body, err, sizeof(err))) {
            free(body);
            send_error(conn, 400, err);
            return 400;
        }

        if (!json_get_string(body, "door", door, sizeof(door), &present, err, sizeof(err))) {
            free(body);
            send_error(conn, 400, err);
            return 400;
        }
        if (present) {
            if (!validate_string(door, 63, "door", err, sizeof(err))) {
                free(body);
                send_error(conn, 400, err);
                return 400;
            }
            if (door[0])
                snprintf(source, sizeof(source), "%s", door);
        }

        if (!json_get_string(body, "card", card, sizeof(card), &present, err, sizeof(err))) {
            free(body);
            send_error(conn, 400, err);
            return 400;
        }
        if (present) {
            if (!validate_string(card, 63, "card", err, sizeof(err))) {
                free(body);
                send_error(conn, 400, err);
                return 400;
            }
            if (card[0])
                snprintf(badge_id, sizeof(badge_id), "%s", card);
        }
        free(body);
    }

    if (!badge_id[0])
        strncpy(badge_id, "unknown", sizeof(badge_id) - 1);

    int count = token_add(badge_id, source);

    char resp[384];
    char safe_badge[128];
    char safe_source[128];
    json_escape(badge_id, safe_badge, sizeof(safe_badge));
    json_escape(source, safe_source, sizeof(safe_source));
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"message\":\"Token created\","
             "\"token_count\":%d,\"badge_id\":\"%s\",\"source\":\"%s\"}",
             count, safe_badge, safe_source);
    send_json(conn, 200, resp);
    return 200;
}

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
             authorized ? "authorized" : "alarm", remaining);
    send_json(conn, 200, resp);
    return 200;
}

static int handler_status(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    HistoryEvent events[MAX_HISTORY];
    AlarmRecord alarms[MAX_HISTORY];
    int alarm_count = 0;
    int ev_count = history_snapshot(events, MAX_HISTORY,
                                    alarms, MAX_HISTORY, &alarm_count);
    int tc = token_count();

    GString *buf = g_string_new(NULL);
    g_string_append_printf(buf, "{\"token_count\":%d,\"events\":[", tc);

    for (int i = 0; i < ev_count; i++) {
        char ts[32];
        char safe_badge[128];
        char safe_source[128];
        const char *type_str =
            events[i].type == EVENT_BADGE_READ ? "badge_read" : "line_crossing";
        const char *outcome_str;

        switch (events[i].outcome) {
            case OUTCOME_TOKEN_CREATED: outcome_str = "token_created"; break;
            case OUTCOME_AUTHORIZED:    outcome_str = "authorized";    break;
            case OUTCOME_ALARM:         outcome_str = "alarm";         break;
            case OUTCOME_EXPIRED:       outcome_str = "expired";       break;
            default:                    outcome_str = "unknown";       break;
        }

        fmt_time(events[i].timestamp, ts, sizeof(ts));
        json_escape(events[i].badge_id, safe_badge, sizeof(safe_badge));
        json_escape(events[i].source, safe_source, sizeof(safe_source));

        g_string_append_printf(buf,
                               "%s{\"timestamp\":\"%s\",\"type\":\"%s\","
                               "\"source\":\"%s\",\"outcome\":\"%s\","
                               "\"badge_id\":\"%s\"}",
                               i > 0 ? "," : "",
                               ts, type_str, safe_source, outcome_str, safe_badge);
    }

    g_string_append(buf, "],\"alarms\":[");

    for (int i = 0; i < alarm_count; i++) {
        char ts[32];
        char safe_status[64];
        fmt_time(alarms[i].timestamp, ts, sizeof(ts));
        json_escape(alarms[i].action_status, safe_status, sizeof(safe_status));
        g_string_append_printf(buf,
                               "%s{\"timestamp\":\"%s\",\"action_status\":\"%s\"}",
                               i > 0 ? "," : "",
                               ts, safe_status);
    }

    g_string_append(buf, "]}");
    send_json(conn, 200, buf->str);
    g_string_free(buf, TRUE);
    return 200;
}

static int handler_config_get(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char *ttl = config_get_string("TokenExpirationSeconds", "7");
    char *aoa = config_get_string("AoaScenarioId", "1");
    char *iop = config_get_string("InputTriggerPort", "none");
    char *clear_seconds = config_get_string("AlarmClearSeconds", "2");
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
    char s_type[64], s_host[128], s_port[16], s_dur[16], s_clear[16];
    char s_user[128], s_url[512], s_method[16], s_payload[512], s_header[256];

    json_escape(aoa, safe_aoa, sizeof(safe_aoa));
    json_escape(iop, safe_iop, sizeof(safe_iop));
    json_escape(clear_seconds, s_clear, sizeof(s_clear));
    json_escape(aa_type, s_type, sizeof(s_type));
    json_escape(aa_host, s_host, sizeof(s_host));
    json_escape(aa_port, s_port, sizeof(s_port));
    json_escape(aa_duration, s_dur, sizeof(s_dur));
    json_escape(aa_user, s_user, sizeof(s_user));
    json_escape(aa_url, s_url, sizeof(s_url));
    json_escape(aa_method, s_method, sizeof(s_method));
    json_escape(aa_payload, s_payload, sizeof(s_payload));
    json_escape(aa_header, s_header, sizeof(s_header));

    char resp[4096];
    snprintf(resp, sizeof(resp),
             "{\"TokenExpirationSeconds\":%s,"
             "\"AoaScenarioId\":\"%s\","
             "\"InputTriggerPort\":\"%s\","
             "\"AlarmClearSeconds\":%s,"
             "\"AlarmActionType\":\"%s\","
             "\"AlarmActionHost\":\"%s\","
             "\"AlarmActionPort\":\"%s\","
             "\"AlarmActionDuration\":\"%s\","
             "\"AlarmActionUser\":\"%s\","
             "\"AlarmActionPassConfigured\":%s,"
             "\"AlarmActionUrl\":\"%s\","
             "\"AlarmActionMethod\":\"%s\","
             "\"AlarmActionPayload\":\"%s\","
             "\"AlarmActionHeader\":\"%s\"}",
             ttl, safe_aoa, safe_iop, s_clear,
             s_type, s_host, s_port, s_dur, s_user,
             (aa_pass && aa_pass[0]) ? "true" : "false",
             s_url, s_method, s_payload, s_header);

    free(ttl); free(aoa); free(iop); free(clear_seconds);
    free(aa_type); free(aa_host); free(aa_port); free(aa_duration);
    free(aa_user); free(aa_pass); free(aa_url); free(aa_method);
    free(aa_payload); free(aa_header);

    send_json(conn, 200, resp);
    return 200;
}

static int handler_config_post(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    char *body = read_body(conn);
    char err[256] = "";
    GString *updated = g_string_new(NULL);
    GString *errors = g_string_new(NULL);
    int status_code = 200;

    if (!body) {
        g_string_free(updated, TRUE);
        g_string_free(errors, TRUE);
        send_error(conn, 400, "Missing or oversized request body");
        return 400;
    }

    if (!validate_flat_json_object(body, err, sizeof(err))) {
        free(body);
        g_string_free(updated, TRUE);
        g_string_free(errors, TRUE);
        send_error(conn, 400, err);
        return 400;
    }

    {
        int ttl = 0;
        bool present = false;
        if (!json_get_int(body, "TokenExpirationSeconds", &ttl, &present, err, sizeof(err)) ||
            (present && !validate_int_range(ttl, 1, 3600, "TokenExpirationSeconds", err, sizeof(err)))) {
            status_code = 400;
            goto finish;
        }
        if (present) {
            char value[32];
            snprintf(value, sizeof(value), "%d", ttl);
            if (config_set_checked("TokenExpirationSeconds", value, updated, errors))
                token_set_expiration(ttl);
        }
    }

    {
        int clear_seconds = 0;
        bool present = false;
        if (!json_get_int(body, "AlarmClearSeconds", &clear_seconds, &present, err, sizeof(err)) ||
            (present && !validate_int_range(clear_seconds, 1, 60, "AlarmClearSeconds", err, sizeof(err)))) {
            status_code = 400;
            goto finish;
        }
        if (present) {
            char value[32];
            snprintf(value, sizeof(value), "%d", clear_seconds);
            config_set_checked("AlarmClearSeconds", value, updated, errors);
        }
    }

    {
        char value[256];
        bool present = false;
        if (!json_get_string(body, "AoaScenarioId", value, sizeof(value), &present, err, sizeof(err)) ||
            (present && (!validate_string(value, 255, "AoaScenarioId", err, sizeof(err)) || value[0] == '\0'))) {
            if (present && value[0] == '\0')
                snprintf(err, sizeof(err), "Field 'AoaScenarioId' must not be empty");
            status_code = 400;
            goto finish;
        }
        if (present) {
            if (config_set("AoaScenarioId", value) && event_subscriber_resubscribe(value)) {
                g_string_append_printf(updated, "%s\"AoaScenarioId\"",
                                       updated->len > 0 ? "," : "");
            } else {
                g_string_append(errors, errors->len > 0 ? ",\"AoaScenarioId apply failed\"" :
                                                    "\"AoaScenarioId apply failed\"");
            }
        }
    }

    {
        char value[32];
        bool present = false;
        if (!json_get_string(body, "InputTriggerPort", value, sizeof(value), &present, err, sizeof(err)) ||
            (present && !validate_string(value, 15, "InputTriggerPort", err, sizeof(err)))) {
            status_code = 400;
            goto finish;
        }
        if (present) {
            if (config_set("InputTriggerPort", value) && input_trigger_resubscribe(value)) {
                g_string_append_printf(updated, "%s\"InputTriggerPort\"",
                                       updated->len > 0 ? "," : "");
            } else {
                g_string_append(errors, errors->len > 0 ? ",\"InputTriggerPort apply failed\"" :
                                                    "\"InputTriggerPort apply failed\"");
            }
        }
    }

    {
        const struct {
            const char *name;
            size_t max_len;
        } fields[] = {
            { "AlarmActionType", 31 },
            { "AlarmActionHost", 127 },
            { "AlarmActionPort", 15 },
            { "AlarmActionDuration", 15 },
            { "AlarmActionUser", 127 },
            { "AlarmActionPass", 127 },
            { "AlarmActionUrl", 1023 },
            { "AlarmActionMethod", 15 },
            { "AlarmActionPayload", 1023 },
            { "AlarmActionHeader", 255 }
        };

        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            char value[1024];
            bool present = false;
            if (!json_get_string(body, fields[i].name, value, sizeof(value),
                                 &present, err, sizeof(err)) ||
                (present && !validate_string(value, fields[i].max_len,
                                             fields[i].name, err, sizeof(err)))) {
                status_code = 400;
                goto finish;
            }
            if (present) {
                /* Field-specific shape checks beyond length. */
                if (strcmp(fields[i].name, "AlarmActionHost") == 0 &&
                    !validate_host(value, "AlarmActionHost", err, sizeof(err))) {
                    status_code = 400;
                    goto finish;
                }
                if (strcmp(fields[i].name, "AlarmActionPort") == 0 &&
                    !validate_numeric_str(value, 1, 65535,
                                          "AlarmActionPort", err, sizeof(err))) {
                    status_code = 400;
                    goto finish;
                }
                if (strcmp(fields[i].name, "AlarmActionDuration") == 0 &&
                    !validate_numeric_str(value, 1, 3600,
                                          "AlarmActionDuration", err, sizeof(err))) {
                    status_code = 400;
                    goto finish;
                }
                config_set_checked(fields[i].name, value, updated, errors);
            }
        }
    }

    {
        bool clear_pass = false;
        bool present = false;
        if (!json_get_bool(body, "AlarmActionClearPass", &clear_pass, &present, err, sizeof(err))) {
            status_code = 400;
            goto finish;
        }
        if (present && clear_pass)
            config_set_checked("AlarmActionPass", "", updated, errors);
    }

    {
        char *type = config_get_string("AlarmActionType", "none");
        bool valid = type &&
                     (strcmp(type, "none") == 0 ||
                      strcmp(type, "virtual_input") == 0 ||
                      strcmp(type, "a9210_output") == 0 ||
                      strcmp(type, "custom_http") == 0);
        if (!valid)
            g_string_append(errors, errors->len > 0 ? ",\"AlarmActionType invalid\"" :
                                                "\"AlarmActionType invalid\"");
        free(type);
    }

    {
        char *method = config_get_string("AlarmActionMethod", "GET");
        bool valid = method &&
                     (strcmp(method, "GET") == 0 ||
                      strcmp(method, "POST") == 0 ||
                      strcmp(method, "PUT") == 0);
        if (!valid)
            g_string_append(errors, errors->len > 0 ? ",\"AlarmActionMethod invalid\"" :
                                                "\"AlarmActionMethod invalid\"");
        free(method);
    }

    if (errors->len > 0)
        status_code = 400;

finish:
    if (status_code == 400 && err[0]) {
        send_error(conn, 400, err);
    } else {
        char resp[2048];
        if (status_code == 200) {
            snprintf(resp, sizeof(resp),
                     "{\"status\":\"ok\",\"updated\":[%s]}",
                     updated->str);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"status\":\"error\",\"updated\":[%s],\"errors\":[%s]}",
                     updated->str, errors->str);
        }
        send_json(conn, status_code, resp);
    }

    free(body);
    g_string_free(updated, TRUE);
    g_string_free(errors, TRUE);
    return status_code;
}

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
    history_clear();
    send_json(conn, 200, "{\"status\":\"ok\",\"message\":\"History cleared\"}");
    return 200;
}

static int handler_reset_defaults(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;

    config_set("TokenExpirationSeconds", "7");
    config_set("AoaScenarioId", "1");
    config_set("InputTriggerPort", "none");
    config_set("AlarmClearSeconds", "2");
    config_set("AlarmActionType", "none");
    config_set("AlarmActionHost", "");
    config_set("AlarmActionPort", "1");
    config_set("AlarmActionDuration", "5");
    config_set("AlarmActionUser", "");
    config_set("AlarmActionPass", "");
    config_set("AlarmActionUrl", "");
    config_set("AlarmActionMethod", "GET");
    config_set("AlarmActionPayload", "");
    config_set("AlarmActionHeader", "");
    token_set_expiration(7);
    event_subscriber_resubscribe("1");
    input_trigger_resubscribe("none");

    send_json(conn, 200, "{\"status\":\"ok\",\"message\":\"Reset to defaults\"}");
    return 200;
}

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
    send_json(conn, 200, "{\"status\":\"ok\",\"message\":\"Alarm action test fired\"}");
    return 200;
}

static void register_handler(const char *path, mg_request_handler handler)
{
    mg_set_request_handler(g_ctx, path, handler, NULL);
}

bool web_server_init(int port)
{
    const char *options[] = {
        "listening_ports",    "127.0.0.1:8080",
        "num_threads",        "10",
        "request_timeout_ms", "10000",
        NULL
    };
    struct mg_callbacks callbacks;
    (void)port;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = begin_request_callback;

    g_ctx = mg_start(&callbacks, NULL, options);
    if (!g_ctx) {
        syslog(LOG_ERR, "antitailgate: CivetWeb failed to start on localhost:8080");
        return false;
    }

    register_handler("/badge-read", handler_badge_read);
    register_handler(INGEST_PREFIX "/badge-read", handler_badge_read);
    register_handler(INGEST_PROXY_PFX "/badge-read", handler_badge_read);

    register_handler("/test", handler_test);
    register_handler("/status", handler_status);
    register_handler("/config", handler_config);
    register_handler("/threshold-crossing", handler_threshold_crossing);
    register_handler("/clear-history", handler_clear_history);
    register_handler("/reset-defaults", handler_reset_defaults);
    register_handler("/test-alarm-action", handler_test_alarm_action);

    register_handler(ADMIN_PREFIX "/test", handler_test);
    register_handler(ADMIN_PREFIX "/status", handler_status);
    register_handler(ADMIN_PREFIX "/config", handler_config);
    register_handler(ADMIN_PREFIX "/threshold-crossing", handler_threshold_crossing);
    register_handler(ADMIN_PREFIX "/clear-history", handler_clear_history);
    register_handler(ADMIN_PREFIX "/reset-defaults", handler_reset_defaults);
    register_handler(ADMIN_PREFIX "/test-alarm-action", handler_test_alarm_action);

    register_handler(ADMIN_PROXY_PFX "/test", handler_test);
    register_handler(ADMIN_PROXY_PFX "/status", handler_status);
    register_handler(ADMIN_PROXY_PFX "/config", handler_config);
    register_handler(ADMIN_PROXY_PFX "/threshold-crossing", handler_threshold_crossing);
    register_handler(ADMIN_PROXY_PFX "/clear-history", handler_clear_history);
    register_handler(ADMIN_PROXY_PFX "/reset-defaults", handler_reset_defaults);
    register_handler(ADMIN_PROXY_PFX "/test-alarm-action", handler_test_alarm_action);

    syslog(LOG_INFO, "antitailgate: CivetWeb started on localhost:8080");
    return true;
}

void web_server_cleanup(void)
{
    if (g_ctx) {
        mg_stop(g_ctx);
        g_ctx = NULL;
    }
}
