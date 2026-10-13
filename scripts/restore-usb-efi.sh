#!/bin/bash
# restore-usb-efi.sh  -- make the USB OpenCore EFI an exact copy of the one
# this Mac booted from (the NVMe), without QwzMac, so the USB is a working
# rescue boot again.
#
# The source is found from OpenCore's boot-path and only read. The target must
# be the EFI partition of an external USB disk. Its config.plist and QwzMac
# kext are saved to <USB EFI>/qwzmac-saved-<date>/ before EFI/ is replaced.
set -euo pipefail
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

OCGUID=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
uuid=$(nvram "$OCGUID:boot-path" | grep -oE 'GPT,[0-9A-F-]{36}' | cut -d, -f2)
SRC_DISK=$(diskutil info "$uuid" | awk '/Device Identifier/{print $3}')

USB_DISK=
for d in $(diskutil list external physical | awk '/ EFI /{print $NF}'); do
  diskutil info "${d%s*}" | grep -q 'Protocol: *USB' && USB_DISK=$d
done
[ -n "$SRC_DISK" ] || { echo "STOP: cannot find the EFI OpenCore booted from"; exit 1; }
[ -n "$USB_DISK" ] || { echo "STOP: no USB disk with an EFI partition"; exit 1; }
[ "$SRC_DISK" != "$USB_DISK" ] || { echo "STOP: booted from the USB; boot from the NVMe first"; exit 1; }
echo "source (booted, read-only): $SRC_DISK"
echo "target (USB):               $USB_DISK"

diskutil unmount "$SRC_DISK" >/dev/null 2>&1 || true
diskutil unmount "$USB_DISK" >/dev/null 2>&1 || true
diskutil mount readOnly "$SRC_DISK" >/dev/null
diskutil mount "$USB_DISK" >/dev/null
SRC=$(diskutil info "$SRC_DISK" | sed -n 's/^ *Mount Point: *//p')
USB=$(diskutil info "$USB_DISK" | sed -n 's/^ *Mount Point: *//p')
[ -f "$SRC/EFI/OC/config.plist" ] && [ -f "$USB/EFI/OC/config.plist" ] || { echo "STOP: OpenCore not found on both"; exit 1; }

SAVE=$USB/qwzmac-saved-$(date +%Y%m%d-%H%M%S)
mkdir -p "$SAVE"
cp "$USB/EFI/OC/config.plist" "$SAVE/"
[ -d "$USB/EFI/OC/Kexts/QwzMac.kext" ] && cp -R "$USB/EFI/OC/Kexts/QwzMac.kext" "$SAVE/"

rsync -rt --delete --exclude '._*' --exclude '.Spotlight-V100' --exclude '.fseventsd' "$SRC/EFI/" "$USB/EFI/"
sync

if diff -rq -x '._*' -x '.Spotlight-V100' -x '.fseventsd' "$SRC/EFI" "$USB/EFI"; then
  echo "USB EFI now matches the NVMe EFI (QwzMac removed); old USB config saved in $SAVE"
else
  echo "WARNING: differences remain (listed above)"
fi
diskutil unmount "$USB_DISK" >/dev/null
diskutil unmount "$SRC_DISK" >/dev/null
