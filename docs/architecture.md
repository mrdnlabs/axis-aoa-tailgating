# Architecture: Anti-Tailgating ACAP

## System Context

```
+------------------+          HTTP badge read (POST /badge-read)     +-----------------------------+
|  Badge Reader /  |  ----------------------------------------------> |                             |
|  Door Controller |    JSON body: {"door":"...","card":"..."}        |    Axis Camera (ARTPEC-8)   |
|  (e.g. A1210)    |                                                  |                             |
+------------------+                                                  |  +-----------------------+  |
                                                                      |  |  AXIS Object Analytics|  |
+------------------+     AOA line crossing event                      |  |  (line crossing cfg)  |  |
|  Physical Door   |  --> (person crosses line)  ------------------> |  +-----------+-----------+  |
|  Entry Point     |                                                  |              |               |
+------------------+                                                  |  +-----------v-----------+  |
                                                                      |  |  Anti-Tailgating ACAP |  |
+------------------+     Native Axis stateful event                   |  |  (this application)   |  |
|  Axis Event      |  <---------------------------------------------- |  +-----------+-----------+  |
|  System / Rules  |    TailgatingAlarm (active=bool)                 |              |               |
+------------------+                                                  |  +-----------v-----------+  |
                                                                      |  |  Apache + Web UI      |  |
+------------------+     HTTP (browser -> Apache or port 8080)        |  |  /local/antitailgate/ |  |
|  Admin Browser   |  <---------------------------------------------> |  +-----------------------+  |
+------------------+                                                  +-----------------------------+
```

---

## Component Diagram

```
main.c
├── Initializes all modules
├── Starts GLib GMainLoop (blocking)
└── Handles SIGTERM/SIGINT for clean shutdown

config.c / config.h
├── AXParameter API wrappers
├── get_param_string(name, default) -> char*
├── get_param_int(name, default)    -> int
└── set_param(name, value)          -> bool

token_manager.c / token_manager.h
├── Token struct: { expiry_time, id }
├── HistoryEvent struct: { timestamp, type, source[64], badge_id[64], outcome }
├── GMutex for thread-safe access
├── token_add()          -- called from web_server on badge-read
├── token_consume()      -- called from event_subscriber on line crossing
├── token_expire_tick()  -- GLib timer callback, runs every 1s in GMainLoop
└── token_count()        -- called from web_server for status

event_subscriber.c / event_subscriber.h
├── AXEventHandler setup
├── Subscribes to AOA line crossing topic for configured scenario ID
├── on_line_crossing() callback:
│   ├── Tries token_consume()
│   ├── If consumed: log "authorized entry" to history
│   └── If not: log alarm to history, call alarm_handler_notify()
└── Graceful skip if AOA unavailable

event_publisher.c / event_publisher.h
├── Declares TailgatingAlarm stateful event on startup (ax_event_handler_declare, TRUE)
├── event_publisher_send_alarm(bool active) -- schedules send via g_idle_add()
└── Idle callback runs on GLib main loop thread (thread-safe)

alarm_handler.c / alarm_handler.h
├── Thin wrapper: logs alarm
└── Calls event_publisher_send_alarm(true)

web_server.c / web_server.h
├── CivetWeb embedded HTTP server on port 8080
├── Endpoint handlers:
│   ├── /badge-read          POST -> parse optional JSON body, token_add()
│   ├── /threshold-crossing  POST -> simulate line crossing
│   ├── /status              GET  -> JSON status (token_count, events, alarms)
│   ├── /config              GET/POST -> config read/write
│   ├── /test                GET  -> health check
│   ├── /clear-history       POST -> clear in-memory history
│   └── /reset-defaults      POST -> reset config to defaults
└── All handlers are thread-safe (GMutex on shared state)

civetweb.c / civetweb.h
└── Bundled CivetWeb v1.16 (MIT license)
```

---

## Threading Model

```
Main Thread (GLib GMainLoop)
├── AXEvent callback: on_line_crossing()
│   ├── token_consume() [acquires GMutex]
│   └── alarm_handler_notify()
│       └── event_publisher_send_alarm(true) -> g_idle_add(send_idle_cb, payload)
├── GLib timer (every 1s): token_expire_tick()
│   └── token_expire() [acquires GMutex]
└── g_idle_add callback: send_idle_cb()
    └── ax_event_handler_send_event() [must run on main loop thread]

CivetWeb Thread Pool (N threads, default 10)
├── /badge-read handler
│   ├── mg_read() body, parse JSON {"door","card"}
│   └── token_add(source, badge_id) [acquires GMutex]
├── /threshold-crossing handler
│   └── Simulates line crossing (same logic as AXEvent callback)
│       └── event_publisher_send_alarm() -> g_idle_add() [thread-safe dispatch]
├── /status handler
│   └── token_count(), history_snapshot() [acquires GMutex]
└── /config GET/POST handler
    └── config_get/set() [AXParameter, no mutex needed]
```

