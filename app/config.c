#include "config.h"

#include <axsdk/axparameter.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>

/* Fallback: file used when AXParameter is unavailable.  Contains
 * AlarmActionPass and other credentials, so mode 0600 owned by APPUSR;
 * writes go through g_file_set_contents_full so a crash mid-write cannot
 * corrupt the file. */
#define FALLBACK_CONFIG_DIR  "/usr/local/packages/antitailgate/localdata"
#define FALLBACK_CONFIG_PATH FALLBACK_CONFIG_DIR "/config.json"

static AXParameter *g_ax_param = NULL;
static bool g_use_fallback = false;

/* Guards both AXParameter calls and fallback file reads/writes.  These
 * paths are mutually exclusive at init time (either g_ax_param is set OR
 * g_use_fallback is true) so one mutex covers both. */
static GMutex g_config_mutex;
static bool   g_config_mutex_initialized = false;

/* ---------- Fallback helpers (JSON key/value file via GKeyFile) ---------- */

/* Load once per call.  Non-existent file returns an empty GKeyFile, not an
 * error, because a fresh install has no config yet. */
static GKeyFile *fallback_load(void)
{
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(kf, FALLBACK_CONFIG_PATH,
                                   G_KEY_FILE_NONE, &err)) {
        /* File-not-found is expected on first boot; anything else worth
         * a warning. */
        if (!err || err->code != G_FILE_ERROR_NOENT) {
            syslog(LOG_WARNING,
                   "antitailgate: fallback config load failed (%s), "
                   "continuing with defaults",
                   err ? err->message : "unknown");
        }
        if (err) g_error_free(err);
        /* kf is empty but valid — treat as "no keys yet". */
    }
    return kf;
}

static char *fallback_get(const char *name, const char *default_val)
{
    g_mutex_lock(&g_config_mutex);
    GKeyFile *kf = fallback_load();
    char *value = g_key_file_get_string(kf, "antitailgate", name, NULL);
    g_key_file_free(kf);
    g_mutex_unlock(&g_config_mutex);
    if (!value || !value[0]) {
        g_free(value);
        return g_strdup(default_val);
    }
    return value;
}

static bool fallback_set(const char *name, const char *value)
{
    g_mutex_lock(&g_config_mutex);
    GKeyFile *kf = fallback_load();
    g_key_file_set_string(kf, "antitailgate", name, value ? value : "");

    gsize len = 0;
    char *out = g_key_file_to_data(kf, &len, NULL);
    g_key_file_free(kf);

    GError *err = NULL;
    /* CONSISTENT + DURABLE = write to .tmp, fsync, rename atomically,
     * fsync the parent dir.  Guarantees the file is either the old
     * contents or the new contents on any crash. */
    bool ok = g_file_set_contents_full(
        FALLBACK_CONFIG_PATH, out, (gssize)len,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
        0600, &err);
    if (!ok) {
        syslog(LOG_ERR, "antitailgate: fallback config write failed: %s",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
    } else {
        /* g_file_set_contents_full honours the mode arg on create, but on
         * an existing file the mode is preserved from the rename.  Force
         * it in case an older build left a world-readable file behind. */
        if (chmod(FALLBACK_CONFIG_PATH, 0600) != 0) {
            syslog(LOG_WARNING,
                   "antitailgate: chmod 0600 on fallback config failed");
        }
    }
    g_free(out);
    g_mutex_unlock(&g_config_mutex);
    return ok;
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

        /* Ensure localdata directory exists.  Mode 0700 because the file
         * inside will hold AlarmActionPass; keep it out of world/group
         * traversal.  APPUSR owns this directory. */
        if (g_mkdir_with_parents(FALLBACK_CONFIG_DIR, 0700) != 0) {
            syslog(LOG_ERR,
                   "antitailgate: cannot create %s (fallback config unusable)",
                   FALLBACK_CONFIG_DIR);
        }
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
