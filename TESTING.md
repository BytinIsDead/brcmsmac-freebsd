# Testing and verification

A successful build is necessary but not sufficient. The definitive test is a
fresh module on real BCM4313 hardware, starting from a cold power cycle.

## Verification status

| Area | Status |
|---|---|
| SHM offsets and scan-counter offsets | Checked against the bundled brcmsmac headers |
| Shell syntax for the helper scripts | `sh -n` passed |
| Whitespace/error checks | `git diff --check` passed |
| Previous source baseline on FreeBSD 14.2 | Recorded clean `-fsyntax-only` check, plain and `BCM4313_DEBUG` |
| Previous source baseline on FreeBSD 15.1 | Recorded clean `-fsyntax-only` check, plain and `BCM4313_DEBUG` |
| Previous source baseline on then-current headers | Recorded clean `-fsyntax-only` check |
| Latest SHM/PHY/radio/RCM patch | **Not rebuilt or hardware-tested yet** |
| Beacon/probe-response reception | **Pending** |
| Scan results, association, DHCP, and sustained traffic | **Pending** |
| N-PHY family support | Not implemented |

Earlier hardware runs reached attach, microcode upload, DMA activity, and
short control-frame RX. They did not clear the management-RX, association, or
DHCP gates.

## Local checks before deployment

On a shell with Git and POSIX `sh` available:

```sh
sh -n build.sh install.sh config.sh diag.sh lib.sh
git diff --check
```

These checks do not compile kernel code. Build on FreeBSD against matching
kernel source:

```sh
uname -U
sh build.sh check
sh build.sh
# Optional debug build:
sh build.sh debug
```

A previous full syntax/type check used the kernel warning set and real kernel
headers under `-ffreestanding -D_KERNEL -DKLD_MODULE`. Repeat that check, or a
full module build, after changing the current RX patch. Syntax-only checking
does not link the `.ko` or exercise hardware.

## Definitive cold-power-cycle test

Use a FreeBSD 15.1-RELEASE amd64 machine when reproducing the current bring-up.
A cold power cycle matters: DMP/DMA wedge state can survive `kldunload` and a
warm reboot.

### 1. Deploy the exact module

```sh
sh build.sh
sudo cp if_bcm4313.ko /boot/modules/if_bcm4313.ko
```

If an older driver is loaded, use `sudo sh install.sh --skip-build` rather than
unloading it while a `wlan` interface still references it. Confirm the loaded
file and registered module:

```sh
kldstat | grep if_bcm4313
kldstat -m bhnd/if_bcm4313
```

### 2. Verify attach and enable diagnostics before `up`

```sh
sysctl net.wlan.devices
devinfo -r | grep -E 'bcm4313|bhnd'
dmesg | grep -i bcm4313
sudo sysctl dev.bcm4313.0.debug=3
sudo ifconfig wlan0 create wlandev bcm43130
sudo ifconfig wlan0 up
```

Expected device chain:

```text
bcm4313_pci0 -> bhndb0 -> bhnd0 -> ... -> bcm43130
```

Expected SHM readback during MAC start:

```text
SHM maxrx 2346 slot 9|20 channel N
```

`maxrx` must be `2346`. The old bug produced `9` or `20` here because the slot
write landed on `M_MAXRXFRM_LEN`.

### 3. Prove management-frame reception

Keep a known 2.4 GHz AP active on channel 1 or 6:

```sh
ifconfig wlan0 scan
ifconfig wlan0 list scan
sysctl dev.bcm4313.0.rxframes
dmesg | tail -60
```

Pass this gate only if all of the following hold:

- `rxframes` increases during the scan.
- `list scan` returns real AP entries with plausible signal values.
- `toolong` in `scan RX stats` is not explaining away the received traffic.
- There is no dead-core latch, PSM watchdog, or DMA error flood.

Do not accept reception of 20-byte ACK/CF-End frames as proof of this gate.
Those frames fit inside the old, incorrectly small maximum frame length.

### 4. Prove association and DHCP

Use a WPA2 AP on 2.4 GHz:

```sh
sudo sh config.sh MySSID MyPassword
ifconfig wlan0
netstat -rn
ping -c3 192.168.1.1    # router address
ping -c3 8.8.8.8
```

Pass requires `status: associated`, an IPv4 address, a default route, and
working traffic. Capture `wpa_supplicant`/`dmesg` output if any stage fails.

### 5. Soak and recovery

- Exchange traffic in both directions for at least 15 minutes.
- Watch `txframes`, `rxframes`, `txdone`, `txnomatch`, `dmaerr`, and `wdogfires`.
- On tempsense boards, `calticks` should advance while the MAC is up and
  `calfires` should eventually increase during a long enough run.
- Recreate the interface once and verify recovery.
- Reboot once and verify boot-time module loading and Wi-Fi reconnection.

```sh
sysctl dev.bcm4313.0.txframes dev.bcm4313.0.rxframes
sysctl dev.bcm4313.0.txdone dev.bcm4313.0.txnomatch
sysctl dev.bcm4313.0.wdogfires dev.bcm4313.0.dmaerr
sysctl dev.bcm4313.0.calticks dev.bcm4313.0.calfires
```

## Acceptance checklist

- [ ] Fresh module loads without a version mismatch.
- [ ] PCI front-end, bhnd chain, and `bcm43130` attach cleanly.
- [ ] Microcode upload completes and the PSM self-suspend handshake succeeds.
- [ ] SHM readback reports `maxrx 2346`.
- [ ] A scan returns real APs and `rxframes` increases.
- [ ] Association succeeds on a 2.4 GHz WPA2 network.
- [ ] DHCP, routing, and ping work.
- [ ] Sustained bidirectional traffic causes no watchdog/DMA/dead-core failure.
- [ ] Interface re-up recovers cleanly.
- [ ] Boot persistence reconnects after reboot.
- [ ] Results are recorded as hardware-verified, with the tested FreeBSD release.

## Failure report

Run:

```sh
sudo sh diag.sh
```

Also include:

```sh
uname -a
uname -U
git -C /usr/src log -1 --oneline
ifconfig wlan0 list scan
dmesg | tail -80
```

For an empty scan, include the complete `SHM maxrx ...` and `scan RX stats`
lines. For an association failure, include supplicant logs and the exact
`ifconfig wlan0` state. Do not include Wi-Fi passwords in reports.

Return to normal logging after diagnosis:

```sh
sudo sysctl dev.bcm4313.0.debug=0
```
