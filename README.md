# Anti-Tailgating ACAP

A self-contained ACAP (Axis Camera Application Platform) running directly on an Axis camera to detect tailgating at controlled entry points.

## What It Does

- Subscribes natively to AXIS Object Analytics (AOA) line crossing events via the AXEvent API
- Accepts HTTP POST badge-read events from external systems (badge readers, door controllers, ACS) via an Axis-authenticated operator route
- Maintains a FIFO queue of access tokens with configurable TTL (default 7 seconds)
- Detects tailgating: line crossing with no valid token fires an alarm
- Publishes a native Axis stateful event (`tnsaxis:CameraApplicationPlatform/antitailgate/TailgatingAlarm`) that appears in the camera's event system and can drive action rules (recordings, relay outputs, PTZ, notifications)
- Hosts a web dashboard and settings UI directly on the camera

## Quick Start

### 1. Build

```bash
./build.sh aarch64
# Produces: app/Anti-Tailgating_1_0_0_aarch64.eap
```

For ARTPEC-7 cameras:
```bash
./build.sh armv7hf
```

### 2. Enable Unsigned Applications

Either via the camera web UI (**Settings → Apps → Allow unsigned applications**)
or over VAPIX:

```bash
curl -k --anyauth -u "$CAMERA_USER:$CAMERA_PASS" -X POST \
  "https://$CAMERA_IP/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
```

> A factory reset turns this back off (the default is `false` from AXIS OS
> 12.0), after which `upload.cgi` answers a bare `Error:2`. Axis plans to
> remove the toggle around AXIS OS 13 — the durable path is a signed `.eap`.

### 3. Install

Set device-specific values in `.env.devices` (gitignored — see `.env.devices.example` if provided) or substitute inline. Then upload via VAPIX. `curl` (with HTTPS) works on most setups:

```bash
curl -k --digest -u "$CAMERA_USER:$CAMERA_PASS" \
  -F "packfil=@app/Anti-Tailgating_1_0_0_aarch64.eap" \
  "https://$CAMERA_IP/axis-cgi/applications/upload.cgi"
```

Or with Python `requests` (HTTP works for older firmware):

```python
import requests
from requests.auth import HTTPDigestAuth
with open('app/Anti-Tailgating_1_0_0_aarch64.eap', 'rb') as f:
    r = requests.post(f'https://{CAMERA_IP}/axis-cgi/applications/upload.cgi',
                      auth=HTTPDigestAuth(CAMERA_USER, CAMERA_PASS),
                      files={'packfil': f}, verify=False)
print(r.status_code, r.text)
```

### 4. Open Web UI

```
https://<CAMERA_IP>/local/antitailgate/
```

## API

See [docs/api-reference.md](docs/api-reference.md) for full API documentation.

Key endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/local/antitailgate/ingest/badge-read` | POST | Operator-authenticated badge-read ingestion endpoint |
| `/local/antitailgate/admin/status` | GET | Admin-only status, event history, and alarm history |
| `/local/antitailgate/admin/config` | GET/POST | Admin-only config read/write |
| `/local/antitailgate/admin/threshold-crossing` | POST | Admin-only test/fallback crossing trigger |
| `/local/antitailgate/admin/test` | GET | Admin-only health check |
| `/local/antitailgate/admin/clear-history` | POST | Admin-only history reset |
| `/local/antitailgate/admin/reset-defaults` | POST | Admin-only config reset |

## Configuration Parameters

Stored via the AXParameter API; persist across restarts and firmware upgrades.

### Detection

| Parameter | Default | Description |
|-----------|---------|-------------|
| `TokenExpirationSeconds` | `7` | Token TTL in seconds |
| `AoaScenarioId` | `1` | AOA line crossing scenario ID |
| `InputTriggerPort` | `none` | Digital input port that mints a token (device-native **0-based** index: `0` = "Port 1"), or `none` |
| `AlarmClearSeconds` | `2` | Seconds before the stateful TailgatingAlarm event auto-clears |

### Alarm Action (outbound HTTP/VAPIX on alarm)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AlarmActionType` | `none` | `none`, `virtual_input`, `a9210_output`, or `custom_http` |
| `AlarmActionTlsMode` | `verify` | Outbound TLS: `verify` (system CA), `pin` (pinned key), `noverify` (self-signed), `http` (cleartext) |
| `AlarmActionPin` | `""` | `sha256//…` pinned public key, used when `TlsMode=pin` |
| `AlarmActionHost` | `""` | Target host for `virtual_input` / `a9210_output` (Axis device IP) |
| `AlarmActionPort` | `"1"` | Virtual input / output port number (1–65535) |
| `AlarmActionDuration` | `"5"` | Pulse duration in **seconds** (1–3600); the device times the pulse itself |
| `AlarmActionUser` | `""` | Username for auth on the action endpoint |
| `AlarmActionPass` | `""` | **Credential (masked).** See Security below. |
| `AlarmActionUrl` | `""` | Target URL for `custom_http` (`http`/`https`, no embedded credentials) |
| `AlarmActionMethod` | `GET` | HTTP method for `custom_http` (GET/POST/PUT) |
| `AlarmActionPayload` | `""` | Request body for `custom_http` |
| `AlarmActionHeader` | `""` | **Secret (masked).** Extra request header for `custom_http` (`Name: value`) |

