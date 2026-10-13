#!/bin/bash
# install-efi.sh [kext] [--remove]  -- inject QwzMac with OpenCore.
#
# Mounts the EFI partition OpenCore booted from (or $DISK), backs up
# config.plist, copies the kext into EFI/OC/Kexts, adds or enables its
# Kernel -> Add entry, disables SSDT-Disable_Network_GPP7.aml (it is meant to
# hide this card) and adds qwz_delay=120 qwz_debug=0x5 to boot-args.
# --remove disables the Kernel -> Add entry and leaves everything else alone.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

KEXT=build-debug/QwzMac.kext
REMOVE=0
for a in "$@"; do
  case "$a" in
    --remove) REMOVE=1 ;;
    *) KEXT=$a ;;
  esac
done
PB=/usr/libexec/PlistBuddy
OCGUID=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
BOOTARGS_KEY=":NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args"

# The EFI partition OpenCore booted from, unless DISK is given.
if [ -z "${DISK:-}" ]; then
  uuid=$(nvram "$OCGUID:boot-path" | grep -oE 'GPT,[0-9A-F-]{36}' | cut -d, -f2)
  DISK=$(diskutil info "$uuid" | awk '/Device Identifier/{print $3}')
fi
echo "EFI partition: $DISK"
diskutil mount "$DISK" >/dev/null
MNT=$(diskutil info "$DISK" | sed -n 's/^ *Mount Point: *//p')
OC=$MNT/EFI/OC
CFG=$OC/config.plist
[ -f "$CFG" ] || { echo "STOP: no $CFG"; exit 1; }

BACKUP=$CFG.before-qwzmac-$(date +%Y%m%d-%H%M%S)
cp "$CFG" "$BACKUP"
echo "backup: $BACKUP"

# Index of an array entry whose <field> equals <value>, or nothing.
find_entry() {  # find_entry <array path> <field> <value>
  local i=0
  while v=$($PB -c "Print $1:$i:$2" "$CFG" 2>/dev/null); do
    [ "$v" = "$3" ] && { echo $i; return; }
    i=$((i + 1))
  done
}

idx=$(find_entry :Kernel:Add BundlePath QwzMac.kext)

if [ $REMOVE = 1 ]; then
  [ -n "$idx" ] && $PB -c "Set :Kernel:Add:$idx:Enabled false" "$CFG"
  echo "QwzMac disabled in Kernel -> Add"
else
  [ -d "$KEXT/Contents/MacOS" ] || { echo "STOP: no kext at $KEXT (build it first)"; exit 1; }
  rm -rf "$OC/Kexts/QwzMac.kext"
  cp -R "$KEXT" "$OC/Kexts/QwzMac.kext"

  if [ -z "$idx" ]; then
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
  else
    $PB -c "Set :Kernel:Add:$idx:Enabled true" "$CFG"
  fi

  ssdt=$(find_entry :ACPI:Add Path SSDT-Disable_Network_GPP7.aml)
  [ -n "$ssdt" ] && $PB -c "Set :ACPI:Add:$ssdt:Enabled false" "$CFG" && echo "disabled SSDT-Disable_Network_GPP7.aml"

  args=$($PB -c "Print $BOOTARGS_KEY" "$CFG")
  for a in qwz_delay=120 qwz_debug=0x5; do
    case " $args " in *" ${a%%=*}="*) ;; *) args="$args $a" ;; esac
  done
  $PB -c "Set $BOOTARGS_KEY $args" "$CFG"
  echo "boot-args: $args"
fi

plutil -lint "$CFG"
sync
diskutil unmount "$DISK" >/dev/null
echo "done; reboot to apply"
