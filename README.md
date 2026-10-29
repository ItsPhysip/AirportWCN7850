# AirportWCN7850

Fork of the [razvanccr/AirportWCN7850](https://github.com/razvanccr/AirportWCN7850) macOS Wi-Fi driver for the Qualcomm WCN7850 that fixes tx power, country code, and signal strength reading. Tested and works on my Gigabyte X870E AORUS MASTER with macOS 26.7.2 and OpenCore. Most of the driver is upstream's work, so check their README for what it supports.

Results in 280 Mbit/s down and 200 up through a UniFi AC-LR on channel 149, compared to a 130 Mbit/s link at -73 dBm before, when macOS couldn't read signal strength and kept joining a further away access point or failing outright to connect. YMMV.

## Fixed/Changed

- `qwz_cc=NZ` (or any two-letter country code) in boot-args sets the Wi-Fi country. Without it the card stays on the world domain, which caps every band at 20 dBm.
- The tx power limit is 30 dBm instead of 20. The firmware still applies the country's limits on top.
- Signal strength is in dBm. The card reports it relative to the noise floor, and macOS was seeing numbers like 118, so it picked the wrong access points.
- On 5 GHz the driver takes a network's channel from its beacon, like Linux does.
- `qwz_bdf=<variant>` picks the board calibration variant, and the driver logs which one it loaded.
- Joins are logged (state, channel, signal and BSSID) in the driver's log: `ioreg -l -w0 -a -k QwzMacLog -r`.

## Building

You need the Command Line Tools, git, curl and python3.

```bash
scripts/fetch-firmware.sh && PRODUCT=AirportWCN7850 ./build.sh
```

The kext ends up in `build/AirportWCN7850.kext`. Put it in `EFI/OC/Kexts`, add it under `Kernel → Add` with `MinKernel` 25.0.0, and don't load itlwm or AirportItlwm alongside it. If your config has `SSDT-Disable_Network_GPP7`, disable it.

The firmware comes from linux-firmware and is embedded in the kext, not committed here.

## Licence

GPL-2.0-or-later, see `LICENSE`. The qwz code is from OpenBSD (ISC) and Linux ath12k, and the 802.11 stack is from itlwm. Upstream's README has the full credits.
