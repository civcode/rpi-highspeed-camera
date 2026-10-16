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

# Debian APT percent-encodes a package epoch in the downloaded filename.
# For example: linux-source-6.18_1%3a6.18.50-1+rpt1_all.deb.
# Never assume that filename omits the epoch; validate the DEB's own Version
# field, and reuse the already-downloaded file rather than fetching 160 MB again.
find_pinned_deb() {
    local candidate declared_version
    for candidate in "${PKG}_"*.deb; do
        [[ -f "$candidate" ]] || continue
        declared_version=$(dpkg-deb --field "$candidate" Version 2>/dev/null) || continue
        if [[ "$declared_version" == "$APT_VERSION" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if ! deb=$(find_pinned_deb); then
    printf '\nDownloading pinned source package (no root privileges)...\n'
    apt download "${PKG}=${APT_VERSION}" ||
        fail "Exact source version unavailable. Check apt-cache policy $PKG; do not substitute latest."
    deb=$(find_pinned_deb) ||
        fail "Downloaded package missing or its internal Version differs from $APT_VERSION"
fi

printf 'Using verified source package: %s\n' "$deb"
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
