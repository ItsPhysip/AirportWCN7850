#!/bin/bash
# fetch-firmware.sh -> firmware/FwBinary.cpp
# Downloads the WCN7850 firmware from linux-firmware at the release OpenBSD's
# qwz firmware package uses (ports/sysutils/firmware/qwz), checks it, and
# compiles it into a C++ table that loadfirmware() serves from.
# The firmware is Qualcomm's (LICENSE.QualcommAtheros_ath10k): redistributable
# in binary form, so it is fetched at build time and never committed.
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=20260309
BASE=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$TAG
DL=firmware/dl
mkdir -p "$DL"

fetch() {  # fetch <path in linux-firmware> <local name>
  [ -s "$DL/$2" ] || curl -fsSL -o "$DL/$2" "$BASE/$1"
}
fetch ath12k/WCN7850/hw2.0/amss.bin    qwz-wcn7850-hw2.0-amss
fetch ath12k/WCN7850/hw2.0/m3.bin      qwz-wcn7850-hw2.0-m3
fetch ath12k/WCN7850/hw2.0/board-2.bin qwz-wcn7850-hw2.0-board-2
fetch LICENSE.QualcommAtheros_ath10k   LICENSE.QualcommAtheros_ath10k

if [ -f firmware/SHA256SUMS ]; then
  (cd "$DL" && shasum -a 256 -c ../SHA256SUMS)
else
  (cd "$DL" && shasum -a 256 qwz-* LICENSE.* > ../SHA256SUMS)
  echo "recorded firmware/SHA256SUMS (commit it)"
fi

python3 -I scripts/fw_gen.py firmware/FwBinary.cpp "$DL"/qwz-*
