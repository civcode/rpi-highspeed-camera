#!/usr/bin/env bash
# Read-only sensor-driver inventory for experimental Raspberry Pi camera work.
# Never writes a sysfs control, invokes i2cset, unbinds a driver, or changes
# the installed kernel/firmware.
set -euo pipefail

printf '%s\n' '# hscam kernel preflight (read-only)'
date -u '+UTC: %Y-%m-%dT%H:%M:%SZ'
printf '\n%s\n' '## Kernel'
uname -a
printf 'Kernel release: '; uname -r
if [[ -r /proc/device-tree/model ]]; then
    printf 'Board: '
    tr -d '\000' < /proc/device-tree/model
    printf '\n'
fi

printf '\n%s\n' '## libcamera and available tools'
if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --modversion libcamera 2>/dev/null || true
fi
for cmd in media-ctl v4l2-ctl rpicam-raw; do
    if command -v "$cmd" >/dev/null 2>&1; then
        printf '%s: %s\n' "$cmd" "$(command -v "$cmd")"
    fi
done

printf '\n%s\n' '## Bound I2C camera devices'
shopt -s nullglob
for d in /sys/bus/i2c/devices/*; do
    [[ -e "$d/name" ]] || continue
    name=$(cat "$d/name" 2>/dev/null || true)
    case "$name" in
        ov5647*|imx219*|imx708*|imx477*|imx296*|imx500*)
            printf 'device: %s sensor: %s\n' "$d" "$name"
            if [[ -L "$d/driver" ]]; then
                printf '  bound_driver: %s\n' "$(readlink -f "$d/driver")"
            else
                printf '%s\n' '  bound_driver: none'
            fi
            if [[ -r "$d/modalias" ]]; then
                printf '  modalias: %s\n' "$(cat "$d/modalias")"
            fi
            ;;
    esac
done

printf '\n%s\n' '## V4L2 sensor subdevices'
for d in /sys/class/video4linux/v4l-subdev*; do
    [[ -e "$d" ]] || continue
    name=$(cat "$d/name" 2>/dev/null || true)
    printf '%s: %s\n' "$d" "$name"
done
printf '\n%s\n' '## Safety'
printf '%s\n' 'No driver bindings or sensor registers were modified.'
