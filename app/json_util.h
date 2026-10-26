#pragma once

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal JSON support for the app's HTTP API.
 *
 * Only a flat object of scalar values is accepted:
 *   { "key": "string" | number | true | false | null, ... }
 * Nested objects and arrays are rejected.  Strings are decoded to UTF-8,
 * including \uXXXX escapes and surrogate pairs; \u0000 and raw control
 * characters are rejected.  Duplicate keys are rejected so the server and
 * any intermediary can never disagree about which value wins.
 */

typedef enum {
    JV_STRING,
    JV_NUMBER,   /* str holds the literal text, e.g. "-12" or "1.5e3" */
    JV_BOOL,
    JV_NULL,
} JsonValType;

typedef struct {
    char        *key;
    JsonValType  type;
    char        *str;   /* JV_STRING: decoded UTF-8; JV_NUMBER: literal */
    bool         b;     /* JV_BOOL */
} JsonField;

typedef struct {
    JsonField *fields;
    size_t     n;
} JsonObject;

/* Parse text[0..len).  Returns NULL and fills err on any error.
 * An empty / whitespace-only body is an error; callers that accept an
 * empty body should check len before calling. */
JsonObject *json_object_parse(const char *text, size_t len,
                              char *err, size_t err_len);
void json_object_free(JsonObject *obj);

const JsonField *json_object_get(const JsonObject *obj, const char *key);

/* Typed getters.  Return 1 if present and valid, 0 if absent, -1 on a
 * type/range error (err filled). */
int json_get_str(const JsonObject *obj, const char *key,
                 const char **out, char *err, size_t err_len);
int json_get_int(const JsonObject *obj, const char *key,
                 long min, long max, int *out, char *err, size_t err_len);
int json_get_bool(const JsonObject *obj, const char *key,
                  bool *out, char *err, size_t err_len);
/* A string, or an integer number rendered as its literal text. */
int json_get_scalar_str(const JsonObject *obj, const char *key,
                        const char **out, char *err, size_t err_len);

/* Fail if obj has a key not in allowed[] (NULL-terminated). */
bool json_object_only_keys(const JsonObject *obj, const char *const *allowed,
                           char *err, size_t err_len);

/* RFC 8259 string escaping (no surrounding quotes). */
void json_escape_append(GString *out, const char *s);
