#!/bin/sh
# Host-side unit tests for the pure modules (json_util, validate).
# Runs in an Alpine container so it needs no Axis SDK and no device.
#   app/test/run_tests.sh
set -e

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$SELF_DIR/.." && pwd)"

# If not already inside the container, re-exec there with app/ mounted.
if [ "$IN_TEST_CONTAINER" != "1" ]; then
  exec docker run --rm -e IN_TEST_CONTAINER=1 \
    -v "$APP_DIR":/src:ro -w /src/test \
    alpine:3.20 sh -c \
    'apk add --no-cache gcc musl-dev glib-dev pkgconf >/dev/null && ./run_tests.sh'
fi

# musl/Alpine gcc ships no libasan; use UBSan in trap mode (no runtime lib).
CFLAGS="$(pkg-config --cflags glib-2.0) -D_GNU_SOURCE -Wall -Wextra -Werror -std=c11 -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error"
LIBS="$(pkg-config --libs glib-2.0)"

cc $CFLAGS \
  /src/test/test_json_validate.c \
  /src/json_util.c \
  /src/validate.c \
  $LIBS -o /tmp/test_jv

exec /tmp/test_jv
