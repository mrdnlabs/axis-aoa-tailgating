#include "config.h"

#include <axsdk/axparameter.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Fallback: JSON file path used when AXParameter is unavailable */
#define FALLBACK_CONFIG_PATH \
    "/usr/local/packages/antitailgate/localdata/config.json"

static AXParameter *g_ax_param = NULL;
static bool g_use_fallback = false;

/* AXParameter is accessed from the GLib main loop, CivetWeb worker threads,
 * and the detached alarm-action worker threads.  The library's own thread
 * safety is not documented; serialize all get/set here. */
static GMutex g_config_mutex;
static bool   g_config_mutex_initialized = false;

/* ---------- Fallback helpers (minimal key=value file) ---------- */

static char *fallback_get(const char *name, const char *default_val)
{
    FILE *f = fopen(FALLBACK_CONFIG_PATH, "r");
    if (!f)
        return g_strdup(default_val);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *sep = strchr(line, '=');
        if (!sep)
            continue;
        *sep = '\0';
        /* Trim trailing newline from value */
        char *val = sep + 1;
        size_t vlen = strlen(val);
        if (vlen > 0 && val[vlen - 1] == '\n')
            val[vlen - 1] = '\0';
        if (strcmp(line, name) == 0) {
            fclose(f);
            return g_strdup(val);
        }
    }
    fclose(f);
    return g_strdup(default_val);
}

static bool fallback_set(const char *name, const char *value)
{
    /* Read all lines, replace or append the key */
    FILE *f = fopen(FALLBACK_CONFIG_PATH, "r");
    GString *buf = g_string_new(NULL);
    bool found = false;

    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *sep = strchr(line, '=');
            if (sep) {
                char key[256];
                size_t klen = (size_t)(sep - line);
                if (klen < sizeof(key)) {
                    strncpy(key, line, klen);
                    key[klen] = '\0';
                    if (strcmp(key, name) == 0) {
                        g_string_append_printf(buf, "%s=%s\n", name, value);
                        found = true;
                        continue;
                    }
                }
            }
            g_string_append(buf, line);
        }
        fclose(f);
    }

    if (!found)
        g_string_append_printf(buf, "%s=%s\n", name, value);

    f = fopen(FALLBACK_CONFIG_PATH, "w");
    if (!f) {
        g_string_free(buf, TRUE);
        return false;
    }
    fputs(buf->str, f);
    fclose(f);
    g_string_free(buf, TRUE);
    return true;
}

/* ---------- Public API ---------- */

bool config_init(const char *app_name)
{
    if (!g_config_mutex_initialized) {
        g_mutex_init(&g_config_mutex);
        g_config_mutex_initialized = true;
    }
    GError *err = NULL;
    g_ax_param = ax_parameter_new(app_name, &err);
    if (!g_ax_param) {
        syslog(LOG_WARNING,
               "antitailgate: AXParameter init failed (%s), using file fallback",
               err ? err->message : "unknown");
        if (err)
            g_error_free(err);
        g_use_fallback = true;

        /* Ensure localdata directory exists */
        g_mkdir_with_parents(
            "/usr/local/packages/antitailgate/localdata", 0755);
        return true; /* non-fatal */
    }
    syslog(LOG_INFO, "antitailgate: AXParameter initialized");
    return true;
}

char *config_get_string(const char *name, const char *default_val)
{
    if (g_use_fallback)
        return fallback_get(name, default_val);

    g_mutex_lock(&g_config_mutex);
    GError *err = NULL;
    gchar *value = NULL;
    if (!ax_parameter_get(g_ax_param, name, &value, &err)) {
        g_mutex_unlock(&g_config_mutex);
        if (err)
            g_error_free(err);
        return g_strdup(default_val);
    }
    g_mutex_unlock(&g_config_mutex);
    if (!value || value[0] == '\0') {
        g_free(value);
        return g_strdup(default_val);
    }
    return value; /* caller must g_free / free */
}

int config_get_int(const char *name, int default_val)
{
    char *s = config_get_string(name, NULL);
    if (!s || !s[0]) {
        free(s);
        return default_val;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    bool ok = (end != s && *end == '\0');
    free(s);
    return ok ? (int)v : default_val;
}

bool config_set(const char *name, const char *value)
{
    if (g_use_fallback)
        return fallback_set(name, value);

    g_mutex_lock(&g_config_mutex);
    GError *err = NULL;
    bool ok = ax_parameter_set(g_ax_param, name, value, TRUE, &err);
    g_mutex_unlock(&g_config_mutex);
    if (!ok) {
        syslog(LOG_ERR, "antitailgate: config_set(%s) failed: %s",
               name, err ? err->message : "unknown");
        if (err)
            g_error_free(err);
        return false;
    }
    return true;
}

void config_cleanup(void)
{
    if (g_ax_param) {
        g_mutex_lock(&g_config_mutex);
        ax_parameter_free(g_ax_param);
        g_ax_param = NULL;
        g_mutex_unlock(&g_config_mutex);
    }
    if (g_config_mutex_initialized) {
        g_mutex_clear(&g_config_mutex);
        g_config_mutex_initialized = false;
    }
}
