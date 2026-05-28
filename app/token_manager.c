#include "token_manager.h"

#include <glib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static GMutex  g_mutex;
static Token   g_tokens[MAX_TOKENS];
static int     g_token_count = 0;
static uint64_t g_next_token_id = 1;
static uint64_t g_next_alarm_id = 1;
static int     g_expiration_seconds = 7;

static HistoryEvent g_events[MAX_HISTORY];
static int          g_event_count = 0;
static AlarmRecord  g_alarms[MAX_HISTORY];
static int          g_alarm_count = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers (caller must hold mutex)                           */
/* ------------------------------------------------------------------ */

static void push_event_locked(EventType type, EventOutcome outcome,
                               const char *source, const char *badge_id)
{
    if (g_event_count >= MAX_HISTORY) {
        /* Shift left to drop the oldest */
        memmove(&g_events[0], &g_events[1],
                sizeof(HistoryEvent) * (MAX_HISTORY - 1));
        g_event_count = MAX_HISTORY - 1;
    }
    HistoryEvent *ev = &g_events[g_event_count++];
    ev->timestamp = time(NULL);
    ev->type      = type;
    ev->outcome   = outcome;
    strncpy(ev->source,   source   ? source   : "",    sizeof(ev->source)   - 1);
    strncpy(ev->badge_id, badge_id ? badge_id : "",    sizeof(ev->badge_id) - 1);
    ev->source[sizeof(ev->source) - 1]     = '\0';
    ev->badge_id[sizeof(ev->badge_id) - 1] = '\0';
}

static uint64_t push_alarm_locked(const char *action_status)
{
    if (g_alarm_count >= MAX_HISTORY) {
        memmove(&g_alarms[0], &g_alarms[1],
                sizeof(AlarmRecord) * (MAX_HISTORY - 1));
        g_alarm_count = MAX_HISTORY - 1;
    }
    AlarmRecord *al = &g_alarms[g_alarm_count++];
    al->id        = g_next_alarm_id++;
    al->timestamp = time(NULL);
    strncpy(al->action_status, action_status ? action_status : "unknown",
            sizeof(al->action_status) - 1);
    al->action_status[sizeof(al->action_status) - 1] = '\0';
    return al->id;
}

static void update_alarm_locked(uint64_t alarm_id, const char *action_status)
{
    for (int i = g_alarm_count - 1; i >= 0; i--) {
        if (g_alarms[i].id == alarm_id) {
            strncpy(g_alarms[i].action_status,
                    action_status ? action_status : "unknown",
                    sizeof(g_alarms[i].action_status) - 1);
            g_alarms[i].action_status[sizeof(g_alarms[i].action_status) - 1] = '\0';
            return;
        }
    }
}

