# Architecture and developer guide

`if_bcm4313` translates Linux `brcmsmac` D11/LCN behavior into FreeBSD's
`bhnd(4)`, DMA64, and `net80211` driver model. The bundled `brcmsmac/` tree is
the reference for register semantics, microcode initialization, PHY tables, and
recovery ordering.

## Stack and file map

```text
PCI 14e4:4727
  bcm4313_pci             if_bcm4313_pci.c
    bhndb_pci / bhndb     FreeBSD bridge drivers
      bcma_bhndb          BCMA backplane adapter
        bhnd              Backplane bus
          D11 rev 24      if_bcm4313.c
            LCN rev 1     if_bcm4313_phy_lcn.c
              net80211   wlan0
```

| File | Responsibility |
|---|---|
| `if_bcm4313.c` | Attach, D11 reset/PSM, DMA rings, TX/RX, scan, net80211 callbacks, rfkill, diagnostics |
| `if_bcm4313_pci.c` | Claim `14e4:4727` and attach the bhndb PCI bridge |
| `if_bcm4313_phy_lcn.c` | LCN-PHY init, channel programming, tables, calibration, radio 2064 control |
| `if_bcm4313var.h` | Softc, ring structures, register/SHM constants, PHY prototypes |
| `bcm4313_ucode.c/.h` | Embedded D11/LCN microcode and section metadata |
| `bcm4313_lcntab.h` | Generated switch-control and RX-gain tables |
| `bcm4313_phytbl_lcn.h` | Generated remaining LCN tables |
| `brcmsmac/` | Upstream reference sources |
| `firmware/brcm/` | Original Broadcom firmware and license |

The driver matches BCM4313 only, D11 core rev 24, and LCN PHY type 8. It rejects
other chip IDs and PHY types during attach. N-PHY family support is a separate
porting project, not a configuration option.

## Data path

### RX

- RX FIFO 0 uses a DMA64 ring of page-sized mbufs.
- D11 rev >= 16 delivers a 38-byte header region, optional pad bytes, a 6-byte
  PLCP header, the 802.11 frame, and a 4-byte FCS.
- `RC_RO = 38` places frame data at the expected offset.
- The harvest path strips the header region, pad, PLCP, and FCS before calling
  `ieee80211_input()` with the softc lock dropped.
- A-MSDU RX is detected and dropped; it is not yet reconstructed.
- LCN RSSI uses signed JSSI plus the upstream gain-index correction, not the
  N-PHY/HTPHY formula.
- Monitor radiotap fields are refreshed per frame for signal, rate, short
  preamble, and channel.

### TX

- Transmits are queued and kicked under the softc lock.
- TX uses the best-effort FIFO 1. The WME callback programs upstream AC entries,
  but the other AC FIFOs do not have DMA queues in this port.
- TX framing distinguishes CCK, OFDM, and HT; HT is 1x1/HT20.
- Unicast frames request immediate ACK handling, and supported short CCK
  preambles are marked.
- FreeBSD 14 and 15 transmit-rate representations are decoded separately.
  Basic-rate and MCS bits are masked before selecting the PLCP/rate-table entry.
- TX statuses are read from the D11 `frmtxstatus` registers, matched to
  in-flight frame IDs, and fed back to net80211 rate control.
- There is no invented RX DMA engine on TX-status FIFO 3.

### Recovery

- MAC suspend is nested, preserves `MCTL_WAKE`, and waits for
  `MI_MACSSPNDD` with a bounded timeout.
- Timeout or an all-ones core read fails closed and stops the MAC instead of
  issuing resets against a dead device.
- DMA faults and PSM watchdog events stop the interface and require a re-up.
  The next up path disarms DMA before resetting/reinitializing the core.
- The watchdog tracks unfinished TX; it does not reset an idle scanning MAC.

## Management-RX root cause

SHM offsets are **byte offsets**. The D11 object window addresses 32-bit words:

```text
OBJADDR = SHM_SEL | (byte_offset >> 2)
OBJDATA       -> selected word's low 16 bits  when (byte_offset & 2) == 0
OBJDATA + 2   -> selected word's high 16 bits when (byte_offset & 2) != 0
```

The broken helper used `offset >> 1`. In particular:

| SHM field | Byte offset | Correct OBJADDR word | Data window |
|---|---:|---:|---|
| `M_DOT11_SLOT` | `0x10` | `0x04` | `OBJDATA` |
| `M_MAXRXFRM_LEN` | `0x20` | `0x08` | `OBJDATA + 2` |

The old calculation made a write to slot time `0x10` select word `0x08`,
which is maximum RX frame length. Writing slot time 9 or 20 therefore capped
incoming frames at 9 or 20 bytes. That exactly matches the hardware symptom:
short ACK/CF-End control frames crossed DMA, while beacons and probe responses
were rejected as too long.

The corrected helpers:

1. Select the word with `offset >> 2`.
2. Choose `OBJDATA` or `OBJDATA + 2` from bit 1 of the byte offset.
3. Read back `OBJADDR` before accessing data.
4. Read back the data register after an SHM write to flush the posted write.

`M_MAXRXFRM_LEN` is explicitly initialized to `2346`, and `debug >= 2` prints
its readback together with slot time and channel. `debug >= 3` prints scan MAC
counters, including too-long rejects and RX starts.

