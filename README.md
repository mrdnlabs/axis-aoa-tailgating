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

On the camera web UI: **Settings → Apps → Allow unsigned applications**

This cannot be set via VAPIX — it must be done through the browser UI.

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
| `InputTriggerPort` | `none` | Digital input port to treat as badge-read (or `none`) |
| `AlarmClearSeconds` | `2` | Seconds before the stateful TailgatingAlarm event auto-clears |

### Alarm Action (outbound HTTP/VAPIX on alarm)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AlarmActionType` | `none` | `none`, `virtual_input`, `a9210_output`, or `custom_http` |
| `AlarmActionHost` | `""` | Target host for `virtual_input` / `a9210_output` (Axis device IP) |
| `AlarmActionPort` | `"1"` | Virtual input / output port number |
| `AlarmActionDuration` | `"5"` | Pulse duration in seconds (`virtual_input` / `a9210_output`) |
| `AlarmActionUser` | `""` | Username for digest auth on the action endpoint |
| `AlarmActionPass` | `""` | **Credential. See Security below.** |
| `AlarmActionUrl` | `""` | Target URL for `custom_http` action |
| `AlarmActionMethod` | `GET` | HTTP method for `custom_http` (GET/POST/PUT) |
| `AlarmActionPayload` | `""` | Request body for `custom_http` |
| `AlarmActionHeader` | `""` | Extra request header for `custom_http` (`Name: value`) |

## Security

### Transport and access control

- The web UI and API are reached through Axis's reverse proxy, which enforces digest authentication and TLS. The internal CivetWeb listener binds to `127.0.0.1:8080` only.
- Two proxy roles are exposed: `/local/antitailgate/admin/*` (admin) and `/local/antitailgate/ingest/*` (operator).
- Input validation: `AlarmActionHost` must be a hostname or IP (no schemes, no spaces); `AlarmActionPort` and `AlarmActionDuration` must be numeric and in range. JSON bodies are validated against a flat-object schema with per-field length limits.

### Alarm-action credential handling

- `AlarmActionPass` is declared with the AXParameter `password:maxlen=127` type. The runtime masks the value in:
  - `param.cgi?action=list` (returns `******` instead of the value)
  - The AXIS OS 12.7+ audit log
  - The settings UI (rendered as a password field)
- The ACAP itself reads the value via `ax_parameter_get` and uses it as the HTTP digest credential for the configured `AlarmActionHost`.
- `/admin/config` (GET) **does not** echo `AlarmActionPass`. It returns `AlarmActionPassConfigured` (boolean). Set the password by POSTing `{"AlarmActionPass": "..."}` and clear it with `{"AlarmActionClearPass": true}`.

### Residual risk

- AXParameter stores `password`-typed values unencrypted on the device's flash. A root-level compromise of the device — or physical extraction of the flash — still exposes the value. There is no Axis-provided keystore for ACAPs ([standards §4.3](https://github.com/) acknowledges this).
- Upgrading an existing device from a build that stored `AlarmActionPass` as a plain `String` will mask the value in `param.cgi` going forward, but the on-flash residue from the old build remains until the next write. **Rotate any password that was previously exposed via `param.cgi`** after upgrading.

## Project Structure

```
app/
├── manifest.json          # ACAP manifest (schemaVersion 1.9.0)
├── main.c                 # Entry point, GMainLoop, signal handling
├── config.c / config.h    # AXParameter wrappers
├── token_manager.c / .h   # Token queue, GMutex, expiry timer, event history
├── event_subscriber.c/.h  # AXEvent subscription to AOA line crossing events
├── event_publisher.c/.h   # AXEvent declaration and sending of TailgatingAlarm
├── alarm_handler.c / .h   # Thin wrapper: logs alarm, calls event_publisher
├── web_server.c / .h      # CivetWeb embedded HTTP server on port 8080
├── civetweb.c / .h        # Bundled CivetWeb v1.16 (MIT license)
└── html/
    ├── index.html         # Dashboard + Settings SPA
    ├── app.js             # Polling and DOM updates
    └── style.css          # Responsive styling
Dockerfile                 # Multi-arch ACAP build (axisecp/acap-native-sdk:12.9.0)
build.sh                   # Build helper script
docs/                      # Documentation
```

## Documentation

- [Requirements](docs/requirements.md)
- [Architecture](docs/architecture.md)
- [API Reference](docs/api-reference.md)
- [Developer Guide](docs/developer-guide.md)
- [Camera Credentials](docs/camera-credentials.md)
- [Learnings](docs/learnings.md)
