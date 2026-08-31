# Architecture & developer guide

How `if_bcm4313` is structured and how it maps to the upstream Linux
`brcmsmac` driver it was ported from.

## Big picture

The FreeBSD driver started from the logic in Linux `brcmsmac` and re-expresses
it for the FreeBSD driver model:

```
            Linux brcmsmac                        this repo
   ----------------------------------------------------------------
   wlc_* MAC/D11 driver            ->  if_bcm4313.c   (net80211 + DMA + bhnd)
   wlc_lcnphy_* LCN-PHY            ->  if_bcm4313_phy_lcn.c
   brcms_ucode data                ->  bcm4313_ucode.c/.h     (embedded)
   phytbl_lcn.c tuning tables      ->  bcm4313_lcntab.h / bcm4313_phytbl_lcn.h
   bcma/bcma bus                   ->  bhnd(4)
   mac80211                        ->  net80211
```

The `brcmsmac/` directory in this repo holds the unmodified upstream reference
sources so a reviewer can diff the port against them.

## File layout

| File | Content |
|------|---------|
| `if_bcm4313.c` | `bcm4313_attach(9)` / detach, DMA64 rings, TX/RX `mbuf` path, net80211 glue (`ic_*` callbacks), microcode upload, SPROM/`bhnd_nvram` reads, `bhnd` device table |
| `if_bcm4313_phy_lcn.c` | The LCN-PHY: `bcm4313_lcnphy_init`, channel set, RX-gain/TX-power tables loading, IQ / tempsense calibration, switch-control write, per-rate txpower |
| `if_bcm4313var.h` | `struct bcm4313_softc`, DMA ring structs, register offsets/bitfields, LCN-PHY prototypes and calibration-mode defines |
| `bcm4313_ucode.c/.h` | Embedded D11/LCN microcode blob + firmware section header (generated) |
| `bcm4313_lcntab.h` | Switch-control + RX-gain tuning tables, only the BCM4313 (LCN rev 1) material (generated) |
| `bcm4313_phytbl_lcn.h` | The rest of the LCN tables (generated, non-overlapping with `lcntab`) |

## Hardware claims the driver makes

- **bhnd device match**: `BHND_MFGID_BCM` + `BHND_COREID_D11` at core rev 24
  (`BCM4313_D11_HWREV`), declared with `BHND_MATCH_CORE_REV(HWREV_EQ(...))`.
- The driver attaches as the `bhnd` child of the D11 core, uploads microcode,
  then registers net80211 capabilities:
  - 1×1, 802.11 **b/g/n**, HT20, MCS 0–7, `SHORTGI20`
  - modes: `11B | 11G | 11NG`, `SHPREAMBLE | SHSLOT | WME | WPA`, SW beacon miss
- Calibration defaults and the `tempsense`/IQ-cal switches are read from SPROM
  at attach.

## RX/TX data path

- RX: DMA ring refilled with `m_getcl()` mbufs via `bus_dmamap_load_mbuf`
  (5-arg `bus_dmamap_callback2_t`); `bcm4313_rx_harvest` converts the hardware
  RX header into net80211 facts and delivers through `ieee80211_find_rxnode` +
  `ieee80211_input` with the softc lock dropped (the bwn(4) pattern, since
  `ieee80211_input` can re-enter the driver).
- TX: `bcm4313_transmit` enqueues onto `sc_snd` under lock and kicks
  `bcm4313_start_locked`; the TX header is built in `bcm4313_set_txhdr`, which
  picks the MCS or legacy PLCP rate depending on whether the current channel
  is HT.
- DMA maps: descriptor rings use `bus_dmamap_load` with a 4-arg
  `bus_dmamap_callback_t`; mbuf TX/RX use the 5-arg callback2 flavor.

## Porting conventions

- Function names keep the `wlc_lcnphy_*` identity but are prefixed
  `bcm4313_lcnphy_*` to avoid collisions.
- References to Linux types are translated: `u8/u16/u32` → FreeBSD `uint8_t/16/32`,
  `bool` → FreeBSD `bool`, `printk` → `device_printf`/`BCM4313_DPRINTF`.
- Dead code from the upstream tree (e.g. helpers only reachable from
  functions the port does not use) is deleted rather than kept dead, so the
  module builds warning-free.
- Any `__FreeBSD_version` gating is documented inline and keyed off
  `sys/param.h` (see [`COMPATIBILITY.md`](COMPATIBILITY.md)).

## Generating the embedded artifacts

The checked-in generated files are produced **byte-for-byte** from the bundled
sources so a fresh clone doesn't need them regenerated, but they can be:

```sh
perl gen_lcntab.pl          # -> bcm4313_lcntab.h      (from brcmsmac/phy/phytbl_lcn.c)
perl gen_phytbl.pl > bcm4313_phytbl_lcn.h              # -> bcm4313_phytbl_lcn.h
perl gen_ucode.pl           # -> bcm4313_ucode.c/.h    (from firmware/brcm/*.fw)
```

- `gen_ucode.pl` embeds `bcm43xx-0.fw` (the blob) and `bcm43xx_hdr-0.fw` (the
  section list) verbatim and emits the arrays + `BCM4313_UCODE_*_SZ` defines.
- `gen_lcntab.pl` / `gen_phytbl.pl` extract the `dot11lcn_*_4313_*` switch
  control and 2.4GHz RX-gain tables straight from `phytbl_lcn.c` / `phy_lcn.c`
  and wrap them in `struct bcm4313_phytbl` descriptors.

## Adding or changing behavior

1. Edit the C sources, not the generated files.
2. If a table/microcode must change, update the reference source under
   `brcmsmac/` or `firmware/brcm/` first, then re-run the matching generator.
3. Rebuild-check against the release you target
   (see [`TESTING.md`](TESTING.md)).