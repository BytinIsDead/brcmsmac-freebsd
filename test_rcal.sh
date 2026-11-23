#!/bin/sh
# full bring-up + scan test with RC calibration enabled
kenv hw.bcm4313.skip_rcal=0
kldload /boot/modules/if_bcm4313.ko
sleep 1
ifconfig wlan0 create wlandev bcm43130 2>&1
ifconfig wlan0 up 2>&1
sleep 2
echo "=== SCAN ==="
ifconfig wlan0 scan 2>&1 | head -25
echo "=== SCAN LIST ==="
ifconfig wlan0 list scan 2>&1 | head -15
echo "=== DEAD CHECK ==="
dmesg | grep -cE "DEAD"
echo "=== PMU WIRING ==="
dmesg | grep -E "PMU provider|pll_upd PMU_CTL" | head -4
echo "=== LAST TRACES ==="
dmesg | grep -E "setchan|init:mac|DEAD" | tail -12