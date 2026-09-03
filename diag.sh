#!/bin/sh
# diag.sh -- collect a labeled bring-up report for if_bcm4313 on FreeBSD.
#
# Usage (as root):
#   sh diag.sh           # print the report
#   sh diag.sh --help    # this text
#
# It gathers the kernel modules, device tree, PCI info, net80211 devices,
# driver sysctls and recent dmesg lines -- each under a labeled heading --
# and prints them to stdout.  Paste the entire output into your report.
#
# When to run it:
#   - after `sh install.sh` if the driver did not attach
#   - when `sysctl net.wlan.devices` shows no bcm4313* after kldload
#   - before opening an issue: run it and paste everything

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"

case "${1:-}" in
-h|--help) fb_usage; exit 0 ;;
"") ;;
*)
    echo "ERROR: diag.sh takes no arguments (found '$1')" >&2
    echo "       full usage: sh diag.sh --help" >&2
    exit 1
    ;;
esac

fb_sane_os
fb_ensure_driver

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
echo "===== driver sysctls (dev.bcm4313.*) ====="
sysctl -a 2>&1 | grep '^dev.bcm4313' || echo "(none -- module predates the sysctl build)"
echo
echo "===== kernel messages: bcm/wlan/bhnd/pci ====="
dmesg 2>&1 | grep -iE "bcm4313|bcm|bhnd|wlan|d11|pci" | tail -40
echo
echo "===== end of report ====="