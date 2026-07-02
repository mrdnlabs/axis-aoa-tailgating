#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_TOKENS   64
#define MAX_HISTORY  50

typedef struct {
    uint64_t  id;
    int64_t   expiry_mono;  /* CLOCK_MONOTONIC seconds; NTP-jump safe */
    char      badge_id[64];
    char      source[64];
} Token;

typedef enum {
    EVENT_BADGE_READ,
    EVENT_LINE_CROSSING,
} EventType;

typedef enum {
    OUTCOME_TOKEN_CREATED,
    OUTCOME_AUTHORIZED,
    OUTCOME_ALARM,
    OUTCOME_EXPIRED,
} EventOutcome;

typedef struct {
    time_t       timestamp;
    EventType    type;
    EventOutcome outcome;
    char         source[64];   /* "aoa" | "http" | door name from door controller */
    char         badge_id[64];
} HistoryEvent;

typedef struct {
    uint64_t id;
    time_t   timestamp;
    char     action_status[32];
} AlarmRecord;

/**
 * Initialize the token manager (creates mutex, etc.).
 * expiration_seconds: default token TTL.
 */
void token_manager_init(int expiration_seconds);

/** Add a token for a given badge ID and source. Returns new token count.
 *  source: e.g. "badge", "http", or a door name from a door controller */
int token_add(const char *badge_id, const char *source);

/**
 * Attempt to consume the oldest valid token.
 * source: "aoa" or "http"
 * Returns true if a token was consumed (authorized), false if alarm.
 */
bool token_consume(const char *source);

/** Return the number of currently active (unexpired) tokens. */
int token_count(void);

/** Remove expired tokens. Called by GLib timer every second. */
void token_expire_tick(void);

/** Copy up to MAX_HISTORY events into out_events. Returns count written. */
int history_snapshot(HistoryEvent *out_events, int max_events,
                     AlarmRecord *out_alarms, int max_alarms,
                     int *out_alarm_count);

/** Create an alarm history record and return its identifier. */
uint64_t alarm_record_create(const char *action_status);

/** Update an existing alarm history record. */
void alarm_record_update(uint64_t alarm_id, const char *action_status);

/** Clear all history. */
void history_clear(void);

/** Drop every in-flight token.  Used by admin reset paths. */
void token_clear_all(void);

/** Update the token expiration duration (called on config change). */
void token_set_expiration(int seconds);

/** Clean up the token manager. */
void token_manager_cleanup(void);
