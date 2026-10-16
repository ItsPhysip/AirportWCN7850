# AirportWCN7850

Native Wi-Fi for the **Qualcomm WCN7850** (FastConnect 7800, the NCM865 module) on macOS: the macOS Wi-Fi menu and settings, driven by a port of OpenBSD's [`qwz(4)`](https://man.openbsd.org/qwz.4) driver on the [itlwm](https://github.com/OpenIntelWireless/itlwm) 802.11 stack. macOS 26 only, no OCLP.

The Bluetooth half of the chip is a separate project: [BluetoothWCN7850](https://github.com/razvanccr/BluetoothWCN7850).

**Tested on:** MSI MAG X870 Tomahawk WiFi (onboard WCN7850), macOS 26.7.1 Tahoe, OpenCore. Scans, joins WPA2 networks, DHCP, about 170 Mbit/s each way, auto-connect at boot, private Wi-Fi addresses.

## Download

[release/AirportWCN7850-0.0.1.zip](release/AirportWCN7850-0.0.1.zip): the kext with the firmware embedded, and an `INSTALL.txt`.

## Install

1. Copy `AirportWCN7850.kext` to `EFI/OC/Kexts` and add it to `Kernel → Add` with `MinKernel` 25.0.0.
2. Not alongside itlwm or AirportItlwm; disable an `SSDT-Disable_Network_GPP7` if your config hides the card.
3. Reboot and join a network from the Wi-Fi menu.

`-qwzoff` in boot-args disables the driver. For a first boot on unknown hardware read [Testing safely](docs/bring-up.md#testing-safely).

## What does not work yet

- WPA3/SAE, 802.1X without a supplicant; WPA1 and WEP use software crypto (this is what OpenBSD's `qwz(4)` supports).
- 802.11n rates only; the chip's 11ac/ax/be, 320 MHz and MLO are not driven.
- AirDrop/AWDL and the rest of Continuity. Sleep/wake is not verified.
- Broadcast and multicast are only received on open networks or WPA2 with a CCMP group cipher (ARP/IPv6 can fail on TKIP-group networks).

How it works, the bring-up log, the debugging tools and the stage-by-stage bisection are in [docs/bring-up.md](docs/bring-up.md).

## Building

Command Line Tools, git, curl and python3:

```bash
scripts/fetch-firmware.sh && PRODUCT=AirportWCN7850 ./build.sh
```

`build/AirportWCN7850.kext` is the result (`DEBUG=1` for the traced build). The firmware comes from linux-firmware (pinned release, checksums in `firmware/SHA256SUMS`), is embedded, and is not committed.

## Licences and credits

- `qwz/qwz.c`, `qwz/if_qwz_pci.c`, `qwz/qwzreg.h`, `qwz/qwzvar.h`: OpenBSD `qwz(4)` (Stefan Sperling and the OpenBSD developers), ISC, with code derived from the Linux `ath12k` driver under the Linux Foundation / Qualcomm BSD licence.
- `itl80211/`, `include/`, `controller/` (except `ItlQwz.*`): itlwm by zxystd, GPL-2.0-or-later, with OpenBSD net80211 under its BSD/ISC licences. `airport/`: the AirportItlwm "WCL" layer by laobamac, as reshaped by [SimpleMtkWlan](https://github.com/pengubow/SimpleMtkWlan).
- `qwz/qwz_compat.*`, `controller/ItlQwz.*`, `scripts/`: GPL-2.0-or-later.
- WCN7850 firmware: Qualcomm, redistributable in binary form under `LICENSE.QualcommAtheros_ath10k`.
- Built with [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK).

The whole kext is distributed under GPL-2.0-or-later (`LICENSE`).
