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
   concept. If `list scan` is empty on such a machine while Linux works,
   check the physical switch first. (brcmsmac's `MI_RFDISABLE` interrupt
   exists for this; not yet wired into this port.)
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

## Where to look if bringing up a different Broadcom chip

brcmsmac only ever supported the 4313 family (LCN + BCM4331-class N-PHY in
the same tree). For other chipsets on FreeBSD, `bwn(4)` is the in-tree
alternative — it uses the same bhnd(4) bus this driver attaches to, so the
PCI front-end + DMA64 + net80211 glue patterns transfer.