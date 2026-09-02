#!/bin/sh
# diag.sh -- collect a labeled bring-up report for if_bcm4313 on FreeBSD.
# Usage (as root):  sh diag.sh
# Paste the entire output back into your report.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"

fb_sane_os
fb_require_driver

echo "===== uname ====="
uname -a
uname -U
echo
echo "===== kernel modules present on disk ====="
# NOTE: bhnd_sprom is compiled INTO bhnd.ko (no separate .ko file).
for m in bhnd bhndb bhndb_pci bcma_bhndb siba_bhndb; do
    ls -l /boot/kernel/$m.ko 2>&1
done
echo
echo "===== loaded modules ====="
for m in bhnd bhndb bhndb_pci bcma_bhndb siba_bhndb if_bcm4313; do
    kldstat -m $m 2>&1
done
echo
echo "===== device tree: did the PCI front-end build the bhnd chain? ====="
# Expect: bcm4313_pci0 -> bhndb0 -> bhnd0 -> ... -> bcm43130.  A missing
# bcm4313_pci0 means the module predates the if_bcm4313_pci.c front-end.
devinfo -r 2>&1 | grep -E 'bcm4313|bhnd'
echo
echo "===== built driver present in this dir? ====="
pwd
ls -l if_bcm4313.ko 2>&1
echo
echo "===== PCI: Broadcom devices ====="
pciconf -lv 2>&1 | grep -iA5 -e 14e4 -e broadcom
echo
echo "===== net80211 registered devices ====="
sysctl net.wlan.devices 2>&1
echo
echo "===== kernel messages: bcm/wlan/bhnd/pci ====="
dmesg 2>&1 | grep -iE "bcm4313|bcm|bhnd|wlan|d11|pci" | tail -40
echo
echo "===== end of report ====="