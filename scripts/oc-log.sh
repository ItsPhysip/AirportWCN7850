#!/bin/bash
# oc-log.sh <EFI mount> on|off  -- turn OpenCore's boot log file on or off.
#
# "on" makes OpenCore write EFI/../opencore-<date>.txt on every boot (Misc ->
# Debug -> Target 65 = logging + file, DisplayLevel = errors, warnings, info),
# which records each kext injection and why one failed. "off" restores the
# previous Target and DisplayLevel saved by "on".
set -euo pipefail
EFI=${1:?usage: oc-log.sh <EFI mount> on|off}
MODE=${2:?usage: oc-log.sh <EFI mount> on|off}
CFG=$EFI/EFI/OC/config.plist
SAVE=$EFI/EFI/OC/.oc-log-saved
PB=/usr/libexec/PlistBuddy
[ -f "$CFG" ] || { echo "STOP: no $CFG"; exit 1; }

case "$MODE" in
  on)
    [ -f "$SAVE" ] || echo "$($PB -c 'Print :Misc:Debug:Target' "$CFG") $($PB -c 'Print :Misc:Debug:DisplayLevel' "$CFG")" > "$SAVE"
    $PB -c "Set :Misc:Debug:Target 65" -c "Set :Misc:Debug:DisplayLevel 2147483714" "$CFG"
    ;;
  off)
    [ -f "$SAVE" ] || { echo "STOP: nothing saved by 'on'"; exit 1; }
    read -r target level < "$SAVE"
    $PB -c "Set :Misc:Debug:Target $target" -c "Set :Misc:Debug:DisplayLevel $level" "$CFG"
    rm -f "$SAVE"
    ;;
  *) echo "usage: oc-log.sh <EFI mount> on|off"; exit 1 ;;
esac
plutil -lint "$CFG"
echo "Target=$($PB -c 'Print :Misc:Debug:Target' "$CFG") DisplayLevel=$($PB -c 'Print :Misc:Debug:DisplayLevel' "$CFG")"
sync
