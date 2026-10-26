#include "validate.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool has_control_chars(const char *v)
{
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return true;
    return false;
}

bool validate_host(const char *v, const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    if (strlen(v) > 255) {
        snprintf(err, err_len, "Field '%s' is too long", field);
        return false;
    }
    /* Bracketed IPv6: [....]:port */
    if (v[0] == '[') {
        const char *close = strchr(v, ']');
        if (!close) {
            snprintf(err, err_len, "Field '%s' has an unclosed IPv6 bracket", field);
            return false;
        }
        for (const char *p = v + 1; p < close; p++)
            if (!(isxdigit((unsigned char)*p) || *p == ':' || *p == '.')) {
                snprintf(err, err_len, "Field '%s' is not a valid IPv6 address", field);
                return false;
            }
        if (close[1] == '\0')
            return true;
        if (close[1] != ':') {
            snprintf(err, err_len, "Field '%s' has junk after the IPv6 address", field);
            return false;
        }
        for (const char *p = close + 2; *p; p++)
            if (!isdigit((unsigned char)*p)) {
                snprintf(err, err_len, "Field '%s' has a non-numeric port", field);
                return false;
            }
        return true;
    }
    /* hostname or IPv4, optional :port.  Letters, digits, '.', '-', '_'. */
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ':') {
            for (const char *q = p + 1; *q; q++)
                if (!isdigit((unsigned char)*q)) {
                    snprintf(err, err_len, "Field '%s' has a non-numeric port", field);
                    return false;
                }
            return true;
        }
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) {
            snprintf(err, err_len,
                     "Field '%s' must be a hostname or IP (letters, digits, '.', '-', '_', optional ':port')",
                     field);
            return false;
        }
    }
    return true;
}

bool validate_url(const char *v, const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    if (strncmp(v, "http://", 7) != 0 && strncmp(v, "https://", 8) != 0) {
        snprintf(err, err_len, "Field '%s' must start with http:// or https://", field);
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if (*p <= 0x20 || *p == 0x7f) {
            snprintf(err, err_len, "Field '%s' contains a space or control character", field);
            return false;
        }
    /* Reject userinfo: a '@' in the authority (before the first '/' after
     * the scheme) means embedded credentials. */
    const char *authority = strstr(v, "://") + 3;
    const char *slash = strchr(authority, '/');
    const char *at = strchr(authority, '@');
    if (at && (!slash || at < slash)) {
        snprintf(err, err_len, "Field '%s' must not contain embedded credentials (user@host)", field);
        return false;
    }
    return true;
}

bool validate_header(const char *v, const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    if (has_control_chars(v)) {
        snprintf(err, err_len, "Field '%s' contains a control character", field);
        return false;
    }
    const char *colon = strchr(v, ':');
    if (!colon || colon == v) {
        snprintf(err, err_len, "Field '%s' must have the form 'Name: value'", field);
        return false;
    }
    for (const char *p = v; p < colon; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) {
            snprintf(err, err_len, "Field '%s' header name has an invalid character", field);
            return false;
        }
    }
    return true;
}

bool validate_scenario_id(const char *v, const char *field, char *err, size_t err_len)
{
    if (!v || !v[0]) {
        snprintf(err, err_len, "Field '%s' must not be empty", field);
        return false;
    }
    size_t n = strlen(v);
    if (n > 64) {
        snprintf(err, err_len, "Field '%s' is too long", field);
        return false;
    }
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-')) {
            snprintf(err, err_len, "Field '%s' must be letters, digits, or '-'", field);
            return false;
        }
    }
    return true;
}

bool validate_numeric_str(const char *v, long min, long max,
                          const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    for (const char *p = v; *p; p++)
        if (!isdigit((unsigned char)*p)) {
            snprintf(err, err_len, "Field '%s' must be numeric", field);
            return false;
        }
    errno = 0;
    char *end = NULL;
    long val = strtol(v, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || val < min || val > max) {
        snprintf(err, err_len, "Field '%s' must be between %ld and %ld", field, min, max);
        return false;
    }
    return true;
}

bool validate_pinned_key(const char *v, const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    if (strlen(v) > 1024) {
        snprintf(err, err_len, "Field '%s' is too long", field);
        return false;
    }
    /* One or more ';'-separated "sha256//<base64>" entries. */
    const char *p = v;
    while (*p) {
        if (strncmp(p, "sha256//", 8) != 0) {
            snprintf(err, err_len, "Field '%s' entries must begin with 'sha256//'", field);
            return false;
        }
        p += 8;
        const char *start = p;
        while (*p && *p != ';') {
            unsigned char c = (unsigned char)*p;
            if (!(isalnum(c) || c == '+' || c == '/' || c == '=')) {
                snprintf(err, err_len, "Field '%s' has a non-base64 character", field);
                return false;
            }
            p++;
        }
        if (p == start) {
            snprintf(err, err_len, "Field '%s' has an empty hash", field);
            return false;
        }
        if (*p == ';')
            p++;
    }
    return true;
}

bool validate_text(const char *v, size_t max_len,
                   const char *field, char *err, size_t err_len)
{
    if (!v || !v[0])
        return true;
    if (strlen(v) > max_len) {
        snprintf(err, err_len, "Field '%s' exceeds %zu characters", field, max_len);
        return false;
    }
    if (has_control_chars(v)) {
        snprintf(err, err_len, "Field '%s' contains a control character", field);
        return false;
    }
    return true;
}
