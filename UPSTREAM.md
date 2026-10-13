# Upstream sources

| Path | From | Revision |
|---|---|---|
| `qwz/qwz.c`, `qwz/qwzreg.h`, `qwz/qwzvar.h` | OpenBSD `sys/dev/ic/` | openbsd/src 0a0964b495419288cabfc55084d7a01338753141 |
| `qwz/if_qwz_pci.c` | OpenBSD `sys/dev/pci/` | openbsd/src 0a0964b495419288cabfc55084d7a01338753141 |
| `itl80211/`, `include/` | OpenIntelWireless/itlwm | 53c51c2cdd6e4b69beb91f310d74c53422b0f8bd |
| `controller/` | OpenIntelWireless/itlwm `itlwm/` | 53c51c2cdd6e4b69beb91f310d74c53422b0f8bd |

The first commit in this repository holds these files unmodified, so `git diff <first commit> -- <path>` shows every local change.
