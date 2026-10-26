#include "alarm_handler.h"
#include "event_publisher.h"
#include "settings.h"
#include "token_manager.h"

#include <curl/curl.h>
#include <errno.h>
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>

#define COOLDOWN_SECONDS          2
#define WORKERS_DRAIN_TIMEOUT_MS  5000
#define MAX_INFLIGHT_WORKERS      8

static int64_t         last_action_mono = 0;
static pthread_mutex_t g_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;

static int             g_workers_inflight = 0;
static pthread_mutex_t g_workers_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_workers_cv   = PTHREAD_COND_INITIALIZER;
static atomic_bool     g_shutting_down = false;

static int64_t mono_now_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec;
}

static bool workers_try_add(void)
{
    bool ok = false;
    pthread_mutex_lock(&g_workers_mtx);
    if (!atomic_load(&g_shutting_down) && g_workers_inflight < MAX_INFLIGHT_WORKERS) {
        g_workers_inflight++;
        ok = true;
    }
    pthread_mutex_unlock(&g_workers_mtx);
    return ok;
}

static void workers_remove(void)
{
    pthread_mutex_lock(&g_workers_mtx);
    if (g_workers_inflight > 0)
        g_workers_inflight--;
    pthread_cond_broadcast(&g_workers_cv);
    pthread_mutex_unlock(&g_workers_mtx);
}

static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
}

/* Abort in-flight transfers promptly once shutdown starts. */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return atomic_load(&g_shutting_down) ? 1 : 0;
}

/* Everything a worker needs, copied so it owns its data. */
typedef struct {
    char         url[1024];
    char         user[128];
    char         pass[128];
    char         method[8];
    char         payload[1024];
    char         header[256];
    char         pin[1024];
    AlarmTlsMode tls;
    bool         is_test;
    uint64_t     alarm_id;
} Job;

static void job_free(Job *j)
{
    if (!j) return;
    explicit_bzero(j->user, sizeof j->user);
    explicit_bzero(j->pass, sizeof j->pass);
    explicit_bzero(j->header, sizeof j->header);
    explicit_bzero(j->payload, sizeof j->payload);
    free(j);
}

static void sanitize_url_for_log(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) return;
    out[0] = '\0';
    const char *se = strstr(in, "://");
    if (!se) { snprintf(out, out_len, "%s", in); return; }
    const char *auth = se + 3;
    const char *at = strchr(auth, '@');
    const char *slash = strchr(auth, '/');
    if (at && (!slash || at < slash)) auth = at + 1;
    size_t sl = (size_t)(se - in);
    if (sl + 3 >= out_len) { snprintf(out, out_len, "<url>"); return; }
    memcpy(out, in, sl); memcpy(out + sl, "://", 3);
    size_t k = sl + 3;
    for (size_t i = 0; auth[i] && auth[i] != '?' && auth[i] != '#' && k + 1 < out_len; i++)
        out[k++] = auth[i];
    out[k] = '\0';
}

/* Perform one request.  Returns HTTP status, or 0 on transport error. */
static long do_curl_request(const Job *j, const char *url)
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
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "antitailgate/1.0.0");
    /* No redirects on a credentialed request: a redirect can re-target it
     * (and its auth / body) to another host, or downgrade https->http. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    /* CURLOPT_PROTOCOLS_STR is an enum (not a macro), so gate on the
     * libcurl version rather than #ifdef.  Added in 7.85.0 (0x075500). */
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    /* TLS posture. */
    switch (j->tls) {
        case TLS_VERIFY:
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            break;
        case TLS_PIN:
            /* Pin the peer's public key; still require the hostname match
             * off, since Axis peripherals use IP-only self-signed certs. */
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            if (j->pin[0])
                curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, j->pin);
            break;
        case TLS_NOVERIFY:
        case TLS_HTTP:
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            break;
    }

    if (j->user[0]) {
        char userpwd[260];
        snprintf(userpwd, sizeof userpwd, "%s:%s", j->user, j->pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)(CURLAUTH_DIGEST | CURLAUTH_BASIC));
        explicit_bzero(userpwd, sizeof userpwd);
    }

    if (!strcmp(j->method, "POST")) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, j->payload);
    } else if (!strcmp(j->method, "PUT")) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, j->payload);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    struct curl_slist *headers = NULL;
    if (j->header[0]) {
        headers = curl_slist_append(NULL, j->header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res != CURLE_OK)
        syslog(LOG_ERR, "antitailgate: alarm action failed: %s", curl_easy_strerror(res));
    else
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return http_code;
}

static const char *status_for(long code)
{
    return (code >= 200 && code < 300) ? "request_succeeded" : "request_failed";
}

static void *alarm_worker(void *arg)
{
    Job *j = arg;
    char safe[1024];
    sanitize_url_for_log(j->url, safe, sizeof safe);
    syslog(LOG_INFO, "antitailgate: alarm action: %s %s", j->method, safe);

    long code = do_curl_request(j, j->url);
    syslog(LOG_INFO, "antitailgate: alarm action completed: HTTP %ld", code);
    if (j->alarm_id)
        alarm_record_update(j->alarm_id, status_for(code));

    job_free(j);
    workers_remove();
    return NULL;
}