This fix is source-applied but still awaits hardware confirmation. See
[TESTING.md](TESTING.md#definitive-cold-power-cycle-test).

## Indirect-register ordering

BHND memory I/O is weakly ordered. Accessors now follow the upstream ordering:

- **PHY read:** write `PHYREGADDR`, read it back, then read `PHYREGDATA`.
- **PHY write:** issue one 32-bit write containing address and value.
- **Radio read/write:** flush `RADIOADDR` before touching `RADIODATA`; 2064
  reads use the upstream read-address offset.
- **32-bit radio reads:** read the low word first, then the high word after a
  single address selection.
- **32-bit PHY table reads:** read `TBLDATALO` first and `TBLDATAHI` second,
  storing each half in a separate variable so C operand evaluation order cannot
  reverse the accesses.
- **RCM address programming:** write exactly `RCM_INC_DATA | index`, flush the
  control register, then write address bytes. It is not a read/modify/write that
  preserves unrelated RCM bits.

## PSM and SHM initialization

`bcm4313_psm_start_locked()` follows brcmsmac's core-init shape:

1. Reset and upload the embedded microcode.
2. Release the PSM with `MCTL_IHR_EN`, `MCTL_PSM_RUN`, `MCTL_WAKE`, and the
   appropriate operating-mode bits.
3. Wait for the `MI_MACSSPNDD` self-suspend handshake.
4. Re-apply host SHM/RCM state after firmware init lists have run.

Missing upstream defaults were added:

```text
M_HOST_FLAGS5  = 0
M_MBURST_SIZE  = 8
M_MAX_ANTCNT   = 10
M_MAXRXFRM_LEN = 2346
```

`M_HOST_FLAGS1` preserves the board-dependent force-fast-clock requirement.
Full upstream EDCF enablement is not complete: the port programs per-AC WME
entries, but does not yet apply the complete `MHF1_EDCF`/`IFS_USEEDCF` sequence.
That remains deferred until the RX path is proven and TX can be regression
tested safely.

## Earlier stabilization fixes

These are retained alongside the current RX patch:

- Populate the default 2.4 GHz channel list before `ieee80211_ifattach()`.
- Use firmware section index 13 for the LCN microcode size.
- Release the core with the PHY clock enabled and without asserting PHY reset
  during probe; let bhnd own its clock-control bits.
- Actually start the D11 PSM and wait for self-suspend before declaring the MAC
  ready.
- Set `XtraFrameTypes` and scan filtering so other-BSS beacons and unicast probe
  responses reach net80211. Scanning uses `PROMISC | BCNS_PROMISC` and restores
  the requested filter afterward.
- Stop DMA engines before core reset and drain TX statuses on `MI_TFS`.
- Probe DMA descriptor alignment and program descriptor-table/enable ordering
  as upstream requires.
- Fix RX header/pad/PLCP/FCS layout, LCN RSSI, TX framing, legacy-rate masking,
  transmit-rate decoding, and TX rate-control feedback.
- Wire hardware rfkill and defer periodic PHY recalibration during scans.
- Add per-frame radiotap data and attach-rescan fallback in `install.sh`.
- Treat dead-core reads and suspend timeouts as fatal state rather than reset
  loops.

## WPA tooling

FreeBSD's tested supplicant driver is `-Dbsd`. Inline `ifconfig wpakey` failed
with `EINVAL` on the FreeBSD 15.1 test setup, so `config.sh` generates a
temporary supplicant configuration for inline credentials.

Persistent generation uses `wpa_passphrase`'s complete `network={...}` block
without adding a second wrapper, and removes the commented plaintext `#psk=`
line. Generated files are root-only (`0600`). Boot entries create the VAP over
`bcm43130`, enable `wpa_supplicant`, and request WPA plus DHCP.

## Deliberate limits

- Power save is not advertised or enabled.
- HT40, VHT, multi-stream MCS, and N-PHY are not supported.
- A-MSDU RX is dropped.
- Host-AP/IBSS/monitor capabilities are advertised but not hardware-qualified.
- Only the best-effort TX FIFO has a DMA queue.
- Full EDCF/IFS enablement is pending.
- High channels 12-14 are board/regdomain-dependent; bring up on 1/6/11.
- Firmware and RF tables are board-specific; SPROM values still matter.

## Generated artifacts

Edit the reference source or generator, not generated headers directly:

```sh
perl gen_lcntab.pl
perl gen_phytbl.pl > bcm4313_phytbl_lcn.h
perl gen_ucode.pl
```

Regeneration must be byte-compared with the checked-in artifacts. The microcode
blob itself must remain complete and unmodified under Broadcom's license.

## Porting rules

- Keep the bundled `brcmsmac/` tree as behavioral reference.
- Verify constants and access ordering against upstream before changing them.
- Preserve source attribution and license headers.
- Prefer concrete parity fixes over broad speculative rewrites.
- Record source-level completion separately from hardware verification.

See [LICENSE](LICENSE) and
[firmware/brcm/LICENCE.broadcom_bcm43xx](firmware/brcm/LICENCE.broadcom_bcm43xx)
for redistribution terms.
