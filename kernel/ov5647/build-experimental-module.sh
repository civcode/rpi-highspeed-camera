#!/usr/bin/env bash
# Compile-only: never calls depmod, insmod, modprobe or installs any files.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
KERNEL="6.18.50+rpt-rpi-2712"
WORK="${1:-$ROOT/kernel/work/ov5647-6.18.50/experimental-module}"
case "$WORK" in /*) ;; *) WORK="$PWD/$WORK" ;; esac
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -r)" == "$KERNEL" ]] ||
    fail "Running kernel $(uname -r), expected $KERNEL"
[[ -f "$WORK/ov5647.c" && -f "$WORK/Makefile" ]] ||
    fail "Module source missing; first run python3 kernel/ov5647/prepare-experimental-module.py"
grep -q 'hscam_experimental.*ov5647-640x240-rev1-unvalidated' "$WORK/ov5647.c" ||
    fail "Source lacks expected experimental metadata"
[[ "$(cat "$WORK/Makefile")" == "obj-m += ov5647.o" ]] ||
    fail "Unexpected Kbuild Makefile content"
HEADERS="/lib/modules/$KERNEL/build"
[[ -d "$HEADERS" ]] ||
    fail "Matching prepared Linux headers not found at $HEADERS. Install the exact running kernel's matching linux-headers package first."

printf 'Kernel headers: %s\n' "$(readlink -f "$HEADERS")"
printf 'Candidate source: %s\n' "$WORK/ov5647.c"
printf '%s\n' 'Building an out-of-tree experimental module; no live driver is touched.'
make -C "$HEADERS" "M=$WORK" modules

MODULE="$WORK/ov5647.ko"
[[ -f "$MODULE" ]] || fail "Build did not produce $MODULE"
if command -v modinfo >/dev/null 2>&1; then
    ACTUAL=$(modinfo -F vermagic "$MODULE")
    printf 'Module vermagic: %s\n' "$ACTUAL"
    [[ "$ACTUAL" == "$KERNEL "* ]] ||
        fail "Module vermagic does not match the running kernel; do not install."
    [[ "$(modinfo -F hscam_experimental "$MODULE")" ==
       "ov5647-640x240-rev1-unvalidated" ]] ||
        fail "Module metadata mismatch; do not install."
fi
printf 'Compiled candidate: %s\n' "$MODULE"
sha256sum "$MODULE"
printf '%s\n' 'Compilation only: no module was installed, loaded, unbound or replaced.'
