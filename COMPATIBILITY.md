# Compatibility & supported FreeBSD releases

This page records which FreeBSD releases the driver was validated against and
how to make sure the source tree you build from matches the kernel you run.

## Validation matrix

Build validation is done on **amd64** against the official FreeBSD kernel
headers for the exact release (not just a `releng/` HEAD):

| FreeBSD | `__FreeBSD_version` | API profile used | Build check |
|---------|---------------------|------------------|-------------|
| **14.2-RELEASE** | `1402000` | `ieee80211_var.h` etc. for 14.x | ✅ clean (`-Wall -Wextra`, plain + `BCM4313_DEBUG`) |
| **15.1-RELEASE** | `1501000` | `struct ieee80211_node_txrate` `.mcs`/`.dot11rate`; 2-arg `ic_transmit` | ✅ clean (plain + `BCM4313_DEBUG`) |
| **current / main** | `16000xx` | same struct branch (`>= 1500000`) | ✅ clean (plain + `BCM4313_DEBUG`) |

> "Build check" here means a syntax/type (`-fsyntax-only`) compile under
> `-ffreestanding -D_KERNEL -DKLD_MODULE` with real kernel headers. It is not
> a runtime test on hardware — see [`TESTING.md`](TESTING.md).

## Release-specific notes

### FreeBSD 15.x — `__FreeBSD_version >= 1500000`

Since FreeBSD 15, net80211 carries the current transmit rate in
`struct ieee80211_node_txrate` rather than a bare `uint8_t`:

```c
struct ieee80211_node_txrate {
        enum ieee80211_node_txrate_type type;
        uint8_t nss;        /* VHT - number of spatial streams */
        uint8_t mcs;        /* HT  - MCS */
        uint8_t dot11rate;  /* Legacy/HT - ratecode */
};
```

The driver selects the struct fields with an `#if __FreeBSD_version >= 1500000`
branch when filling the TX header:

- HT frames use `ni->ni_txrate.mcs`
- legacy frames use `ni->ni_txrate.dot11rate`

`ic_transmit` remains 2-argument `(struct ieee80211com *, struct mbuf *)` on
both 14 and 15, so there is one implementation (the net80211 node is recovered
from the mbuf on both).

### bhnd source layout (affects out-of-tree builds)

FreeBSD 14 and 15 both keep the bhnd/bhndb `.m` files in subdirectories:

```
sys/dev/bhnd/bhnd_bus_if.m
sys/dev/bhnd/cores/chipc/bhnd_chipc_if.m
sys/dev/bhnd/cores/chipc/pwrctl/bhnd_pwrctl_if.m
sys/dev/bhnd/cores/pmu/bhnd_pmu_if.m
sys/dev/bhnd/bhndb/bhndb_bus_if.m
sys/dev/bhnd/bhndb/bhndb_if.m
sys/kern/bus_if.m  sys/kern/device_if.m
```

The `Makefile` sets `.PATH.m` to all of these, so `if_bcm4313.ko` builds
against a plain `/usr/src/sys` on either branch.

## Matching your source tree to your kernel

A kernel module must be built against headers matching the **running kernel**,
else `kldload` fails with a version mismatch.

- Check your running kernel's version id:
  ```sh
  uname -U          # __FreeBSD_version, e.g. 1501000 = 15.1-RELEASE
  uname -r          # e.g. 15.1-RELEASE
  ```
- Use kernel source that matches it. On a released version, use the **release
  tag** (not the `releng/` branch, which drifts after the release):
  ```sh
  git clone --depth 1 --branch release/15.1.0 https://github.com/freebsd/freebsd-src.git /usr/src
  ```
  or install the matching source via `bsdinstall` / `freebsd-update` so
  `/usr/src` already matches.
- Then build:
  ```sh
  make SYSDIR=/usr/src/sys clean depend all
  ```
  and for a custom kernel also pass `KERNBUILDDIR=` (see
  [`INSTALL.md`](INSTALL.md#2-build-the-module)).

## Reporting a compatibility issue

For a report to be actionable, include:

```
uname -a && uname -U
git -C /usr/src log -1 --oneline    # kernel source rev
```

- attach/detach messages and any `version mismatch` from `kldload`
- the `pciconf -lv` line for the BCM4313