/* Host-side unit tests for json_util.c and validate.c.
 * Build/run with app/test/run_tests.sh (Alpine + glib).  No device needed. */
#include "../json_util.h"
#include "../validate.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define OK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                  \
    else { g_fail++; printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static JsonObject *P(const char *s, char *err, size_t errlen)
{
    return json_object_parse(s, strlen(s), err, errlen);
}

static void test_json_basic(void)
{
    char err[256];
    JsonObject *o = P("{\"a\":\"x\",\"b\":7,\"c\":true,\"d\":null}", err, sizeof err);
    OK(o != NULL, "basic object parses");
    if (o) {
        const char *s = NULL; int i = 0; bool b = false;
        OK(json_get_str(o, "a", &s, err, sizeof err) == 1 && !strcmp(s, "x"), "string a");
        OK(json_get_int(o, "b", 0, 10, &i, err, sizeof err) == 1 && i == 7, "int b");
        OK(json_get_bool(o, "c", &b, err, sizeof err) == 1 && b, "bool c");
        OK(json_object_get(o, "d") && json_object_get(o, "d")->type == JV_NULL, "null d");
        OK(json_get_str(o, "missing", &s, err, sizeof err) == 0, "absent key -> 0");
        json_object_free(o);
    }
    OK(P("{}", err, sizeof err) != NULL, "empty object parses");
}

static void test_json_reject(void)
{
    char err[256];
    OK(P("", err, sizeof err) == NULL, "empty string rejected");
    OK(P("   ", err, sizeof err) == NULL, "whitespace rejected");
    OK(P("[]", err, sizeof err) == NULL, "array rejected");
    OK(P("{\"a\":{\"b\":1}}", err, sizeof err) == NULL, "nested object rejected");
    OK(P("{\"a\":[1,2]}", err, sizeof err) == NULL, "nested array rejected");
    OK(P("{\"a\":1}junk", err, sizeof err) == NULL, "trailing junk rejected");
    OK(P("{\"a\":1,\"a\":2}", err, sizeof err) == NULL, "duplicate key rejected");
    OK(P("{\"a\":01}", err, sizeof err) == NULL, "leading-zero number rejected");
    OK(P("{\"a\":}", err, sizeof err) == NULL, "missing value rejected");
    OK(P("{\"a\" 1}", err, sizeof err) == NULL, "missing colon rejected");
    OK(P("{\"a\":\"\\x\"}", err, sizeof err) == NULL, "bad escape rejected");
    OK(P("{\"a\":\"\\u0000\"}", err, sizeof err) == NULL, "NUL escape rejected");
    /* raw control char (literal newline inside string) */
    OK(P("{\"a\":\"line\nbreak\"}", err, sizeof err) == NULL, "raw control char rejected");
}

static void test_json_unicode(void)
{
    char err[256];
    /* \u00e9 = é (UTF-8 C3 A9) */
    JsonObject *o = P("{\"a\":\"caf\\u00e9\"}", err, sizeof err);
    OK(o != NULL, "unicode escape parses");
    if (o) {
        const char *s = NULL;
        json_get_str(o, "a", &s, err, sizeof err);
        OK(s && strcmp(s, "caf\xc3\xa9") == 0, "unicode decoded to UTF-8");
        json_object_free(o);
    }
    /* surrogate pair: U+1F600 */
    o = P("{\"a\":\"\\ud83d\\ude00\"}", err, sizeof err);
    OK(o != NULL, "surrogate pair parses");
    if (o) json_object_free(o);
    OK(P("{\"a\":\"\\ud83d\"}", err, sizeof err) == NULL, "lone high surrogate rejected");
    OK(P("{\"a\":\"\\ude00\"}", err, sizeof err) == NULL, "lone low surrogate rejected");
    /* raw invalid UTF-8 byte */
    OK(P("{\"a\":\"\xff\"}", err, sizeof err) == NULL, "invalid UTF-8 byte rejected");
}

static void test_json_int_range(void)
{
    char err[256];
    int i;
    JsonObject *o = P("{\"n\":5,\"big\":99999999999,\"flt\":1.5}", err, sizeof err);
    OK(o != NULL, "numbers parse");
    if (o) {
        OK(json_get_int(o, "n", 1, 10, &i, err, sizeof err) == 1, "in range");
        OK(json_get_int(o, "n", 6, 10, &i, err, sizeof err) == -1, "below range -> -1");
        OK(json_get_int(o, "big", 0, 100, &i, err, sizeof err) == -1, "overflow -> -1");
        OK(json_get_int(o, "flt", 0, 10, &i, err, sizeof err) == -1, "float not int -> -1");
        json_object_free(o);
    }
}

static void test_json_only_keys(void)
{
    char err[256];
    JsonObject *o = P("{\"a\":1,\"z\":2}", err, sizeof err);
    const char *allowed[] = { "a", "b", NULL };
    OK(o && !json_object_only_keys(o, allowed, err, sizeof err), "unknown key flagged");
    if (o) json_object_free(o);
    o = P("{\"a\":1}", err, sizeof err);
    OK(o && json_object_only_keys(o, allowed, err, sizeof err), "known keys pass");
    if (o) json_object_free(o);
}

static void test_json_escape(void)
{
    GString *g = g_string_new(NULL);
    json_escape_append(g, "a\"b\\c\n\x01");
    OK(strcmp(g->str, "a\\\"b\\\\c\\n\\u0001") == 0, "escape output");
    g_string_free(g, TRUE);
}

static void test_validate_host(void)
{
    char e[256];
    OK(validate_host("192.168.1.10", "h", e, sizeof e), "ipv4 ok");
    OK(validate_host("cam.example.com:8443", "h", e, sizeof e), "host:port ok");
    OK(validate_host("[2001:db8::1]:80", "h", e, sizeof e), "ipv6 ok");
    OK(validate_host("", "h", e, sizeof e), "empty host ok (unset)");
    OK(!validate_host("evil/../x", "h", e, sizeof e), "slash rejected");
    OK(!validate_host("h ost", "h", e, sizeof e), "space rejected");
    OK(!validate_host("host:abc", "h", e, sizeof e), "non-numeric port rejected");
    OK(!validate_host("user@host", "h", e, sizeof e), "userinfo rejected");
}

static void test_validate_url(void)
{
    char e[256];
    OK(validate_url("https://x.example/w", "u", e, sizeof e), "https ok");
    OK(validate_url("http://x.example", "u", e, sizeof e), "http ok");
    OK(validate_url("", "u", e, sizeof e), "empty url ok");
    OK(!validate_url("file:///etc/passwd", "u", e, sizeof e), "file scheme rejected");
    OK(!validate_url("https://u:p@x.example", "u", e, sizeof e), "userinfo rejected");
    OK(!validate_url("https://x.example/\x01", "u", e, sizeof e), "control char rejected");
    OK(!validate_url("https://x.example/a b", "u", e, sizeof e), "space rejected");
}

static void test_validate_header(void)
{
    char e[256];
    OK(validate_header("Authorization: Bearer abc", "h", e, sizeof e), "header ok");
    OK(validate_header("", "h", e, sizeof e), "empty header ok");
    OK(!validate_header("no-colon", "h", e, sizeof e), "missing colon rejected");
    OK(!validate_header("X-Foo: a\r\nX-Bar: b", "h", e, sizeof e), "CRLF rejected");
    OK(!validate_header("Bad Name: v", "h", e, sizeof e), "space in name rejected");
}

static void test_validate_misc(void)
{
    char e[256];
    OK(validate_scenario_id("1", "s", e, sizeof e), "scenario int ok");
    OK(validate_scenario_id("Device1-abc", "s", e, sizeof e), "scenario dash ok");
    OK(!validate_scenario_id("", "s", e, sizeof e), "empty scenario rejected");
    OK(!validate_scenario_id("a b", "s", e, sizeof e), "scenario space rejected");
    OK(!validate_scenario_id("<img>", "s", e, sizeof e), "scenario markup rejected");

    OK(validate_numeric_str("5", 1, 10, "n", e, sizeof e), "numeric ok");
    OK(validate_numeric_str("", 1, 10, "n", e, sizeof e), "empty numeric ok");
    OK(!validate_numeric_str("0", 1, 10, "n", e, sizeof e), "below min rejected");
    OK(!validate_numeric_str("x", 1, 10, "n", e, sizeof e), "non-numeric rejected");

    OK(validate_pinned_key("sha256//AAAA", "p", e, sizeof e), "one pin ok");
    OK(validate_pinned_key("sha256//AA==;sha256//BB", "p", e, sizeof e), "two pins ok");
    OK(validate_pinned_key("", "p", e, sizeof e), "empty pin ok");
    OK(!validate_pinned_key("AAAA", "p", e, sizeof e), "pin without prefix rejected");
    OK(!validate_pinned_key("sha256//", "p", e, sizeof e), "empty-hash pin rejected");
}

int main(void)
{
    test_json_basic();
    test_json_reject();
    test_json_unicode();
    test_json_int_range();
    test_json_only_keys();
    test_json_escape();
    test_validate_host();
    test_validate_url();
    test_validate_header();
    test_validate_misc();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