**Key invariants**:
1. Token queue and event/alarm history — protected by `g_token_mutex`
2. Config — read/written via AXParameter API (internally thread-safe)
3. `ax_event_handler_send_event` — always called from GLib main loop thread via `g_idle_add()`

---

## Data Flows

### Badge Read Path — Normal Entry (Door Controller Integration)
```
Door Controller --[POST :8080/badge-read]-->  CivetWeb
  Body: {"door":"Front Door","card":"ABC123"}
CivetWeb: parse body, source="Front Door", badge_id="ABC123"
CivetWeb: acquire mutex, push token(expiry = now + TTL), append history event
CivetWeb: release mutex, return 200 {"status":"ok","tokens":N}
... later ...
Person crosses line --> AOA --> AXEvent callback (main loop thread)
callback: acquire mutex, pop oldest token, append history event, release mutex
callback: log "authorized entry"
```

### Badge Read Path — Simple (No Body)
```
Badge reader --[POST :8080/badge-read]--> CivetWeb
  No body (or query string ?badge_id=card-42)
CivetWeb: source="http", badge_id from query string or "unknown"
CivetWeb: acquire mutex, push token, release mutex
CivetWeb: return 200 {"status":"ok","tokens":N}
```

### Line Crossing — Tailgating Alarm
```
Person crosses line without badge read
AOA --> AXEvent callback (main loop thread)
callback: acquire mutex, no tokens available, release mutex
callback: append alarm to history
callback: alarm_handler_notify()
  -> event_publisher_send_alarm(true)
  -> g_idle_add(send_idle_cb, {active=true})
  -> send_idle_cb() runs on main loop:
     ax_event_handler_send_event(handler, event_id, key_values)
     Axis event system receives TailgatingAlarm(active=true)
     Any configured action rules fire (recording, relay, etc.)
```

### Web UI Polling
```
Browser --[GET /local/antitailgate/]--> Apache --[serve static index.html]
Browser JS: fetch("http://<camera-ip>:8080/status") every 2s
CivetWeb /status: return JSON {token_count, events[], alarms[]}
Browser JS: update DOM
```

---

## URL Structure

| URL Pattern | Served By | Description |
|---|---|---|
| `/local/antitailgate/` | Apache (static) | index.html |
| `/local/antitailgate/index.html` | Apache (static) | Main SPA |
| `/local/antitailgate/app.js` | Apache (static) | JavaScript |
| `/local/antitailgate/style.css` | Apache (static) | Stylesheet |
| `http://<camera-ip>:8080/*` | CivetWeb (direct) | REST API (primary path) |
| `/local/antitailgate/api/*` | CivetWeb via Apache proxy | REST API (alternate path, reliability not guaranteed) |

The web UI uses the direct port 8080 URL for all API calls. The `reverseProxy` block in `manifest.json` configures Apache automatically, but direct port 8080 access is the reliable path for external integrations.

---

## State Diagram: Token Lifecycle

```
[Badge Read received]
        |
        v
   Token created
   expiry = now + TTL
        |
   +----+----+
   |         |
   v         v
[Line     [TTL expires]
crossing]      |
   |       Token removed
   v       from queue (alarm NOT fired)
Token consumed
(authorized entry)
```

---

## Axis Event System Integration

The `TailgatingAlarm` event is declared on startup as a stateful (property) event. This makes it immediately visible in the camera's event system:

- Camera web UI path: **System → Events → Rules → Add rule → Trigger → Application → Anti-Tailgating → TailgatingAlarm**
- Event key: `active` (bool) — `true` when alarm fires
- The event is self-resetting: a subsequent `active=false` send is not currently implemented; the event remains `true` until the application restarts

Action rules that can be driven by this event:
- Start recording on SD card or to a network share
- Activate a relay output (door lock, alarm siren)
- Send an email or HTTP notification (configured in camera, not in ACAP)
- Trigger a PTZ preset move

---

## Persistence Model

All configuration is stored via AXParameter API. Parameters are defined in `manifest.json` `paramConfig` and initialized to defaults on first install.

AXParameter works reliably on AXIS OS 12.8. A file-based fallback to `/usr/local/packages/antitailgate/localdata/config.json` is implemented but is not expected to be needed.

Event and alarm history are in-memory only and do not persist across restarts.
