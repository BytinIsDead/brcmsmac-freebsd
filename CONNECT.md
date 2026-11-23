# Connect to Wi-Fi

Use this guide after the module is loaded and `sysctl net.wlan.devices` contains
`bcm43130`. Build/load instructions are in [INSTALL.md](INSTALL.md).

## Recommended: let config.sh do it

On a trusted console:

```sh
sudo sh config.sh
```

This opens a scan-and-pick menu when `dialog(1)` and a terminal are available.
Without a terminal, it prompts for an SSID and hidden password.

One-shot examples:

```sh
sudo sh config.sh MySSID MyPassword
sudo sh config.sh --ping MySSID MyPassword
sudo sh config.sh --boot MySSID MyPassword
sudo sh config.sh --wpa /etc/wpa_supplicant.conf
```

`config.sh` destroys and recreates `WLANIF` before joining. The default is
`wlan0`; override it when necessary:

```sh
sudo env WLANIF=wlan1 sh config.sh MySSID MyPassword
```

Command-line passwords can be visible in shell history and process listings.
Prefer the interactive prompt on a trusted console when that matters.

## Manual connection

### 1. Create the interface and scan

```sh
sudo ifconfig wlan0 destroy 2>/dev/null
sudo ifconfig wlan0 create wlandev bcm43130
sudo ifconfig wlan0 up
ifconfig wlan0 scan
ifconfig wlan0 list scan
```

Use a 2.4 GHz network on channel 1, 6, or 11 during bring-up. Channels 12-14
are board/regdomain-dependent and should not be the first test.

### 2. Join WPA/WPA2 with the FreeBSD supplicant driver

The tested FreeBSD path is `wpa_supplicant -Dbsd`. Do **not** substitute the
Linux `nl80211` driver, and do not use inline `ifconfig wpakey` on this setup.

Create a root-only configuration file:

```text
# /etc/wpa_supplicant.conf
ctrl_interface=/var/run/wpa_supplicant
network={
    ssid="MySSID"
    psk="MyPassword"
}
```

```sh
chmod 600 /etc/wpa_supplicant.conf
sudo mkdir -p /var/run/wpa_supplicant
sudo wpa_supplicant -B -Dbsd -i wlan0 \
    -c /etc/wpa_supplicant.conf -C /var/run/wpa_supplicant
sudo dhclient wlan0
```

To derive a PSK without keeping a plaintext password in the file:

```sh
umask 077
wpa_passphrase 'MySSID' 'MyPassword' \
    | sed '/^[[:space:]]*#psk=/d' > /tmp/wpa_supplicant.conf
```

`wpa_passphrase` emits a complete `network={...}` block and also a commented
plaintext PSK. `config.sh` strips that comment from both temporary and
persistent generated configurations.

### 3. Verify association and IP configuration

```sh
ifconfig wlan0
netstat -rn | grep default
ping -c3 192.168.1.1    # replace with the AP/router address
ping -c3 8.8.8.8
```

Success requires `status: associated`, an IPv4 address, a default route, and
working traffic. A completed `wpa_supplicant` startup alone is not success.

## Open networks

The guided tool handles an empty password as an open network. Manually:

```sh
sudo ifconfig wlan0 ssid MyOpenSSID up
sudo dhclient wlan0
```

## Boot persistence

Load the module at boot using either `if_bcm4313_load="YES"` in
`/boot/loader.conf` or `sysrc kld_list+=if_bcm4313`.

The easiest complete connection setup is:

```sh
sudo sh config.sh --boot MySSID MyPassword
```

It writes a root-only `/etc/wpa_supplicant.conf` and appends the required
`rc.conf` settings when absent:

```text
wlans_if_bcm43130="wlan0"
ifconfig_wlan0="WPA DHCP"
wpa_supplicant_enable="YES"
```

Equivalent manual commands:

```sh
sudo sysrc wlans_if_bcm43130=wlan0
sudo sysrc ifconfig_wlan0="WPA DHCP"
sudo sysrc wpa_supplicant_enable=YES
```

For an existing configuration, `--wpa` is suitable for a one-shot join. When
persisting it, make sure the network is in `/etc/wpa_supplicant.conf`, because
that is the default file used by FreeBSD's boot-time WPA setup.

`config.sh` appends missing boot keys; it does not repair conflicting existing
values. Verify the final files:

```sh
sysrc wlans_if_bcm43130 ifconfig_wlan0 wpa_supplicant_enable
ls -l /etc/wpa_supplicant.conf
sudo service netif restart
```

## RX and TX health

```sh
sysctl dev.bcm4313.0.rxframes dev.bcm4313.0.txframes dev.bcm4313.0.txdone
sysctl dev.bcm4313.0.rxpad dev.bcm4313.0.rxamsdu dev.bcm4313.0.rssiinvalid
sysctl dev.bcm4313.0.txnomatch
```

During a scan:

- `rxframes` must increase.
- With `debug=3`, `SHM maxrx` must read `2346`.
- The `scan RX stats` line makes TX activity, too-long rejects, RX starts,
  bad FCS/PLCP, and overflow visible.
- Short control-frame reception alone is not proof that beacons or probe
  responses work; the scan must return real AP entries.

After traffic starts, `txdone` should advance with transmitted frames and
`txnomatch` should not continuously climb.

## Common failures

| Symptom | Likely cause or next step |
|---|---|
| `wpa_supplicant` cannot find a driver | Use `-Dbsd`; `nl80211` is not the FreeBSD interface. |
| `IOCTL ... op=20 ... Invalid Argument` | Inline `wpakey` is unsupported on the tested setup. Use `config.sh` or `wpa_supplicant -Dbsd`. |
| No association after 30 seconds | Check the 2.4 GHz channel, password, `rfkill`, and `dmesg`. Raise the wait with `TIMEOUT=60` if needed. |
| Association succeeds but DHCP hangs | Inspect `dmesg`, run `dhclient` manually, and check router reachability. |
| Boot does not reconnect | Verify the module-load setting, all three `rc.conf` keys, and `/etc/wpa_supplicant.conf`. |
| Link drops during a long session | Power save is disabled already. Re-up once, then capture counters and logs if the fault returns. |

For a complete failure report, run:

```sh
sudo sh diag.sh
```
