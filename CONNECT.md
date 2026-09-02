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

Add to `/etc/rc.conf`:

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
`dev.bcm4313.0.dmatx`/`dmarx` (ring positions), and
`sysctl dev.bcm4313.0.debug=2` for channel-hop logging (no rebuild needed).

## Troubleshooting

| Problem | Likely cause → fix |
|---|---|
| Driver loads, but `ifconfig` shows no interface | It's down — use `ifconfig -a`. The base device is `bcm43130`, then `ifconfig wlan0 create wlandev bcm43130`. |
| Empty `scan` on a laptop | Physical rfkill: check the hardware switch / Fn key / BIOS. FreeBSD has no rfkill concept, so a hard-blocked radio shows up as an empty scan. |
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