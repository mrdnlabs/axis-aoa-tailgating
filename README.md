# Anti-Tailgating ACAP

A self-contained ACAP (Axis Camera Application Platform) running directly on an Axis camera to detect tailgating at controlled entry points.

## What It Does

- Subscribes natively to AXIS Object Analytics (AOA) line crossing events via the AXEvent API
- Accepts HTTP POST badge-read events from external systems (badge readers, door controllers, ACS)
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

Use Python (curl has issues with HTTP Digest auth combined with multipart upload):

```python
import requests
from requests.auth import HTTPDigestAuth
with open('app/Anti-Tailgating_1_0_0_aarch64.eap', 'rb') as f:
    r = requests.post('http://192.168.1.238/axis-cgi/applications/upload.cgi',
                      auth=HTTPDigestAuth('admin', 'admin'),
                      files={'packfil': ('Anti-Tailgating_1_0_0_aarch64.eap', f, 'application/octet-stream')})
print(r.status_code, r.text)
```

> **Note**: Device credentials are stored in `.env.devices` (not committed to git). Update the username/password above to match your device.

### 4. Open Web UI

```
http://192.168.1.238/local/antitailgate/
```

## API

See [docs/api-reference.md](docs/api-reference.md) for full API documentation.

Key endpoints (accessible via port 8080 directly or via Apache proxy at `/local/antitailgate/api/`):

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/badge-read` | POST | Create access token; optional JSON body `{"door":"...","card":"..."}` |
| `/threshold-crossing` | POST | Simulate line crossing (test/fallback) |
| `/status` | GET | Token count, event history, alarm history |
| `/config` | GET/POST | Read/write TokenExpirationSeconds and AoaScenarioId |
| `/test` | GET | Health check |
| `/clear-history` | POST | Clear in-memory history |
| `/reset-defaults` | POST | Reset config to defaults |

## Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `TokenExpirationSeconds` | `7` | Token TTL in seconds |
| `AoaScenarioId` | `1` | AOA line crossing scenario ID |

All parameters persist across restarts and firmware upgrades via AXParameter API.

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
