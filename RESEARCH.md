# Linux-side reference notes (BCM4313)

What the Linux ecosystem says about this exact chip, and why it matters for
this port. Sources: linux-wireless forums, kernel bug trackers, distro
wikis, and the bundled `brcmsmac/` reference tree.

## Who drives the BCM4313 on Linux

| Driver | PHY support | Status |
|---|---|---|
| `brcmsmac` | LCN (type 8) — the 4313's only PHY | The working driver; this port is transcribed from it |
| `b43` | No LCN support — logs `UNSUPPORTED PHY (Type 8 (LCN))` | Cannot drive the 4313 at all |
| `bcmwl` (Broadcom STA blob) | closed | Works for some users, flaky after kernel updates |

Conclusion: **brcmsmac is the only open-source option for this card.** b43's
absence of LCN support is why this repo embeds the LCN source + microcode
rather than sharing b43's firmware machinery.

## Firmware

- Linux loads `brcm/bcm43xx-0.fw` (ucode blob) + `brcm/bcm43xx_hdr-0.fw`
  (section index) from `linux-firmware` at runtime.
- This port embeds those **same two files** (`firmware/brcm/`) into the
  module at build time — no runtime firmware loading needed.
- The section tags were validated against brcmsmac's `ucode_loader.h` enum
  (the earlier off-by-one: LCN ucode = idx 12, LCN size = idx 13).

## Known 4313 quirks (worth remembering during bring-up)

1. **High channels (12–14) are unreliable on some boards.** Multiple reports
   (e.g. Launchpad #1916028) of brcmsmac going deaf when the AP is on
   channels 12–14, fixed by pinning the AP to ch ≤ 11. The PHY tunes
   fine per the code; it appears board/radio-dependent. If scan never finds
   an AP that is on ch 13, try moving the AP to 1/6/11.
2. **Power save is the classic stability killer** on this chip: "connection
   drops after an hour", "watchdog resets" complaints on Linux usually get
   fixed with `iwconfig wlan0 power off`. This port does **not** set
   `IEEE80211_C_PS`, so net80211 power save never engages — good, keep it
   that way.
3. **Hardware rfkill**: some 4313 laptops hard-disable the radio at the
   antenna switch; Linux shows `rfkill`, FreeBSD net80211 has no equivalent
   concept. This port now wires brcmsmac's `MI_RFDISABLE` interrupt (plus
   the `rfdisabledly` debounce timer): the driver logs "hardware radio
   disabled (RF kill switch)", stops the MAC while blocked, and exposes the
   state as `dev.bcm4313.0.rfkill` (1 = hard-blocked). If `list scan` is
   empty on a laptop, check that sysctl / the dmesg tail before chasing
   the RX path.
