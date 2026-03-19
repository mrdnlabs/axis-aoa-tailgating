# Camera Credentials & Device Info

## Devices

### Camera — Axis Q3538-SLVE

| Property | Value |
|---|---|
| IP Address | 192.168.1.238 |
| Model | Axis Q3538-SLVE |
| Architecture | **aarch64** (ARTPEC-8) |
| Firmware | AXIS OS 12.9.57 |
| Object Analytics | Installed, Running (v1.23.66) |
| Install target | `Anti-Tailgating_1_0_0_aarch64.eap` |
| Web UI | `http://192.168.1.238/local/antitailgate/` |

**Credentials**:

| User | Password | Notes |
|---|---|---|
| `admin` | `admin` | **Working** — set after factory default, Mar 2026 |

### Door Controller — Axis A1210

| Property | Value |
|---|---|
| IP Address | 192.168.1.233 |
| Model | Axis A1210 |
| Architecture | armv7hf |
| Firmware | AXIS OS 12.8.55.1 |
| Web UI | `http://192.168.1.233` (accesscontrol-e2e interface) |

**Credentials**:

| User | Password | Notes |
|---|---|---|
| `root` | `pass` | Verified working |

---

## Authentication Notes

### Camera (Q3538-SLVE)

- Uses **HTTP Digest auth** (not Basic)
- Use `--digest` flag with curl, or `HTTPDigestAuth` with Python requests
- Basic auth (`--basic`) is rejected
- `curl` combined with `--digest` and multipart `--form` upload fails — use Python `requests` for EAP upload

### Door Controller (A1210)

- Uses **HTTP Digest auth**
- Web UI uses the `accesscontrol-e2e` interface — there is NO "System → Events → Rules" page
- Action rules must be configured via VAPIX SOAP at `/vapix/services`
- Standard VAPIX CGI endpoints and SOAP action rule APIs work normally

---

## Camera VAPIX Examples

```bash
# List installed apps
curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/list.cgi'

# Get device info
curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/param.cgi?action=list&group=Properties.System.Architecture,Brand.ProdNbr'

# Upload .eap (use Python — curl has issues with Digest + multipart)
python3 -c "
import requests
from requests.auth import HTTPDigestAuth
auth = HTTPDigestAuth('admin', 'admin')
with open('app/Anti-Tailgating_1_0_0_aarch64.eap', 'rb') as f:
    r = requests.post('http://192.168.1.238/axis-cgi/applications/upload.cgi',
        auth=auth,
        files={'packfil': ('Anti-Tailgating_1_0_0_aarch64.eap', f, 'application/octet-stream')})
print(r.status_code, r.text)
"

# Start/stop/restart the app
curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/control.cgi?action=start&package=antitailgate'

curl -s --digest -u 'admin:admin' \
  'http://192.168.1.238/axis-cgi/applications/control.cgi?action=restart&package=antitailgate'
```

---

## Door Controller VAPIX SOAP Notes

The A1210 SOAP endpoint is at `http://192.168.1.233/vapix/services`.

Working SOAP actions:
- `GetActionTemplates` — lists available action types including `com.axis.action.fixed.notification.http`
- `GetActionRules` — lists configured rules
- `GetRecipientTemplates` — lists notification target types
- `AddActionRule` — creates a new action rule

Non-working SOAP action:
- `GetActionConditions` — returns `ter:ActionNotSupported`

The HTTP notification action template supports a configurable POST body. Use this to send `{"door":"<name>","card":"%CardNumber%"}` to the camera's `/badge-read` endpoint on each access-granted event.

See [docs/learnings.md](learnings.md) for full details on A1210 integration.

---

## Enable Unsigned Applications (Camera Only)

Required before installing the unsigned `.eap`.

Cannot be done via VAPIX — intentionally restricted in AXIS OS 11+.

**Steps:**
1. Open `http://192.168.1.238` in a browser
2. Log in as `admin` / `admin`
3. Go to **Settings → Apps**
4. Enable **"Allow unsigned applications"**
5. Confirm the warning dialog
