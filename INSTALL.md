# Build, install, and load

This guide covers the driver module only. Once it is loaded, continue with
[CONNECT.md](CONNECT.md).

## 1. Check the machine

You need a FreeBSD machine with kernel source matching the running kernel and a
BCM4313 card.

```sh
uname -r
uname -U
pciconf -lv | grep -A3 -i 14e4
ls /usr/src/sys/sys/param.h
```

Expect a Broadcom device with PCI ID `14e4:4727`. This driver does not support
BCM43142/FullMAC hardware or the N-PHY BCM43131/43224/43225/43236 family.

Kernel modules must be built on FreeBSD against matching kernel headers. A
Linux or macOS build host cannot produce a loadable module.

## 2. Match the kernel source

If `/usr/src/sys/sys/param.h` is missing, install the source package for the
installed release or fetch the exact release tag. Do not use a moving `releng/`
branch for a released kernel.

```sh
uname -U    # for example, 1501000 for 15.1-RELEASE
uname -r    # for example, 15.1-RELEASE
```

For a custom kernel, build against its generated `opt_*.h` directory as well as
the source tree.

## 3. Build

From this directory:

```sh
sh build.sh check       # show the detected source/build paths and commands
sh build.sh             # -> if_bcm4313.ko
```

Useful variants:

```sh
sh build.sh debug       # BCM4313_DEBUG, -g, and extra driver messages
sh build.sh clean
sudo sh build.sh install
```

Environment overrides:

```sh
SYSDIR=/path/to/freebsd/sys sh build.sh
KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/MYKERNEL sh build.sh
JOBS=4 sh build.sh
```

The equivalent direct build is:

```sh
make SYSDIR=/usr/src/sys clean depend all
```

A previous baseline passed syntax/type checks on FreeBSD 14.2, 15.1, and
then-current headers. The latest SHM/RX patch has not yet been rebuilt or loaded;
see [TESTING.md](TESTING.md#verification-status).

## 4. Install and load

### Recommended: use the deployment helper

```sh
sudo sh install.sh
```

This builds, installs, unloads any stale driver, destroys stale `wlan` VAPs,
attempts a PCI reset when needed, loads the fresh module, creates `wlan0`, and
scans. It also rescans the parent PCI bus if the card remains unclaimed.

To reuse an already-built module:

```sh
sudo sh install.sh --skip-build
```

The helper prefers `/boot/modules/if_bcm4313.ko` when that file exists. When
comparing builds, make sure a stale copy there is not hiding the newly built
module; copying the same fresh `.ko` to both `/boot/modules` and `/boot/kernel`
avoids ambiguity.

### Manual load

Copy the module to a known location and load that exact file:

```sh
sudo cp if_bcm4313.ko /boot/modules/if_bcm4313.ko
sudo kldload /boot/modules/if_bcm4313.ko
```

`make install` installs under `/boot/kernel`. Either location works when the
loaded path is explicit. Loading `./if_bcm4313.ko` directly also works.

The module pulls in `bhndb_pci`, `bhndb`, and `bcma_bhndb` automatically. GENERIC
already includes `wlan(4)`; a custom kernel without it may need:

```sh
kldload wlan
```

## 5. Verify attachment

```sh
kldstat -m bhnd/if_bcm4313
sysctl net.wlan.devices
devinfo -r | grep -E 'bcm4313|bhnd'
dmesg | tail -40
```

Expected shape:

```text
bcm4313_pci0 -> bhndb0 -> bhnd0 -> ... -> bcm43130
```

`net.wlan.devices` should contain `bcm43130`. The registered module name is
`bhnd/if_bcm4313`; `kldstat -m if_bcm4313` is not a valid check.

Create and inspect the wireless interface:

```sh
sudo ifconfig wlan0 create wlandev bcm43130
sudo ifconfig wlan0 up
ifconfig wlan0
```

`config.sh` and `install.sh` destroy and recreate their `WLANIF` (default
`wlan0`). Set `WLANIF=wlan1` if you need to preserve an existing `wlan0`.

## 6. Load at boot

Choose one module-loading method:

```sh
# /boot/loader.conf
if_bcm4313_load="YES"
```

or:

```sh
sudo sysrc kld_list+=if_bcm4313
```

Then configure Wi-Fi as described in [CONNECT.md](CONNECT.md#boot-persistence).
`sudo sh install.sh boot` can set up the module and launch
`config.sh --boot` for the connection.

## Troubleshooting

| Symptom | Check or fix |
|---|---|
| `version mismatch` | Rebuild against source matching `uname -U` and the running kernel. Unload the old module first. |
| `kldload: File exists` | The module is already loaded. Verify with `kldstat -m bhnd/if_bcm4313`. |
| Module loads but no `bcm43130` | Inspect `devinfo -r` and `dmesg`. A missing `bcm4313_pci0` means the PCI front-end did not attach. `install.sh` retries and rescans the parent bus. |
| `core reset failed: 22` | An old build passed bhnd-owned clock bits into `bhnd_reset_hw()`. Rebuild; the current code lets bhnd manage those bits. |
| `unsupported PHY type 15 (rev 15)` | An old build read `PHYVER` while the PHY was held in reset. Rebuild; current probe releases reset with the PHY clock enabled and does not assert `PHYRESET`. |
| `missing LCN ucode size` | An old build used firmware section index 12 instead of 13 for the LCN size. Rebuild. |
| Panic about no rate table / `freq 0` | An old build attached net80211 before filling the channel list. Rebuild; channels 1-14 are now installed before `ieee80211_ifattach()`. |
| Empty scan | Check `dev.bcm4313.0.rfkill`, use a 2.4 GHz AP on channel 1/6/11, set `dev.bcm4313.0.debug=3`, and collect the scan diagnostics. The latest max-frame-length fix still needs hardware confirmation. |
| Scan shows networks but `rxframes` stays zero | Record the `SHM maxrx ...` line and `scan RX stats` from `dmesg`. `maxrx` must read `2346`; a small value indicates the old SHM bug. |
| `DMA core not responding` or `core is dead` | Reload the module or power-cycle. A hard wedge can survive `kldunload`; `install.sh` attempts a PCI reset before reloading. |
| `PSM microcode watchdog fired` | Recreate/up the interface once. If it repeats, preserve `dmesg`, DMA counters, and scan output instead of reset-looping. |
| `SIOCS80211 ... op=20 ... Invalid Argument` | The inline `ifconfig wpakey` path is unsupported here. Use `wpa_supplicant -Dbsd` or `config.sh`. |
| Associated but no IPv4 address | Run `dhclient wlan0`, inspect `dmesg`, and check that the AP is using a supported 2.4 GHz mode. |
| No wireless interface in plain `ifconfig` | Down interfaces are hidden; use `ifconfig -a`, then create `wlan0` over `bcm43130`. |

## Diagnostics

```sh
sudo sh diag.sh
```

The report includes kernel version, modules, PCI/device-tree state,
`net.wlan.devices`, all `dev.bcm4313.*` sysctls, and recent kernel messages.
Paste the complete output when reporting a failure.

Common runtime checks:

```sh
sysctl dev.bcm4313.0.fwinfo
sysctl dev.bcm4313.0.rfkill
sysctl dev.bcm4313.0.txframes dev.bcm4313.0.rxframes dev.bcm4313.0.txdone
sysctl dev.bcm4313.0.dmaerr
sudo sysctl dev.bcm4313.0.debug=3
```

`debug=3` adds SHM readback, channel changes, scan counter summaries, and a
small number of RX/TX samples. Return to quiet operation with `debug=0`.