/* Remove expired tokens in-place. Caller holds mutex. */
static void expire_locked(void)
{
    time_t now = time(NULL);
    int i = 0;
    while (i < g_token_count) {
        if (g_tokens[i].expiry <= now) {
            push_event_locked(EVENT_BADGE_READ, OUTCOME_EXPIRED,
                              g_tokens[i].source, g_tokens[i].badge_id);
            /* Remove by shifting */
            memmove(&g_tokens[i], &g_tokens[i + 1],
                    sizeof(Token) * (g_token_count - i - 1));
            g_token_count--;
            /* don't increment i */
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void token_manager_init(int expiration_seconds)
{
    g_mutex_init(&g_mutex);
    g_expiration_seconds = expiration_seconds;
    g_token_count  = 0;
    g_event_count  = 0;
    g_alarm_count  = 0;
    g_next_token_id = 1;
    g_next_alarm_id = 1;
    syslog(LOG_INFO, "antitailgate: token_manager initialized (TTL=%ds)",
           expiration_seconds);
}

int token_add(const char *badge_id, const char *source)
{
    const char *src = (source && source[0]) ? source : "badge";

    g_mutex_lock(&g_mutex);

    if (g_token_count >= MAX_TOKENS) {
        syslog(LOG_WARNING, "antitailgate: token queue full, dropping oldest");
        memmove(&g_tokens[0], &g_tokens[1],
                sizeof(Token) * (MAX_TOKENS - 1));
        g_token_count--;
    }

    Token *t = &g_tokens[g_token_count++];
    t->id     = g_next_token_id++;
    t->expiry = time(NULL) + g_expiration_seconds;
    strncpy(t->badge_id, badge_id ? badge_id : "unknown",
            sizeof(t->badge_id) - 1);
    t->badge_id[sizeof(t->badge_id) - 1] = '\0';
    strncpy(t->source, src, sizeof(t->source) - 1);
    t->source[sizeof(t->source) - 1] = '\0';

    push_event_locked(EVENT_BADGE_READ, OUTCOME_TOKEN_CREATED,
                      src, badge_id);

    int count = g_token_count;
    g_mutex_unlock(&g_mutex);

    syslog(LOG_INFO, "antitailgate: token created (badge=%s, source=%s, TTL=%ds, count=%d)",
           badge_id ? badge_id : "unknown", src, g_expiration_seconds, count);
    return count;
}

bool token_consume(const char *source)
{
    g_mutex_lock(&g_mutex);

    expire_locked();

    if (g_token_count == 0) {
        /* ALARM: no valid token */
        push_event_locked(EVENT_LINE_CROSSING, OUTCOME_ALARM, source, NULL);
        g_mutex_unlock(&g_mutex);

        syslog(LOG_WARNING, "antitailgate: TAILGATING ALARM (source=%s)",
               source ? source : "unknown");
        return false;
    }

    /* Consume oldest token (FIFO) */
    char badge_id[64];
    strncpy(badge_id, g_tokens[0].badge_id, sizeof(badge_id) - 1);
    badge_id[sizeof(badge_id) - 1] = '\0';

    memmove(&g_tokens[0], &g_tokens[1],
            sizeof(Token) * (g_token_count - 1));
    g_token_count--;

    push_event_locked(EVENT_LINE_CROSSING, OUTCOME_AUTHORIZED, source, badge_id);

    g_mutex_unlock(&g_mutex);

    syslog(LOG_INFO, "antitailgate: authorized entry (badge=%s, source=%s)",
           badge_id, source ? source : "unknown");
    return true;
}

int token_count(void)
{
    g_mutex_lock(&g_mutex);
    expire_locked();
    int count = g_token_count;
    g_mutex_unlock(&g_mutex);
    return count;
}

void token_expire_tick(void)
{
    g_mutex_lock(&g_mutex);
    expire_locked();
    g_mutex_unlock(&g_mutex);
}

int history_snapshot(HistoryEvent *out_events, int max_events,
                     AlarmRecord *out_alarms, int max_alarms,
                     int *out_alarm_count)
{
    g_mutex_lock(&g_mutex);

    int ev_count = g_event_count < max_events ? g_event_count : max_events;
    if (out_events && ev_count > 0)
        memcpy(out_events, g_events, sizeof(HistoryEvent) * ev_count);

    int al_count = g_alarm_count < max_alarms ? g_alarm_count : max_alarms;
    if (out_alarms && al_count > 0)
        memcpy(out_alarms, g_alarms, sizeof(AlarmRecord) * al_count);
    if (out_alarm_count)
        *out_alarm_count = al_count;

    g_mutex_unlock(&g_mutex);
    return ev_count;
}

uint64_t alarm_record_create(const char *action_status)
{
    g_mutex_lock(&g_mutex);
    uint64_t alarm_id = push_alarm_locked(action_status);
    g_mutex_unlock(&g_mutex);
    return alarm_id;
}

void alarm_record_update(uint64_t alarm_id, const char *action_status)
{
    g_mutex_lock(&g_mutex);
    update_alarm_locked(alarm_id, action_status);
    g_mutex_unlock(&g_mutex);
}

void history_clear(void)
{
    g_mutex_lock(&g_mutex);
    g_event_count = 0;
    g_alarm_count = 0;
    g_mutex_unlock(&g_mutex);
    syslog(LOG_INFO, "antitailgate: history cleared");
}

void token_set_expiration(int seconds)
{
    g_mutex_lock(&g_mutex);
    g_expiration_seconds = seconds;
    g_mutex_unlock(&g_mutex);
    syslog(LOG_INFO, "antitailgate: token TTL updated to %ds", seconds);
}

void token_manager_cleanup(void)
{
    g_mutex_clear(&g_mutex);
}
