# Developer Guide: Anti-Tailgating ACAP

## Prerequisites

- Docker Desktop with WSL2 integration enabled
- Access to the target Axis camera (192.168.1.238, credentials in `.env.devices`)
- AXIS Object Analytics installed on the camera (for AOA event subscription)
- Python 3 with `requests` library installed (for EAP upload)
- Git and standard Unix tools in WSL

---

## Step 1: Detect Device Architecture

Before building, query the camera to confirm its processor architecture:

```bash
curl -s --digest -u 'admin:admin' \
  -X POST \
  -H "Content-Type: application/json" \
  -d '{"apiVersion":"1.0","method":"getAllProperties"}' \
  http://192.168.1.238/axis-cgi/basicdeviceinfo.cgi \
  | python3 -m json.tool
```

Look for `"Architecture"` in the response:
- `"aarch64"` → ARTPEC-8 (e.g., Q3538-SLVE) — use `aarch64` build
- `"armv7hf"` → ARTPEC-7 (e.g., P3245, P3265) — use `armv7hf` build

The target device (Axis Q3538-SLVE at 192.168.1.238) is **aarch64**.

---

## Step 2: Build the ACAP

### Build for aarch64 (ARTPEC-8) — primary target

```bash
cd /mnt/c/aoa-antitailgate-acap
./build.sh aarch64
```

### Build for armv7hf (ARTPEC-7)

```bash
./build.sh armv7hf
```

The `.eap` file is created in `app/` after a successful build:
- `app/Anti-Tailgating_1_0_0_aarch64.eap`
- `app/Anti-Tailgating_1_0_0_armv7hf.eap`

Manual Docker commands (equivalent to build.sh):
```bash
docker build --build-arg ARCH=aarch64 -t antitailgate-acap:aarch64 .
CID=$(docker create antitailgate-acap:aarch64)
docker cp "${CID}:/opt/app/Anti-Tailgating_1_0_0_aarch64.eap" app/
docker rm "${CID}"
```

---

## Step 3: Enable Unsigned Applications on the Camera

Signed EAP packages require an Axis developer account. For testing with unsigned packages:

1. Open a browser and navigate to `http://192.168.1.238`
2. Log in as `admin` / `admin`
3. Go to **Settings → Apps**
4. Enable **Allow unsigned applications**
5. Confirm the warning dialog

> **Note**: This setting cannot be configured via VAPIX — it is intentionally restricted to the web UI in AXIS OS 11+. Re-enable after firmware upgrades if needed.

---

## Step 4: Install the ACAP

### Via Python (recommended)

`curl` has issues combining HTTP Digest authentication with multipart file upload to Axis cameras. Use Python instead:

```python
import requests
from requests.auth import HTTPDigestAuth

with open('app/Anti-Tailgating_1_0_0_aarch64.eap', 'rb') as f:
    r = requests.post(
        'http://192.168.1.238/axis-cgi/applications/upload.cgi',
        auth=HTTPDigestAuth('admin', 'admin'),
        files={'packfil': ('Anti-Tailgating_1_0_0_aarch64.eap', f, 'application/octet-stream')}
    )
print(r.status_code, r.text)
```

### Via Web UI

1. Navigate to `http://192.168.1.238` → **Settings → Apps**
2. Click **Add app (+)**
3. Upload the `.eap` file from `app/`
4. Wait for installation to complete and start the application

### Start/restart the application

```bash
curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/control.cgi?action=start&package=antitailgate'

curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/control.cgi?action=restart&package=antitailgate'
```

---

## Step 5: Access the Web UI

Navigate to:
```
http://192.168.1.238/local/antitailgate/
```

Log in with camera credentials when prompted. The web UI provides:
- **Dashboard tab**: Live token count, session stats, simulate buttons, event history table (Time/Type/Source/Badge ID/Outcome), alarm history table
- **Settings tab**: Token TTL, AOA Scenario ID, Badge Read Endpoint URL with copy button, Door Controller Integration info card

The camera's **Apps** page also links to the UI via the `settingPage` manifest entry:
```
http://192.168.1.238 → Settings → Apps → Anti-Tailgating → Open
```

---

## Step 6: Configure AOA Scenario

1. Ensure AXIS Object Analytics is installed and running on the camera
2. Create a **Line Crossing** scenario in AOA: AOA settings → Add scenario → Line Crossing
3. Note the scenario ID (visible in the AOA interface, typically 1, 2, 3...)
4. In the Anti-Tailgating web UI → Settings, set **AOA Scenario ID** to match
5. The ACAP subscribes to that specific scenario's events

To list all AOA scenarios via VAPIX:
```bash
curl -s --digest -u 'admin:admin' \
  "http://192.168.1.238/local/objectanalytics/control.cgi?action=list&resource=scenario"
```

---

## Step 7: Configure Door Controller Integration (Optional)

