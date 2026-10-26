#!/bin/bash
set -e

ARCH="${1:-armv7hf}"
VERSION="12.9.0"
UBUNTU_VERSION="24.04"

if [[ "$ARCH" != "armv7hf" && "$ARCH" != "aarch64" ]]; then
  echo "Usage: $0 [armv7hf|aarch64]"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Download + integrity-check CivetWeb.  A git tag is mutable, so pin the
# content by sha256 (v1.16, verified 2026-10): tampering or a moved tag
# fails the build instead of compiling unknown code into the HTTP server.
CIVETWEB_VERSION="1.16"
BASE_URL="https://raw.githubusercontent.com/civetweb/civetweb/v${CIVETWEB_VERSION}"

# file  path-under-repo  remote-subpath  sha256
CIVETWEB_FILES=(
  "civetweb.c|src/civetweb.c|e572a29680c063b6202377131cee65f238fc801692a57ec7797fdad381fde786"
  "civetweb.h|include/civetweb.h|e3a12c0a659b3bc6f75aa67799232ca343572dcb3add8b2475666633fa4afad3"
  "md5.inl|src/md5.inl|0b9c95a45d00c4aa74c2ef0de24baad7f9961a95c3d5835a262a31173d0c3916"
  "sort.inl|src/sort.inl|2a371807ed7e4b6cf078251dd36df4ea3385becafdc4dd0c50d39f5258e59f6e"
  "match.inl|src/match.inl|1f575b361135b13c377fa84b6d5f9b6faf8b6c6416dfec476509d09a7eb743e3"
  "response.inl|src/response.inl|e6455d9739845593e18c3ae756b6791cb051f57fafd40806b65ba56e2ff7b20b"
  "sha1.inl|src/sha1.inl|8955644bf6aa8ffdae8a3ab61d09eac1d8bfe0abd160edf83fbe8e1a357151e8"
  "timer.inl|src/timer.inl|dfb39732ea7a105eaa385154a3a1ac4f5a71ae9c29401a5c9a388c3d9d06f012"
  "handle_form.inl|src/handle_form.inl|bddad9e1e9d0ec227cf3ad3ca6bd44ffacf44bd756f631527d2b5798c4e36fbf"
)

for entry in "${CIVETWEB_FILES[@]}"; do
  IFS='|' read -r name sub want <<< "$entry"
  dst="${SCRIPT_DIR}/app/${name}"
  if [[ ! -f "$dst" ]]; then
    echo "Downloading CivetWeb ${name}..."
    curl -fsSL "${BASE_URL}/${sub}" -o "$dst"
  fi
  got="$(sha256sum "$dst" | cut -d' ' -f1)"
  if [[ "$got" != "$want" ]]; then
    echo "ERROR: CivetWeb ${name} sha256 mismatch:" >&2
    echo "  expected $want" >&2
    echo "  got      $got" >&2
    echo "Refusing to build with unverified vendored sources. Delete app/${name} to re-download." >&2
    exit 1
  fi
done
echo "CivetWeb ${CIVETWEB_VERSION} verified."

IMAGE_TAG="antitailgate-acap:${ARCH}"
EAP_NAME="Anti-Tailgating_1_0_0_${ARCH}.eap"

echo "Building for ${ARCH}..."
# --no-cache: BuildKit on WSL2 can serve a stale COPY layer when source files
# change, silently shipping an old manifest.json. The whole rebuild is a few
# seconds; not worth the diagnostic cost of cache poisoning.
docker build \
  --no-cache \
  --build-arg ARCH="${ARCH}" \
  --build-arg VERSION="${VERSION}" \
  --build-arg UBUNTU_VERSION="${UBUNTU_VERSION}" \
  -t "${IMAGE_TAG}" \
  "${SCRIPT_DIR}"

echo "Extracting .eap file..."
CID=$(docker create "${IMAGE_TAG}")
docker cp "${CID}:/opt/app/${EAP_NAME}" "${SCRIPT_DIR}/app/"
docker rm "${CID}"

EAP_PATH="${SCRIPT_DIR}/app/${EAP_NAME}"
echo ""
echo "Build complete: ${EAP_PATH}"
echo "Size: $(du -h "${EAP_PATH}" | cut -f1)"
echo ""
echo "To install (set CAMERA_IP / CAMERA_USER / CAMERA_PASS from .env.devices):"
echo "  curl -k --anyauth -u \"\$CAMERA_USER:\$CAMERA_PASS\" \\"
echo "    -F \"packfil=@${EAP_PATH}\" \\"
echo "    \"https://\$CAMERA_IP/axis-cgi/applications/upload.cgi\""
echo ""
echo "  # unsigned builds need AllowUnsigned=true first:"
echo "  curl -k --anyauth -u \"\$CAMERA_USER:\$CAMERA_PASS\" -X POST \\"
echo "    \"https://\$CAMERA_IP/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true\""
