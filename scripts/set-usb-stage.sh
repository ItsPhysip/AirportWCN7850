#!/bin/bash
# [QWZ_DEBUG=<mask>] set-usb-stage.sh <n>: mount the USB EFI, copy build-debug/QwzMac.kext there,
# set qwz_stage=<n> in the config's boot-args (the stage limit; 0 = driver idle,
# 1000 = full bring-up), then unmount. Needs sudo for the mount.
set -euo pipefail
cd "$(dirname "$0")/.."
N=${1:?usage: $0 <stage>}
PB=/usr/libexec/PlistBuddy
KEY=":NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args"
disk=$(diskutil list external physical | awk '/^\/dev\/disk/{d=$1} /EFI/{print d; exit}')
[ -n "$disk" ] || { echo "STOP: no external USB disk with an EFI partition"; exit 1; }
part=${disk}s1
sudo diskutil mount "$part" >/dev/null
vol=$(mount | awk -v p="$part" '$1==p{print $3}')
CFG=$vol/EFI/OC/config.plist
[ -f "$CFG" ] || { echo "STOP: $CFG not found"; exit 1; }
# QWZ_BUILD=release (env) deploys build/QwzMac.kext instead of build-debug/.
SRC=build-debug; [ "${QWZ_BUILD:-}" = release ] && SRC=build
rm -rf "$vol/EFI/OC/Kexts/QwzMac.kext"
cp -R "$SRC"/QwzMac.kext "$vol/EFI/OC/Kexts/"
# QWZ_DEBUG=<mask> (env) also sets qwz_debug=<mask>; 0x5 dumps every QMI byte
# as its own synced log line, which slows the handshake enough to time out.
args=$($PB -c "Print $KEY" "$CFG" | tr ' ' '\n' | grep -v '^qwz_stage=' | { [ -n "${QWZ_DEBUG:-}" ] && grep -v '^qwz_debug=' || cat; } | tr '\n' ' ')
[ -n "${QWZ_DEBUG:-}" ] && args="${args}qwz_debug=$QWZ_DEBUG "
$PB -c "Set $KEY ${args}qwz_stage=$N" "$CFG"
echo "kext $SRC $(shasum -a 256 "$SRC"/QwzMac.kext/Contents/MacOS/QwzMac | cut -c1-8) copied to $vol"
echo "boot-args: $($PB -c "Print $KEY" "$CFG")"
# Spotlight sometimes holds a freshly written volume for a few seconds.
unmounted=0
for i in 1 2 3 4 5; do
  diskutil unmount "$vol" >/dev/null 2>&1 && { echo "unmounted $part"; unmounted=1; break; }
  sleep 3
done
[ $unmounted = 1 ] || { echo "could not unmount $vol; eject it in Finder before rebooting"; exit 1; }
# Hard power-offs can leave the FAT dirty; check it (read-only) after every deploy.
raw=${part/\/dev\/disk//dev/rdisk}
echo "--- filesystem check of $raw"
if sudo fsck_msdos -n "$raw"; then
  echo "USB EFI is clean"
else
  echo "USB EFI has errors: repair with  sudo fsck_msdos -y $raw"
  exit 1
fi
