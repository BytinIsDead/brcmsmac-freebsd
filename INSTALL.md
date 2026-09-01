# Getting your BCM4313 Wi-Fi working on FreeBSD

A friendly, step-by-step guide to building and loading **if_bcm4313**, the
FreeBSD driver for the Broadcom **BCM4313** Wi-Fi chip.

**What you're really doing:** taking the driver source in this folder,
compiling it into a kernel module (a `.ko` file), and telling FreeBSD to load
it, so the operating system can talk to your Wi-Fi chip and you can connect
to a network. Three jobs: **build → load → connect**.

> **Quick check first — is your chip a BCM4313?**
> Run `pciconf -lv | grep -A3 14e4` (Broadcom's vendor id is `14e4`). If you
> see **`BCM4313`**, this guide is for you.
>
> **Not a BCM4313?** This driver only speaks to the *softMAC* BCM4313 (and
> 43224/43225). If your card is a **BCM943142HM / BCM43142**, that's a
> *FullMAC* chip (Linux drives it with `brcmfmac` + proprietary firmware) —
> **this driver will not attach to it.** FreeBSD doesn't ship a FullMAC
> Broadcom driver, so that hardware needs a separate port.

---

## The 30-second version

If you already have FreeBSD installed with kernel source, this is all of it:

```sh
sh build.sh                                  # build -> if_bcm4313.ko
kldload ./if_bcm4313.ko                      # driver + whole bhnd chain (auto)
dmesg | grep -i bcm4313                      # did it attach?
ifconfig wlan0 create wlandev <iface>        # make a wireless interface
ifconfig wlan0 scan                          # see networks?
```

That's it — one `kldload` pulls in the PCI front-end and the entire `bhnd`
bridge chain automatically (see Step 4).

Everything below is the same thing, explained slowly.

---

## Step 1 — What you need

| Requirement | How to check it | What if it's missing |
|---|---|---|
| A FreeBSD machine (14.x or 15.x) | `uname -r` | Install FreeBSD first. |
| Kernel source in `/usr/src/sys` | `ls /usr/src/sys/sys/param.h` | See Step 2. |
| A compiler (comes with FreeBSD) | `cc --version` | Nothing to do — stock FreeBSD ships one. |

You must do this on a **real FreeBSD machine** (or VM). Kernel drivers can't
be built or tested on Linux/macOS — they compile against FreeBSD kernel
headers and load only into a FreeBSD kernel.

## Step 2 — Get the kernel source (if `/usr/src` is empty)

The driver needs FreeBSD's kernel headers to build against. Two easy ways:

**Way A — let FreeBSD give you the matching source** (recommended):

```sh
sudo freebsd-update fetch
```

then, during install, choose *Install source*. Or use `bsdinstall`'s "Install
source" option on a fresh system. Either way `/usr/src` now matches your
kernel exactly.

**Way B — clone the exact release tag** (for power users):

```sh
uname -U    # prints something like 1501000  (= 15.1-RELEASE)
git clone --depth 1 --branch release/15.1.0 https://github.com/freebsd/freebsd-src.git /usr/src
```

> **Why "release tag" and not `releng/15.1`?** `releng/15.1` is the
> *development branch* — it keeps changing after the release ships, so its
> headers don't match your installed 15.1-RELEASE kernel. The `release/15.1.0`
> tag is the actual release. Mismatched headers = `version mismatch` when you
> load the module (see Step 6).

## Step 3 — Build the driver

From this folder (the one with the `Makefile`), either:

```sh
sh build.sh          # wrapper: auto-detects SYSDIR and KERNBUILDDIR
# or, the long way:
make SYSDIR=/usr/src/sys clean depend all
```

You should see compiler output and end with a file called **`if_bcm4313.ko`**
appearing in this folder. (`sh build.sh check` prints the exact commands
without running them.)

**Custom kernel?** If you built your FreeBSD kernel from a custom config, add
`KERNBUILDDIR=` pointing at its build directory:

```sh
make SYSDIR=/usr/src/sys \
    KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/GENERIC \
    clean depend all
```

(Change `amd64.amd64/sys/GENERIC` to your architecture and kernel name —
`uname -mp` shows the architecture part.)

## Step 4 — Load the driver

Just load the module:

```sh
kldload ./if_bcm4313.ko
```

That one command does the whole job. The module carries its own **PCI
front-end** and declares the full dependency chain, so the whole stack is
built for you:

```
PCI 14e4:4727 (your BCM4313 card)
  └─ bcm4313_pci     our PCI front-end (inside this module)
       └─ bhndb      PCI→bhnd bridge        (bhndb_pci.ko)
            └─ bhnd  backplane bus          (bcma_bhndb.ko)
                 └─ D11 core rev 24 → bcm4313 (this driver)
```

**Why is the front-end necessary?** The D11 core only becomes a device after
a PCI driver claims the card and creates the bhnd bus. On FreeBSD that PCI
driver is `bwn_pci` — but its device table lists only the BCM4331/43224/
43225, **not the BCM4313**. So previously `kldload` succeeded yet nothing
ever attached: no bridge, no bus, no D11 core, no interface. This module's
own `bcm4313_pci` claims `14e4:4727` and builds the chain itself — no
`/usr/src` patching, no rebuilding FreeBSD's own modules.

`wlan(4)` needs nothing: it is compiled into the GENERIC kernel. Only a
custom kernel that dropped `device wlan` needs `kldload wlan`.

To load at every boot, install the module and enable it:

```sh
make SYSDIR=/usr/src/sys install    # copies if_bcm4313.ko into /boot/kernel
```

```
# /boot/loader.conf
if_bcm4313_load="YES"
```

The bridge-chain modules are pulled in automatically by the module's
dependencies.

**Did it work?** Check:

```sh
kldstat -m if_bcm4313                # is the module in memory?
devinfo -r | grep -E 'bcm4313|bhnd'  # the whole chain, top to bottom
dmesg | tail -20                     # recent kernel messages
```

`devinfo` should show the chain: `bcm4313_pci0` → `bhndb0` → `bhnd0` → … →
`bcm43130`. If `bcm4313_pci0` is missing, you're running a stale module
without the PCI front-end (Step 4).

You want to see the driver *attaching* — e.g. a line mentioning
`bcm4313` and the D11 core. Then look at your network interfaces:

```sh
ifconfig
```

The driver registers an interface (a name like `wlan0` after Step 5, or a
base interface before it).

> **Got `version mismatch`?** Your headers don't match your running kernel.
> Go back to Step 2, get the *matching* source, rebuild, and `kldunload`
> the old module first.

## Step 5 — Connect to Wi-Fi

**1. Create a wireless interface** over the driver's base interface
(`<base_if>` is whatever the driver registered, e.g. `wlan0` if it's already
the device name, or the name `ifconfig` showed):

```sh
ifconfig wlan0 create wlandev <base_if>
```

**2. Scan for networks** — sanity check that the radio works:

```sh
ifconfig wlan0 scan
```

You should see a list of nearby network names (SSIDs).

**3. Join a network (WPA2)**. Create `/etc/wpa_supplicant.conf`:

```
network={
    ssid="MyNetwork"
    psk="my-password"
}
```

then run:

```sh
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
dhclient wlan0
```

**4. Make it automatic at boot** — add to `/etc/rc.conf`:

```
wlans_wlan0="wlan0"
ifconfig_wlan0="WPA DHCP"
```

Now `service netif restart` (or a reboot) brings up Wi-Fi by itself.

## Step 6 — Common problems, explained plainly

| You see... | What it means | What to do |
|---|---|---|
| `kldload: File exists` | Module already loaded | `kldunload if_bcm4313` first. |
| `devinfo` shows no `bcm4313_pci0` | Old module without the PCI front-end | `git pull` and rebuild — `if_bcm4313_pci.c` is what creates the bridge chain. |
| `version mismatch` | Headers ≠ running kernel | Rebuild against the release-tag source matching `uname -U`. |
| Driver loads but no interface | Didn't attach to the chip | `sysctl net.wlan.devices` — if it lists `bcm43130`, attach worked and you just need `ifconfig wlan0 create wlandev bcm43130`. If empty, run `devinfo -r | grep -E 'bcm4313|bhnd'` — a missing `bcm4313_pci0` means the front-end didn't probe; otherwise `dmesg | tail` shows the failing attach step. |
| `bcm43130: core reset failed: 22` | You're running a build with the old D11 core ioctl bits. The driver used to pass the clock bits that the `bhnd` backend owns exclusively (`BHND_IOCTL_CLK_EN`/`CLK_FORCE`) into `bhnd_reset_hw()`, and the bcma backend rejects that with `EINVAL` (22). | `git pull` and rebuild — current code uses brcmsmac's real `SICF_*` values (`SUPPORT_G=0x2000`, `PHYRESET=0x0008`, `PHYCLOCK_ENABLE=0x0004`) and lets the backend manage the clock bits itself. |
| `ifconfig` shows nothing | The interface is simply down | Plain `ifconfig` hides down interfaces — use `ifconfig -a`. The base device is named `bcm43130` (from the module name), then `ifconfig wlan0 create wlandev bcm43130`. |
| `ifconfig` has no `wlan` | `wlan(4)` missing | Built into GENERIC; only a custom kernel without `device wlan` needs `kldload wlan`. |
| Loads but Wi-Fi is flaky | Usually SPROM/tuning issues | Set `sysctl hw.bcm4313.debug=1` (needs a debug build) and collect `dmesg` output for a report. |
| **No attach at all** | Chip isn't BCM4313 | If it's a BCM943142HM/BCM43142 (FullMAC), this softMAC driver can't drive it — see the note at the top. |

Still stuck? Grab everything at once and paste it in your report:

```sh
kldstat -m if_bcm4313; kldstat -m bhnd; echo ---; \
  sysctl net.wlan.devices; echo ---; dmesg | tail -40; echo ---; \
  pciconf -lv | grep -iA5 broadcom; uname -a; uname -U
```

## For developers

- **Enable debug prints:** compile with `-DBCM4313_DEBUG` (or uncomment
  `#define BCM4313_DEBUG 1` in `opt_bcm4313.h`), then
  `sysctl hw.bcm4313.debug=1`.
- **Regenerate the embedded tables/microcode** from the bundled sources:

  ```sh
  perl gen_lcntab.pl                 # -> bcm4313_lcntab.h
  perl gen_phytbl.pl > bcm4313_phytbl_lcn.h
  perl gen_ucode.pl                  # -> bcm4313_ucode.c/.h
  ```

- **No firmware files needed** — the D11/LCN microcode and RF tables are
  compiled into the module. That's why there's no `/boot/modules/<fw>` step.

## More docs

- [`README.md`](README.md) — what the driver is and how it's put together
- [`COMPATIBILITY.md`](COMPATIBILITY.md) — FreeBSD version support matrix
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — how the port maps to Linux `brcmsmac`
- [`TESTING.md`](TESTING.md) — what's verified, and the hardware test deck