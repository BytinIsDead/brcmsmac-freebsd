# if_bcm4313(4) — BCM4313 driver installation tutorial

A native FreeBSD SoftMAC 802.11b/g/n driver for the Broadcom **BCM4313** PCIe
chipset (D11 MAC core rev 24, LCN-PHY). It is a port of the Linux `brcmsmac`
driver, wired into `bhnd(4)` and net80211.

The driver is **self-contained**: the D11/LCN microcode and all LCN-PHY tuning
tables are embedded, so **no firmware files** need to be loaded separately.

---

## 1. What you need

- A FreeBSD machine. The code is verified against **FreeBSD 14.2**, **15.1-RELEASE**
  and **main** (amd64).
- Kernel **source** at `/usr/src/sys`. Check it exists:
  ```sh
  ls /usr/src/sys/sys/param.h
  ```
  If `/usr/src` is missing or `/usr/src/sys` is empty, install the source
tree. On a release, use "Install source" in `bsdinstall` (or let `freebsd-update`
pull the matching source with `freebsd-update fetch`), or clone the exact
**release tag** for the version you run — not the `releng/` branch:
  ```sh
  git clone --depth 1 --branch release/15.1.0 https://github.com/freebsd/freebsd-src.git /usr/src
  uname -U   # __FreeBSD_version of your running kernel, e.g. 1501000 = 15.1-RELEASE
  ```
  > Use the `release/<major.minor.0>` tag matching your installed release.
  > `releng/15.1` is the *branch* that keeps receiving HEAD commits after the
  > release; it does **not** match a shipped 15.1-RELEASE kernel. Check out the
  > release tag (or the source that `freebsd-update` provides) so the headers
  > match your running kernel and avoid version mismatches.
- A working compiler/toolchain. A stock FreeBSD release already ships `cc`,
  `make`, `bmake` and the kernel build glue under `/usr/src/sys — no extra
  packages are needed for a kernel module build.

**You must build on a real FreeBSD host.** This is a kernel driver for actual
BCM4313 hardware; it cannot be built or loaded in a non-FreeBSD environment.

---

## 2. Build the module

From this repository directory:

```sh
make SYSDIR=/usr/src/sys clean depend all
```

This produces **`if_bcm4313.ko`** in the current directory.

### If your kernel used a custom config (`KERNCONF`)

A custom kernel generates `opt_*.h` files (in `/usr/obj/usr/src/<arch>.amd64/sys/<KERNCONF>`)
that net80211 expects to find. Build against that directory too:

```sh
make SYSDIR=/usr/src/sys \
    KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/GENERIC \
    clean depend all
```

Substitute `amd64.amd64/sys/GENERIC` with your actual `uname -mp` architecture
and kernel config name if different.

### What the build does

`bsd.kmod.mk` turns the `.m` interface files under `sys/dev/bhnd`, `sys/dev/bhnd/...`
and `sys/kern/` into `*_if.h` (and `bhnd_nvram_map.h`) using `makeobjops`, then
compiles:

| file | purpose |
|------|---------|
| `if_bcm4313.c` | attach/init, DMA rings, net80211 plumbing, microcode upload |
| `if_bcm4313_phy_lcn.c` | full LCN-PHY programming & calibration |
| `bcm4313_ucode.c` | embedded D11/LCN microcode (from `firmware/brcm/`) |

---

## 3. Install / load

Copy the module into the kernel module tree (optional, for `kldload if_bcm4313`):

```sh
make SYSDIR=/usr/src/sys install
```

or load it directly from the build directory:

```sh
# make sure the bus it lives on is present (bhnd for the backplane,
# wlan for net80211). Load them first if they're not already.
kldstat -m bhnd || kldload bhnd
kldstat -m wlan || kldload wlan

kldload ./if_bcm4313.ko        # or: kldload if_bcm4313 after install
```

### Verify it attached

```sh
dmesg | grep -i bcm4313
kldstat -m if_bcm4313
ifconfig -v
```

You should see the driver claim the D11 core and register an interface. The
interface name is the one shown by `ifconfig -v` (net80211 will present it as a
base VAP such as `wlan0` once created, or the driver may register one directly).

> A `version mismatch` error means headers don't match the running kernel —
> rebuild against your exact release (`uname -a` + your `/usr/src` rev).

---

## 4. Use Wi-Fi

### Create a wlan device (if not already present)

```sh
ifconfig wlan0 create wlandev <base_if>
```
`<base_if>` is the interface the driver registered (see step 3).

### Scan for networks

```sh
ifconfig wlan0 scan
```

### Connect (open / WPA2 via wpa_supplicant)

Edit `/etc/wpa_supplicant.conf`:

```
network={
    ssid="MyNetwork"
    psk="my-passphrase"
}
```

Then:

```sh
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
dhclient wlan0
```

Persist it for boot by setting these in `/etc/rc.conf`:

```
wlans_wlan0="wlan0"
ifconfig_wlan0="WPA DHCP"
```

and adding your network block to `/etc/wpa_supplicant.conf` (or pointing
`ifconfig_wlan0`'s WPA config at it).

---

## 5. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `kldload`: no such file | Enter the build dir / install first (step 3). |
| `version mismatch` | Rebuild with the kernel source matching your running release. |
| driver doesn't attach | Confirm hardware is a BCM4313 (`pciconf -lv`); check `dmesg` for bus errors. |
| `ifconfig` shows no `wlan` | Ensure `wlan` and `bhnd` are loaded (`kldload wlan bhnd`). |
| weak/no signal | RF tuning tables are loaded from SPROM board flags automatically at attach — check `dmesg` warnings about missing SPROM values. |

Enable driver debug (built-in):

```sh
sysctl hw.bcm4313.debug=1
```

---

## 6. Reproducing the embedded artifacts (for developers)

The checked-in generated files (`bcm4313_ucode.c/.h`, `bcm4313_lcntab.h`,
`bcm4313_phytbl_lcn.h`) are produced byte-for-byte from the bundled reference
sources. Regenerate them from this directory:

```sh
perl gen_lcntab.pl       # -> bcm4313_lcntab.h      (from brcmsmac/phy/phytbl_lcn.c)
perl gen_phytbl.pl > bcm4313_phytbl_lcn.h   # -> bcm4313_phytbl_lcn.h
perl gen_ucode.pl        # -> bcm4313_ucode.c/.h   (from firmware/brcm/*.fw)
```

---

## 7. References

- FreeBSD kernel module build: `man 9 kmod`, `man 9 driver`
- Wireless framework: `man 4 wlan`, `man 4 wpa_supplicant` (or `man 8`)
- Upstream reference driver: Linux `brcmsmac` (bundled under `brcmsmac/` for reference)
- FreeBSD `bhnd(4)` backplane: `man 4 bhnd`