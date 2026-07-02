#include "alarm_handler.h"
#include "event_publisher.h"
#include "config.h"
#include "token_manager.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* Cooldown: minimum seconds between alarm actions */
#define COOLDOWN_SECONDS 2

/* Cleanup drain: how long to wait for in-flight workers before curl teardown. */
#define WORKERS_DRAIN_TIMEOUT_MS 5000

/* Monotonic timestamp so an NTP step of the wall clock cannot freeze the
 * cooldown (last_action_time - now goes negative and > COOLDOWN_SECONDS on
 * backward jumps) or short-circuit it on forward jumps. */
static int64_t         last_action_mono = 0;
static pthread_mutex_t g_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;

static int64_t mono_now_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec;
}

/* In-flight worker tracking so we can drain before curl_global_cleanup and
 * before config_cleanup / token_manager_cleanup release things the worker
 * touches (a curl_easy_perform() in a still-running detached thread would
 * otherwise be a use-after-free of the curl global state or of the
 * AXParameter handle). */
static int             g_workers_inflight = 0;
static pthread_mutex_t g_workers_mtx      = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_workers_cv       = PTHREAD_COND_INITIALIZER;

static void workers_add(void)
{
    pthread_mutex_lock(&g_workers_mtx);
    g_workers_inflight++;
    pthread_mutex_unlock(&g_workers_mtx);
}

static void workers_remove(void)
{
    pthread_mutex_lock(&g_workers_mtx);
    if (g_workers_inflight > 0)
        g_workers_inflight--;
    pthread_cond_broadcast(&g_workers_cv);
    pthread_mutex_unlock(&g_workers_mtx);
}

/* No-op write callback — discard response body */
static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
}

/* Copy scheme://host[:port]/path from in into out; drop userinfo, query,
 * and fragment.  Used to keep operator-supplied secrets out of syslog. */
static void sanitize_url_for_log(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0)
        return;
    out[0] = '\0';
    const char *scheme_end = strstr(in, "://");
    if (!scheme_end) {
        snprintf(out, out_len, "%s", in);
        return;
    }
    const char *authority = scheme_end + 3;
    /* Skip userinfo if present */
    const char *at = strchr(authority, '@');
    const char *slash = strchr(authority, '/');
    if (at && (!slash || at < slash))
        authority = at + 1;

    /* Copy scheme:// */
    size_t scheme_len = (size_t)(scheme_end - in);
    if (scheme_len + 3 >= out_len) {
        snprintf(out, out_len, "<url>");
        return;
    }
    memcpy(out, in, scheme_len);
    memcpy(out + scheme_len, "://", 3);
    size_t j = scheme_len + 3;

    /* Copy authority + path only (stop at ? or #) */
    for (size_t i = 0; authority[i] && authority[i] != '?' &&
                       authority[i] != '#' && j + 1 < out_len; i++)
        out[j++] = authority[i];
    out[j] = '\0';
}

/* Thread argument: owns all strings */
typedef struct {
    char url[1024];
    char deactivate_url[1024]; /* non-empty for output types: called after sleep */
    int  pulse_ms;             /* sleep duration between activate/deactivate */
    char user[128];
    char pass[128];
    char method[16];
    char payload[1024];
    char header[256];
    bool is_test;
    uint64_t alarm_id;
} AlarmActionArgs;

