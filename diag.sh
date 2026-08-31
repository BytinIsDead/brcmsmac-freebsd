#!/bin/sh
# diag.sh -- collect a labeled bring-up report for if_bcm4313 on FreeBSD.
# Usage (as root):  sh diag.sh
# Paste the entire output back into your report.
echo "===== uname ====="
uname -a
uname -U
echo
echo "===== kernel modules present on disk ====="
ls -l /boot/kernel/bhnd.ko /boot/kernel/bhndb.ko 2>&1
echo
echo "===== loaded modules ====="
kldstat -m bhnd 2>&1
kldstat -m if_bcm4313 2>&1
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