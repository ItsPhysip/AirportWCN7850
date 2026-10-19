Second release: 802.11ac and a correct link display.

**AirportWCN7850-0.0.2.zip** contains only the kext (firmware embedded); nothing else is needed. Install as for 0.0.1 (copy to `EFI/OC/Kexts`, `Kernel → Add` with `MinKernel` 25.0.0, reboot).

Changes since 0.0.1
- 802.11ac: the driver advertises the firmware's VHT capabilities, sends the negotiated VHT state in the peer-assoc command and runs the vdev at 80 MHz (VHT80, up to MCS 9, 2 spatial streams, beamformee). `-novht` in boot-args keeps 802.11n.
- Link display: `wdutil info` and the Wi-Fi menu show the real RSSI, noise, Tx/Rx rate, MCS, guard interval and NSS. They come from the RX descriptors and the link-quality snapshot the driver posts to IO80211Family.
- The 802.11 phy mode stays 11ac (or 11n) while associated; macOS's background scans used to leave it at "auto".
- Fixed: the 0.0.1 zip could not join WPA/WPA2 networks. Optimised builds sent an RSN element with garbage suite OUIs (a compound literal used from C++), and access points refused the association with status 40. Only debug builds had worked.
- `debug.qwz.*` sysctls and `scripts/qwz-log.sh` work with this kext too; the log ring records the first request of every apple80211 ioctl that reaches the driver.

Still not: WPA3/SAE, 11ax/be, AirDrop/AWDL and Continuity; sleep/wake is untested.

Tested on an MSI MAG X870 Tomahawk WiFi, macOS 26.7.1. Bluetooth for the same chip: https://github.com/razvanccr/BluetoothWCN7850
