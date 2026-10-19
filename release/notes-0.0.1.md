Native Wi-Fi for the Qualcomm WCN7850 (FastConnect 7800 / NCM865) on macOS 26 Tahoe with OpenCore. First release.

**AirportWCN7850-0.0.1.zip** contains only the kext (firmware embedded); nothing else is needed.

Install
1. Copy `AirportWCN7850.kext` to `EFI/OC/Kexts` and add it to `Kernel → Add` (`MinKernel` 25.0.0).
2. Not alongside itlwm or AirportItlwm. Disable `SSDT-Disable_Network_GPP7` if your config hides the card.
3. Reboot and join a network from the Wi-Fi menu.

Works: scanning, WPA2-PSK and open networks, DHCP, about 170 Mbit/s each way, auto-connect at boot, private Wi-Fi addresses.
Not yet: WPA3/SAE, 11ac/ax/be rates, AirDrop/AWDL and Continuity; sleep/wake is untested. `-qwzoff` in boot-args disables the driver.

Tested on an MSI MAG X870 Tomahawk WiFi, macOS 26.7.1. Bluetooth for the same chip: https://github.com/razvanccr/BluetoothWCN7850
