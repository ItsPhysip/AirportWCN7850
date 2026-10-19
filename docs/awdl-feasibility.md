# Can AirportWCN7850 do AirDrop? An AWDL feasibility study

Written 2026-10-17 against macOS 26.7.1 (IO80211Family 1200.13.1), the WCL build of
`AirportWCN7850.kext`, OpenBSD `qwz(4)` as mirrored in `qwz/`, and Linux ath12k.
Research only; no driver code was written for it.

## Verdict: no-go

AirDrop needs AWDL, and AWDL needs a radio that hops between three social
channels (6, 44, 149) plus the AP's channel on a 64 TU grid aligned to a peer's
clock within about 3 ms, while staying associated. On Apple's Macs that
schedule runs inside the Broadcom firmware; macOS only configures it. The
WCN7850 firmware has no such engine and exposes nothing I could build one from:
the host does not own the channel, cannot time a transmission to the TSF, and
its only off-channel primitive is the scan engine, whose start latency and
millisecond dwell granularity are of the same order as AWDL's whole error
budget. Every AWDL implementation that exists runs either in firmware (Apple)
or on a softMAC card where the host owns the channel register (owl on ath9k).
The WCN7850 is neither.

What I could build is the shell: `awdl0` appearing, the ~50 AWDL ioctls
answered, sharingd going quiet. That is one to two weeks and gets AirDrop to
show an empty window. Everything after that is a research project with a low
chance of ever reaching "sometimes works", and it would cost the AP link while
it ran. The practical route to AirDrop on this machine remains a Broadcom card
(section 6).

## 1. What macOS 26 wants from a driver

### 1.1 Where the pieces live

I pulled the AWDL strings and class names out of the boot kernel collection
(`/System/Library/KernelCollections/BootKernelExtensions.kc`; the macOS
`strings` tool skips fileset collections, a Python scan works). The split is:

| Layer | Owns | Evidence |
|---|---|---|
| wifip2pd / airportd / sharingd / rapportd (user space; Tahoe has no `wifid`) | wifip2pd creates and owns `awdl0` and sets the sync and election parameters; sharingd does the BLE triggering, mDNS/DNS-SD and the HTTPS Discover/Ask/Upload of AirDrop; rapportd the Continuity links | sharingd `[AirDrop] AirDrop not ready: wifi=...` every few seconds on this machine; rapportd clients activated with `NoAWDL`; wifip2pd strings `No AWDL interface found. Creating AWDL interface` and `Cannot create %s interface because the device is missing the capability`; sharingd strings `Bonjour browser failed to find awdl0 interface`, `AWDL network failed to start` |
| IO80211Family (Apple, generic) | Peer database (`IO80211AWDLPeerManager`, `IO80211AWDLPeer`, `IO80211AWDLMulticastPeer`), action-frame TLV building and parsing (`buildAWDLElectionParamsVer2`, `AwdlSyncTreeTLV`), an optional host-side election (`awdlElectionOnHostEnabled`), the data path over the virtual interface, the AirPlay/NAN channel-sequence arbitration, a watchdog (`Did not get AW start event for (%u ms)`) | strings and symbols attributed to `com.apple.iokit.IO80211Family` |
| Vendor driver (`AppleBCMWLANCoreMac` on Tahoe) | Creates the virtual interface (`AppleBCMWLANCore::createVirtualInterface`, `creating virtual interface with prefix = awdl`), implements the `get/setAWDL_*` handlers, translates each into a firmware iovar | 47 `get/setAWDL_*` method names and 55 `awdl_*` iovar names in that kext |
| Broadcom firmware | Synchronization, availability-window timing, channel hopping, PSF/MIF generation, election, peer table, AW events back to the host | iovars `awdl_sync_params`, `awdl_chan_seq`, `awdl_fw_election`, `awdl_election_tree`, `awdl_psf_dwell`, `awdl_af_hdr`, `awdl_payload`, `awdl_oob_af`, `awdl_peer_op`, `awdl_master_sync_grace_period`, `awdl_extcounts`, `awdl_presencemode`; driver messages such as `Creating AWDL chip interface %d, fw ver %d` and `AWDL interface index is: %u (bsscfg=%u)` |

The abstract `IO80211AWDLProtocol` class exists in the family but exports no
method symbols (its virtuals are pure), so the Broadcom driver's handler list
is the best public description of the driver-side surface:

