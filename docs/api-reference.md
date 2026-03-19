# API Reference: Anti-Tailgating ACAP

The embedded CivetWeb server listens on port 8080. All endpoints are accessible directly at:

```
http://<camera-ip>:8080/<endpoint>
```

They are also available via the Apache reverse proxy (reliability varies by camera configuration):

```
http://<camera-ip>/local/antitailgate/api/<endpoint>
```

The web UI uses the direct port 8080 URL. External integrations (door controllers, badge readers) should also use port 8080 directly.

---

## Endpoints

### POST `/badge-read`

Creates one access token. Accepts an optional JSON body to enrich event history with door and card information.

**Request body** (optional):
```json
{
  "door": "Front Door",
  "card": "ABC123"
}
```

When the body is present, `door` is stored as the `source` field and `card` as the `badge_id` field in the event history. If no body is provided, `source` defaults to `"http"` and `badge_id` comes from the `badge_id` query parameter (or `"unknown"`).

**Query parameters** (optional, used when no JSON body is provided):
- `badge_id` — Badge identifier string for logging

**Examples**:

Door controller with JSON body:
```bash
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"door":"Front Door","card":"ABC123"}' \
  "http://192.168.1.238:8080/badge-read"
```

Simple POST with no body:
```bash
curl -s -X POST "http://192.168.1.238:8080/badge-read"
```

With badge ID in query string:
```bash
curl -s -X POST "http://192.168.1.238:8080/badge-read?badge_id=card-42"
```

**Response** `200 OK`:
```json
{
  "status": "ok",
  "message": "Token created",
  "token_count": 1,
  "badge_id": "ABC123"
}
```

---

### POST `/threshold-crossing`

Simulates a line crossing event. Used for testing or as a fallback when AOA is not installed.

**Request**: No body required.

**Example**:
```bash
curl -s -X POST "http://192.168.1.238:8080/threshold-crossing"
```

**Response** `200 OK` — token consumed (authorized entry):
```json
{
  "status": "ok",
  "outcome": "authorized",
  "tokens_remaining": 0
}
```

**Response** `200 OK` — no token (tailgating alarm):
```json
{
  "status": "ok",
  "outcome": "alarm",
  "tokens_remaining": 0
}
```

When outcome is `"alarm"`, the application fires the native Axis `TailgatingAlarm` event, which can drive camera action rules.

---

### GET `/status`

Returns current system status including token count, event history, and alarm history.

**Example**:
```bash
curl -s "http://192.168.1.238:8080/status"
```

**Response** `200 OK`:
```json
{
  "token_count": 1,
  "events": [
    {
      "timestamp": "2026-03-08T10:15:01Z",
      "type": "badge_read",
      "source": "Front Door",
      "badge_id": "ABC123",
      "outcome": "token_created"
    },
    {
      "timestamp": "2026-03-08T10:15:06Z",
      "type": "line_crossing",
      "source": "aoa",
      "badge_id": "",
      "outcome": "authorized"
    },
    {
      "timestamp": "2026-03-08T10:15:20Z",
      "type": "line_crossing",
      "source": "aoa",
      "badge_id": "",
      "outcome": "alarm"
    }
  ],
  "alarms": [
    {
      "timestamp": "2026-03-08T10:15:20Z"
    }
  ]
}
```

**Field descriptions**:
- `token_count` — Number of active (unexpired) tokens currently queued
- `events` — Last 50 events (badge reads + line crossings), newest last
- `events[].source` — Origin of the event: door name (from JSON body), `"http"` (plain POST), or `"aoa"` (AOA event)
- `events[].badge_id` — Card number from JSON body, query string, or empty string
- `alarms` — Last 50 alarm records, newest last

---

### GET `/config`

Returns current configuration as JSON.

**Example**:
```bash
curl -s "http://192.168.1.238:8080/config"
```

**Response** `200 OK`:
```json
{
  "TokenExpirationSeconds": 7,
  "AoaScenarioId": 1
}
```

---

### POST `/config`

Updates one or more configuration parameters. Only supplied fields are updated.

**Request body** (JSON, partial updates supported):
```json
{
  "TokenExpirationSeconds": 10,
  "AoaScenarioId": 2
}
```

**Example**:
```bash
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"TokenExpirationSeconds": 10}' \
  "http://192.168.1.238:8080/config"
```

**Response** `200 OK`:
```json
{
  "status": "ok",
  "updated": ["TokenExpirationSeconds"]
}
```

**Response** `400 Bad Request` (invalid JSON):
```json
{
  "status": "error",
  "message": "Invalid JSON body"
}
```

---

### GET `/test`

Health check endpoint. Always returns 200 if the application is running.

**Example**:
```bash
curl -s "http://192.168.1.238:8080/test"
```

**Response** `200 OK`:
```json
{
  "status": "ok",
  "app": "antitailgate",
  "version": "1.0.0"
}
```

---

### POST `/clear-history`

Clears the in-memory event and alarm history.

**Example**:
```bash
curl -s -X POST "http://192.168.1.238:8080/clear-history"
```

**Response** `200 OK`:
```json
{
  "status": "ok",
  "message": "History cleared"
}
```

---

### POST `/reset-defaults`

Resets all configuration parameters to their factory defaults.

**Example**:
```bash
curl -s -X POST "http://192.168.1.238:8080/reset-defaults"
```

**Response** `200 OK`:
```json
{
  "status": "ok",
  "message": "Defaults restored"
}
```

---

## Door Controller Integration

Configure the door controller (e.g., Axis A1210) to POST a JSON body to the badge-read endpoint on each access-granted event.

**Target URL**:
```
http://192.168.1.238:8080/badge-read
```

**Request body**:
```json
{"door": "Front Door", "card": "%CardNumber%"}
```

Replace `%CardNumber%` with the door controller's variable substitution syntax for the credential number.

On the Axis A1210, action rules must be configured via VAPIX SOAP (not the web UI) because the A1210's `accesscontrol-e2e` web interface does not expose a "System → Events → Rules" page. Use the `com.axis.action.fixed.notification.http` action template at `/vapix/services`. See [docs/learnings.md](learnings.md) for details.

---

## Axis Event System Integration

On every tailgating alarm the application publishes:

| Property | Value |
|---|---|
| Topic | `tnsaxis:CameraApplicationPlatform/antitailgate/TailgatingAlarm` |
| Key | `active` |
| Type | `bool` |
| Value on alarm | `true` |

This event appears in the camera under **System → Events → Rules → Add rule → Trigger → Application → Anti-Tailgating → TailgatingAlarm**. It can drive any built-in camera action without requiring an external alarm system.

---

## Error Responses

All endpoints return standard HTTP status codes:

| Code | Meaning |
|------|---------|
| `200` | Success |
| `400` | Bad request (malformed JSON, missing required params) |
| `405` | Method not allowed |
| `500` | Internal server error |

Error response body:
```json
{
  "status": "error",
  "message": "<description>"
}
```
