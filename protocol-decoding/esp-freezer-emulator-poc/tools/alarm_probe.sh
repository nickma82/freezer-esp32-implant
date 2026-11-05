#!/usr/bin/env bash
# Probe unknown alarm flags: set each 0C/<id> to 01 with the alarm event flags, hold, clear, next.
# Watch the app and note what appears at which time. Needs the console in quiet mode (default).
# usage: tools/alarm_probe.sh <keys.py> [port] [on_s] [off_s] [ids...]
set -euo pipefail

KEYS=${1:?path to keys.py}
PORT=${2:-/dev/ttyUSB0}
ON_S=${3:-60}
OFF_S=${4:-30}
shift $(( $# >= 4 ? 4 : $# ))
IDS=("${@:-0B 0D 0F 11}")
read -r -a IDS <<<"${IDS[*]}"
export BAUD=${BAUD:-57600}

send_until_ack() {  # $1 command line, $2 ack pattern
  local cmd=$1 ack=$2 out i
  for i in 1 2 3 4 5 6; do
    out=$(python3 -I "$KEYS" "$PORT" '\r' 0.2 "$cmd\\r" 1.2)
    if grep -a -q "$ack" <<<"$out"; then return 0; fi
  done
  echo "FAILED: $cmd" >&2
  return 1
}

set_flag() {  # $1 id, $2 value
  send_until_ack "p 0C $1 20 $2" "# param 0C/$1 set"
  send_until_ack "e 00000100 08000000" "# event flags 00000100 08000000"
}

main() {
  local id
  for id in "${IDS[@]}"; do
    set_flag "$id" 01
    echo "$(date +%H:%M:%S)  0C/$id = 01  (alarm flags raised)"
    sleep "$ON_S"
    set_flag "$id" 00
    echo "$(date +%H:%M:%S)  0C/$id = 00"
    sleep "$OFF_S"
  done
  echo "$(date +%H:%M:%S)  done"
}

main
