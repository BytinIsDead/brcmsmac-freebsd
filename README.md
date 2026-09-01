# if_bcm4313 — FreeBSD BCM4313 802.11b/g/n SoftMAC driver

A native **FreeBSD** device driver for the Broadcom **BCM4313** PCIe Wi-Fi
chipset (D11 MAC core rev 24, LCN PHY), ported from the Linux
[`brcmsmac`](https://www.kernel.org/) driver and wired into the
[`bhnd(4)`](https://man.freebsd.org/bhnd/4) backplane bus and the **net80211**
framework.

It is **self-contained**: the D11/LCN microcode and every LCN-PHY RF tuning
table are embedded in the module, so no firmware files (`/boot/modules/...`)
are required at runtime.

```
  BCM4313 (PCIe)  -->  bhnd(4) backplane  -->  if_bcm4313  -->  net80211
                                              (D11 MAC rev 24, LCN PHY)    `- wlan(4)
```

---

## Highlights

- Full SoftMAC implementation: scanning, 802.11b/g/n (1x1, HT20), WPA/WPA2 via
  `wpa_supplicant`, monitor mode plumbing.
- LCN-PHY programming and calibration (RX-gain, TX-power, IQ, tempsense)
  ported from `brcmsmac`.
- switch-control and RX-gain tables selected at attach from **SPROM board
  flags**, with the table set reproduced byte-for-byte from upstream.
- Microcode uploaded from the embedded `bcm4313_ucode.c` at attach.
- Verified to **compile** clean against FreeBSD 14.2, **15.1-RELEASE** and
  current (`main`) kernel headers (see [`COMPATIBILITY.md`](COMPATIBILITY.md)).

> Status: source-level complete and build-verified against real FreeBSD kernel
> headers. It has **not** been runtime-tested on BCM4313 hardware in this
> repository's CI, so treat the first load on real silicon as bring-up.

## Documentation

| Guide | Purpose |
|-------|---------|
| [`INSTALL.md`](INSTALL.md) | Build the `.ko`, load it, connect to Wi-Fi (step-by-step). |
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | FreeBSD release support matrix and how to match your `/usr/src`. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | How the port maps to `brcmsmac`; file layout; how artifacts are generated. |
| [`TESTING.md`](TESTING.md) | How the build is verified upstream and on real hardware. |

## Quick start

```sh
# on a FreeBSD host with kernel source at /usr/src/sys:
sh build.sh                                   # -> if_bcm4313.ko (auto-detects paths)
kldload ./if_bcm4313.ko                       # one load: front-end + whole bhnd chain
# or after `make install`: kldload if_bcm4313
```

The module carries its own PCI front-end (`if_bcm4313_pci.c`) that claims
the 4313's PCI id (`14e4:4727`) and creates the `bhnd(4)` bridge chain
itself — FreeBSD's own `bwn_pci` doesn't list the BCM4313, so without this
front-end the module would load but nothing would ever attach.

Full walkthrough (including custom `KERNCONF` builds and Wi-Fi setup) is in
[`INSTALL.md`](INSTALL.md).

## Repository layout

```
if_bcm4313.c           Driver core: attach/init, DMA rings, net80211, microcode upload
if_bcm4313_pci.c       PCI front-end: claims 14e4:4727, creates the bhnd bridge chain (adapted from bwn_pci)
if_bcm4313_phy_lcn.c   LCN-PHY programming + calibration (ported from brcmsmac)
if_bcm4313var.h        Shared structures / prototypes / register constants
if_bcm4313_pcivar.h    PCI front-end softc / device table
bcm4313_ucode.c/.h     Embedded D11/LCN microcode (generated)
bcm4313_lcntab.h       Switch-control + RX-gain tables (generated)
bcm4313_phytbl_lcn.h   Remaining LCN-PHY tables (generated)
gen_*.pl               Table/microcode generators
firmware/brcm/         Source .fw blobs + Broadcom license (embedded at build)
brcmsmac/              Upstream Linux reference sources (for porting/review)
Makefile               Out-of-tree kernel module build
```

## License

The driver is **dual-licensed BSD-2-Clause OR GPL-2.0-or-later** (see
[`LICENSE`](LICENSE)). The embedded microcode and RF tables are Broadcom
binaries distributed only under
`firmware/brcm/LICENCE.broadcom_bcm43xx`; see that file before redistributing.