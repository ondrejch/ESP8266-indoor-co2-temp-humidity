#!/usr/bin/env bash
# Headless build/upload for esp-co2-dht (no Arduino IDE GUI needed).
# Uses Arduino 1.8 CLI with Java 21 (default Java 11 fails to start the IDE)
# and the board/menu options saved in ~/.arduino15/preferences.txt
# (currently: esp8266:esp8266:nodemcu, 80 MHz, ip=lm2f / lwIP v2).
# If you change boards in the GUI once, this script picks it up automatically.
#
# Usage:
#   ./build.sh           # same as build
#   ./build.sh build     # compile only
#   ./build.sh upload    # build + upload to $PORT (default: auto /dev/ttyUSB0)
#   ./build.sh monitor   # open Serial Monitor at 115200 baud
#   ./build.sh clean     # remove build/
#
# Override with env vars, e.g.:
#   PORT=/dev/ttyUSB0 ./build.sh upload

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$SCRIPT_DIR/esp-co2-dht/esp-co2-dht.ino"
HEADER="$SCRIPT_DIR/esp-co2-dht/esp-co2-dht.h"
BUILD_DIR="$SCRIPT_DIR/build"
PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"

# Arduino 1.8.19 needs Java 17+ (class file 61). Default Java 11 crashes.
# Java 21 is installed on this machine — prefer it if present.
if [[ -d /usr/lib/jvm/java-21-openjdk-amd64 ]]; then
  export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
  export PATH="$JAVA_HOME/bin:$PATH"
elif [[ -d /usr/lib/jvm/java-1.21.0-openjdk-amd64 ]]; then
  export JAVA_HOME=/usr/lib/jvm/java-1.21.0-openjdk-amd64
  export PATH="$JAVA_HOME/bin:$PATH"
fi
export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:-}"  # silence "Picked up..."

die() { echo "error: $*" >&2; exit 1; }
command -v arduino >/dev/null 2>&1 || die "arduino CLI not found (/usr/bin/arduino)"

check_header() {
  [[ -f "$HEADER" ]] || die "missing $HEADER"
  if grep -q '"your-ssid"' "$HEADER"; then
    echo "warning: $HEADER still has placeholder WiFi credentials —" >&2
    echo "         it will compile but won't connect until you fill it in." >&2
    echo "         (edit locally; do not commit real credentials)" >&2
  fi
}

arduino_args() {
  # No --board here: Arduino 1.8 CLI ignores menu options on the command line
  # ("Unknown board" for fqbn with ,ip=lm2f) but honours the board/menu saved
  # in ~/.arduino15/preferences.txt (custom_ip=nodemcu_lm2f, etc.).
  # That prefs file already selects nodemcu + lwIP v2, which fixes the
  # "lwip/init.h: No such file" error seen with a bare --board flag.
  echo --verify
}

cmd_build() {
  check_header
  echo "==> Building $SKETCH"
  arduino --verify "$SKETCH"
  echo "==> Build OK"
}

cmd_upload() {
  check_header
  if [[ ! -e "$PORT" ]]; then
    echo "warning: $PORT not found — connect the board first, then re-run." >&2
  fi
  echo "==> Building + uploading to $PORT"
  # --upload implies a build. --port selects the serial device.
  arduino --upload --port "$PORT" "$SKETCH"
  echo "==> Upload OK"
}

cmd_monitor() {
  [[ -e "$PORT" ]] || die "$PORT not found"
  echo "==> Serial Monitor $PORT @ $BAUD baud (Ctrl-C to exit)"
  exec stty -F "$PORT" "$BAUD" 2>/dev/null || true
  cat "$PORT"
}

cmd_clean() {
  rm -rf "$BUILD_DIR"
  echo "==> Cleaned $BUILD_DIR"
}

usage() {
  sed -n '2,20p' "$0"
}

case "${1:-build}" in
  build|verify)  cmd_build ;;
  upload)        cmd_upload ;;
  monitor)       cmd_monitor ;;
  clean)         cmd_clean ;;
  -h|--help|help) usage ;;
  *) echo "unknown command: $1" >&2; usage >&2; exit 1 ;;
esac
