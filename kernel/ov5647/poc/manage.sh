#!/usr/bin/env bash
# Explicit POC-only deployment; preserves the prior tested override byte-for-byte.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
kernel=6.18.50+rpt-rpi-2712
old=f8a5b3a6a85b5f1da9d31e95716bdef40d0bc0ce4003620f577de16266217777
new=bc88ac19568d23a54785206b5d0aa808e4198b3be24e7b4e797ce7437b35af93
candidate="$root/kernel/work/ov5647-poc-module/ov5647.ko"
installed="/lib/modules/$kernel/updates/hscam/ov5647.ko"
backup=/var/lib/hscam-poc/ov5647-before-poc.ko
hash() { sha256sum "$1" | awk '{print $1}'; }
[[ $(uname -r) == "$kernel" ]]
[[ $EUID == 0 ]]
case ${1:-status} in
stage)
 [[ $(hash "$installed") == "$old" ]]
 [[ $(hash "$candidate") == "$new" ]]
 [[ $(modinfo -F hscam_experimental "$candidate") == ov5647-poc-128-64-32-rev1-unvalidated ]]
 [[ $(modinfo -F vermagic "$candidate") == "$kernel "* ]]
 [[ ! -e "$backup" ]]
 install -d /var/lib/hscam-poc
 install -m644 "$installed" "$backup"
 cmp "$backup" "$installed"
 install -m644 "$candidate" "$installed.poc-tmp"
 mv "$installed.poc-tmp" "$installed"
 if ! depmod -a "$kernel" || [[ $(modinfo -n ov5647) != "$installed" ]]; then
   install -m644 "$backup" "$installed"; depmod -a "$kernel"; exit 1
 fi
 echo 'POC staged; previous override preserved. Reboot separately.'
 ;;
restore)
 [[ $(hash "$installed") == "$new" ]]
 [[ $(hash "$backup") == "$old" ]]
 install -m644 "$backup" "$installed.poc-tmp"
 mv "$installed.poc-tmp" "$installed"
 depmod -a "$kernel"
 echo 'Previous 640x240 module restored on disk; reboot separately.'
 ;;
status)
 modinfo -n ov5647
 sha256sum "$installed"
 test ! -e "$backup" || sha256sum "$backup"
 ;;
*) echo 'usage: manage.sh stage|restore|status' >&2; exit 2;;
esac