4. **The D11 PSM watchdog** (`MI_GP0` in brcmsmac's `brcms_c_dpc`) is treated
   as fatal with a full restart. This port mirrors that, and now disarms
   the DMA engines (brcmsmac's dma_txreset/dma_rxreset in
   `brcms_b_corereset`) before any core reset so recovery cannot hang.

## Scan/assoc behavior on Linux (mac80211) vs this port (net80211)

- mac80211 runs a hardware-assist-less software scan exactly like net80211's
  `swscan`; brcmsmac's `scan_start`/`scan_end` are mostly no-ops, and the
  per-channel hops go through `bss_info_changed` → `brcms_c_set_chanspec`.
  This port's `scan_start`/`scan_end` are intentionally light, but MUST
  cover the beacon filter: D11 hardware drops beacons not matching the
  programmed BSSID unless `MCTL_BCNS_PROMISC` (1<<20) is set during scan —
  the same bit brcmsmac raises for `FIF_BCN_PRBRESP_PROMISC`. (Fixed in
  this port; see INSTALL.md troubleshooting.)
- brcmsmac enables the MAC with `SICF_GMODE | SICF_PCLKE` and **never
  asserts PHYRESET at probe** on D11 rev ≥ 18 — `PHYVER` must be readable
  with `PHYCLOCK_ENABLE` only. Matches this port's reset sequence.

## FreeBSD sibling port: narqo/freebsd-brcmfmac

A runtime-tested FreeBSD port of the FullMAC `brcmfmac` (BCM4350/BCM43455),
built the same way as this project (AI-written, in the open). Its FullMAC
code has nothing to borrow for a SoftMAC port, but its user-facing choices
are worth mirroring:

- **Boot persistence via `sysrc kld_list+=if_<drv>`** (rc.conf) instead of
  hand-editing `/boot/loader.conf`. Both work on 14/15; install.sh here uses
  loader.conf and documents the `sysrc` alternative.
- **Show "what success looks like"**: its README gives the exact
  `pciconf -lv` line to expect and a sample post-association `ifconfig`
  block, not just bare command lists. CONNECT.md in this repo now does the
  same.
- Linux brcmsmac's only user knob is a `debug` module option that is a
  `BRCM_DL_*` bit vector (`module_param_named(debug, brcm_msg_level)` in
  mac80211_if.c). This port's `dev.bcm4313.0.debug` sysctl is the same idea
  narrowed to the scan/channel-hop logging the port actually emits.
- Their AGENTS.md regdomain note (ifconfig's channel filter on `wlan0
  create` silently drops channels outside the country) does **not** apply
  here: this driver registers the standard 1-14 2.4GHz list the same way
  bwn/rtwn do, and normal country filtering is expected net80211 behavior.

## Where to look if bringing up a different Broadcom chip

brcmsmac only ever supported the 4313 family (LCN + BCM4331-class N-PHY in
the same tree). For other chipsets on FreeBSD, `bwn(4)` is the in-tree
alternative — it uses the same bhnd(4) bus this driver attaches to, so the
PCI front-end + DMA64 + net80211 glue patterns transfer.

## Scan TX path audit (FreeBSD 15.1 net80211, verified in-tree)

Root of truth: `/tmp/fbsd151/sys/net80211/`. The software scanner needs
nothing from this driver beyond what exists:

- **Channel stepping**: `scan_curchan()` in `ieee80211_scan_sw.c` calls
  `ic_set_channel(ic)` per channel; the driver only implements
  `scan_start`/`scan_end` hooks, which net80211 calls around the whole
  scan. No per-channel driver callbacks — `bcm4313_scan_start/end`
  (MCTL_BCNS_PROMISC on/off) is the complete contract.
- **Probe requests are TX-path frames, not scan callbacks**: active scan
  calls `ieee80211_send_probereq()` → `ieee80211_raw_output()` →
  `ic->ic_raw_xmit`. The port implements `bcm4313_raw_xmit` for real
  (feeds `bcm4313_tx_start_locked`, gated on FLAG_RUNNING), so probe
  deliveries are wired end-to-end. A driver with a stub `raw_xmit` would
  scan deaf-silent with zero probe traffic.
- **Passive channels**: `ieee80211_probe_curchan()` defers probes on
  `IEEE80211_CHAN_PASSIVE` channels until a beacon is heard
  (`IEEE80211_FEXT_PROBECHAN`); beacons still pass through the
  promiscuous filter. So even in the worst case (all channels marked
  passive, e.g. no regdomain/country set), scan still discovers APs from
  beacons — just slower, no probe traffic.
- **Debugging**: `wlandebug -i wlan0 scan+assoc+state` prints every scan
  decision, probe send, and state transition from net80211 — the first
  tool when scan returns empty but attach is clean.

## Module-loaded-but-no-device (the "driver not loaded" class)

- Net effect: `kldstat -m if_bcm4313` shows the module, but
  `net.wlan.devices` is empty → the PCI probe/attach ran and failed
  (e.g. "core reset failed" at boot) or the module silently failed to
  load. dmesg grep for `bcm4313|bhnd` is the evidence; `fb_ensure_driver`
  in lib.sh now prints exactly that block instead of a bare message.
- Boot-time persistence: `loader.conf` `if_bcm4313_load="YES"` (install.sh
  `boot` mode) or `sysrc kld_list+=if_bcm4313` — the sibling port
  narqo/freebsd-brcmfmac (an AI-written FreeBSD port of brcmfmac, same
  pattern as this project) uses the latter; both work on 14/15.
- If the machine rebooted between install.sh and the next command,
  everything above is moot: the module simply never got loaded at boot.

## rfkill / hard-block on 4313 laptops (Linux field data)

- On Linux, "BCM4313 scan finds nothing / no networks" is very frequently
  a hard-blocked radio: Arch/Ubuntu/Mint/Porteus threads (2012–2021) all
  converge on checking `rfkill`, the physical switch, Fn+F2, or BIOS
  first. brcmsmac refuses to TX while the radio is disabled and the
  symptom is an empty scan — identical to a broken RX path from the
  user's point of view.
- FreeBSD net80211 has no rfkill concept, but `MI_RFDISABLE` is now wired
  into `bcm4313_intrtask`. Porting notes, for the record:
  - brcmsmac does **not** treat `MI_RFDISABLE` as fatal — it reports the
    state to mac80211 rfkill (`brcms_rfkill_set_hw_state`), and the state
    itself is read from `phydebug & PDBG_RFD` (`brcms_b_radio_read_hwdisabled`,
    d11.h: `phydebug` 0x158, `PDBG_RFD` 1<<16). `brcms_b_up_prep()` refuses
    to bring the radio up while the bit is set (-ENOMEDIUM → `brcms_c_up()`
    leaves the driver down). The port mirrors all three: the phydebug read,
    an up-time refusal in `bcm4313_init_locked()`, and a stop-the-MAC
    reaction on the blocked edge in the ISR.
  - The ucode debounces the switch before raising the interrupt; the delay
    is programmed via `rfdisabledly` (d11.h 0x3DC) with
    `RFDISABLE_DEFAULT` (10 000 000, ~500 ms on the ALP clock). The port
    writes it in `init_locked()` like brcms_c_init does.
  - What the port deliberately does **not** copy: the 800 ms radio-monitor
    polling timer while the driver is down (mac80211 needs the wakeup to
    auto-bring the interface back; on FreeBSD the user re-ups, matching the
    PSM-watchdog recovery contract).

## More brcmsmac behavior worth porting (audit)

Re-checked against the bundled tree after the MI_RFDISABLE and DMA-FIFO
ports. Upstream runtime entry points: the 1 Hz `brcms_c_watchdog` ->
`brcms_b_watchdog` -> `wlc_phy_watchdog` chain (main.c:4158, phy_cmn.c:2243)
and `brcms_c_dpc` (main.c:7673). Status per item: **done**, **candidate**,
or **skipped** (with why).

- **Done -- `MI_RFDISABLE` hardware rfkill.** Interrupt wired into
  `bcm4313_intrtask`, up-time refusal in `bcm4313_init_locked()`,
  `rfdisabledly` debounce written at MAC start, and the
  `dev.bcm4313.0.rfkill` sysctl. Full details in the rfkill sections above.
- **Done -- per-DMA-ring host-controller error interrupts.**
  `bcm4313_fifo_errors_locked()` polls `intctrlregs[].intstatus` (8
  controllers × 8 bytes from 0x20, d11.h, indexed by FIFO number) for
  `I_PC` (descriptor error), `I_PD` (data error), `I_DE` (descriptor
  protocol error), `I_RO` (RX fifo overflow) and `I_XU` (TX fifo
  underflow) every watchdog tick, exactly like `brcms_b_fifoerrors()`.
  `I_RU` (RX descriptor underflow) stays out of the checked set
  (main.h `I_ERRORS` comment). Only the three controllers this driver owns
  (RX 0, TX-BE 1, TX-status 3) are polled.
  Recovery note (revised after the dmesg flood
  `fifo 0/1/3 × 5 errors → DMA engine error; resetting MAC →
  BCMA_DMP_RESETSTATUS timeout → core reset failed: 60`): upstream
  answers a fatal fifo error with a full stop/start (`brcms_fatal_error`),
  *not* with an inline `init_locked()` from the watchdog. The inline
  reset ran while the MAC could still force bus transactions, so the DMP
  handshake could never complete and the card died. The port now mirrors
  the upstream shape: a fault halts the PSM, drops FLAG_RUNNING and lets
  the user re-up; `stop_locked()` always disarms the engines
  (dma_txreset/dma_rxreset ordering) before the next core reset.
  Additionally, an all-ones register read (core wedged past the bus) is
  now detected in the ISR, the intrtask and the fifo poll — the same
  check as `wlc_intstatus()`'s `macintstatus == 0xffffffff` — and latched
  as FLAG_DEAD: the previous build interpreted that value as "every DMA
  error at once" and reset-looped into a permanent error 60.
  Faults are counted per FIFO in `dev.bcm4313.0.dmaerr`. What hardware
  will confirm: that a real DMA fault on this D11 rev actually latches
  these bits in the shared register block (they are read the same way
  upstream, and they latch independently of the MAC interrupt mask), and
  that a re-up after a fault recovers without a DMP timeout.
- **Done -- LCN periodic temperature-based TX-power recalibration.** The
  1 Hz `bcm4313_watchdog` now calls `bcm4313_lcnphy_watchdog()`
  (if_bcm4313_phy_lcn.c, mirroring the LCN branch of `wlc_phy_watchdog`,
  phy_cmn.c:2243-2300) on every tick while the MAC is up and no DMA/TX
  reset happened that tick. It counts ticks in `sc_lcn.lcnphy_now`
  (upstream `sh->now`); once `BCM4313_LCNPHY_CAL_INTERVAL` (120 s =
  `PHY_SW_TIMER_GLACIAL`, phy_int.h) has passed since the last full
  calibration (`sc_lcn.lcnphy_lastcal`, re-armed by `periodic_cal` and
  `glacial_timer_based_cal` exactly where upstream sets `phy_lastcal`), it
  runs the `TEMPBASED_TXPWRCTRL` write followed by the `PERICAL_WATCHDOG`
  sample every second -- so the 90-sample / +/-60 thresholds already in
  `bcm4313_lcnphy_calib_modes()` now actually fire the glacial IQLO + 2064
  VCO recalibration roughly every ~3.5 min on tempsense boards (LCN rev 1,
  SPROM `tempsense_option` != 3), keeping TX power and IQ balance on
  target as the die warms. Deferred while net80211 scans via the new
  `BCM4313_FLAG_SCAN` (set in `scan_start`, cleared in `scan_end` and
  `stop_locked`), the analog of upstream's SCAN_RM/ASSOC_INPROG early
  return -- there is no separate association window to exclude because
  net80211 does not move the channel between scan_end and S_RUN. Runs
  under the same softc lock as the rest of the watchdog; a strict no-op on
  boards without temperature-based power control (the calib modes
  self-gate on `sc_temppwrctrl_capable`, as upstream's do). **What
  hardware will confirm**: that the ~3.5 min glacial recalibration (a few
  ms of MAC suspend + deaf, same as upstream) is stable under active
  traffic, and that TX power stays flat across a long, warm session on a
  tempsense board. Both are observable live via `dev.bcm4313.0.calticks`
  (climbs 1/s) and `dev.bcm4313.0.calfires` (~1 per 3.5 min) — see the
  INSTALL.md sysctl table.
- **Skipped -- per-second `dma_rxfill` top-up** (`brcms_b_watchdog`,
  main.c:4172). Upstream re-fills the RX ring every second as insurance
  against descriptor-accounting leaks. This port reaps and refills in one
  place on every RX batch (`bcm4313_rx_harvest` -> `bcm4313_rx_refill`), so
  there is no window where RX slots are consumed without replacement.
- **Skipped -- 1 Hz PHY noise sampling** (`PHY_NOISE_SAMPLE_MON`
  -> `MI_BG_NOISE`). Upstream samples the noise floor each second to feed
  mac80211 survey/noise reporting; the DPC's `MI_BG_NOISE` case exists
  only for that. net80211 manages channel noise itself and this driver has
  no survey consumer, so the port leaves `MI_BG_NOISE` unmasked.
- **Skipped -- MAC hardware-stat 16-bit wrap** (`brcms_c_statsupd`, every
  30 s, main.c:4212). Upstream folds the MAC's 16-bit TX/RX counters into
  software before wrap purely to feed mac80211's byte counters; FreeBSD
  counts in the stack, so there is no counter to rescue here.
- **Skipped -- `MI_PWRUP` / `MI_TO` / `MI_TBTT` / `MI_BG_NOISE`.** In brcmsmac these
  serve powersave wake (PWRUP), the gptimer (TO, only with that timer
  armed), beacon-TBTT bookkeeping and PHY noise sampling. This port has no
  powersave (`IEEE80211_C_PS` unset, by design), no gptimer and no
  noise-sample consumer, so masking them in would only add spurious
  interrupt clears. Revisit if powersave is ever added.