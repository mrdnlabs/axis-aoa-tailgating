#include "json_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_FIELDS  64
#define JSON_MAX_KEY_LEN 128

typedef struct {
    const char *p;
    const char *end;
    char       *err;
    size_t      err_len;
} Parser;

static bool fail(Parser *ps, const char *msg)
{
    if (ps->err && ps->err_len)
        snprintf(ps->err, ps->err_len, "%s", msg);
    return false;
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_u4(Parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4)
        return fail(ps, "Truncated \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(ps->p[i]);
        if (h < 0)
            return fail(ps, "Invalid \\u escape");
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

/* ps->p points at the opening quote.  Decodes into out. */
static bool parse_string(Parser *ps, GString *out)
{
    if (ps->p >= ps->end || *ps->p != '"')
        return fail(ps, "Expected JSON string");
    ps->p++;

    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (c < 0x20)
            return fail(ps, "Unescaped control character in JSON string");
        if (c != '\\') {
            g_string_append_c(out, (char)c);
            continue;
        }
        if (ps->p >= ps->end)
            return fail(ps, "Invalid JSON escape");
        char e = *ps->p++;
        switch (e) {
            case '"':  g_string_append_c(out, '"');  break;
            case '\\': g_string_append_c(out, '\\'); break;
            case '/':  g_string_append_c(out, '/');  break;
            case 'b':  g_string_append_c(out, '\b'); break;
            case 'f':  g_string_append_c(out, '\f'); break;
            case 'n':  g_string_append_c(out, '\n'); break;
            case 'r':  g_string_append_c(out, '\r'); break;
            case 't':  g_string_append_c(out, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!parse_u4(ps, &cp))
                    return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return fail(ps, "Unpaired low surrogate in \\u escape");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
                        return fail(ps, "Unpaired high surrogate in \\u escape");
                    ps->p += 2;
                    if (!parse_u4(ps, &lo))
                        return false;
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        return fail(ps, "Invalid surrogate pair in \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp == 0)
                    return fail(ps, "NUL character not allowed in JSON string");
                char utf8[8];
                gint n = g_unichar_to_utf8((gunichar)cp, utf8);
                g_string_append_len(out, utf8, n);
                break;
            }
            default:
                return fail(ps, "Invalid JSON escape");
        }
    }

    if (ps->p >= ps->end)
        return fail(ps, "Unterminated JSON string");
    ps->p++; /* closing quote */

    if (!g_utf8_validate(out->str, (gssize)out->len, NULL))
        return fail(ps, "JSON string is not valid UTF-8");
    return true;
}

/* JSON number grammar; stores the literal. */
static bool parse_number(Parser *ps, char **out)
{
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    if (ps->p >= ps->end)
        return fail(ps, "Invalid JSON number");
    if (*ps->p == '0') {
        ps->p++;
    } else if (*ps->p >= '1' && *ps->p <= '9') {
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    } else {
        return fail(ps, "Invalid JSON number");
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
            return fail(ps, "Invalid JSON number");
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9')
            return fail(ps, "Invalid JSON number");
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    *out = g_strndup(start, (gsize)(ps->p - start));
    return true;
}

static bool match_literal(Parser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
        return false;
    ps->p += n;
    return true;
}

static bool parse_value(Parser *ps, JsonField *f)
{
    if (ps->p >= ps->end)
        return fail(ps, "Expected JSON value");
    char c = *ps->p;
    if (c == '"') {
        GString *s = g_string_new(NULL);
        if (!parse_string(ps, s)) {
            g_string_free(s, TRUE);
            return false;
        }
        f->type = JV_STRING;
        f->str = g_string_free(s, FALSE);
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        f->type = JV_NUMBER;
        return parse_number(ps, &f->str);
    }
    if (match_literal(ps, "true"))  { f->type = JV_BOOL; f->b = true;  return true; }
    if (match_literal(ps, "false")) { f->type = JV_BOOL; f->b = false; return true; }
    if (match_literal(ps, "null"))  { f->type = JV_NULL;               return true; }
    if (c == '{' || c == '[')
        return fail(ps, "Nested objects and arrays are not supported");
    return fail(ps, "Unsupported JSON value");
}

JsonObject *json_object_parse(const char *text, size_t len,
                              char *err, size_t err_len)
{
    Parser ps = { text, text + len, err, err_len };
    JsonObject *obj = g_new0(JsonObject, 1);
    obj->fields = g_new0(JsonField, JSON_MAX_FIELDS);

    if (err && err_len)
        err[0] = '\0';
    if (!text)
        goto bad_obj;

    skip_ws(&ps);
    if (ps.p >= ps.end || *ps.p != '{')
        goto bad_obj;
    ps.p++;
    skip_ws(&ps);

    if (ps.p < ps.end && *ps.p == '}') {
        ps.p++;
    } else {
        for (;;) {
            if (obj->n >= JSON_MAX_FIELDS) {
                fail(&ps, "Too many fields in JSON object");
                goto error;
            }
            GString *key = g_string_new(NULL);
            if (!parse_string(&ps, key)) {
                g_string_free(key, TRUE);
                goto error;
            }
            if (key->len == 0 || key->len > JSON_MAX_KEY_LEN) {
                g_string_free(key, TRUE);
                fail(&ps, "Invalid JSON key");
                goto error;
            }
            for (size_t i = 0; i < obj->n; i++) {
                if (strcmp(obj->fields[i].key, key->str) == 0) {
                    g_string_free(key, TRUE);
                    fail(&ps, "Duplicate key in JSON object");
                    goto error;
                }
            }
            JsonField *f = &obj->fields[obj->n];
            f->key = g_string_free(key, FALSE);
            obj->n++;

            skip_ws(&ps);
            if (ps.p >= ps.end || *ps.p != ':') {
                fail(&ps, "Expected ':' after key");
                goto error;
            }
            ps.p++;
            skip_ws(&ps);
            if (!parse_value(&ps, f))
                goto error;
            skip_ws(&ps);
            if (ps.p < ps.end && *ps.p == ',') {
                ps.p++;
                skip_ws(&ps);
                continue;
            }
            if (ps.p < ps.end && *ps.p == '}') {
                ps.p++;
                break;
            }
            fail(&ps, "Expected ',' or '}' in JSON object");
            goto error;
        }
    }

    skip_ws(&ps);
    if (ps.p != ps.end) {
        fail(&ps, "Unexpected data after JSON object");
        goto error;
    }
    return obj;

bad_obj:
    fail(&ps, "JSON body must be an object");
error:
    json_object_free(obj);
    return NULL;
}

void json_object_free(JsonObject *obj)
{
    if (!obj)
        return;
    for (size_t i = 0; i < obj->n; i++) {
        g_free(obj->fields[i].key);
        if (obj->fields[i].str) {
            /* Values may be credentials; don't leave them in the heap. */
            explicit_bzero(obj->fields[i].str, strlen(obj->fields[i].str));
            g_free(obj->fields[i].str);
        }
    }
    g_free(obj->fields);
    g_free(obj);
}

const JsonField *json_object_get(const JsonObject *obj, const char *key)
{
    if (!obj || !key)
        return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (strcmp(obj->fields[i].key, key) == 0)
            return &obj->fields[i];
    return NULL;
}

int json_get_str(const JsonObject *obj, const char *key,
                 const char **out, char *err, size_t err_len)
{
    const JsonField *f = json_object_get(obj, key);
    if (!f)
        return 0;
    if (f->type != JV_STRING) {
        snprintf(err, err_len, "Field '%s' must be a string", key);
        return -1;
    }
    *out = f->str;
    return 1;
}

static bool number_is_integer(const char *lit)
{
    return lit && !strpbrk(lit, ".eE");
}

int json_get_int(const JsonObject *obj, const char *key,
                 long min, long max, int *out, char *err, size_t err_len)
{
    const JsonField *f = json_object_get(obj, key);
    if (!f)
        return 0;
    if (f->type != JV_NUMBER || !number_is_integer(f->str)) {
        snprintf(err, err_len, "Field '%s' must be an integer", key);
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(f->str, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || v < min || v > max ||
        v < INT_MIN || v > INT_MAX) {
        snprintf(err, err_len, "Field '%s' must be between %ld and %ld",
                 key, min, max);
        return -1;
    }
    *out = (int)v;
    return 1;
}

int json_get_bool(const JsonObject *obj, const char *key,
                  bool *out, char *err, size_t err_len)
{
    const JsonField *f = json_object_get(obj, key);
    if (!f)
        return 0;
    if (f->type != JV_BOOL) {
        snprintf(err, err_len, "Field '%s' must be a boolean", key);
        return -1;
    }
    *out = f->b;
    return 1;
}

int json_get_scalar_str(const JsonObject *obj, const char *key,
                        const char **out, char *err, size_t err_len)
{
    const JsonField *f = json_object_get(obj, key);
    if (!f)
        return 0;
    if (f->type == JV_STRING ||
        (f->type == JV_NUMBER && number_is_integer(f->str))) {
        *out = f->str;
        return 1;
    }
    snprintf(err, err_len, "Field '%s' must be a string or an integer", key);
    return -1;
}

bool json_object_only_keys(const JsonObject *obj, const char *const *allowed,
                           char *err, size_t err_len)
{
    for (size_t i = 0; obj && i < obj->n; i++) {
        bool ok = false;
        for (const char *const *a = allowed; *a; a++) {
            if (strcmp(obj->fields[i].key, *a) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            GString *k = g_string_new(NULL);
            json_escape_append(k, obj->fields[i].key);
            snprintf(err, err_len, "Unknown field '%.64s'", k->str);
            g_string_free(k, TRUE);
            return false;
        }
    }
    return true;
}

void json_escape_append(GString *out, const char *s)
{
    if (!s)
        return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\b': g_string_append(out, "\\b");  break;
            case '\f': g_string_append(out, "\\f");  break;
            case '\n': g_string_append(out, "\\n");  break;
            case '\r': g_string_append(out, "\\r");  break;
            case '\t': g_string_append(out, "\\t");  break;
            default:
                if (*p < 0x20 || *p == 0x7f)
                    g_string_append_printf(out, "\\u%04x", *p);
                else
                    g_string_append_c(out, (char)*p);
        }
    }
}
