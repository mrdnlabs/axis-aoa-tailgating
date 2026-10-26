# Device Inventory & Access (local test rig)

> **Credentials and device IPs are NOT stored here.** This repository is public.
> Keep per-device IPs, usernames, and passwords in `.env.devices` at the repo
> root (git-ignored, see `.gitignore`). Rotate any credential that was ever
> committed to this repo's history.

## Devices used for development

| Role | Model | Arch | Notes |
|---|---|---|---|
| Camera | AXIS Q3538-SLVE | aarch64 (ARTPEC-8) | Runs the ACAP. Object Analytics bundled. |
| Door controller | AXIS A1210 | armv7hf | Source of access-granted events (not an ACAP host). |

Put the live values in `.env.devices`:

```sh
CAMERA_IP=...
CAMERA_USER=...
CAMERA_PASS=...
CAMERA_AUTH=basic          # AXIS OS 12.9+ default policy is basic_digest
DOOR_CONTROLLER_IP=...
DOOR_CONTROLLER_USER=...
DOOR_CONTROLLER_PASS=...
```

## Authentication notes

- AXIS OS 12.x defaults to the `basic_digest` policy. Use `curl --anyauth`,
  which negotiates whichever the device offers; do not hard-code `--digest`
  (a Basic-only device will 401) or `--basic`.
- `curl --anyauth` with `-F` multipart upload works for `upload.cgi` on
  current firmware; the old "must use Python requests" note no longer applies.

## Common VAPIX calls

```sh
# identify
curl -k --anyauth -u "$CAMERA_USER:$CAMERA_PASS" \
  "https://$CAMERA_IP/axis-cgi/param.cgi?action=list&group=Brand.ProdNbr"

# allow unsigned, then upload
curl -k --anyauth -u "$CAMERA_USER:$CAMERA_PASS" -X POST \
  "https://$CAMERA_IP/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
curl -k --anyauth -u "$CAMERA_USER:$CAMERA_PASS" \
  -F "packfil=@app/Anti-Tailgating_1_0_0_aarch64.eap" \
  "https://$CAMERA_IP/axis-cgi/applications/upload.cgi"

# start / restart
curl -k --anyauth -u "$CAMERA_USER:$CAMERA_PASS" \
  "https://$CAMERA_IP/axis-cgi/applications/control.cgi?action=restart&package=antitailgate"
```

## Door controller integration

The A1210 is wired in as an **event source**, not an ACAP host. Configure its
access-control rules (or an HTTP notification action) to `POST` an
access-granted event to the camera's operator endpoint:

```
POST https://<camera-ip>/local/antitailgate/ingest/badge-read
Content-Type: application/json
{"door":"<door name>","card":"<card id>"}
```

In many deployments the VMS / access-control platform sends this directly,
and the door controller is not involved at all.
