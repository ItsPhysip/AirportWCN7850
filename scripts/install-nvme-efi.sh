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
python3 - "$CFG" <<'PY'
import plistlib, sys
cfg = sys.argv[1]
d = plistlib.load(open(cfg, 'rb'))
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
