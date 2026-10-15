#!/bin/bash
# [QWZ_DEBUG=<mask>] [QWZ_BUILD=release] [QWZ_PRODUCT=AirportWCN7850] set-usb-stage.sh <n>: mount the USB EFI, copy build-debug/QwzMac.kext there,
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
# QWZ_BUILD=release (env) deploys from build/ instead of build-debug/.
# QWZ_PRODUCT=AirportWCN7850 (env) deploys the native Wi-Fi kext instead of the
# HeliPort one; both match the card, so the other one is removed and the
# config's Kernel>Add entry is pointed at the chosen kext.
SRC=build-debug; [ "${QWZ_BUILD:-}" = release ] && SRC=build
PRODUCT=${QWZ_PRODUCT:-QwzMac}
OTHER=QwzMac; [ "$PRODUCT" = QwzMac ] && OTHER=AirportWCN7850
rm -rf "$vol/EFI/OC/Kexts/QwzMac.kext" "$vol/EFI/OC/Kexts/AirportWCN7850.kext" "$vol/EFI/OC/Kexts/AirportQwz.kext"
cp -R "$SRC"/$PRODUCT.kext "$vol/EFI/OC/Kexts/"
python3 - "$CFG" "$PRODUCT" <<'PY'
import plistlib, sys
cfg, product = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
for k in d['Kernel']['Add']:
    if k['BundlePath'] in ('QwzMac.kext', 'AirportWCN7850.kext', 'AirportQwz.kext'):
        k['BundlePath'] = product + '.kext'
        k['ExecutablePath'] = 'Contents/MacOS/' + product
        k['Comment'] = 'Qualcomm WCN7850 Wi-Fi (' + product + ')'
        k['Enabled'] = True
plistlib.dump(d, open(cfg, 'wb'))
PY
# QWZ_DEBUG=<mask> (env) also sets qwz_debug=<mask>; 0x5 dumps every QMI byte
# as its own synced log line, which slows the handshake enough to time out.
args=$($PB -c "Print $KEY" "$CFG" | tr ' ' '\n' | grep -v '^qwz_stage=' | { [ -n "${QWZ_DEBUG:-}" ] && grep -v '^qwz_debug=' || cat; } | tr '\n' ' ')
[ -n "${QWZ_DEBUG:-}" ] && args="${args}qwz_debug=$QWZ_DEBUG "
# QWZ_PANIC=1 keeps the kernel on the panic screen (debug=0x144) instead of rebooting; QWZ_PANIC=0 restores debug=0x100.
[ "${QWZ_PANIC:-}" = 1 ] && args=$(echo "$args" | sed 's/debug=0x100/debug=0x144/')
[ "${QWZ_PANIC:-}" = 0 ] && args=$(echo "$args" | sed 's/debug=0x144/debug=0x100/')
# QWZ_AIRPORT_DELAY=<s> sets qwz_airport_delay=<s> (AirportWCN7850 starts its Wi-Fi layer <s> seconds after boot, logging to the file); 0 removes it.
args=$(echo "$args" | tr ' ' '\n' | grep -v '^qwz_airport_delay=' | tr '\n' ' ')
[ -n "${QWZ_AIRPORT_DELAY:-}" ] && [ "${QWZ_AIRPORT_DELAY}" != 0 ] && args="${args}qwz_airport_delay=$QWZ_AIRPORT_DELAY "
# QWZ_OFF=1 adds -qwzoff (the driver refuses to match); QWZ_OFF=0 removes it.
[ "${QWZ_OFF:-}" = 1 ] && args="-qwzoff ${args}"
[ "${QWZ_OFF:-}" = 0 ] && args=$(echo "$args" | sed 's/-qwzoff //')
$PB -c "Set $KEY ${args}qwz_stage=$N" "$CFG"
echo "$PRODUCT ($SRC) $(shasum -a 256 "$SRC"/$PRODUCT.kext/Contents/MacOS/$PRODUCT | cut -c1-8) copied to $vol"
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