- getters: `AF_SECONDARY_PAYLOAD_CAPABILITIES`, `AF_TX_MODE`, `CCA`, `DEV_STATS`,
  `DFSP_CONFIG`, `ELECTION_METRIC`, `ELECTION_PARAMS`, `ELECTION_RSSI_THRESHOLDS`,
  `ENCRYPTION_TYPE`, `EXTENSION_STATE_MACHINE_PARAMETERS`, `MASTER_CHANNEL`,
  `PRESENCE_MODE`, `SECONDARY_MASTER_CHANNEL`, `SYNCHRONIZATION_CHANNEL_SEQUENCE`,
  `SYNC_ENABLED`, `SYNC_PARAMS`, `SYNC_STATE`, `TXCAL_PERIOD`
- setters: the same plus `AWDL_ADVERTISERS`, `BSSID`, `CONFIG`, `DFSP_UCSA_CONFIG`,
  `ELECTION_ID`, `ENCRYPTION_KEYS`, `MIN_RATE`, `OOB_AUTO_REQUEST`, `OOB_REQUEST`,
  `OOB_SEC_PAYLOAD_REQUEST`, `PEER_TRAFFIC_REGISTRATION`, `RTG_PEER_STATS_CONFIG`,
  `SYNC_FRAME_TEMPLATE`

The ioctl numbers are in `include/Airport/apple80211_ioctl.h`: 116 to 168
(`AWDL_SYNC_PARAMS` ... `AWDL_QUIET`), 201 to 263 (`AWDL_BTLE_*`,
`AWDL_PEER_DATABASE`, `AWDL_AF_TX_MODE`, `AWDL_OPER_MODE`, `AWDL_ENCRYPTION_*`,
`AWDL_RSDB_CAPS`, `AWDL_DFSP_*`) and 338 to 351 (`AWDL_CCA`,
`AWDL_SIDECAR_*`, `AWDL_CAPABILITIES`). The structures that matter for timing
are small: `apple80211_awdl_sync_params` is AW length, AW period, extension
length and sync-frame period; `apple80211_awdl_sync_channel_sequence` is 16
`(flags, channel)` steps with step and duplicate counts and a fill channel;
`apple80211_awdl_sync_frame_template` is an opaque PSF payload the host wants
the chip to beacon on its behalf.