/* Perform a single curl GET/POST and return HTTP status (0 on error) */
static long do_curl_request(const AlarmActionArgs *a, const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        syslog(LOG_ERR, "antitailgate: alarm action: curl_easy_init failed");
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "antitailgate/1.0.0");

    /* Multithread safety: libcurl uses SIGALRM for DNS timeouts by default;
     * with detached worker threads, that signal is delivered to an
     * unpredictable thread and can kill the process.  NOSIGNAL disables it. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /* TLS: explicitly on.  libcurl defaults to 1L/2L on modern builds, but
     * the intent should be in the code, not the toolchain. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Scheme allowlist: default-open libcurl accepts file://, gopher://,
     * dict://, sftp://, etc.  An operator setting AlarmActionUrl to a
     * non-web scheme should get a dispatch failure, not a filesystem read. */
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,       "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    /* Digest auth if credentials provided.  Prefer digest, but if the target
     * only advertises Basic (e.g. AXIS OS 12.9 with Basic-only policy), fall
     * back so alarm-action still works.  Never fall forward from Basic-only
     * targets to Digest guessing (CURLAUTH_ANY explicitly excludes NTLM). */
    if (a->user[0]) {
        char userpwd[260];
        snprintf(userpwd, sizeof(userpwd), "%s:%s", a->user, a->pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH,
                         (long)(CURLAUTH_DIGEST | CURLAUTH_BASIC));
    }

    /* Custom method */
    if (strcmp(a->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, a->payload);
    } else if (strcmp(a->method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, a->payload);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* Custom header */
    struct curl_slist *headers = NULL;
    if (a->header[0]) {
        headers = curl_slist_append(headers, a->header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res != CURLE_OK) {
        syslog(LOG_ERR, "antitailgate: alarm action failed: %s",
               curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return http_code;
}

/* Free credentials-holding args, zeroing before free so plaintext does not
 * linger in the heap (recoverable from core dumps). */
static void free_args_zeroed(AlarmActionArgs *a)
{
    if (!a)
        return;
    explicit_bzero(a->user,    sizeof(a->user));
    explicit_bzero(a->pass,    sizeof(a->pass));
    explicit_bzero(a->header,  sizeof(a->header));
    explicit_bzero(a->payload, sizeof(a->payload));
    free(a);
}

static void *alarm_action_thread(void *arg)
{
    AlarmActionArgs *a = (AlarmActionArgs *)arg;

    char safe_url[1024];
    sanitize_url_for_log(a->url, safe_url, sizeof(safe_url));
    syslog(LOG_INFO, "antitailgate: alarm action: %s %s (test=%d)",
           a->method, safe_url, a->is_test);

    long code = do_curl_request(a, a->url);
    syslog(LOG_INFO, "antitailgate: alarm action completed: HTTP %ld", code);
    if (a->alarm_id != 0)
        alarm_record_update(a->alarm_id,
                            (code >= 200 && code < 300) ?
                            "request_succeeded" : "request_failed");

    /* For output types: sleep then deactivate */
    if (a->deactivate_url[0] && code >= 200 && code < 300) {
        usleep((useconds_t)a->pulse_ms * 1000);
        syslog(LOG_INFO, "antitailgate: alarm action: deactivating output");
        long code2 = do_curl_request(a, a->deactivate_url);
        syslog(LOG_INFO, "antitailgate: alarm action deactivate: HTTP %ld", code2);
    }

    free_args_zeroed(a);
    workers_remove();
    return NULL;
}

void alarm_handler_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* Bounded wait for all in-flight alarm workers to finish before we tear
 * down libcurl.  Public so main() can drain before earlier-in-the-chain
 * cleanups (config, token_manager) run, since worker threads touch those
 * modules through config_get_string / alarm_record_update. */
void alarm_handler_drain(int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_workers_mtx);
    while (g_workers_inflight > 0) {
        int rc = pthread_cond_timedwait(&g_workers_cv, &g_workers_mtx,
                                        &deadline);
        if (rc != 0) {
            syslog(LOG_WARNING,
                   "antitailgate: %d alarm worker(s) still in flight after "
                   "%d ms; leaving them (cleanup may race)",
                   g_workers_inflight, timeout_ms);
            break;
        }
    }
    pthread_mutex_unlock(&g_workers_mtx);
}

void alarm_handler_cleanup(void)
{
    /* Drain any workers that beat us here.  Callers ordinarily call
     * alarm_handler_drain() first before tearing down the modules those
     * workers depend on (config, token_manager); this is a last-chance
     * safety net. */
    alarm_handler_drain(WORKERS_DRAIN_TIMEOUT_MS);
    curl_global_cleanup();
}

void alarm_handler_notify(bool is_test)
{
    syslog(LOG_INFO, "antitailgate: alarm fired (test=%d)", is_test);

    /* Publish the stateful TailgatingAlarm as soon as we know a tailgate
     * happened.  This is the canonical notification: Axis action rules
     * (record video, illuminate light, PTZ) subscribe here.  The outbound
     * HTTP alarm action below is a supplementary integration and may be
     * skipped by config (type=none), cooldown, or dispatch failure -- none
     * of those should affect whether the event fires. */
    event_publisher_send_alarm(true);

    /* Read config */
    char *type     = config_get_string("AlarmActionType",     "none");
    char *host     = config_get_string("AlarmActionHost",     "");
    char *port     = config_get_string("AlarmActionPort",     "1");
    char *duration = config_get_string("AlarmActionDuration", "5");
    char *user     = config_get_string("AlarmActionUser",     "");
    char *pass     = config_get_string("AlarmActionPass",     "");
    char *url      = config_get_string("AlarmActionUrl",      "");
    char *method   = config_get_string("AlarmActionMethod",   "GET");
    char *payload  = config_get_string("AlarmActionPayload",  "");
    char *header   = config_get_string("AlarmActionHeader",   "");
    uint64_t alarm_id = 0;

    if (!is_test)
        alarm_id = alarm_record_create("pending");

    if (!type || strcmp(type, "none") == 0) {
        syslog(LOG_DEBUG, "antitailgate: alarm action type is 'none', skipping");
        if (alarm_id != 0)
            alarm_record_update(alarm_id, "not_configured");
        goto cleanup;
    }

    /* Cooldown check (bypass for test) */
    if (!is_test) {
        pthread_mutex_lock(&g_alarm_mutex);
        int64_t now_mono = mono_now_sec();
        if (now_mono - last_action_mono < COOLDOWN_SECONDS) {
            pthread_mutex_unlock(&g_alarm_mutex);
            syslog(LOG_INFO, "antitailgate: alarm action skipped (cooldown)");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "skipped_cooldown");
            goto cleanup;
        }
        last_action_mono = now_mono;
        pthread_mutex_unlock(&g_alarm_mutex);
    }

    AlarmActionArgs *args = calloc(1, sizeof(AlarmActionArgs));
    if (!args) {
        if (alarm_id != 0)
            alarm_record_update(alarm_id, "dispatch_failed");
        goto cleanup;
    }

    args->is_test = is_test;
    args->alarm_id = alarm_id;
    strncpy(args->method, "GET", sizeof(args->method) - 1);

    /* Outbound scheme for the built-in Axis-CGI targets.  Default https://:
     * digest/basic credentials in cleartext over http:// is exactly how
     * pass3/root3 leaked to param.cgi in the first place.  An admin who
     * knows their LAN and their target device has no TLS can opt out via
     * AlarmActionInsecure=true. */
    char *insecure_str = config_get_string("AlarmActionInsecure", "false");
    bool insecure = (insecure_str && strcmp(insecure_str, "true") == 0);
    free(insecure_str);
    const char *scheme = insecure ? "http" : "https";

    /* Build URL per action type */
    if (strcmp(type, "virtual_input") == 0) {
        if (!host[0]) {
            syslog(LOG_ERR, "antitailgate: virtual_input requires AlarmActionHost");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "dispatch_failed");
            free_args_zeroed(args);
            goto cleanup;
        }
        snprintf(args->url, sizeof(args->url),
                 "%s://%s/axis-cgi/virtualinput/activate.cgi"
                 "?schemaversion=1&port=%s&duration=%s",
                 scheme, host, port, duration);
        strncpy(args->user, user, sizeof(args->user) - 1);
        strncpy(args->pass, pass, sizeof(args->pass) - 1);

    } else if (strcmp(type, "a9210_output") == 0) {
        if (!host[0]) {
            syslog(LOG_ERR, "antitailgate: a9210_output requires AlarmActionHost");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "dispatch_failed");
            free_args_zeroed(args);
            goto cleanup;
        }
        /* Activate: action=<port>:/ then deactivate: action=<port>:\ */
        snprintf(args->url, sizeof(args->url),
                 "%s://%s/axis-cgi/io/port.cgi?action=%s%%3A%%2F",
                 scheme, host, port);
        snprintf(args->deactivate_url, sizeof(args->deactivate_url),
                 "%s://%s/axis-cgi/io/port.cgi?action=%s%%3A%%5C",
                 scheme, host, port);
        args->pulse_ms = atoi(duration);
        strncpy(args->user, user, sizeof(args->user) - 1);
        strncpy(args->pass, pass, sizeof(args->pass) - 1);

    } else if (strcmp(type, "custom_http") == 0) {
        if (!url[0]) {
            syslog(LOG_ERR, "antitailgate: custom_http requires AlarmActionUrl");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "dispatch_failed");
            free_args_zeroed(args);
            goto cleanup;
        }
        /* Server-side allowlist even though the config POST validator
         * rejects invalid schemes: belt-and-suspenders against stale param
         * values written before validation existed. */
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            syslog(LOG_ERR,
                   "antitailgate: custom_http URL must start with http:// or https://");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "dispatch_failed");
            free_args_zeroed(args);
            goto cleanup;
        }
        if (!insecure && strncmp(url, "http://", 7) == 0) {
            syslog(LOG_ERR,
                   "antitailgate: custom_http URL is http:// but "
                   "AlarmActionInsecure=false; set true to allow cleartext");
            if (alarm_id != 0)
                alarm_record_update(alarm_id, "dispatch_failed");
            free_args_zeroed(args);
            goto cleanup;
        }
        strncpy(args->url, url, sizeof(args->url) - 1);
        strncpy(args->method, method, sizeof(args->method) - 1);
        strncpy(args->payload, payload, sizeof(args->payload) - 1);
        strncpy(args->header, header, sizeof(args->header) - 1);
        strncpy(args->user, user, sizeof(args->user) - 1);
        strncpy(args->pass, pass, sizeof(args->pass) - 1);

    } else {
        syslog(LOG_WARNING, "antitailgate: unknown alarm action type '%s'", type);
        if (alarm_id != 0)
            alarm_record_update(alarm_id, "dispatch_failed");
        free_args_zeroed(args);
        goto cleanup;
    }

    /* Spawn detached thread.  Reserve an in-flight slot BEFORE the thread
     * starts (so alarm_handler_drain sees it even if we race with cleanup);
     * release the slot on failure. */
    workers_add();

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, alarm_action_thread, args) != 0) {
        syslog(LOG_ERR, "antitailgate: failed to create alarm action thread");
        if (alarm_id != 0)
            alarm_record_update(alarm_id, "dispatch_failed");
        free_args_zeroed(args);
        workers_remove();
    }
    pthread_attr_destroy(&attr);

cleanup:
    free(type); free(host); free(port); free(duration);
    free(user); free(pass); free(url); free(method);
    free(payload); free(header);
}