## Security

### Transport and access control

- The API is reached only through Axis's reverse proxy (device authentication + TLS). The internal CivetWeb listener binds `127.0.0.1:8080`; there is no direct network access, and handlers are registered only at the proxy-prefixed paths.
- Two proxy roles: `/local/antitailgate/admin/*` (admin) and `/local/antitailgate/ingest/*` (operator).
- **Path canonicalization:** requests whose decoded path differs from their cleaned path (encoded `..`, backslashes) are rejected with `400`, so an operator cannot reach an admin handler via separator tricks.
- **CSRF / method contract:** mutating endpoints require `POST`; admin mutations additionally require `Content-Type: application/json` and `X-Requested-With: antitailgate`. A simple cross-origin form can set neither.
- **Input validation:** every field is validated (host/URL/header/scenario/port/pin shape, control-char rejection, UTF-8, integer ranges) *before* anything is persisted — a config POST is all-or-nothing.

### Outbound alarm actions

- `AlarmActionTlsMode` controls the outbound TLS posture: `verify` (public CA), `pin` (pin the peer's key via `AlarmActionPin`, the right choice for a self-signed Axis peripheral on a bare IP), `noverify`, or `http` (cleartext, last resort). Redirects are never followed on a credentialed request, so auth/body cannot be re-targeted to another host. The scheme is restricted to `http`/`https`.
- `a9210_output` uses the device-side timed-pulse form in a single request, so the relay drops even if this app crashes or is restarted mid-pulse.

### Credential handling

- `AlarmActionPass` and `AlarmActionHeader` use the AXParameter `password` type: masked in `param.cgi?action=list` (returns `******`), in the AXIS OS 12.7+ audit log, and in the settings UI. The ACAP reads them via `ax_parameter_get` for the outbound request.
- `/admin/config` (GET) never echoes a secret's value — only `…Configured` booleans. Both secrets are **keep-if-blank** on save; clear with `{"AlarmActionClearPass": true}` / `{"AlarmActionClearHeader": true}`.

### Residual risk

- AXParameter stores `password`-typed values unencrypted on the device's flash. A root compromise or physical flash extraction still exposes them; there is no Axis keystore for ACAPs.
- Upgrading from an older build that stored a secret as a plain `String` masks it from `param.cgi` going forward, but the on-flash residue remains until the next write. **Rotate any credential previously exposed via `param.cgi`.**

## Project Structure

```
app/
├── manifest.json          # ACAP manifest (schemaVersion 1.9.0)
├── main.c                 # Entry point, GMainLoop, signal handling, shutdown watchdog
├── config.c / config.h    # AXParameter get/set + fallback file (raw layer)
├── settings.c / settings.h# Validated in-memory config snapshot + change callbacks
├── json_util.c / .h       # Strict flat-object JSON parser (UTF-8, no dup keys)
├── validate.c / validate.h# Field validators (host/url/header/scenario/pin/numeric)
├── token_manager.c / .h   # Token queue, GMutex, expiry timer, history, session stats
├── event_subscriber.c/.h  # AXEvent subscription to AOA line-crossing (idempotent)
├── event_publisher.c/.h   # AXEvent declaration + stateful TailgatingAlarm
├── alarm_handler.c / .h    # Outbound action dispatch (libcurl, TLS modes, worker pool)
├── web_server.c / .h      # CivetWeb server on 127.0.0.1:8080 (proxy-only handlers)
├── civetweb.c / .h        # Vendored CivetWeb v1.16, sha256-pinned by build.sh (MIT)
├── test/                  # Host-side unit tests (json_util, validate) — run_tests.sh
└── html/
    ├── index.html         # Dashboard + Settings SPA (strict CSP, no inline JS)
    ├── app.js             # Polling, DOM updates, settings
    └── style.css          # Responsive styling
Dockerfile                 # Multi-arch ACAP build (axisecp/acap-native-sdk:12.9.0)
build.sh                   # Build helper (--no-cache; verifies CivetWeb sha256)
docs/                      # Documentation
```

## Tests

Pure modules (JSON parser, validators) have host-side unit tests that need no
device or SDK:

```bash
app/test/run_tests.sh      # compiles + runs in an Alpine container
```

## Documentation

- [Requirements](docs/requirements.md)
- [Architecture](docs/architecture.md)
- [API Reference](docs/api-reference.md)
- [Developer Guide](docs/developer-guide.md)
- [Camera Credentials](docs/camera-credentials.md)
- [Learnings](docs/learnings.md)