Events the driver posts on the virtual interface (`apple80211_var.h`, and the
family's own string table): `APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START` (42),
`..._EXTENSIONS_END` (43), `..._EXTENSION_START`, `SYNC_STATE_CHANGED` (46),
`PEER_PRESENCE` (47), `ACTION_FRAME_RX`, `CHAN_SEQ_CHANGED`, `STATISTICS` (65),
`REALTIME_MODE_START/END` (67/68), `DFS_CSA` (88/94), `INTERLEAVED_SCAN_START/STOP`,
`OOB_REQUEST_END`, `IDLE_DETECT`, `SDB_TRANSITION`. The family complains when
the AW start event stops arriving, so the chip has to produce a tick every
availability window (every 64 TU in practice), not just once at enable.

### 1.2 Interface creation

`awdl0` is an `IO80211VirtualInterface` with role `APPLE80211_VIF_AWDL`, and
it is created on demand from user space, not at driver start: wifip2pd sends
`APPLE80211_IOC_VIRTUAL_IF_CREATE` (94) when it finds no AWDL interface and
the card advertises the capability. The driver answers by allocating the
interface and calling `IO80211Controller::attachVirtualInterface`, which calls
back into the controller's `createVirtualInterface(ether_addr *, UInt role)`;
the base implementation fails (`failure to create virtual interface %d`), so
the driver has to override it. On Tahoe there is no `IO80211P2PInterface`
class any more (the `ioreg` class table lists `IO80211VirtualInterface` and
the vendor's `AppleBCMWLANNANInterface`, nothing else), the data path is the
Skywalk one (`ERROR: Skywalk AWDL interface init fail`), and AWDL gets a
companion `llw0` low-latency interface (`setCompanionLowLatencySkywalkInterface`)
for Sidecar.

The legacy AirportItlwm did this: `setVIRTUAL_IF_CREATE` allocated an
`IO80211P2PInterface` named "awdl" pinned to channel 149/80 MHz,
`createVirtualInterface` built a plain `IO80211VirtualInterface`
(`qwzmac-deps/itlwm/AirportItlwm/AirportItlwm.cpp`), and
`AirportVirtualIOCTL.cpp` answered the AWDL ioctls with constants:
`getAWDL_SYNC_PARAMS` returns zeros, `getAWDL_CAPABILITIES` returns
`APPLE80211_AWDL_CAP_CCA_STATS`, `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`
logs and succeeds, `setAWDL_SYNC_STATE` stores the value and echoes it back,
and `requestPacketTx` for the AWDL role is commented out. Since Ventura the
same function returns `kIOReturnUnsupported` with the comment that the
interface "is no functionality" and only delayed association.

Two Intel forks went further on Sonoma and Tahoe and set the empirical
ceiling for a stub: ReAWDL (InsanelyMac, late 2025) and AirportItlwm-Tahoe
1.0.0 (itlwm issue 1062, 2026). Both get `awdl0` up with
`IO80211VirtualInterfaceRole = AirLink`, wifip2pd attached, IPv6 and Bonjour
on the interface, and AirPlay discovery working, because that discovery also
exists on the infrastructure network. Neither gets an AirDrop handshake; the
Tahoe one panics in `IO80211VirtualInterface::_outputStartGated` when the
family tries to dequeue, and its author writes that "firmware radio timing
remains unverified". That is the shell: `awdl0` exists, AirDrop opens, no
peer is ever found.

### 1.3 Where AirportWCN7850 stands today

- `setVIRTUAL_IF_CREATE` returns `kIOReturnUnsupported`
  (`airport/AirportItlwmSkywalkInterface.hpp:304`) and there is no
  `createVirtualInterface` override. The live `ioreg` class table shows
  `IO80211VirtualInterface = 0`, `IO80211AWDLPeerManager = 0`; `ifconfig` has
  no `awdl0` and no `llw0`.
- The WCL interface class (`airport/AirportItlwmSkywalkInterface.hpp`, which
  derives from laobamac's `IO80211InfraProtocol`) carries three AWDL stubs,
  all `kIOReturnUnsupported`: `getAWDL_RSDB_CAPS`, `setAWDL_FORCED_ROAM_CONFIG`,
  `setAWDL_ENABLE_ROAMING`. Those are the only AWDL entries the infra protocol
  has; the real AWDL surface is on the virtual interface's protocol object,
  which the WCL layer does not model at all.
- `AirportItlwm::awdlSyncEnable`, `awdlSyncState`, `awdlBSSID`, etc. in
  `airport/AirportItlwmV2.hpp` are inherited fields with no reader.
- `getCARD_CAPABILITIES` advertises no AWDL feature bits
  (`APPLE80211_CAPA_AWDL_FEATURE_*` are defined in `AirportItlwmV2.cpp:980`
  and unused), which is why wifip2pd never asks for the interface and
  sharingd settles for `AirDrop not ready` instead of logging failures.

## 2. The protocol, as reverse-engineered by Open Wireless Link

Sources: Stute et al., "One Billion Apples' Secret Sauce" (MobiCom 2018,
arXiv 1808.03156), "A Billion Open Interfaces for Eve and Mallory" (USENIX
Security 2019), "Linux Goes Apple Picking" (arXiv 1812.06743), and the owl
sources (`src/sync.c`, `election.c`, `channel.c`, `schedule.c`, `frame.h`,
`daemon/core.c`).

### 2.1 Timing

| Quantity | Value |
|---|---|
| Time unit (TU) | 1024 us |
| Availability window (AW) | 16 TU |
| Presence mode | 4: every AW is followed by 3 extension windows, so the practical slot (EAW) is 64 TU = 65.5 ms |
| Channel sequence | 16 entries, each spanning 4 AWs; one full sequence is 1024 TU, about 1.05 s |
| Sync reference | No TSF in the frame. The Synchronization Parameters TLV carries `tx_down_counter` (TU until the next EAW) and the AW sequence number; the action-frame header carries two 32-bit microsecond timestamps, `phy_tx` and `target_tx`, and the follower computes `T_AW = tAW * 1024 - (phy_tx - target_tx) + T_rx` |
| Tolerated error | 3 ms. Apple's own driver counts a misalignment above 3 ms; measured error on Apple devices is Gaussian, mean -0.45 TU, sigma 0.98 TU, >99 % within +-3 TU |
| Guard time | 3 TU at both ends of each EAW (about 9 % of the slot lost) |
| Channel switch cost on Apple's chip | >= 8 ms per switch (`wl dump chanswitch`), which is why Apple only uses 64 TU EAWs: 16 TU AWs minus 8 ms switch minus 2 x 3 TU guard would leave nothing |
| Sync frames | PSF (periodic synchronization frame) every 110 TU as master, 440 TU as follower; MIF (master indication frame) once per EAW near its middle |
| No-master timeout | 96 AWs, about 1.5 s |

### 2.2 Channel sequence

Social channels are 6, 44 and 149; a typical idle Mac announces
`44 44 44 0 0 0 0 0 6 44 44 0 0 0 0 0` (channel 0 = not listening), and
when associated it reserves at least a quarter of the slots for the AP's
channel, e.g. `pppp iiii s ppp iiii` with `p` = 44, `i` = infra, `s` = 6.
There is always one slot on channel 6 (slot 8). A follower must be on the
announced channel when the slot starts, within the 3 ms budget, and must be
back on the AP's channel for the infra slots so the association survives.

### 2.3 Election

Election Parameters v2 TLV (master address, sync address, master counter,
distance to master, master metric, self metric, self counter) and a
Synchronization Tree TLV. A node wins on a higher counter, then a higher
metric; metrics start at 60 and are randomized into 405-436 or 505-536 after
two seconds of listening. The election itself is slow and host-friendly (owl
runs it once a second in user space). It is the synchronization that follows
the election result that needs the radio.

### 2.4 Data path

AWDL data are plain 802.11 data frames, To/From DS = 0, BSSID
`00:25:00:ff:94:73`, LLC/SNAP with Apple's OUI `00:17:f2`, an AWDL header,
then IPv6 only. Link-local addresses are derived from the peer MAC, so no NDP;
mDNS answers ride in the action frames. Peers are never associated: the radio
has to transmit unicast data to a station it has no association with, at a
rate the peer supports (Apple devices use VHT80 on 44/149; owl fixes 12 Mbit/s).

### 2.5 What owl needs from a Linux card

owl runs entirely in user space on a monitor-mode interface with injection:
it switches channel with `NL80211_CMD_SET_CHANNEL` at every EAW boundary from a
libev timer, injects frames through pcap, and timestamps received action
frames with `CLOCK_MONOTONIC` on delivery. It meets the 3 ms budget that way
because on a softMAC card (ath9k, AR9280) the host writes the channel
register itself and the switch is sub-millisecond and synchronous. The owl
README states the consequence: no concurrent connection to an AP, the card is
owned exclusively, cards whose channels are "no IR" cannot transmit, and the
channel sequence is static.

The precision class, then, is milliseconds, not microseconds. That is good
news for a kernel driver on macOS: a timer that fires at EAW boundaries is
enough, provided the radio does what it is told within about a millisecond
and the host knows where the radio is.

## 3. What the WCN7850 firmware offers

### 3.1 What is in the code base already (`qwz/`)

OpenBSD's `qwz(4)` is a port of ath12k, so its WMI vocabulary is ath12k's.
Relevant pieces, by grep:

- Interfaces: `qwz_wmi_init_wcn7850` configures `num_vdevs = 4`,
  `num_peers = 16` (ath12k's `TARGET_NUM_VDEVS` is 17). The chip is
  `single_pdev_only`, so `preferred_hw_mode` is forced to
  `WMI_HOST_HW_MODE_SINGLE`; the DBS/SBS enumerations exist in `qwzreg.h`
  but are not used for this part.
- Off-channel: there is no remain-on-channel implementation. The scan command
  (`qwz_wmi_send_scan_start_cmd`) carries `dwell_time_active/passive` in
  milliseconds, `min_rest_time`/`max_rest_time`, `burst_duration`, a channel
  list and the flags `scan_f_offchan_mgmt_tx` / `scan_f_offchan_data_tx`;
  `qwz_wmi_start_scan_init` uses 50 ms active, 150 ms passive, 50-500 ms rest.
  The scan state machine (`ATH12K_SCAN_STARTING/RUNNING/ABORTING`) learns about
  channel arrival only through `WMI_SCAN_EVENT_FOREIGN_CHANNEL`, which carries
  a frequency and a firmware `tsf_timestamp`. The `is_roc`/`roc_freq` fields in
  `struct scan` are inherited from ath12k and unused.
- Management TX: `qwz_mac_mgmt_tx_wmi` wraps `WMI_MGMT_TX_SEND_CMDID`; the
  completion (`wmi_mgmt_tx_compl_event`) carries `desc_id`, `status`,
  `ppdu_id`, `ack_rssi`, no timestamp. `WMI_OFFCHAN_DATA_TX_SEND_CMDID` is in
  the enum and unused. Nothing takes a "transmit at TSF" argument.
- Time: `WMI_VDEV_TSF_TSTAMP_ACTION_CMDID` / `WMI_VDEV_TSF_REPORT_EVENTID` read
  a vdev's TSF on request (a round trip, not a clock the host can read); the
  service bit `WMI_TLV_SERVICE_TX_COMPL_TSF64` exists; `WMI_VDEV_PARAM_TSF_INCREMENT`
  nudges a vdev's TSF. None of it is plumbed in qwz.
- Concurrency knobs, all firmware-internal: `WMI_PDEV_PARAM_RESMGR_OFFCHAN_MODE`,
  `WMI_VDEV_LIMIT_OFFCHAN_CMDID`, `WMI_MCC_SCHED_TRAFFIC_STATS_CMDID`,
  `WMI_VDEV_PARAM_MCC_RTSCTS_PROTECTION_ENABLE`, `WMI_VDEV_MCC_BCN_INTERVAL_CHANGE_REQ_EVENTID`,
  and the firmware-test-only `WMI_FWTEST_VDEV_MCC_SET_TBTT_MODE_CMDID`.
- Neighbour-aware networking: `WMI_TLV_SERVICE_NAN_DBS_SUPPORT`,
  `WMI_TLV_SERVICE_NDI_DBS_SUPPORT` service bits are defined; there is no NAN
  command in the OpenBSD or upstream ath12k code, and nothing named AWDL
  anywhere in WMI.

### 3.2 What Linux ath12k adds

Read at Linux v6.17 (`drivers/net/wireless/ath/ath12k`), with Qualcomm's
downstream `qcacld-3.0` / `fw-api` from CodeLinaro for the parts ath12k only
names.

- **Remain-on-channel is a scan.** `ath12k_mac_op_remain_on_channel` (added by
  Kang Yang's P2P series for the WCN7850, January 2024) issues a one-channel
  passive `WMI_START_SCAN` with `burst_duration = duration` and the default
  dwell/rest values, waits up to 1 s for `WMI_SCAN_EVENT_STARTED`, then up to
  3 s for `WMI_SCAN_EVENT_FOREIGN_CHAN` on the requested frequency, and ends it
  with a delayed `WMI_STOP_SCAN` after `duration` ms. The host learns that the
  radio arrived only from the asynchronous event; nothing lets it schedule the
  arrival. The event does carry a 32-bit firmware `tsf_timestamp` that ath12k
  copies and ignores. No measured ROC or channel-switch latency for ath11k or
  ath12k exists anywhere I could find; Qualcomm's own driver sizes ROC in
  units of 200 ms (`P2P_ROC_DEFAULT_DURATION`), capped at 250-350 ms when NAN
  is active.
- **Concurrency.** `wcn7850 hw2.0` is `single_pdev_only` with
  `interface_modes = STATION | AP | P2P_DEVICE | P2P_CLIENT | P2P_GO`, one
  station, up to 16 AP/P2P interfaces, `num_different_channels = 1`, plus a
  second combination with `num_different_channels = 2` added in April 2025 for
  a second MLO link on the other band, not for multi-channel concurrency. The
  maintainers' statement (May 2025) is that AP+STA concurrency "might work with
  some specific configurations, like channels". No `WMI_PDEV_SET_HW_MODE` is
  sent for this chip; the firmware keeps its default. Whatever MCC the firmware
  does between a station and a P2P GO is a firmware scheduler keyed to beacon
  intervals, with downstream knobs in percent (`WMI_RESMGR_SET_CHAN_TIME_QUOTA`)
  and milliseconds (`WMI_RESMGR_SET_CHAN_LATENCY`, `WMI_VDEV_LIMIT_OFFCHAN`:
  `max_offchan_time` ms and `rest_time` ms). The fw-api comment on long passive
  dwells says it plainly: "for dwell time greater than beacon interval, bmiss
  is expected".
- **Monitor mode.** RX monitor support for the WCN7850 landed in 6.16
  (13 patches, May 2025); transmission on the monitor interface is blocked.
  There is no raw-injection path, so owl's "monitor + inject" model has no
  equivalent here.
- **Management and raw TX.** `ath12k_wmi_mgmt_send` fills `vdev_id`,
  `desc_id`, `chanfreq` (the ROC frequency when sent during a ROC, else 0),
  frame address and length; Qualcomm's full command adds power, rate and retry
  masks. There is no field to transmit at a TSF. `WMI_OFFCHAN_DATA_TX_SEND` is
  used downstream for off-channel data frames inside a ROC, again with no time
  anchor. Completions carry no timestamp; the 64-bit TX TSF service applies to
  the HTT data-path completions and is off unless the host asks for it in the
  resource config.
- **Time.** RX frames get a `mactime` only on the monitor path (from the
  PPDU-end TLV); the normal data path and the WMI management RX path (which
  carries `rx_tsf_l32/u32` and `tsf_delta`) are not plumbed for it in ath12k.
  `WMI_VDEV_TSF_TSTAMP_ACTION` can read the vdev TSF (one round trip, or a
  periodic report at a minimum of 1000 ms) and `WMI_VDEV_PARAM_TSF_INCREMENT`
  can nudge it; ath12k implements neither.
- **NAN.** ath12k names `WMI_NAN_CMDID` and the NAN service bits and implements
  nothing. qcacld implements NAN and NDP by passing opaque blobs to the
  firmware, whose discovery engine owns the whole discovery-window schedule.
  Whether the retail WCN7850 firmware sets the NAN service bits is not
  determinable from source; the FastConnect 7800 brief advertises Wi-Fi Aware.
- **Nothing anywhere** in the WMI surface, upstream or downstream, says "be on
  channel X at TSF T for N TU".

### 3.3 Mapping AWDL's needs onto the primitives

| AWDL needs | WCN7850 primitive | Fit |
|---|---|---|
| Be on channel X at the start of each 64 TU slot, within 3 ms, four different channels per second | A WMI scan request per slot (ath12k's ROC), or a firmware MCC schedule between vdevs | Scan requests start when the firmware gets to them, dwell in integer milliseconds and report arrival afterwards through `FOREIGN_CHANNEL`; the only published sizing is Qualcomm's 200 ms default ROC. MCC schedules exist only between the firmware's own vdevs (STA + P2P GO/client) on fixed operating channels, driven by beacon intervals and percent quotas, not by a host grid |
| Stay associated on the AP's channel in the infra slots | The firmware's scan engine already does this for scans (it sends a power-save null before leaving) | Works for scans measured in hundreds of milliseconds; repeated every 65 ms for ever it is continuous scanning, with the AP seeing a client that is asleep 75 % of the time |
| Transmit PSF/MIF action frames with `phy_tx`/`target_tx` timestamps the peer can subtract | `WMI_MGMT_TX_SEND` with no TSF, completion with no TSF | The timestamps would be the host's guesses; the error the peer sees becomes our queueing latency, which is exactly the term Apple's firmware removes |
| Timestamp received action frames | WMI management RX carries `rx_tsf` and `tsf_delta`; the monitor path has `mactime`; qwz uses neither | Fixable in the driver, but only half the problem |
| Transmit unicast data frames to peers it is not associated with, on a hopping channel, at VHT rates | ath12k data TX requires a firmware peer entry on a vdev whose channel is fixed; monitor-mode TX is blocked, there is no injection path | No primitive. A second vdev (AP type) could hold peer entries, but its channel cannot follow the sequence; `WMI_OFFCHAN_DATA_TX_SEND` sends single frames inside a ROC at scan-engine timing |
| An "AW start" tick for IO80211Family every 64 TU | Host timer | Trivial, and worthless if the radio is not actually there |

### 3.4 Why it is different from owl, and from AirportItlwm's situation

owl's card is a softMAC: mac80211 hands the host the channel register, the
host switches synchronously, injects raw frames and sees hardware timestamps.
The WCN7850 is a fullMAC in all the ways that matter here: the firmware owns
the channel, the TSF, the power-save handshake with the AP and the peer table;
the host asks, waits for an event, and never knows where the radio is between
events. Intel's iwlwifi has the same architecture. The AirportItlwm authors
never published a technical reason (the pinned itlwm issue 292 says only
"not expected to work, still needs further investigation", and the FAQ lists
AirDrop as unsupported without a why); the "needs firmware support" line is
community shorthand, but it is the right shorthand: it is not that the chip is
too slow, it is that the host is not in charge of the schedule. Apple's
Broadcom solution is the proof: the whole synchronization lives in firmware
(`awdl_sync_params`, `awdl_chan_seq`, `awdl_fw_election`, `awdl_psf_dwell`),
and the kext only sets parameters and moves frames.

## 4. Options I considered

1. **The shell** (`awdl0` + stubs). One to two weeks. Outcome: AirDrop opens
   and is empty; sharingd and WiFiAgent now have an interface to complain
   about; the family's watchdog logs missing AW events. Negative value unless
   it is a step towards 2.
2. **Host-scheduled AWDL over the scan engine.** Run owl's state machine in
   the kext, issue a one-channel scan per slot, inject action frames with
   `WMI_MGMT_TX_SEND`, read peers' frames from the scan's management RX path.
   Two to four months to find out what the alignment distribution is; my
   expectation from the primitives is several milliseconds of jitter against a
   3 ms budget, no usable data path to unassociated peers, and the AP link
   degraded to a client that is away three quarters of the time. Best case is
   discovery that sometimes works and transfers that do not.
3. **Firmware help.** The firmware has a NAN engine (Wi-Fi Aware, discovery
   windows every 512 TU on channels 6 and 149), which is the industry's answer
   to the same problem, but its schedule, frame formats and sync beacons are
   not AWDL's, its WMI interface is not in ath12k or qwz, and Apple's AirDrop
   does not use NAN for discovery. There is no AWDL in any Qualcomm firmware
   and no way to add one.
4. **A second radio for AWDL only.** A monitor-capable card with injection
   (ath9k class; omdrop-owl reports an Intel AX201 doing it under iwlwifi with
   Wi-Fi otherwise off) could run owl's schedule unassociated, as owl does on
   Linux, while the WCN7850 keeps the AP link. macOS has no owl, so this would
   mean porting owl into a kext that presents `awdl0` to IO80211Family for a
   card that has no macOS driver either. More work than option 2 and the
   result is still a 12 Mbit/s AWDL with no VHT and no Apple-grade
   coexistence.
5. **A Broadcom card.** The only route where the firmware does the job.

## 5. If someone insists: milestones and what they measure

Written so that the stop condition is explicit.

- **M0, the shell (1-2 weeks).** Override `createVirtualInterface` for
  `APPLE80211_VIF_AWDL`, implement a protocol object with the 47 handlers above
  returning the values Apple's driver would (AW 16, period 16, extension 16,
  PSF 110, presence mode 4, the host's channel sequence echoed back, sync state
  as set), post `AVAILABILITY_WINDOW_START` from a 64 TU timer, advertise the
  AWDL capability bits. Measure: `awdl0` present, `sharingd` stops logging
  `AirDrop not ready`, AirDrop window opens. Stop condition: none, this part
  always "works".
- **M1, the alignment experiment (3-4 weeks).** Without any peer traffic:
  issue a 16 TU single-channel scan every 64 TU for a minute, record the
  `FOREIGN_CHANNEL` event time against the host timer, and the AP-side effect
  (ping loss, beacon misses). Measure the jitter distribution. Stop condition:
  if the 99th percentile of arrival error exceeds 3 ms, or the AP link drops,
  AWDL cannot be synchronized from this host, and the project ends here. I
  expect it to end here.
- **M2, passive discovery (4-6 weeks).** Receive peers' PSFs during the scan
  dwells, hand the action frames to the family, let it build the peer list.
  Measure: an iPhone appears in the AirDrop window. This still needs no data
  path.
- **M3, data path (open-ended).** Unassociated peer entries and raw data TX on
  a hopping vdev. No primitive identified; this is where I would need
  something the firmware does not have.

## 6. Recommendation

Do not implement AWDL in AirportWCN7850. Keep the README's "AirDrop/AWDL
unsupported" line and point at this document.

For AirDrop on this machine, use a Broadcom card with Apple firmware, and
know what Tahoe drives natively: the kernel collection ships
`AppleBCMWLANCoreMac` with firmware hash stores for BCM4364, BCM4378, BCM4387
and BCM4388 only. The BCM4360/43602 generation (`AirPortBrcmNIC`; the FENVI
T919 is a BCM94360CD on a PCIe carrier, the BCM943602CDP is its newer
sibling) is gone from Tahoe and only comes back with OpenCore Legacy Patcher's
"modern wireless" root patches, which inject `IO80211FamilyLegacy` and
`AirPortBrcmNIC-Tahoe` and patch airportd, wifip2pd and the Wi-Fi frameworks.
Reports from OCLP 3.0 nightlies show a BCM943602CDP doing bidirectional AirDrop
on Tahoe that way. This machine runs no OCLP today, so that card means taking
on root patches; a BCM4364-class card avoids them but comes in Apple's own
module form factors.

The Wi-Fi driver itself is not wasted: Bluetooth (BluetoothWCN7850) is the
other half AirDrop needs, and that half works.

## Sources

- IO80211 headers in `include/Airport/` (apple80211_ioctl.h, apple80211_var.h,
  IO80211VirtualInterface.h, IO80211P2PInterface.h, IO80211ControllerV2.h,
  macOS26/IO80211ControllerV2.h, IO80211InfraProtocol.h).
- `BootKernelExtensions.kc` of macOS 26.7.1: strings and symbols of
  `com.apple.iokit.IO80211Family` and `com.apple.driver.AppleBCMWLANCoreMac`.
- `qwzmac-deps/itlwm/AirportItlwm/{AirportItlwm.cpp,AirportVirtualIOCTL.cpp,AirportSTAIOCTL.cpp}`
  (legacy AirportItlwm 2.4.0); OpenIntelWireless itlwm issues 292, 780, 801,
  1047, 1062 and the FAQ; kgp-macPro/AirportItlwm-Tahoe; the ReAWDL thread on
  InsanelyMac (topic 363255).
- Strings of `/usr/libexec/sharingd`, `/usr/libexec/wifip2pd`,
  `/usr/libexec/airportd` on macOS 26.7.1.
- I. Beer, "An iOS zero-click radio proximity exploit odyssey", Project Zero,
  December 2020 (AWDL frame parsing in `IO80211AWDLPeer`, 16 ms slots).
- Yu Wang, "Dive into Apple IO80211FamilyV2", Black Hat USA 2020 and Vol. 2,
  2022 (plugin architecture, Skywalk, Sidecar/LLW ioctls).
- Dortania Wireless Buyers Guide; acidanthera/AirportBrcmFixup; OpenCore Legacy
  Patcher `modern_wireless.py`, `wireless.py` and changelog; seemoo-lab/opendrop
  README; t4t5/omdrop-owl wiki.
- M. Stute et al., "One Billion Apples' Secret Sauce: Recipe for the Apple
  Wireless Direct Link Ad hoc Protocol", MobiCom 2018, arXiv:1808.03156.
- M. Stute et al., "A Billion Open Interfaces for Eve and Mallory: MitM,
  DoS, and Tracking Attacks on iOS and macOS Through Apple Wireless Direct
  Link", USENIX Security 2019.
- M. Stute, D. Kreitschmann, M. Hollick, "Linux Goes Apple Picking:
  Cross-Platform Ad hoc Communication with Apple Wireless Direct Link",
  MobiCom 2018 demo, arXiv:1812.06743.
- github.com/seemoo-lab/owl (sync.c, election.c, channel.c, schedule.c,
  frame.h, daemon/core.c, README).
- OpenBSD `sys/dev/ic/qwz.c`, `qwzreg.h` as mirrored in `qwz/`.
- Linux v6.17 `drivers/net/wireless/ath/ath12k` (mac.c, wmi.c, wmi.h, core.h,
  hw.c, dp_mon.c); ath12k list threads: "implement remain on channel for P2P
  mode" (January 2024), "support 2 channels for single pdev device" (April
  2025), "add monitor mode support for WCN7850" (May 2025), and the May 2025
  concurrency statement.
- Qualcomm `qcacld-3.0`, `qca-wifi-host-cmn` and `fw-api` (CodeLinaro,
  LA.VENDOR.14.3.4.c1): `wlan_p2p_roc.c/.h`, `wmi_unified_nan_tlv.c`,
  `wmi_unified_concurrency_tlv.c`, `wmi_unified.h`.
