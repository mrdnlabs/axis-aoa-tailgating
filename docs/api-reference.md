# API Reference: Anti-Tailgating ACAP

All endpoints are served **only** through the Axis reverse proxy (TLS + device
authentication). The embedded web server binds `127.0.0.1:8080` and is **not**
reachable from the network — there is no direct `:8080` access.

```
https://<camera-ip>/local/antitailgate/<role>/<endpoint>
```

Two roles, enforced by the device at the proxy layer:

| Prefix | Access level | Endpoints |
|---|---|---|
| `/local/antitailgate/ingest/` | **operator** | `badge-read` |
| `/local/antitailgate/admin/`  | **admin**    | everything else |

### Request rules

- **Mutating endpoints require `POST`.** A `GET` to one returns `405`.
- **Admin mutating endpoints require** `Content-Type: application/json` **and**
  `X-Requested-With: antitailgate` (CSRF defense). Missing/ wrong → `415` / `403`.
- The ingest `badge-read` endpoint requires `POST`; a body, if present, must be
  `application/json`. An empty `POST` is accepted (mints an anonymous token).
- Non-canonical paths (encoded `..`, backslashes) are rejected with `400`.
- All examples use `curl -k --anyauth -u "$U:$P"`; set `$U`/`$P` from `.env.devices`.

---

## POST `/ingest/badge-read`  (operator)

Creates one access token. Optional JSON body enriches the event history.

**Body** (optional): `{"door": "Front Door", "card": "ABC123"}`
- `door` → stored as the event `source`; `card` (or legacy `badge_id`) → `badge_id`.
- `card`/`door` may be a JSON string or integer.

```sh
curl -k --anyauth -u "$U:$P" -X POST \
  -H "Content-Type: application/json" \
  -d '{"door":"Front Door","card":"ABC123"}' \
  "https://$CAM/local/antitailgate/ingest/badge-read"

# bodyless trigger (anonymous token)
curl -k --anyauth -u "$U:$P" -X POST \
  "https://$CAM/local/antitailgate/ingest/badge-read"
```

`200`: `{"status":"ok","message":"Token created","token_count":1,"badge_id":"ABC123","source":"Front Door"}`

---

## POST `/admin/threshold-crossing`  (admin)

Simulates a line crossing: consumes a token, or raises the alarm if none.
Requires the CSRF header.

```sh
curl -k --anyauth -u "$U:$P" -X POST \
  -H "Content-Type: application/json" -H "X-Requested-With: antitailgate" \
  "https://$CAM/local/antitailgate/admin/threshold-crossing"
```

`200`: `{"status":"ok","outcome":"authorized|alarm","tokens_remaining":N}`

---

## GET `/admin/status`  (admin)

```sh
curl -k --anyauth -u "$U:$P" "https://$CAM/local/antitailgate/admin/status"
```

```json
{
  "token_count": 0,
  "stats": { "badge_reads": 12, "authorized": 9, "alarms": 2 },
  "events": [ { "timestamp": "…Z", "type": "badge_read|line_crossing",
                "source": "…", "outcome": "token_created|authorized|alarm|expired",
                "badge_id": "…" } ],
  "alarms": [ { "timestamp": "…Z", "action_status": "request_succeeded|…" } ]
}
```

`stats` are monotonic session counters (correct even after the 50-entry history
ring wraps). `events`/`alarms` are the most recent 50 each.

---

## GET / POST `/admin/config`  (admin)

`GET` returns the current configuration. Secrets are **never** returned as
values — only `…Configured` booleans:

```json
{
  "TokenExpirationSeconds": 7, "AoaScenarioId": "1", "InputTriggerPort": "none",
  "AlarmClearSeconds": 2, "AlarmActionType": "none",
  "AlarmActionTlsMode": "verify", "AlarmActionHost": "", "AlarmActionPort": "1",
  "AlarmActionDuration": "5", "AlarmActionUser": "", "AlarmActionUrl": "",
  "AlarmActionMethod": "GET", "AlarmActionPayload": "",
  "AlarmActionPassConfigured": false, "AlarmActionHeaderConfigured": false,
  "AlarmActionPinConfigured": false
}
```

`POST` is **two-phase**: every field is validated first; if any field is
invalid the whole request is rejected with `400` and **nothing** is persisted.
Only changed fields are written. Unknown fields are rejected.

```sh
curl -k --anyauth -u "$U:$P" -X POST \
  -H "Content-Type: application/json" -H "X-Requested-With: antitailgate" \
  -d '{"TokenExpirationSeconds":10,"AlarmActionType":"custom_http",
       "AlarmActionTlsMode":"pin","AlarmActionUrl":"https://host/hook",
       "AlarmActionPin":"sha256//BASE64…"}' \
  "https://$CAM/local/antitailgate/admin/config"
```

`200`: `{"status":"ok","updated":["TokenExpirationSeconds",…]}`

### Secret fields (password / header)

`AlarmActionPass` and `AlarmActionHeader` are **keep-if-blank**: send them only
to change the value. Omitting them (or sending `""`) leaves the stored value
untouched. To erase: `{"AlarmActionClearPass": true}` /
`{"AlarmActionClearHeader": true}`.

### Field summary

| Field | Values |
|---|---|
| `TokenExpirationSeconds` | int 1–3600 |
| `AlarmClearSeconds` | int 1–60 |
| `AoaScenarioId` | 1–64 chars `[A-Za-z0-9-]`, non-empty |
| `InputTriggerPort` | `"none"` or a device-native 0-based port index |
| `AlarmActionType` | `none` \| `virtual_input` \| `a9210_output` \| `custom_http` |
| `AlarmActionTlsMode` | `verify` \| `pin` \| `noverify` \| `http` |
| `AlarmActionHost` | hostname / IP / `[ipv6]`, optional `:port` |
| `AlarmActionPort` | numeric 1–65535 |
| `AlarmActionDuration` | seconds, numeric 1–3600 |
| `AlarmActionMethod` | `GET` \| `POST` \| `PUT` (custom_http only) |
| `AlarmActionUrl` | `http://` or `https://`, no embedded credentials |
| `AlarmActionPin` | `sha256//…` (TLS pin mode) |

---

## POST `/admin/test-alarm-action`  (admin)

Runs the configured action once, **synchronously**, and returns the HTTP
result. Does **not** publish the TailgatingAlarm event.

`200`: `{"status":"ok","result":"HTTP 200"}` — `502` with the result on failure.

## POST `/admin/clear-history`  (admin)

Clears event/alarm history **and** drops all active tokens, and clears the
stateful alarm. `200`.

## POST `/admin/reset-defaults`  (admin)

Restores default configuration and clears runtime state. `200`.

## GET `/admin/test`  (admin)

Health check: `{"status":"ok","app":"antitailgate","version":"1.0.0"}`.
