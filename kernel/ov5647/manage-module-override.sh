#!/usr/bin/env bash
# stage/unstage only a matching-kernel OV5647 module override. NEVER unloads
# the live sensor driver, edits boot files or reboots.
set -euo pipefail

KERNEL="6.18.50+rpt-rpi-2712"
PROFILE="ov5647-640x240-rev1-unvalidated"
CANDIDATE_SHA256="f8a5b3a6a85b5f1da9d31e95716bdef40d0bc0ce4003620f577de16266217777"
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
CANDIDATE="$ROOT/kernel/work/ov5647-6.18.50/experimental-module/ov5647.ko"
STOCK="/lib/modules/$KERNEL/kernel/drivers/media/i2c/ov5647.ko.xz"
OVERRIDE="/lib/modules/$KERNEL/updates/hscam/ov5647.ko"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
usage:
  bash kernel/ov5647/manage-module-override.sh status
  bash kernel/ov5647/manage-module-override.sh check
  sudo bash kernel/ov5647/manage-module-override.sh stage
  sudo bash kernel/ov5647/manage-module-override.sh unstage

check: read-only preflight of pinned module, stock driver, and resolver.
stage/unstage only modify an OV5647 module file and depmod index.
They NEVER unload/reload live drivers, modify boot files, or reboot.
A separate explicit reboot is needed to select a staged driver.
EOF
}

for tool in modinfo modprobe sha256sum readlink; do
    command -v "$tool" >/dev/null || fail "Required command missing: $tool"
done

selected_module() {
    modinfo -n ov5647 2>/dev/null || true
}

require_pinned_kernel() {
    [[ "$(uname -r)" == "$KERNEL" ]] ||
        fail "Kernel $(uname -r) is not pinned $KERNEL"
    [[ -f "$STOCK" ]] || fail "Stock OV5647 module missing: $STOCK"
}

checksum() {
    sha256sum "$1" | awk '{ print $1 }'
}

verify_candidate() {
    [[ -f "$CANDIDATE" ]] || fail "Missing candidate: $CANDIDATE"
    [[ "$(modinfo -F name "$CANDIDATE")" == "ov5647" ]] ||
        fail "Candidate module name is not ov5647"
    local version profile hash
    version=$(modinfo -F vermagic "$CANDIDATE")
    [[ "$version" == "$KERNEL "* ]] ||
        fail "Candidate vermagic does not match running kernel"
    profile=$(modinfo -F hscam_experimental "$CANDIDATE")
    [[ "$profile" == "$PROFILE" ]] ||
        fail "Candidate experimental metadata does not match"
    hash=$(checksum "$CANDIDATE")
    [[ "$hash" == "$CANDIDATE_SHA256" ]] ||
        fail "Candidate SHA256 differs from tested artifact: $hash"
}

verify_override() {
    [[ -f "$OVERRIDE" && ! -L "$OVERRIDE" ]] ||
        fail "No regular hscam module override at $OVERRIDE"
    [[ "$(checksum "$OVERRIDE")" == "$CANDIDATE_SHA256" ]] ||
        fail "The override differs from the known candidate; refusing to remove it"
}

status() {
    printf '# OV5647 override status (read-only)\nKernel: %s\n' "$(uname -r)"
    printf 'Packaged stock: %s\n' "$STOCK"
    printf 'Stock present: %s\n' "$([[ -f "$STOCK" ]] && echo yes || echo no)"
    printf 'Override: %s\n' "$OVERRIDE"
    if [[ -f "$OVERRIDE" ]]; then
        printf 'Override SHA256: %s\n' "$(checksum "$OVERRIDE")"
    else
        printf 'Override present: no\n'
    fi
    printf 'Resolver selected: %s\n' "$(selected_module)"
    printf 'modprobe --show-depends:\n'
    modprobe --show-depends ov5647 || true
    if [[ -L /sys/bus/i2c/devices/10-0036/driver ]]; then
        printf 'Live driver bound: %s\n' \
            "$(basename "$(readlink -f /sys/bus/i2c/devices/10-0036/driver)")"
    fi
    grep '^ov5647 ' /proc/modules 2>/dev/null || true
    printf 'Resolver selection is not proof of which driver is currently loaded.\n'
}

