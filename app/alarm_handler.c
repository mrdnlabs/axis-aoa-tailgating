#include "alarm_handler.h"
#include "event_publisher.h"
#include "config.h"

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

static time_t last_action_time = 0;

/* No-op write callback — discard response body */
static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
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

    /* Digest auth if credentials provided */
    if (a->user[0]) {
        char userpwd[260];
        snprintf(userpwd, sizeof(userpwd), "%s:%s", a->user, a->pass);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_DIGEST);
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

static void *alarm_action_thread(void *arg)
{
    AlarmActionArgs *a = (AlarmActionArgs *)arg;

    syslog(LOG_INFO, "antitailgate: alarm action: %s %s (test=%d)",
           a->method, a->url, a->is_test);

    long code = do_curl_request(a, a->url);
    syslog(LOG_INFO, "antitailgate: alarm action completed: HTTP %ld", code);

    /* For output types: sleep then deactivate */
    if (a->deactivate_url[0] && code >= 200 && code < 300) {
        usleep((useconds_t)a->pulse_ms * 1000);
        syslog(LOG_INFO, "antitailgate: alarm action: deactivating output");
        long code2 = do_curl_request(a, a->deactivate_url);
        syslog(LOG_INFO, "antitailgate: alarm action deactivate: HTTP %ld", code2);
    }

    free(a);
    return NULL;
}

void alarm_handler_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void alarm_handler_cleanup(void)
{
    curl_global_cleanup();
}

void alarm_handler_notify(bool is_test)
{
    syslog(LOG_INFO, "antitailgate: alarm fired (test=%d)", is_test);
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

    if (!type || strcmp(type, "none") == 0) {
        syslog(LOG_DEBUG, "antitailgate: alarm action type is 'none', skipping");
        goto cleanup;
    }

    /* Cooldown check (bypass for test) */
    if (!is_test) {
        time_t now = time(NULL);
        if (now - last_action_time < COOLDOWN_SECONDS) {
            syslog(LOG_INFO, "antitailgate: alarm action skipped (cooldown)");
            goto cleanup;
        }
        last_action_time = now;
    }

    AlarmActionArgs *args = calloc(1, sizeof(AlarmActionArgs));
    if (!args)
        goto cleanup;

    args->is_test = is_test;
    strncpy(args->method, "GET", sizeof(args->method) - 1);

    /* Build URL per action type */
    if (strcmp(type, "virtual_input") == 0) {
        if (!host[0]) {
            syslog(LOG_ERR, "antitailgate: virtual_input requires AlarmActionHost");
            free(args);
            goto cleanup;
        }
        snprintf(args->url, sizeof(args->url),
                 "http://%s/axis-cgi/virtualinput/activate.cgi"
                 "?schemaversion=1&port=%s&duration=%s",
                 host, port, duration);
        strncpy(args->user, user, sizeof(args->user) - 1);
        strncpy(args->pass, pass, sizeof(args->pass) - 1);

    } else if (strcmp(type, "a9210_output") == 0) {
        if (!host[0]) {
            syslog(LOG_ERR, "antitailgate: a9210_output requires AlarmActionHost");
            free(args);
            goto cleanup;
        }
        /* Activate: action=<port>:/ then deactivate: action=<port>:\ */
        snprintf(args->url, sizeof(args->url),
                 "http://%s/axis-cgi/io/port.cgi?action=%s%%3A%%2F",
                 host, port);
        snprintf(args->deactivate_url, sizeof(args->deactivate_url),
                 "http://%s/axis-cgi/io/port.cgi?action=%s%%3A%%5C",
                 host, port);
        args->pulse_ms = atoi(duration);
        strncpy(args->user, user, sizeof(args->user) - 1);
        strncpy(args->pass, pass, sizeof(args->pass) - 1);

    } else if (strcmp(type, "custom_http") == 0) {
        if (!url[0]) {
            syslog(LOG_ERR, "antitailgate: custom_http requires AlarmActionUrl");
            free(args);
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
        free(args);
        goto cleanup;
    }

    /* Spawn detached thread */
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, alarm_action_thread, args) != 0) {
        syslog(LOG_ERR, "antitailgate: failed to create alarm action thread");
        free(args);
    }
    pthread_attr_destroy(&attr);

cleanup:
    free(type); free(host); free(port); free(duration);
    free(user); free(pass); free(url); free(method);
    free(payload); free(header);
}
