#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Field validators shared by the HTTP config handler and the alarm
 * dispatcher.  Every validator that can fail writes a human-readable
 * reason into err.  All treat an empty string as "unset" = valid unless
 * noted.  None allow control characters (< 0x20 or 0x7f).
 */

/* Hostname, IPv4, or bracketed IPv6 literal; optional ":port".  No scheme,
 * no path, no userinfo, no credentials.  This is what goes into an
 * Axis-CGI URL we build ourselves. */
bool validate_host(const char *v, const char *field, char *err, size_t err_len);

/* http:// or https:// absolute URL with no control chars, no spaces, and
 * no userinfo (`user:pass@`).  Used for the operator-supplied webhook URL. */
bool validate_url(const char *v, const char *field, char *err, size_t err_len);

/* "Name: value" HTTP header; RFC 7230 tchar name, no CR/LF/NUL anywhere. */
bool validate_header(const char *v, const char *field, char *err, size_t err_len);

/* AOA scenario id: 1-64 chars of [A-Za-z0-9-] (covers integer ids and the
 * GUID-ish forms Axis uses).  Empty is rejected here; callers that allow
 * "unchanged" must check emptiness before calling. */
bool validate_scenario_id(const char *v, const char *field, char *err, size_t err_len);

/* Decimal string in [min,max].  Empty = unset = valid. */
bool validate_numeric_str(const char *v, long min, long max,
                          const char *field, char *err, size_t err_len);

/* A libcurl CURLOPT_PINNEDPUBLICKEY value: "sha256//" + base64, possibly
 * several ';'-separated.  Empty = unset = valid. */
bool validate_pinned_key(const char *v, const char *field, char *err, size_t err_len);

/* Free-form text destined for a URL query value / POST body: reject only
 * control characters, allow the rest (it gets percent-encoded or sent as a
 * body).  Empty = valid. */
bool validate_text(const char *v, size_t max_len,
                   const char *field, char *err, size_t err_len);