check() {
    require_pinned_kernel
    verify_candidate
    [[ ! -e "$OVERRIDE" && ! -L "$OVERRIDE" ]] ||
        fail "Override path already exists; use status or unstage"
    local chosen
    chosen=$(selected_module)
    [[ -n "$chosen" && "$(readlink -f "$chosen")" == "$(readlink -f "$STOCK")" ]] ||
        fail "Resolver does not select packaged stock module: $chosen"
    [[ -L /sys/bus/i2c/devices/10-0036/driver ]] ||
        fail "OV5647 I2C device does not currently have a driver bound"
    [[ "$(basename "$(readlink -f /sys/bus/i2c/devices/10-0036/driver)")" == "ov5647" ]] ||
        fail "Unexpected I2C device driver bound"
    printf 'READ-ONLY PREFLIGHT PASSED\n'
    printf 'Kernel: %s\nStock module: %s\nCandidate: %s\n' "$KERNEL" "$STOCK" "$CANDIDATE"
    printf 'Candidate SHA256: %s\n' "$(checksum "$CANDIDATE")"
    printf 'Nothing installed, loaded, unloaded or rebooted.\n'
}

reindex() { depmod -a "$KERNEL"; }
restore_index_after_failed_stage() {
    rm -f -- "$OVERRIDE"
    reindex || true
}

stage() {
    [[ "$EUID" -eq 0 ]] || fail "stage requires sudo/root"
    command -v depmod >/dev/null || fail "depmod missing"
    require_pinned_kernel
    verify_candidate
    [[ ! -e "$OVERRIDE" && ! -L "$OVERRIDE" ]] ||
        fail "Override path occupied: will not overwrite any file"
    local chosen
    chosen=$(selected_module)
    [[ -n "$chosen" && "$(readlink -f "$chosen")" == "$(readlink -f "$STOCK")" ]] ||
        fail "Resolver is not selecting stock module: $chosen"
    install -d -m 0755 "$(dirname "$OVERRIDE")"
    if ! install -m 0644 "$CANDIDATE" "$OVERRIDE"; then
        restore_index_after_failed_stage
        fail "Copying candidate failed"
    fi
    if ! reindex; then
        restore_index_after_failed_stage
        fail "depmod failed, override removed"
    fi
    chosen=$(selected_module)
    if [[ "$chosen" != "$OVERRIDE" ]]; then
        restore_index_after_failed_stage
        fail "Override not selected by module resolver ($chosen); reverted"
    fi
    verify_override
    printf 'STAGED: %s\n' "$OVERRIDE"
    printf 'Resolver selects: %s\n' "$chosen"
    printf 'Stock module remains unchanged. Currently loaded driver unchanged.\n'
    printf 'Read kernel/ov5647/README.md before choosing to reboot.\n'
}

unstage() {
    [[ "$EUID" -eq 0 ]] || fail "unstage requires sudo/root"
    command -v depmod >/dev/null || fail "depmod missing"
    require_pinned_kernel
    verify_override
    rm -- "$OVERRIDE"
    reindex
    local chosen
    chosen=$(selected_module)
    [[ -n "$chosen" && "$(readlink -f "$chosen")" == "$(readlink -f "$STOCK")" ]] ||
        fail "Override removed but stock module was not selected: $chosen"
    printf 'UNSTAGED: stock module will be selected after a reboot.\n'
    printf 'No loaded driver was unloaded; a reboot is necessary if the experimental driver is active.\n'
}

action="status"
if [[ "$#" -ge 1 ]]; then action="$1"; fi
[[ "$#" -le 1 ]] || { usage >&2; exit 2; }
case "$action" in
status) status ;;
check) check ;;
stage) stage ;;
unstage) unstage ;;
--help|-h|help) usage ;;
*) usage >&2; exit 2 ;;
esac
