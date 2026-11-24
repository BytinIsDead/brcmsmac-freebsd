# if_bcm4313

A native, out-of-tree **FreeBSD SoftMAC driver** for the Broadcom **BCM4313**
PCIe Wi-Fi chipset. It ports the D11 MAC and LCN-PHY behavior of Linux
`brcmsmac` to `bhnd(4)`, DMA64, and `net80211`.

```text
BCM4313 PCIe -> bcm4313_pci -> bhndb -> bcma_bhndb -> bhnd -> if_bcm4313 -> wlan
```

The D11/LCN microcode and PHY tuning tables are embedded in the module. No
separate firmware files are needed at runtime.

## Current status

- Source fixes for the management-frame RX failure are applied. The root cause
  was an SHM addressing error that made slot-time writes overwrite the maximum
  RX frame length; see [Architecture](ARCHITECTURE.md#management-rx-root-cause).
- Earlier hardware runs proved attach, microcode upload, DMA activity, and
  reception of short control frames. They did **not** prove beacon/probe-response
  reception, association, or DHCP.
- The latest SHM/PHY/radio patch still needs a clean FreeBSD build and a
  cold-power-cycle hardware test. Do not treat scan or Wi-Fi as verified yet.
- A previous source baseline passed syntax/type checks against FreeBSD 14.2,
  15.1, and then-current kernel headers. Those checks do not replace loading the
  module on real hardware.

## Supported hardware

| Hardware | Support |
|---|---|
| BCM4313, PCI ID `14e4:4727` | Intended target |
| D11 MAC rev 24, LCN PHY type 8 rev 1, radio 2064 | Required configuration |
| 802.11b/g/n, 1x1, HT20, MCS 0-7 | Advertised capabilities |
| BCM43142 / BCM943142HM | Unsupported FullMAC device |
| BCM43131, BCM43224, BCM43225, BCM43236 | Not ported; these need N-PHY support |

Station-mode scanning and WPA/WPA2 joining are the immediate bring-up goals.
Monitor-mode plumbing exists; IBSS, host-AP, and long-running traffic remain
hardware-unverified.

## Quick start

Run these commands on the FreeBSD machine, from this directory:

```sh
pciconf -lv | grep -A3 -i 14e4   # confirm the BCM4313 is present
sh build.sh                     # build if_bcm4313.ko
sudo sh install.sh              # install, (re)load, create wlan0, and scan
sudo sh config.sh MySSID MyPassword
```

`install.sh` is the safest reload path: it destroys stale `wlan` interfaces,
unloads the old module, attempts to clear wedged PCI/D11 state, and verifies
that the new module and device actually attached.

For a persistent connection:

```sh
sudo sh config.sh --boot MySSID MyPassword
```

Passing a password on the command line exposes it to local process/history
inspection. On a trusted console, `sudo sh config.sh` provides a hidden-password
prompt instead.

## Tools

| Task | Command |
|---|---|
| Build | `sh build.sh` |
| Build with debug symbols/prints | `sh build.sh debug` |
| Print build commands without running them | `sh build.sh check` |
| Build and install the module | `sudo sh build.sh install` |
| Install, reload, create `wlan0`, and scan | `sudo sh install.sh` |
| Guided scan-and-connect | `sudo sh config.sh` |
| One-shot WPA/WPA2 join and DHCP | `sudo sh config.sh SSID PASSWORD` |
| Join and persist across boots | `sudo sh config.sh --boot SSID PASSWORD` |
| Use an existing supplicant configuration | `sudo sh config.sh --wpa FILE` |
| Join, then test Internet reachability | `sudo sh config.sh --ping SSID PASSWORD` |
| Collect a diagnostic report | `sudo sh diag.sh` |

Every tool accepts `--help`. The scripts require FreeBSD; they are not Linux
shell utilities.

## Documentation

| Guide | Contents |
|---|---|
| [INSTALL.md](INSTALL.md) | Prerequisites, build, load, boot loading, and troubleshooting |
| [CONNECT.md](CONNECT.md) | Scanning, WPA/WPA2, DHCP, and persistent connections |
| [TESTING.md](TESTING.md) | Verification status, cold-boot test procedure, and pass criteria |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Driver layout, upstream mapping, fixes, and known limits |

Compatibility notes and the former research log are consolidated into those
guides rather than maintained as separate documents.

## Important implementation details

- The module registers as `bhnd/if_bcm4313`. Check it with
  `kldstat -m bhnd/if_bcm4313`, not `kldstat -m if_bcm4313`.
- The net80211 parent device is normally `bcm43130`. Create a VAP with
  `ifconfig wlan0 create wlandev bcm43130`.
- The PCI front-end claims `14e4:4727` and creates the bhnd bridge chain; no
  FreeBSD source patch is required.
- Inline `ifconfig wpakey` is not usable on the tested FreeBSD 15.1 setup.
  Use `wpa_supplicant -Dbsd`; `config.sh` does this automatically.
- Power save is intentionally not enabled.

## Repository layout

```text
if_bcm4313.c             MAC/D11, DMA, net80211, scan, recovery
if_bcm4313_pci.c         PCI front-end and bhnd bridge attachment
if_bcm4313_phy_lcn.c     LCN-PHY programming and calibration
if_bcm4313var.h          Shared structures, registers, and constants
bcm4313_ucode.c/.h       Embedded microcode (generated)
bcm4313_lcntab.h         Switch-control and RX-gain tables (generated)
bcm4313_phytbl_lcn.h     Other LCN-PHY tables (generated)
firmware/brcm/           Original firmware blobs and Broadcom license
brcmsmac/                Bundled upstream reference sources
build.sh install.sh      Build and deployment helpers
config.sh diag.sh        Connection and diagnostic helpers
lib.sh                   Shared shell helpers
Makefile                 Out-of-tree kernel-module build
```

## License and provenance

The driver glue is dual-licensed **BSD-2-Clause OR GPL-2.0-only**. Portions
derived from brcmsmac and FreeBSD retain their respective notices, and the
embedded Broadcom microcode remains under the Broadcom Software License
Agreement. Read [LICENSE](LICENSE) and
[firmware/brcm/LICENCE.broadcom_bcm43xx](firmware/brcm/LICENCE.broadcom_bcm43xx)
before redistributing source or binaries.
