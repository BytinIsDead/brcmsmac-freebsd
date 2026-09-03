# Connecting to Wi-Fi with if_bcm4313

Quick-reference for getting online once the driver is **built and loaded**.
For the full build → load → connect walkthrough, see [`INSTALL.md`](INSTALL.md).

## It's three commands (if the driver is already loaded)

```sh
ifconfig wlan0 create wlandev bcm43130   # 1. make the wireless interface
ifconfig wlan0 scan                      # 2. see nearby networks?
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf   # 3. join WPA2
dhclient wlan0                           #    ...and grab an IP
```

> **The guided version of this:** `sh config.sh` opens a scan-and-pick menu
> (or `sh config.sh MySSID MyPassword` connects in one shot). Everything
> below is what it does by hand.

`bcm43130` is the base device the driver registers (named after the module).
If `ifconfig` shows nothing, remember it hides **down** interfaces — plain
`ifconfig` shows nothing; `ifconfig -a` shows everything.

## Step by step

### 1. Create the wireless interface

```sh
ifconfig wlan0 create wlandev bcm43130
```

`wlan(4)` turns the driver's base interface into a regular Wi-Fi interface.

### 2. Scan (sanity check the radio)

```sh
ifconfig wlan0 scan
```

You should see a list of nearby SSIDs. No networks found? See
[Troubleshooting](#troubleshooting) below before debugging the RX path.

### 3. Join a WPA/WPA2 network

Create `/etc/wpa_supplicant.conf`:

```
network={
    ssid="MyNetwork"
    psk="my-password"
}
```

then run the supplicant and DHCP:

```sh
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
dhclient wlan0
```

### 4. Make it automatic at boot

First make the **module itself** load at boot (one `sysrc` line — the same
job as `if_bcm4313_load="YES"` in `/boot/loader.conf`):

```sh
sysrc kld_list+=if_bcm4313
```

Then add the wireless setup to `/etc/rc.conf`:

```
wlans_wlan0="wlan0"
ifconfig_wlan0="WPA DHCP"
```

then `service netif restart` (or reboot) brings Wi-Fi up by itself.

## Verify the radio path is alive

After connecting, these sysctls tell you whether data is actually moving:

```sh
sysctl dev.bcm4313.0.rxframes dev.bcm4313.0.txframes dev.bcm4313.0.txdone
```

- `rxframes` should climb as beacons arrive during a scan.
- `txdone` should track `txframes` once traffic flows.
- If `rxframes` stays at 0 while scanning, the RX path is dead — that's the
  side to debug next.

Other handy sysctls: `dev.bcm4313.0.fwinfo` (chip identity read from SPROM),
`dev.bcm4313.0.dmatx`/`dmarx` (ring positions),
`dev.bcm4313.0.rfkill` (1 = the hardware RF kill switch is blocking the
radio), and `sysctl dev.bcm4313.0.debug=2` for channel-hop logging (no
rebuild needed).

Over a long session, `sysctl dev.bcm4313.0.calticks dev.bcm4313.0.calfires`
confirms the temperature-based TX-power recalibration loop: `calticks`
climbs 1/s while the MAC is up, and `calfires` increments roughly every
3–4 minutes (each fire briefly suspends the MAC for the glacial
recalibration; on boards without temp-based power control both stay put).

## What success looks like

After `dhclient`, `ifconfig wlan0` should end with `status: associated` and
show your network a few lines up (values will differ):

```
wlan0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> ...
        inet 192.168.1.23 netmask 0xffffff00 broadcast 192.168.1.255
        ssid MyNetwork channel 6 (2437 MHz 11g ht/20) bssid aa:bb:cc:dd:ee:ff
        authmode WPA2/802.11i privacy ON
        parent interface: bcm43130
        media: IEEE 802.11 Wireless Ethernet autoselect mode 11ng
        status: associated
```

Anything else in that last line — no `status:` at all, `no carrier`, or a
`scanning`/`associating` state that never settles — means the join or DHCP
failed; check the Troubleshooting table below.

## Troubleshooting

| Problem | Likely cause → fix |
|---|---|
| Driver loads, but `ifconfig` shows no interface | It's down — use `ifconfig -a`. The base device is `bcm43130`, then `ifconfig wlan0 create wlandev bcm43130`. |
| Empty `scan` on a laptop | Physical rfkill: the driver detects a hard-blocked radio — dmesg prints `hardware radio disabled (RF kill switch)` and `sysctl dev.bcm4313.0.rfkill` reads `1`. Flip the hardware switch / Fn key, then `ifconfig wlan0 down && ifconfig wlan0 up`. |
| Empty `scan`, no rfkill | AP too high: some 4313 boards go deaf on channels 12–14. Move the AP to 1/6/11. |
| Empty `scan`, desktop, AP ≤ 11 | Runtime debug: `sysctl dev.bcm4313.0.debug=2`, re-scan, then collect `dmesg | tail -40` for a report. |
| `version mismatch` on load | Kernel headers don't match the running kernel — rebuild against matching source (see [`COMPATIBILITY.md`](COMPATIBILITY.md)). |
| Flaky after an hour | Power save never engages in this port (by design — it's the classic 4313 stability killer on Linux). If you still see drops, re-up the interface: `ifconfig wlan0 down && ifconfig wlan0 up`. |

## Not a BCM4313?

This driver only speaks to the *softMAC* BCM4313 (and 43224/43225). A
**BCM943142HM / BCM43142** is a *FullMAC* chip and will never attach here —
see the note at the top of [`INSTALL.md`](INSTALL.md).

---

Full build instructions, boot persistence via `loader.conf`, and the complete
troubleshooting table live in [`INSTALL.md`](INSTALL.md).