void alarm_handler_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void alarm_handler_drain(int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_workers_mtx);
    while (g_workers_inflight > 0) {
        if (pthread_cond_timedwait(&g_workers_cv, &g_workers_mtx, &deadline) != 0) {
            syslog(LOG_WARNING,
                   "antitailgate: %d alarm worker(s) still in flight after %d ms",
                   g_workers_inflight, timeout_ms);
            break;
        }
    }
    pthread_mutex_unlock(&g_workers_mtx);
}

void alarm_handler_begin_shutdown(void)
{
    atomic_store(&g_shutting_down, true);
}

void alarm_handler_cleanup(void)
{
    atomic_store(&g_shutting_down, true);
    alarm_handler_drain(WORKERS_DRAIN_TIMEOUT_MS);
    curl_global_cleanup();
}

/* Fill a Job from the snapshot for the current action type.  Returns false
 * (reason in err) if the action is not configured / not dispatchable. */
static bool job_from_settings(const Settings *s, bool is_test, Job *j,
                              char *err, size_t err_len)
{
    memset(j, 0, sizeof *j);
    j->is_test = is_test;
    j->tls = s->alarm_tls;
    char deact[1]; /* unused: single-request pulse now */
    if (!settings_build_alarm_urls(s, j->url, sizeof j->url,
                                   deact, sizeof deact, err, err_len))
        return false;
    g_strlcpy(j->user, s->alarm_user, sizeof j->user);
    g_strlcpy(j->pass, s->alarm_pass, sizeof j->pass);
    g_strlcpy(j->pin,  s->alarm_pin,  sizeof j->pin);
    if (s->alarm_type == ALARM_CUSTOM_HTTP) {
        g_strlcpy(j->method,  s->alarm_method,  sizeof j->method);
        g_strlcpy(j->payload, s->alarm_payload, sizeof j->payload);
        g_strlcpy(j->header,  s->alarm_header,  sizeof j->header);
    } else {
        g_strlcpy(j->method, "GET", sizeof j->method);
    }
    return true;
}

void alarm_handler_notify(bool is_test)
{
    syslog(LOG_INFO, "antitailgate: alarm fired (test=%d)", is_test);

    /* The stateful TailgatingAlarm is the canonical notification and fires
     * for every real tailgate, independent of the optional HTTP action.
     * A test does NOT publish it — testing a webhook must not look like a
     * real tailgate to camera rules or the VMS. */
    if (!is_test)
        event_publisher_send_alarm(true);

    Settings s;
    settings_get(&s);

    uint64_t alarm_id = is_test ? 0 : alarm_record_create("pending");

    if (s.alarm_type == ALARM_NONE) {
        if (alarm_id) alarm_record_update(alarm_id, "not_configured");
        goto done;
    }

    if (!is_test) {
        pthread_mutex_lock(&g_alarm_mutex);
        int64_t now = mono_now_sec();
        if (now - last_action_mono < COOLDOWN_SECONDS) {
            pthread_mutex_unlock(&g_alarm_mutex);
            if (alarm_id) alarm_record_update(alarm_id, "skipped_cooldown");
            goto done;
        }
        last_action_mono = now;
        pthread_mutex_unlock(&g_alarm_mutex);
    }

    {
        char err[128];
        Job *j = calloc(1, sizeof *j);
        if (!j) { if (alarm_id) alarm_record_update(alarm_id, "dispatch_failed"); goto done; }
        if (!job_from_settings(&s, is_test, j, err, sizeof err)) {
            syslog(LOG_ERR, "antitailgate: alarm action not dispatched: %s", err);
            if (alarm_id) alarm_record_update(alarm_id, "dispatch_failed");
            job_free(j);
            goto done;
        }
        j->alarm_id = alarm_id;

        if (!workers_try_add()) {
            syslog(LOG_WARNING, "antitailgate: alarm worker cap reached / shutting down");
            if (alarm_id) alarm_record_update(alarm_id, "dispatch_failed");
            job_free(j);
            goto done;
        }
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&tid, &attr, alarm_worker, j) != 0) {
            syslog(LOG_ERR, "antitailgate: failed to create alarm worker");
            if (alarm_id) alarm_record_update(alarm_id, "dispatch_failed");
            job_free(j);
            workers_remove();
        }
        pthread_attr_destroy(&attr);
    }

done:
    explicit_bzero(s.alarm_pass, sizeof s.alarm_pass);
}

bool alarm_handler_test(char *status, size_t status_len)
{
    Settings s;
    settings_get(&s);
    char err[128];
    Job j;
    if (!job_from_settings(&s, true, &j, err, sizeof err)) {
        g_strlcpy(status, err, status_len);
        explicit_bzero(s.alarm_pass, sizeof s.alarm_pass);
        return false;
    }
    char safe[1024];
    sanitize_url_for_log(j.url, safe, sizeof safe);
    syslog(LOG_INFO, "antitailgate: alarm action TEST: %s %s", j.method, safe);
    long code = do_curl_request(&j, j.url);
    snprintf(status, status_len, "HTTP %ld", code);
    explicit_bzero(j.user, sizeof j.user);
    explicit_bzero(j.pass, sizeof j.pass);
    explicit_bzero(s.alarm_pass, sizeof s.alarm_pass);
    return (code >= 200 && code < 300);
}