The Axis A1210 door controller (192.168.1.233) must be configured via VAPIX SOAP to send badge-read events to the camera. The A1210's web UI (`accesscontrol-e2e`) does not expose a "System → Events → Rules" page — SOAP is required.

See [docs/learnings.md](learnings.md) for details on VAPIX SOAP action rules on the A1210.

Target configuration on the A1210:
- **Action**: HTTP notification (POST)
- **URL**: `http://192.168.1.238:8080/badge-read`
- **Body**: `{"door":"Front Door","card":"%CardNumber%"}`
- **Content-Type**: `application/json`
- **Trigger**: Access granted event on the relevant door

---

## Verification Tests

After installation, run these tests in sequence.

### 1. Health Check
```bash
curl -s "http://192.168.1.238:8080/test"
# Expected: {"status":"ok","app":"antitailgate","version":"1.0.0"}
```

### 2. Badge Read (simple)
```bash
curl -s -X POST "http://192.168.1.238:8080/badge-read"
# Expected: {"status":"ok","token_count":1,...}
```

### 3. Badge Read with JSON Body (door controller simulation)
```bash
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"door":"Front Door","card":"ABC123"}' \
  "http://192.168.1.238:8080/badge-read"
# Expected: token_count incremented, badge_id="ABC123"
# Check /status to confirm source="Front Door" in event history
```

### 4. Authorized Entry (Threshold Crossing with Token)
```bash
curl -s -X POST "http://192.168.1.238:8080/badge-read"
curl -s -X POST "http://192.168.1.238:8080/threshold-crossing"
# Expected: {"outcome":"authorized","tokens_remaining":0}
```

### 5. Tailgating Alarm (Crossing Without Token)
```bash
curl -s -X POST "http://192.168.1.238:8080/threshold-crossing"
# Expected: {"outcome":"alarm","tokens_remaining":0}
# Check /status for alarm in history
```

### 6. Axis Event Verification
After the alarm fires, verify the native event appeared:
1. Open camera web UI: **System → Events → Rules**
2. Click **Add rule** and browse triggers
3. Navigate to **Application → Anti-Tailgating → TailgatingAlarm**
4. Confirm the trigger is listed

This confirms the event was declared correctly on startup and received by the camera's event system.

### 7. Token Expiry
```bash
curl -s -X POST "http://192.168.1.238:8080/badge-read"
sleep 10  # Wait longer than TokenExpirationSeconds (default 7)
curl -s -X POST "http://192.168.1.238:8080/threshold-crossing"
# Expected: alarm (token expired before crossing)
```

### 8. Config Persistence
```bash
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"TokenExpirationSeconds":15}' \
  "http://192.168.1.238:8080/config"

curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/control.cgi?action=restart&package=antitailgate'

sleep 3
curl -s "http://192.168.1.238:8080/config"
# Expected: TokenExpirationSeconds = 15
```

### 9. AOA Native Event (requires physical setup)
Trigger an actual line crossing on the configured AOA scenario by walking through the detection zone while monitoring the web UI dashboard. The event should appear in history within 1-2 seconds.

---

## Debugging

### View Application Logs

SSH into the camera:
```bash
ssh admin@192.168.1.238
# Password: admin (see .env.devices)
```

View ACAP logs:
```bash
# Systemd journal (AXIS OS 12.x)
journalctl -u antitailgate -f

# Or syslog
tail -f /var/log/messages | grep antitailgate
```

### Check Application Status via VAPIX
```bash
curl -s --digest -u 'admin:admin' \
  "http://192.168.1.238/axis-cgi/applications/list.cgi" \
  | grep -A5 antitailgate
```

### Check CivetWeb Is Running
```bash
# On the camera via SSH
curl -s http://localhost:8080/test
```

### Reset to Defaults
```bash
curl -s -X POST "http://192.168.1.238:8080/reset-defaults"
```

Or use the Settings tab in the web UI.

---

## File Locations on Device

After installation, the ACAP files are at:

| Path | Contents |
|------|---------|
| `/usr/local/packages/antitailgate/` | Application binary and manifest |
| `/usr/local/packages/antitailgate/html/` | Static web files (index.html, app.js, style.css) |
| `/usr/local/packages/antitailgate/localdata/` | Writable data directory |

AXParameter values are stored by the Axis parameter subsystem (location is firmware-internal, not a plain file).

---

## Rebuilding After Changes

C source changes require a full rebuild and reinstall:
```bash
./build.sh aarch64
# Then reinstall via Python upload script or web UI
```

Web UI changes (HTML/CSS/JS only) can be hot-patched via SCP:
```bash
scp app/html/index.html root@192.168.1.238:/usr/local/packages/antitailgate/html/
scp app/html/app.js root@192.168.1.238:/usr/local/packages/antitailgate/html/
scp app/html/style.css root@192.168.1.238:/usr/local/packages/antitailgate/html/
# No app restart needed — Apache serves static files directly
```
