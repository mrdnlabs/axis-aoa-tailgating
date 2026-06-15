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

# Download CivetWeb files if not present
CIVETWEB_VERSION="1.16"
CIVETWEB_C="${SCRIPT_DIR}/app/civetweb.c"
CIVETWEB_H="${SCRIPT_DIR}/app/civetweb.h"

if [[ ! -f "$CIVETWEB_C" || ! -f "$CIVETWEB_H" ]]; then
  echo "Downloading CivetWeb ${CIVETWEB_VERSION}..."
  BASE_URL="https://raw.githubusercontent.com/civetweb/civetweb/v${CIVETWEB_VERSION}"
  curl -fsSL "${BASE_URL}/src/civetweb.c"      -o "${SCRIPT_DIR}/app/civetweb.c"
  curl -fsSL "${BASE_URL}/include/civetweb.h"  -o "${SCRIPT_DIR}/app/civetweb.h"
  curl -fsSL "${BASE_URL}/src/md5.inl"         -o "${SCRIPT_DIR}/app/md5.inl"
  curl -fsSL "${BASE_URL}/src/sort.inl"        -o "${SCRIPT_DIR}/app/sort.inl"
  curl -fsSL "${BASE_URL}/src/match.inl"       -o "${SCRIPT_DIR}/app/match.inl"
  curl -fsSL "${BASE_URL}/src/response.inl"    -o "${SCRIPT_DIR}/app/response.inl"
  curl -fsSL "${BASE_URL}/src/sha1.inl"        -o "${SCRIPT_DIR}/app/sha1.inl"
  curl -fsSL "${BASE_URL}/src/timer.inl"       -o "${SCRIPT_DIR}/app/timer.inl"
  curl -fsSL "${BASE_URL}/src/handle_form.inl" -o "${SCRIPT_DIR}/app/handle_form.inl"
  echo "CivetWeb downloaded."
fi

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
echo "To install on device (see .env.devices for credentials):"
echo "  python3 -c \""
echo "  import requests; from requests.auth import HTTPDigestAuth"
echo "  with open('${EAP_PATH}', 'rb') as f:"
echo "    r = requests.post('http://CAMERA_IP/axis-cgi/applications/upload.cgi',"
echo "        auth=HTTPDigestAuth('CAMERA_USER', 'CAMERA_PASS'),"
echo "        files={'packfil': f})"
echo "  print(r.status_code, r.text)\""
