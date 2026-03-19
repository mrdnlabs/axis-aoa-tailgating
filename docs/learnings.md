# Learnings: Anti-Tailgating ACAP

Confirmed findings from development and integration work. These capture decisions, surprises, and tribal knowledge that is not obvious from the source code or Axis documentation.

---

## 1. AXIS OS for Secure Entry Is Not a Separate OS

The Axis A1210 door controller runs standard AXIS OS 12.8 — not a special "Secure Entry OS." The difference is only in the web UI: the A1210 uses an `accesscontrol-e2e` frontend that replaces the standard camera UI. This means:

- The "System → Events → Rules" page is absent from the web interface
- All other VAPIX APIs (device info, application management, SOAP action rules) work identically to a camera

This was not obvious from Axis documentation, which implies a separate product stack for door controllers.

---

## 2. VAPIX SOAP Action Rules on the A1210

The A1210 SOAP endpoint at `/vapix/services` is fully functional for action rule management:

- `GetActionTemplates` — lists available action types
- `GetActionRules` — lists existing rules
- `GetRecipientTemplates` — lists notification target types
- `AddActionRule` — creates a new rule
- `GetActionConditions` — returns `ter:ActionNotSupported` (cannot enumerate trigger conditions this way)

The `com.axis.action.fixed.notification.http` template supports HTTP POST with a configurable body and Content-Type. This is the mechanism for sending badge-read events from the A1210 to the camera's `/badge-read` endpoint.

Action rules on the A1210 must be configured programmatically via SOAP. There is no web UI path for this on the `accesscontrol-e2e` interface.

---

## 3. curl vs Python for Digest + Multipart Upload

`curl` has a known compatibility issue combining `--digest` auth with `--form` (multipart) upload to Axis cameras. The request fails or produces an auth error.

**Solution**: Use Python `requests` with `HTTPDigestAuth`:

```python
import requests
from requests.auth import HTTPDigestAuth

with open('app/Anti-Tailgating_1_0_0_aarch64.eap', 'rb') as f:
    r = requests.post(
        'http://192.168.1.238/axis-cgi/applications/upload.cgi',
        auth=HTTPDigestAuth('admin', 'admin'),
        files={'packfil': ('Anti-Tailgating_1_0_0_aarch64.eap', f, 'application/octet-stream')}
    )
```

This applies specifically to the `upload.cgi` endpoint. Regular VAPIX API calls (JSON POST, GET) work fine with curl and `--digest`.

---

## 4. "Allow Unsigned Applications" Cannot Be Set via VAPIX

In AXIS OS 11 and later, the setting to allow unsigned ACAP packages is intentionally restricted to the camera web UI. There is no VAPIX parameter or API that can set it.

Steps: Camera web UI → Settings → Apps → Allow unsigned applications → Confirm dialog.

Re-enable this after firmware upgrades if needed.

---

## 5. AXEvent Stateful (Property) Events

Declaring an event with `ax_event_handler_declare(..., TRUE, ...)` creates a stateful (property) event rather than a pulse event. Key behaviors:

- The event appears immediately as a trigger option in the camera's action rule system under "Application → Anti-Tailgating → TailgatingAlarm"
- The event must be declared from the GLib main loop thread (i.e., during application initialization, not from a CivetWeb thread)
- The camera retains the last known state of the event across sends

Use stateful events for conditions (alarm is active / not active). Use pulse events for momentary occurrences.

---

## 6. axevent Send Thread Safety

`ax_event_handler_send_event` must be called from the GLib main loop thread. If called from a CivetWeb handler thread, it can cause undefined behavior or silent failures.

**Solution**: Use `g_idle_add()` to schedule the send on the main loop:

```c
typedef struct { bool active; } SendPayload;

static gboolean send_idle_cb(gpointer data) {
    SendPayload *p = data;
    ax_event_handler_send_event(handler, event_id, key_values_for(p->active));
    free(p);
    return G_SOURCE_REMOVE;
}

void event_publisher_send_alarm(bool active) {
    SendPayload *p = malloc(sizeof(SendPayload));
    p->active = active;
    g_idle_add(send_idle_cb, p);
}
```

