#!/bin/bash
# prepare-usb-efi.sh <nvme EFI mount> <usb EFI mount> [kext]
#
# Turns a USB OpenCore EFI into a test copy of the NVMe one plus QwzMac:
#   1. backs up the USB config.plist
#   2. copies the NVMe config.plist, Drivers and Tools to the USB (the NVMe EFI is only read)
#   3. copies the kext into EFI/OC/Kexts and adds its Kernel -> Add entry
#   4. disables SSDT-Disable_Network_GPP7.aml and adds qwz_delay=120 qwz_debug=0x5
#      to boot-args (this config deletes boot-args from NVRAM, so they apply)
# Boot from the USB to test; booting from the NVMe is unaffected.
set -euo pipefail
cd "$(dirname "$0")/.."
NVME=${1:?usage: prepare-usb-efi.sh <nvme EFI mount> <usb EFI mount> [kext]}
USB=${2:?usage: prepare-usb-efi.sh <nvme EFI mount> <usb EFI mount> [kext]}
KEXT=${3:-build-debug/QwzMac.kext}
PB=/usr/libexec/PlistBuddy
CFG=$USB/EFI/OC/config.plist
BOOTARGS_KEY=":NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args"

for d in "$NVME/EFI/OC" "$USB/EFI/OC"; do [ -f "$d/config.plist" ] || { echo "STOP: no $d/config.plist"; exit 1; }; done
[ "$NVME" != "$USB" ] || { echo "STOP: both paths are the same"; exit 1; }
[ -d "$KEXT/Contents/MacOS" ] || { echo "STOP: no kext at $KEXT"; exit 1; }

cp "$CFG" "$CFG.before-qwzmac-$(date +%Y%m%d-%H%M%S)"
cp "$NVME/EFI/OC/config.plist" "$CFG"
cp "$NVME"/EFI/OC/Drivers/*.efi "$USB/EFI/OC/Drivers/"
cp "$NVME"/EFI/OC/Tools/*.efi "$USB/EFI/OC/Tools/"
rm -rf "$USB/EFI/OC/Kexts/QwzMac.kext"
cp -R "$KEXT" "$USB/EFI/OC/Kexts/QwzMac.kext"

find_entry() {  # find_entry <array path> <field> <value>
  local i=0 v
  while v=$($PB -c "Print $1:$i:$2" "$CFG" 2>/dev/null); do
    [ "$v" = "$3" ] && { echo $i; return; }
    i=$((i + 1))
  done
}

n=0; while $PB -c "Print :Kernel:Add:$n" "$CFG" >/dev/null 2>&1; do n=$((n + 1)); done
$PB -c "Add :Kernel:Add:$n dict" \
    -c "Add :Kernel:Add:$n:Arch string Any" \
    -c "Add :Kernel:Add:$n:BundlePath string QwzMac.kext" \
    -c "Add :Kernel:Add:$n:Comment string Qualcomm WCN7850 Wi-Fi (QwzMac)" \
    -c "Add :Kernel:Add:$n:Enabled bool true" \
    -c "Add :Kernel:Add:$n:ExecutablePath string Contents/MacOS/QwzMac" \
    -c "Add :Kernel:Add:$n:MaxKernel string" \
    -c "Add :Kernel:Add:$n:MinKernel string" \
    -c "Add :Kernel:Add:$n:PlistPath string Contents/Info.plist" "$CFG"

ssdt=$(find_entry :ACPI:Add Path SSDT-Disable_Network_GPP7.aml)
if [ -n "$ssdt" ]; then $PB -c "Set :ACPI:Add:$ssdt:Enabled false" "$CFG"; fi

args="$($PB -c "Print $BOOTARGS_KEY" "$CFG") qwz_delay=120 qwz_debug=0x5"
$PB -c "Set $BOOTARGS_KEY $args" "$CFG"

plutil -lint "$CFG"
echo "Kernel -> Add entry $n: $($PB -c "Print :Kernel:Add:$n:BundlePath" "$CFG") enabled=$($PB -c "Print :Kernel:Add:$n:Enabled" "$CFG")"
echo "boot-args: $($PB -c "Print $BOOTARGS_KEY" "$CFG")"
sync
echo "USB EFI ready"
