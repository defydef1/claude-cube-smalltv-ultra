#!/bin/sh
# Flash the Claude Cube over Wi-Fi: full firmware -> loader -> new full firmware.
# The full image is too big to update over itself, so the tiny loader goes first.
#   tools/flash.sh [firmware.bin.gz]   (default: the latest build, gzipped)
set -e
cd "$(dirname "$0")/.."
HOST=${CUBE_HOST:-$(cat ~/.claude/cube_host 2>/dev/null || echo claudecube.local)}
FW=${1:-firmware/.pio/build/ultra/firmware.bin.gz}
LOADER=claude-cube-loader.bin.gz
[ -f "$LOADER" ] || LOADER=release/claude-cube-loader.bin.gz
[ "$FW" = firmware/.pio/build/ultra/firmware.bin.gz ] && gzip -9 -k -f firmware/.pio/build/ultra/firmware.bin

fw() { curl -s -m 3 "http://$HOST/status" | sed -n 's/.*"fw":"\([^"]*\)".*/\1/p'; }
wait_for() {  # wait until /status reports $1
  for i in $(seq 1 60); do [ "$(fw)" = "$1" ] && return 0; sleep 2; done
  echo "cube did not come back as $1" >&2; exit 1
}
upload() { curl -s -m 180 -F "firmware=@$1" "http://$HOST/update" | grep -q "Success" || { echo "upload of $1 failed" >&2; exit 1; }; }

# while a GIF plays the cube refuses connections for a few seconds: wait for it
for i in $(seq 1 30); do [ -n "$(fw)" ] && break; sleep 2; done
if [ "$(fw)" != "claude-cube-loader" ]; then
  echo "1/2 loader -> $HOST"
  upload "$LOADER"
  wait_for claude-cube-loader
fi
echo "2/2 $FW -> $HOST"
upload "$FW"
for i in $(seq 1 60); do v=$(fw); case "$v" in claude-cube\ *) echo "running: $v"; exit 0;; esac; sleep 2; done
echo "cube did not come back with the full firmware" >&2; exit 1