This pattern is safe to call from any thread. The payload is heap-allocated so it survives until the idle callback runs.

---

## 7. CivetWeb `read_body` Pattern

Reading a POST body from a CivetWeb handler:

1. Check `mg_get_header(conn, "Content-Length")` — if absent or zero, skip `mg_read()`
2. Read up to `content_length` bytes using `mg_read(conn, buf, content_length)`
3. Body is only available on POST requests

Attempting to call `mg_read()` with `content_length == 0` or on a GET request can block or return garbage.

Minimum safe pattern:
```c
const char *cl = mg_get_header(conn, "Content-Length");
int len = cl ? atoi(cl) : 0;
if (len > 0 && len < MAX_BODY) {
    mg_read(conn, buf, len);
    buf[len] = '\0';
    // parse JSON...
}
```

---

## 8. Badge-Read Endpoint Enrichment Pattern

Accepting an optional JSON body with graceful fallback is the right pattern for backward-compatible enrichment of a webhook endpoint:

- If body present and valid JSON with `"door"` key: use `door` as `source`, `card` as `badge_id`
- If body absent or not JSON: fall back to existing behavior (source="http", badge_id from query string or "unknown")

This allows the endpoint to be used by both simple badge readers (no body) and door controllers (with body) without requiring callers to upgrade.

The `source` field in `HistoryEvent` was originally 16 bytes — too small for meaningful door names. It was expanded to 64 bytes. Confirm struct size when changing field widths.

---

## 9. Axis A1210 Access Granted Event Topic

The A1210 device event topic list includes `tns1:SecureEntry/tnsaxis:Door` events for door position, forced entry, and open-too-long conditions. The exact topic for a credential-level "access granted" event requires a configured and active badge reader to verify — it is likely `tns1:AccessControl/AccessGranted` or a sub-topic under the SecureEntry namespace. This has not been confirmed in testing as of March 2026.

For the door controller integration, this is not needed: the A1210 action rule fires the HTTP POST on its own trigger logic. The ACAP does not subscribe to A1210 events — it only receives the HTTP POST.

---

## 10. Apache Proxy Reliability

The web UI sets `const API_BASE = \`http://\${hostname}:8080\`` and calls the CivetWeb server directly on port 8080. The Apache reverse proxy (`ProxyPass /local/antitailgate/api/ http://localhost:8080/`) configured via the `manifest.json` `reverseProxy` block may or may not work reliably depending on the camera's mod_proxy configuration.

**Recommendation**: Use port 8080 directly for all external integrations (door controllers, badge readers, test scripts). Do not rely on the Apache proxy path for anything outside the web UI's own internal API calls.

---

## 11. HistoryEvent Source Field Size

The `source` field in the `HistoryEvent` struct was originally 16 bytes — sufficient for short strings like `"aoa"` or `"http"` but too small for door names like `"Front Door (Main Entrance)"`. The field was expanded to 64 bytes.

When making similar changes: update the struct, update any `strncpy` calls that reference the old size, and rebuild fully. The compiler will not warn about this mismatch.

---

## 12. AXParameter vs File-Based Config

AXParameter is the correct persistence mechanism for ACAP configuration on AXIS OS 12.8. Parameters must be declared in `manifest.json` under `paramConfig` with three required fields:

```json
{
  "name": "TokenExpirationSeconds",
  "default": "7",
  "type": "Int"
}
```

Type values must have a capital first letter: `"String"`, `"Int"`. Lowercase types (`"int"`, `"string"`) are rejected by the SDK and cause manifest validation failures.

A file-based fallback to `localdata/config.json` was implemented but AXParameter works reliably and the fallback has not been needed in practice on AXIS OS 12.8.
