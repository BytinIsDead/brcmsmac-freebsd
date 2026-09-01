# Testing & verification

What has been verified, how to reproduce it, and how to run the definitive
hardware test.

## What "build-verified" means here

The strongest check performed in this project's environment is a full
compilation of every driver source file against the **real FreeBSD kernel
headers** for a specific release, using the kernel's own warning set:

```
clang -fsyntax-only -ffreestanding -nostdinc \
    -isystem "$(clang -print-resource-dir)/include" \
    -I<bhnd-if-generated-dir> \
    -I<sysroot>/sys -I<sysroot>/sys/dev/bhnd \
    -I<sysroot>/sys/contrib/ck/include \
    -D_KERNEL -DKLD_MODULE -D__FreeBSD__ \
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-qual -Wno-sign-compare \
    if_bcm4313.c if_bcm4313_phy_lcn.c bcm4313_ucode.c
```

The `<sysroot>` is the checked-out FreeBSD source tree for the release under
test, and the `<bhnd-if-generated-dir>` holds the `makeobjops`-generated
`*_if.h` / `bhnd_nvram_map.h` / `machine` → `amd64/include` symlinks. A clean
run means **zero driver-originated diagnostics** (only upstream header noise
such as `-Wignored-qualifiers` in `net/debugnet.h` and
`net80211/ieee80211_crypto.h`).

Re-run the same command with `-DBCM4313_DEBUG` added to also validate the debug
printing paths.

## Reproducibility checks

- **Generated artifacts**: `gen_*.pl` output is byte-compared to the checked-in
  `bcm4313_ucode.c/.h`, `bcm4313_lcntab.h`, `bcm4313_phytbl_lcn.h`. They must be
  identical after regeneration (see [`ARCHITECTURE.md`](ARCHITECTURE.md)).
- These checks run in CI-less local tooling; there is no GitHub Actions config
  in this repo.

## Limitations of CI-style checks

- `-fsyntax-only` stops at the front end: it does **not** link `if_bcm4313.ko`,
  run `make` / the kernel build glue, or load the device.
- It cannot exercise the D11 MAC core, upload microcode, or touch the radio.

> So a green build is necessary but **not** sufficient. The definitive test is
> loading the module on real BCM4313 hardware.

## Definitive hardware test (FreeBSD 15.1 example)

On a FreeBSD **15.1-RELEASE** amd64 machine with a BCM4313:

```sh
# 1. source matching the running kernel
uname -U                          # expect 1501000
make SYSDIR=/usr/src/sys clean depend all     # -> if_bcm4313.ko

# 2. dependencies (the driver pulls in its own bhnd bridge chain;
#    wlan(4) is compiled into GENERIC -- only custom kernels need it)
kldstat -m wlan || kldload wlan

# 3. attach
kldload ./if_bcm4313.ko

# 4. confirm the PCI front-end built the chain and the D11 core registered
devinfo -r | grep -E 'bcm4313|bhnd'    # bcm4313_pci0 -> bhndb0 -> bhnd0 -> bcm43130
dmesg | grep -i bcm4313
ifconfig -v | grep -A2 -i "bcm4313\|wlan"

# 5. bring up a wlan VAP and scan
ifconfig wlan0 create wlandev <base_if>
ifconfig wlan0 scan        # list visible APs

# 6. functional: join a 2.4GHz WPA2 network
wpa_passphrase MyNetwork "ssid!passphrase" > /tmp/w.cnf
wpa_supplicant -B -i wlan0 -c /tmp/w.cnf
dhclient wlan0
```

### Pass criteria checklist

- [ ] `kldload` succeeds with no `version mismatch`
- [ ] `dmesg` shows the driver attaching to `bhnd`/D11 core rev 24
- [ ] microcode upload reported without error
- [ ] `ifconfig wlan0 scan` lists real APs
- [ ] association at each of 802.11b/g/n rates, HT20, MCS 0–7
- [ ] sustained `iperf3` RX/TX with no watchdog-timeout / dropped-ring errors
- [ ] `hw.bcm4313.debug=1` and a scan shows sane RSSI values

## Debugging a failure

- Enable `hw.bcm4313.debug=1` (module must be loaded with `BCM4313_DEBUG`
  compiled in to see prints).
- Watch for `dmesg` panics from `KASSERT` in the DMA callbacks — these point at
  a chunking / descriptor-layout issue on the real ring width.
- Confirm the SPROM/board flags read back (look for `bhnd_nvram` warnings at
  attach); the RF tables are picked from these.
- Collect `uname -a && uname -U`, the kernel source rev, and the full
  `dmesg | tail` for a report (see [`COMPATIBILITY.md`](COMPATIBILITY.md)).