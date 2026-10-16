#!/usr/bin/env bash
# Read-only audit of the extracted, version-pinned OV5647 source and running Pi.
# Does not require sudo, build/install a module or modify a sensor.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
WORK="${1:-$ROOT/kernel/work/ov5647-6.18.50}"
case "$WORK" in /*) ;; *) WORK="$PWD/$WORK" ;; esac
KERNEL_RELEASE="6.18.50+rpt-rpi-2712"
driver="$WORK/linux-source-6.18/drivers/media/i2c/ov5647.c"

fail() { printf "ERROR: %s\n" "$*" >&2; exit 1; }
[[ "$(uname -r)" == "$KERNEL_RELEASE" ]] || fail "Wrong running kernel: $(uname -r); expected $KERNEL_RELEASE"
[[ -f "$driver" ]] || fail "Missing extracted source: $driver"
[[ -r "$WORK/ov5647-stock.sha256" ]] || fail "Missing stored stock driver hash"

echo "# OV5647 source and running-kernel audit (read-only)"
printf "Kernel: %s\n" "$(uname -r)"
printf "Source file: %s\n" "$driver"
echo
echo "## Source integrity"
if sha256sum --check --status "$WORK/ov5647-stock.sha256"; then
    printf "%s\n" "Unmodified stock driver: yes (matches source-preparation hash)"
else
    fail "Driver has changed since preparation; preserve stock source before patching"
fi
sha256sum "$driver"

echo
echo "## Kernel build configuration"
found_config=no
for cfg in "/boot/config-$(uname -r)" "/boot/firmware/config-$(uname -r)" \
           "/lib/modules/$(uname -r)/build/.config" "/usr/src/linux-headers-$(uname -r)/.config"; do
    if [[ -r "$cfg" ]]; then
        found_config=yes
        printf "%s\n" "Config file: $cfg"
        grep -E "^(CONFIG_VIDEO_OV5647|CONFIG_VIDEO_V4L2|CONFIG_VIDEO_DEV|CONFIG_MEDIA_SUPPORT|CONFIG_MODULES|CONFIG_LOCALVERSION)=" "$cfg" || true
        break
    fi
done
if [[ "$found_config" == no && -r /proc/config.gz ]]; then
    found_config=yes
    echo "Config file: /proc/config.gz"
    zcat /proc/config.gz | grep -E "^(CONFIG_VIDEO_OV5647|CONFIG_VIDEO_V4L2|CONFIG_VIDEO_DEV|CONFIG_MEDIA_SUPPORT|CONFIG_MODULES|CONFIG_LOCALVERSION)=" || true
fi
[[ "$found_config" == yes ]] || echo "Kernel .config not found; inspect installed linux-headers or kernel build tree"

echo
echo "## Active sensor driver and module form"
device="/sys/bus/i2c/devices/10-0036"
[[ -e "$device" ]] || fail "Expected OV5647 at $device not found"
printf "Sensor: %s\n" "$(cat "$device/name" 2>/dev/null || echo unknown)"
if [[ -L "$device/driver" ]]; then
    printf "Bound driver: %s\n" "$(basename "$(readlink -f "$device/driver")")"
    [[ "$(basename "$(readlink -f "$device/driver")")" == ov5647 ]] || fail "Unexpected bound driver"
else
    fail "Camera not bound to ov5647; do not proceed with source-specific driver work"
fi
if command -v modinfo >/dev/null 2>&1; then
    printf "modinfo filename: "
    modinfo -n ov5647 2>/dev/null || echo unavailable
fi
if [[ -r /proc/modules ]]; then
    grep -E "^ov5647 " /proc/modules || true
fi

echo
echo "## Source driver interface and metadata"
grep -n -E "ov5647_(640x480_10bpp|modes\[\]|set_pad_fmt|get_selection|subdev_pad_ops)|\.set_selection[[:space:]]*=|\.set_fmt[[:space:]]*=|\.get_selection[[:space:]]*=" "$driver" || true

echo
echo "## Advertised OV5647 modes from source (static table)"
python3 - "$driver" <<'PY'
import pathlib, re, sys
source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"static\s+const\s+struct\s+ov5647_mode\s+ov5647_modes\s*\[\s*\]\s*=\s*\{", source)
if not match:
    raise SystemExit("No ov5647_modes[] array found; manual driver inspection required")
body = source[match.end():]
end = body.find("\n};")
if end < 0:
    raise SystemExit("No closing brace for ov5647_modes[]")
body = body[:end]
blocks = re.split(r"\n\s*\},\s*(?=/\*|\{)", body)
count = 0
for block in blocks:
    fmt = block.split(".crop", 1)[0]
    m = re.search(r"\.width\s*=\s*(\d+)[\s,\S]*?\.height\s*=\s*(\d+)", fmt)
    if not m:
        continue
    count += 1
    params = {}
    for key in ("pixel_rate", "hts", "vts", "link_freq_index"):
        x = re.search(r"\." + key + r"\s*=\s*([^,\n]+)", block)
        params[key] = x.group(1).strip() if x else "unknown"
    print(f"{m.group(1)}x{m.group(2)}: " + " ".join(f"{k}={v}" for k,v in params.items()))
if not count:
    raise SystemExit("No source mode dimensions parsed; manual inspection required")
PY

echo
echo "## Suggested next source readout"
echo "Inspect driver registers for 640x480 and mode table; do not write sensor registers yet:"
echo "  grep -n -A 125 'ov5647_640x480_10bpp\[\]' \"$driver\" | head -125"
echo "The kernel source, driver binding and camera state were not modified."
