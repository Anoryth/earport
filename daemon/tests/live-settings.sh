#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Live check of the AirPods settings D-Bus API against real, connected AirPods.
# Not run by `meson test`: it needs hardware and changes settings (each one is
# restored afterwards).
#
#   daemon/tests/live-settings.sh            # read + toggle every announced setting
#   daemon/tests/live-settings.sh --read     # only print what the AirPods announced

set -euo pipefail

DEST=io.github.anoryth.EarPort
OBJ=/io/github/anoryth/EarPort
IFACE=io.github.anoryth.EarPort1
failures=0

get_settings() {
    busctl --user --json=short get-property "$DEST" "$OBJ" "$IFACE" Settings |
        python3 -c 'import json,sys; d=json.load(sys.stdin)["data"]; print(json.dumps({k: v["data"] for k, v in d.items()}))'
}

get_one() {
    get_settings | python3 -c "import json,sys; v=json.load(sys.stdin).get('$1'); print('' if v is None else json.dumps(v))"
}

set_one() {  # key, busctl variant type, value
    busctl --user call "$DEST" "$OBJ" "$IFACE" SetSetting sv "$1" "$2" "$3"
}

check() {  # description, expected, actual
    if [[ "$2" == "$3" ]]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1 (expected $2, got $3)"
        failures=$((failures + 1))
    fi
}

expect_error() {  # description, command...
    local desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  FAIL $desc (call succeeded)"
        failures=$((failures + 1))
    else
        echo "  ok   $desc"
    fi
}

connected=$(busctl --user get-property "$DEST" "$OBJ" "$IFACE" Connected | awk '{print $2}')
[[ "$connected" == "true" ]] || { echo "AirPods not connected"; exit 1; }

echo "Announced settings:"
get_settings | python3 -m json.tool
[[ "${1:-}" == "--read" ]] && exit 0

echo "Toggling boolean settings:"
for key in OneBudANC PersonalizedVolume SleepDetection VolumeSwipe; do
    before=$(get_one "$key")
    if [[ -z "$before" ]]; then
        echo "  skip $key (not announced)"
        continue
    fi
    flipped=$([[ "$before" == "true" ]] && echo false || echo true)
    set_one "$key" b "$flipped"
    sleep 0.3
    check "$key -> $flipped" "$flipped" "$(get_one "$key")"
    set_one "$key" b "$before"
    sleep 0.3
    check "$key restored" "$before" "$(get_one "$key")"
done

echo "Cycling choice settings:"
for key in PressSpeed PressHoldDuration VolumeSwipeSpeed; do
    before=$(get_one "$key")
    if [[ -z "$before" ]]; then
        echo "  skip $key (not announced)"
        continue
    fi
    other=$(( before == 0 ? 1 : 0 ))
    set_one "$key" i "$other"
    sleep 0.3
    check "$key -> $other" "$other" "$(get_one "$key")"
    set_one "$key" i "$before"
    sleep 0.3
    check "$key restored" "$before" "$(get_one "$key")"
done

echo "Rejected calls:"
expect_error "unknown key" set_one NoSuchSetting b true
expect_error "wrong type for a boolean" set_one VolumeSwipe i 1
expect_error "out-of-range choice" set_one PressSpeed i 9

echo
if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "All checks passed"
