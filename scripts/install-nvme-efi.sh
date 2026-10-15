#!/bin/bash
# install-nvme-efi.sh: put AirportWCN7850.kext on the NVMe (main) EFI.
#   1. backs up the EFI's OC/config.plist and OC/Kexts to ~/Documents/EFI-backups/nvme-EFI-<date>/
#   2. copies build-debug/AirportWCN7850.kext (QWZ_BUILD=release: build/) into OC/Kexts
#   3. adds/updates its Kernel>Add entry (removes any QwzMac/AirportQwz entry)
#   4. disables SSDT-Disable_Network_GPP7.aml so macOS can see the card
# Boot-args are not touched. Needs sudo for the mount.
set -euo pipefail
cd "$(dirname "$0")/.."
PB=/usr/libexec/PlistBuddy
SRC=build-debug; [ "${QWZ_BUILD:-}" = release ] && SRC=build
[ -d "$SRC/AirportWCN7850.kext" ] || { echo "STOP: $SRC/AirportWCN7850.kext not built"; exit 1; }
# the EFI partition of the disk that holds the running system
sys=$(diskutil info / | awk -F': *' '/Part of Whole/{print $2}')          # e.g. disk2 (APFS container)
phys=$(diskutil info "$sys" | awk -F': *' '/APFS Physical Store/{print $2}' | head -1)   # e.g. disk1s1
disk=${phys%s*}                                                            # disk1
part=$(diskutil list "$disk" | awk '/EFI/{print $NF; exit}')              # disk1s2
[ -n "$part" ] || { echo "STOP: no EFI partition on $disk"; exit 1; }
echo "system disk $disk, EFI $part"
mp=/Volumes/NVMeEFI
sudo mkdir -p "$mp"
mount | grep -q " $mp " || sudo diskutil mount -mountPoint "$mp" "$part" >/dev/null
CFG=$mp/EFI/OC/config.plist
[ -f "$CFG" ] || { echo "STOP: $CFG not found"; exit 1; }
BK=~/Documents/EFI-backups/nvme-EFI-$(date +%Y%m%d-%H%M%S)
mkdir -p "$BK" && cp -R "$mp/EFI/OC/config.plist" "$mp/EFI/OC/Kexts" "$BK/" && echo "backup: $BK"
rm -rf "$mp/EFI/OC/Kexts/AirportWCN7850.kext" "$mp/EFI/OC/Kexts/AirportQwz.kext" "$mp/EFI/OC/Kexts/QwzMac.kext"
cp -R "$SRC/AirportWCN7850.kext" "$mp/EFI/OC/Kexts/"
# QWZ_USBMAP=<dir with USBToolBox.kext and UTBMap.kext> also installs the USB
# port map (USBToolBox + UTBMap enabled, UTBDefault removed): needed for the
# Bluetooth half of the chip, which sits on a port the default map hides.
USBMAP=${QWZ_USBMAP:-}
if [ -n "$USBMAP" ]; then
  [ -d "$USBMAP/UTBMap.kext" ] && [ -d "$USBMAP/USBToolBox.kext" ] || { echo "STOP: $USBMAP lacks UTBMap.kext/USBToolBox.kext"; exit 1; }
  rm -rf "$mp/EFI/OC/Kexts/UTBMap.kext" "$mp/EFI/OC/Kexts/UTBDefault.kext" "$mp/EFI/OC/Kexts/USBToolBox.kext"
  cp -R "$USBMAP/UTBMap.kext" "$USBMAP/USBToolBox.kext" "$mp/EFI/OC/Kexts/"
  echo "USB map installed from $USBMAP"
fi
python3 - "$CFG" "$USBMAP" <<'PY'
import plistlib, sys
cfg, usbmap = sys.argv[1], sys.argv[2]
d = plistlib.load(open(cfg, 'rb'))
if usbmap:
    add = []
    for k in d['Kernel']['Add']:
        if k['BundlePath'] == 'UTBDefault.kext':
            continue
        if k['BundlePath'] == 'USBToolBox.kext':
            k['Enabled'] = True
        if k['BundlePath'] == 'UTBMap.kext':
            continue
        add.append(k)
    i = next((n for n, k in enumerate(add) if k['BundlePath'] == 'USBToolBox.kext'), len(add) - 1)
    add.insert(i + 1, {'Arch': 'Any', 'BundlePath': 'UTBMap.kext', 'Comment': 'USB map (USBToolBox) MSI MAG X870 Tomahawk',
                       'Enabled': True, 'ExecutablePath': '', 'MaxKernel': '', 'MinKernel': '', 'PlistPath': 'Contents/Info.plist'})
    d['Kernel']['Add'] = add
add = [k for k in d['Kernel']['Add'] if k['BundlePath'] not in ('QwzMac.kext', 'AirportQwz.kext', 'AirportWCN7850.kext')]
add.append({'Arch': 'Any', 'BundlePath': 'AirportWCN7850.kext', 'Comment': 'Qualcomm WCN7850 Wi-Fi (AirportWCN7850)',
            'Enabled': True, 'ExecutablePath': 'Contents/MacOS/AirportWCN7850', 'MaxKernel': '', 'MinKernel': '25.0.0',
            'PlistPath': 'Contents/Info.plist'})
d['Kernel']['Add'] = add
for a in d['ACPI']['Add']:
    if a['Path'] == 'SSDT-Disable_Network_GPP7.aml':
        a['Enabled'] = False
plistlib.dump(d, open(cfg, 'wb'))
for k in d['Kernel']['Add']: print('KEXT', k['BundlePath'], k['Enabled'])
for a in d['ACPI']['Add']: print('ACPI', a['Path'], a['Enabled'])
PY
plutil -lint "$CFG"
echo "kext $(shasum -a 256 "$SRC"/AirportWCN7850.kext/Contents/MacOS/AirportWCN7850 | cut -c1-8) installed on $part"
for i in 1 2 3 4 5; do diskutil unmount "$mp" >/dev/null 2>&1 && { echo "unmounted $part"; break; }; sleep 3; done
sudo fsck_msdos -n "/dev/r${part}" && echo "NVMe EFI is clean"
