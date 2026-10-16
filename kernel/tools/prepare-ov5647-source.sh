#!/usr/bin/env bash
# Prepare the exact source version corresponding to the OV5647 development
# baseline in preflight 2026-10-16. Downloads/extracts into a user-owned
# directory. Does NOT install packages, write /boot, alter drivers or use sudo.
set -euo pipefail

KERNEL_RELEASE="6.18.50+rpt-rpi-2712"
SOURCE_VERSION="6.18.50-1+rpt1"
APT_VERSION="1:6.18.50-1+rpt1" # Debian kernel package epoch
PKG="linux-source-6.18"
OUT="${1:-kernel/work/ov5647-6.18.50}"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -m)" == "aarch64" ]] || fail "Expected ARM64 Raspberry Pi environment"
[[ "$(uname -r)" == "$KERNEL_RELEASE" ]] ||
    fail "Running kernel $(uname -r), expected $KERNEL_RELEASE; pin a matching source instead"
command -v apt >/dev/null || fail "apt is required"
command -v dpkg-deb >/dev/null || fail "dpkg-deb is required"
command -v tar >/dev/null || fail "tar is required"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
mkdir -p "$OUT/deb-extract"
cd "$OUT"

printf '%s\n' "# hscam OV5647 kernel source preparation"
printf 'running kernel: %s\nsource package: %s=%s\noutput: %s\n'     "$KERNEL_RELEASE" "$PKG" "$SOURCE_VERSION" "$OUT"

if [[ ! -f "${PKG}_${SOURCE_VERSION}_all.deb" ]]; then
    printf '\nDownloading pinned source package (no root privileges)...\n'
    # Debian kernel packages may have a 1: epoch; omitted from .deb filename.
    apt download "${PKG}=${APT_VERSION}" ||
        apt download "${PKG}=${SOURCE_VERSION}" ||
        fail "Exact source version not in configured apt indexes. Check apt-cache policy $PKG. Do not substitute latest."
fi

deb="${PKG}_${SOURCE_VERSION}_all.deb"
declared_version=$(dpkg-deb --field "$deb" Version)
if [[ "$declared_version" != "$APT_VERSION" &&
      "$declared_version" != "$SOURCE_VERSION" ]]; then
    fail "Downloaded package reports $declared_version; expected $SOURCE_VERSION"
fi
dpkg-deb -x "$deb" "$OUT/deb-extract"
archive="$OUT/deb-extract/usr/src/linux-source-6.18.tar.xz"
[[ -f "$archive" ]] ||
    fail "linux-source archive not present in downloaded package"

if [[ ! -d "$OUT/linux-source-6.18" ]]; then
    tar -xf "$archive" -C "$OUT"
fi
driver="$OUT/linux-source-6.18/drivers/media/i2c/ov5647.c"
[[ -f "$driver" ]] ||
    fail "Expected ov5647.c missing in source tree; inspect archive layout"
sha256sum "$driver" | tee "$OUT/ov5647-stock.sha256"
printf '\nStock OV5647 source: %s\n' "$driver"
printf 'Source package: %s\n' "$SOURCE_VERSION"
printf '%s\n' 'No live driver, camera sensor or boot configuration was changed.'
printf '%s\n' 'NEXT: inspect/verify source and kernel configuration before writing an opt-in mode patch.'
