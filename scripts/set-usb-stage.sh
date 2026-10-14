#!/bin/bash
# set-usb-stage.sh <n>: mount the USB EFI, copy build-debug/QwzMac.kext there,
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
vol=$(mount | awk -v p="/dev/$part" '$1==p{print $3}')
CFG=$vol/EFI/OC/config.plist
[ -f "$CFG" ] || { echo "STOP: $CFG not found"; exit 1; }
rm -rf "$vol/EFI/OC/Kexts/QwzMac.kext"
cp -R build-debug/QwzMac.kext "$vol/EFI/OC/Kexts/"
args=$($PB -c "Print $KEY" "$CFG" | tr ' ' '\n' | grep -v '^qwz_stage=' | tr '\n' ' ')
$PB -c "Set $KEY ${args}qwz_stage=$N" "$CFG"
echo "kext $(shasum -a 256 build-debug/QwzMac.kext/Contents/MacOS/QwzMac | cut -c1-8) copied to $vol"
echo "boot-args: $($PB -c "Print $KEY" "$CFG")"
diskutil unmount "$vol" >/dev/null && echo "unmounted $part"
