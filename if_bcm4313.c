/*-
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
 *
 * Dual-licensed per LICENSE: pick BSD-2-Clause or GPL-2.0-or-later for the
 * driver code.  NOTE: this driver embeds ISC-licensed tuning data
 * (bcm4313_lcntab.h, stays ISC) and a Broadcom-owned binary microcode
 * (bcm4313_ucode.c, redistributable only under
 * firmware/brcm/LICENCE.broadcom_bcm43xx).  See LICENSE before distributing.
 *
 * if_bcm4313.c -- FreeBSD driver for the Broadcom BCM4313 SoftMAC
 * 802.11b/g/n PCIe chipset (D11 MAC core rev 24 / LCN PHY rev 1, radio 0x2056).
 *
 * Architecture:
 *  - Attaches to the enumerated D11 (802.11 MAC) core on the bhnd(4)
 *    backplane.  The PCI front-end (if_bcm4313_pci.c) claims the
 *    0x14e4:0x4727 device and instantiates the bhndb(4) bridge chain,
 *    because FreeBSD's own bwn_pci does not list the BCM4313.
 *  - Hardware programming is translated from the Linux brcmsmac driver:
 *    D11 core registers (d11.h), dma64 descriptor engine (dma.h/dma.c),
 *    and LCN-PHY register space (phy/phy_lcn.h).
 *  - Host side uses native FreeBSD interfaces only: net80211
 *    (struct ieee80211com), bus_dma(9) rings, bhnd(4) bus/PMU/clock access.
 *
 * STATUS: bring-up driver for BCM4313 (D11 core rev 24 / LCN PHY).  The
 * D11 register map, dma64 engine and net80211 plumbing are complete and the
 * D11/LCN microcode is embedded (bcm4313_ucode.c) and uploaded at attach.
 * The real BCM4313 LCN-PHY tuning tables (switch-control + RX-gain) are
 * bundled verbatim in bcm4313_lcntab.h and selected from SPROM board flags;
 * the full LCN-PHY TX-power/iqlo/tempsense calibration sequence is ported
 * in if_bcm4313_phy_lcn.c (reference: brcmsmac/phy/phy_lcn.c).
 *
 * $FreeBSD$
 */

#include "opt_bcm4313.h"
#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_crypto.h>
#include <net80211/ieee80211_phy.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ratectl.h>
#include <net80211/ieee80211_regdomain.h>

#include <dev/bhnd/bhnd.h>
#include <dev/bhnd/bhnd_ids.h>
#include "bhnd_nvram_map.h"

#include "if_bcm4313var.h"
#include "bcm4313_lcntab.h"
#include "bcm4313_ucode.h"

MALLOC_DEFINE(M_BCM4313, "bcm4313", "Broadcom BCM4313 driver");

#ifdef BCM4313_DEBUG
static int bcm4313_debug = 0;
SYSCTL_NODE(_hw, OID_AUTO, bcm4313, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Broadcom BCM4313 driver parameters");
SYSCTL_INT(_hw_bcm4313, OID_AUTO, debug, CTLFLAG_RWTUN, &bcm4313_debug, 0,
    "Broadcom BCM4313 debug printfs");
#define	BCM4313_DPRINTF(sc, fmt, ...)	do {				\
	if (bcm4313_debug)						\
		device_printf((sc)->sc_dev, fmt, ##__VA_ARGS__);	\
} while (0)
#else
#define	BCM4313_DPRINTF(sc, fmt, ...)	do { } while (0)
#endif

/*
 * Forward declarations -- several functions are defined after their first
 * use (ring helpers, init/stop, detach).
 */
static void	bcm4313_ring_free(struct bcm4313_ring *);
static void	bcm4313_rx_refill(struct bcm4313_softc *, struct bcm4313_ring *);
static void	bcm4313_set_macaddr(struct bcm4313_softc *, const uint8_t *);
static void	bcm4313_tx_reclaim(struct bcm4313_softc *,
		    struct bcm4313_ring *, int);
static void	bcm4313_init_locked(struct bcm4313_softc *);
static int	bcm4313_psm_start_locked(struct bcm4313_softc *);
static void	bcm4313_sysctl_setup(struct bcm4313_softc *);
static void	bcm4313_stop_locked(struct bcm4313_softc *);
static int	bcm4313_detach(device_t);
static int	bcm4313_ucode_download(struct bcm4313_softc *);

/* Ring sizes. */
#define	BCM4313_NTX_DESC		64	/* 32 frames, 2 slots each */
#define	BCM4313_NRX_DESC		64
/* Max TX statuses drained per pass (upstream TXSBND). */
#define	BCM4313_TXS_MAX			8
/*
 * RX buffer: must hold the 38-byte DMA header region plus a full-size MPDU
 * with PLCP and FCS (brcmsmac RXBUFSZ; 2048 would cut 2346-byte frames).
 * MJUMPAGESIZE clusters (4096 on 4K-page architectures) are used for it.
 */
#define	BCM4313_RXBUFSZ			4096
#define	BCM4313_TXBUFSZ			4096
#define	BCM4313_TX_FRAMES		(BCM4313_NTX_DESC / 2)

/* D11 core I/O control flags (brcmsmac d11.h: SICF_GMODE/SICF_PRST/
 * SICF_PCLKE).  Bits 0x0001 (BHND_IOCTL_CLK_EN) and 0x0002
 * (BHND_IOCTL_CLK_FORCE) are owned exclusively by the bhnd(4) backend --
 * never pass them in a bhnd_reset_hw()/bhnd_write_ioctl() value, or the
 * bcma backend rejects the call with EINVAL. */
#define	BCM4313_IOCTL_SUPPORT_G		0x2000
#define	BCM4313_IOCTL_PHYRESET		0x0008
#define	BCM4313_IOCTL_PHYCLOCK_ENABLE	0x0004

/* RCM (receive match) registers -- used to program MAC/BSSID. */
#define	BCM4313_D11_RCM_CTL		0x420
#define	BCM4313_D11_RCM_MAT_DATA	0x422
#define	BCM4313_D11_RCM_MAT_MASK	0x424
#define	BCM4313_RCM_INC_DATA		0x0020
#define	BCM4313_RCM_INC_MASK_L		0x0040
#define	BCM4313_RCM_INC_MASK_H		0x0080
#define	BCM4313_RCM_INDEX_MASK		0x001f
#define	BCM4313_RCM_MAC_OFFSET		0
#define	BCM4313_RCM_BSSID_OFFSET	3

/*
 * ---------------------------------------------------------------------------
 * Register access -- bhnd(4) maps the D11 core's register window as a
 * plain SYS_RES_MEMORY resource; bus_read_4()/bus_write_4() are the
 * bus_space(9) accessors for it.  (This is the same access model bwn(4)
 * uses.)
 * ---------------------------------------------------------------------------
 */
uint32_t
bcm4313_read_4(struct bcm4313_softc *sc, uint16_t reg)
{
	return (bus_read_4(sc->sc_mem_res, reg));
}

void
bcm4313_write_4(struct bcm4313_softc *sc, uint16_t reg, uint32_t val)
{
	bus_write_4(sc->sc_mem_res, reg, val);
}

uint16_t
bcm4313_read_2(struct bcm4313_softc *sc, uint16_t reg)
{
	return (bus_read_2(sc->sc_mem_res, reg));
}

void
bcm4313_write_2(struct bcm4313_softc *sc, uint16_t reg, uint16_t val)
{
	bus_write_2(sc->sc_mem_res, reg, val);
}

static void
bcm4313_maskset_4(struct bcm4313_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t set)
{
	bcm4313_write_4(sc, reg, (bcm4313_read_4(sc, reg) & mask) | set);
}

/* Force posted writes out; bhnd(4) memory I/O is weakly ordered. */
static void
bcm4313_postwrite_4(struct bcm4313_softc *sc, uint16_t reg)
{
	(void)bcm4313_read_4(sc, reg);
}

/*
 * Shared-memory access: the D11 ucode SHM is a 16-bit word array accessed
 * through the OBJADDR/OBJDATA window (OBJADDR_SHM_SEL).  Offsets are in
 * bytes; the hardware addresses words.
 */
uint16_t
bcm4313_shm_read_2(struct bcm4313_softc *sc, uint16_t offset)
{
	bcm4313_write_4(sc, BCM4313_D11_OBJADDR,
	    BCM4313_OBJADDR_SHM_SEL | (offset >> 1));
	return (bcm4313_read_2(sc, BCM4313_D11_OBJDATA));
}

void
bcm4313_shm_write_2(struct bcm4313_softc *sc, uint16_t offset, uint16_t val)
{
	bcm4313_write_4(sc, BCM4313_D11_OBJADDR,
	    BCM4313_OBJADDR_SHM_SEL | (offset >> 1));
	bcm4313_write_2(sc, BCM4313_D11_OBJDATA, val);
}

/*
 * ---------------------------------------------------------------------------
 * PHY / radio access.
 *
 * LCN-PHY registers are 16-bit and are reached through D11_PHYREGADDR /
 * D11_PHYREGDATA (0x3FC/0x3FE); the 2.4GHz radio through D11_RADIOADDR /
 * D11_RADIODATA (0x3D8/0x3DA).
 * ---------------------------------------------------------------------------
 */
uint16_t
bcm4313_phy_read(struct bcm4313_softc *sc, uint16_t addr)
{
	bcm4313_write_2(sc, BCM4313_D11_PHYREGADDR, addr);
	return (bcm4313_read_2(sc, BCM4313_D11_PHYREGDATA));
}

void
bcm4313_phy_write(struct bcm4313_softc *sc, uint16_t addr, uint16_t val)
{
	bcm4313_write_2(sc, BCM4313_D11_PHYREGADDR, addr);
	bcm4313_write_2(sc, BCM4313_D11_PHYREGDATA, val);
}

void
bcm4313_phy_maskset(struct bcm4313_softc *sc, uint16_t addr, uint16_t mask,
    uint16_t set)
{
	bcm4313_phy_write(sc, addr,
	    (bcm4313_phy_read(sc, addr) & mask) | set);
}

uint16_t
bcm4313_radio_read(struct bcm4313_softc *sc, uint16_t addr)
{
	bcm4313_write_2(sc, BCM4313_D11_RADIOADDR, addr);
	return (bcm4313_read_2(sc, BCM4313_D11_RADIODATA));
}

void
bcm4313_radio_write(struct bcm4313_softc *sc, uint16_t addr, uint16_t val)
{
	bcm4313_write_2(sc, BCM4313_D11_RADIOADDR, addr);
	bcm4313_write_2(sc, BCM4313_D11_RADIODATA, val);
}

void
bcm4313_radio_maskset(struct bcm4313_softc *sc, uint16_t addr, uint16_t mask,
    uint16_t set)
{
	bcm4313_radio_write(sc, addr,
	    (bcm4313_radio_read(sc, addr) & mask) | set);
}

/*
 * 32-bit radio register read (e.g. RFCTL_ID at 0x1000).  The D11 radio
 * window returns the low word on the first data read and the high word on
 * the second, with a single address write (brcmsmac wlc_radio_read32).
 */
uint32_t
bcm4313_radio_read32(struct bcm4313_softc *sc, uint16_t addr)
{
	uint32_t val;

	bcm4313_write_2(sc, BCM4313_D11_RADIOADDR, addr);
	val = (uint32_t)bcm4313_read_2(sc, BCM4313_D11_RADIODATA) << 16;
	val |= bcm4313_read_2(sc, BCM4313_D11_RADIODATA);
	return (val);
}

/*
 * ---------------------------------------------------------------------------
 * dma64 descriptor rings.
 * ---------------------------------------------------------------------------
 */
/*
 * bus_dmamap_load() completion callback (bus_dmamap_callback_t).
 */
static void
bcm4313_dma_addr_cb(void *arg, bus_dma_segment_t *segs, int nsegs,
    int error)
{
	bus_addr_t *pa = arg;

	KASSERT(error == 0 && nsegs == 1, ("%s: error=%u nsegs=%d\n",
	    __func__, (unsigned)error, nsegs));
	*pa = segs[0].ds_addr;
}

/*
 * bus_dmamap_load_mbuf() completion callback (bus_dmamap_callback2_t;
 * the extra arguments carry the map size and error status).
 */
static void
bcm4313_dma_addr_cb2(void *arg, bus_dma_segment_t *segs, int nsegs,
    bus_size_t mapsize, int error)
{
	bus_addr_t *pa = arg;

	KASSERT(error == 0 && nsegs == 1, ("%s: error=%u nsegs=%d\n",
	    __func__, (unsigned)error, nsegs));
	*pa = segs[0].ds_addr;
}

/* Translate a host physical address to a device DMA address. */
static inline bus_addr_t
bcm4313_dma_addr(struct bcm4313_softc *sc, bus_addr_t pa)
{
	return (pa + sc->sc_dma_translation.base_addr);
}

static int
bcm4313_ring_alloc(struct bcm4313_softc *sc, struct bcm4313_ring *ring,
    uint16_t base, int nslots, int istx, uint16_t bufsz, int nframes)
{
	bus_size_t ringsize;
	int error, i;

	memset(ring, 0, sizeof(*ring));
	ring->r_sc = sc;
	ring->r_base = base;
	ring->r_nslots = nslots;
	ring->r_tx = istx;
	ring->r_bufsz = bufsz;
	ring->r_nframes = nframes;

	/* Descriptor ring: 8KB-aligned, single contiguous segment. */
	ringsize = nslots * sizeof(struct bcm4313_dma64desc);
	if ((error = bus_dma_tag_create(sc->sc_dmatag, BCM4313_D64_RINGALIGN,
	    0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL, ringsize,
	    1, ringsize, 0, NULL, NULL, &ring->r_dtag)) != 0) {
		device_printf(sc->sc_dev, "%s: ring dtag: %d\n", __func__,
		    error);
		return (error);
	}
	/* bus_dmamem_alloc() returns the ring KVA directly. */
	if ((error = bus_dmamem_alloc(ring->r_dtag, (void **)&ring->r_desc,
	    BUS_DMA_NOWAIT, &ring->r_dmap)) != 0) {
		device_printf(sc->sc_dev, "%s: dmamem_alloc: %d\n", __func__,
		    error);
		bus_dma_tag_destroy(ring->r_dtag);
		ring->r_dtag = NULL;
		return (error);
	}
	memset(ring->r_desc, 0, ringsize);

	/* Get the physical address of the descriptor ring. */
	if ((error = bus_dmamap_load(ring->r_dtag, ring->r_dmap, ring->r_desc,
	    ringsize, bcm4313_dma_addr_cb, &ring->r_paddr,
	    BUS_DMA_NOWAIT)) != 0) {
		bus_dmamem_free(ring->r_dtag, ring->r_desc, ring->r_dmap);
		bus_dma_tag_destroy(ring->r_dtag);
		ring->r_dtag = NULL;
		ring->r_desc = NULL;
		return (error);
	}

	ring->r_slots = malloc(nslots * sizeof(struct bcm4313_slot),
	    M_BCM4313, M_NOWAIT | M_ZERO);
	if (ring->r_slots == NULL) {
		error = ENOMEM;
		goto fail;
	}
	for (i = 0; i < nslots; i++) {
		if ((error = bus_dmamap_create(sc->sc_bufdtag, 0,
		    &ring->r_slots[i].s_dmap)) != 0)
			goto fail;
	}

	/* TX header cache: one 112-byte header per frame. */
	if (istx && nframes > 0) {
		if ((error = bus_dmamem_alloc(sc->sc_bufdtag,
		    (void **)&ring->r_txhdr, BUS_DMA_NOWAIT,
		    &ring->r_txhdr_dmap)) != 0) {
			device_printf(sc->sc_dev, "%s: txhdr alloc: %d\n",
			    __func__, error);
			goto fail;
		}
		/*
		 * Map the whole cache once; the per-frame header address is
		 * derived from the base (frame i at r_txhdr_pa + i*112).
		 */
		if ((error = bus_dmamap_load(sc->sc_bufdtag,
		    ring->r_txhdr_dmap, ring->r_txhdr,
		    nframes * BCM4313_D11_TXH_LEN, bcm4313_dma_addr_cb,
		    &ring->r_txhdr_pa, BUS_DMA_NOWAIT)) != 0) {
			device_printf(sc->sc_dev, "%s: txhdr map: %d\n",
			    __func__, error);
			goto fail;
		}
	}
	return (0);
fail:
	bcm4313_ring_free(ring);
	return (error);
}

static void
bcm4313_ring_free(struct bcm4313_ring *ring)
{
	struct bcm4313_softc *sc = ring->r_sc;
	struct bcm4313_slot *slot;
	int i;

	if (sc == NULL || ring->r_dtag == NULL)
		return;
	/* Release any posted buffers. */
	if (ring->r_slots != NULL) {
		for (i = 0; i < ring->r_nslots; i++) {
			slot = &ring->r_slots[i];
			if (slot->s_m != NULL) {
				/* TX buffers need a post-write sync. */
				bus_dmamap_sync(sc->sc_bufdtag, slot->s_dmap,
				    ring->r_tx ? BUS_DMASYNC_POSTWRITE :
				    BUS_DMASYNC_POSTREAD);
				bus_dmamap_unload(sc->sc_bufdtag,
				    slot->s_dmap);
				m_freem(slot->s_m);
				slot->s_m = NULL;
			}
			if (slot->s_ni != NULL) {
				ieee80211_free_node(slot->s_ni);
				slot->s_ni = NULL;
			}
			if (slot->s_dmap != NULL) {
				bus_dmamap_destroy(sc->sc_bufdtag,
				    slot->s_dmap);
				slot->s_dmap = NULL;
			}
		}
		free(ring->r_slots, M_BCM4313);
		ring->r_slots = NULL;
	}
	if (ring->r_txhdr != NULL) {
		bus_dmamap_sync(sc->sc_bufdtag, ring->r_txhdr_dmap,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_bufdtag, ring->r_txhdr_dmap);
		bus_dmamem_free(sc->sc_bufdtag, ring->r_txhdr,
		    ring->r_txhdr_dmap);
		ring->r_txhdr = NULL;
	}
	if (ring->r_desc != NULL) {
		bus_dmamem_free(ring->r_dtag, ring->r_desc, ring->r_dmap);
		ring->r_desc = NULL;
	}
	bus_dma_tag_destroy(ring->r_dtag);
	ring->r_dtag = NULL;
	memset(ring, 0, sizeof(*ring));
}

/*
 * Descriptor-alignment probe (brcmsmac _dma_descriptor_align): write 0xff0
 * to ADDRLow and read it back.  If the core keeps the value, the descriptor
 * table can sit anywhere and must be programmed BEFORE the engine is
 * enabled; if the low bits read back as zero, the core requires the
 * 4K/8K-aligned table and the address is written AFTER the enable bit.
 * (Upstream's own dma.c comment: "DMA engine with out alignment requirement
 * requires table to be inited before enabling the engine".)
 */
static bool
bcm4313_ring_align_probe(struct bcm4313_softc *sc, uint16_t base)
{
	bcm4313_write_4(sc, base + BCM4313_DMA64_ADDRLOW, 0xff0);
	return (bcm4313_read_4(sc, base + BCM4313_DMA64_ADDRLOW) == 0);
}

/* Initialize a TX dma64 channel. */
static int
bcm4313_ring_tx_init(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	bus_addr_t pa;
	bool align4k;

	ring->r_in = ring->r_out = 0;
	memset(ring->r_desc, 0, ring->r_nslots *
	    sizeof(struct bcm4313_dma64desc));
	pa = bcm4313_dma_addr(sc, ring->r_paddr);
	align4k = bcm4313_ring_align_probe(sc, ring->r_base);
	/* Descriptor table base (8KB-aligned ring), upstream dma_txinit. */
	if (!align4k) {
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
		    (uint32_t)pa);
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
		    (uint32_t)((uint64_t)pa >> 32));
	}
	/* Enable the channel (parity disabled; not implemented). */
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL,
	    BCM4313_D64_XC_XE | BCM4313_D64_XC_PD);
	if (align4k) {
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
		    (uint32_t)pa);
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
		    (uint32_t)((uint64_t)pa >> 32));
	}
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR, 0);
	return (0);
}

/* Initialize an RX dma64 channel and post the initial buffers. */
static int
bcm4313_ring_rx_init(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	bus_addr_t pa;
	bool align4k;

	ring->r_in = ring->r_out = 0;
	memset(ring->r_desc, 0, ring->r_nslots *
	    sizeof(struct bcm4313_dma64desc));
	pa = bcm4313_dma_addr(sc, ring->r_paddr);
	align4k = bcm4313_ring_align_probe(sc, ring->r_base);
	if (!align4k) {
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
		    (uint32_t)pa);
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
		    (uint32_t)((uint64_t)pa >> 32));
	}
	/*
	 * Enable RX and program the frame offset: the MAC writes the d11
	 * rx header at offset 0 and frame data (PLCP + 802.11 + FCS) at
	 * BCM4313_HWRXOFF -- brcmsmac _dma_rxenable() always programs
	 * (rxoffset << D64_RC_RO_SHIFT) with rxoffset = BRCMS_HWRXOFF(38).
	 * Without RC_RO the frame data lands on top of the rx header and
	 * every harvested frame parses as garbage.
	 */
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL,
	    ((BCM4313_HWRXOFF << BCM4313_D64_RC_RO_SHIFT) &
	    BCM4313_D64_RC_RO_MASK) | BCM4313_D64_RC_RE | BCM4313_D64_RC_PD);
	if (align4k) {
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
		    (uint32_t)pa);
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
		    (uint32_t)((uint64_t)pa >> 32));
	}
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR, 0);
	bcm4313_rx_refill(sc, ring);
	return (0);
}

/* Disable a channel; reclaim posted buffers. */
static void
bcm4313_ring_stop(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	int i;

	/*
	 * Properly disarm the DMA engine (brcmsmac dma_txreset()/dma_rxreset(),
	 * invoked from brcms_b_corereset() before every D11 core reset).
	 * Writing 0 to the control register does NOT stop an engine that is
	 * mid-transaction: its master stays on the backplane, and the DMP core
	 * reset that follows can never handshake -- "BCMA_DMP_RESETSTATUS
	 * timeout", "core reset failed: 60".
	 *
	 * TX: raise the suspend request, wait until the engine leaves the
	 * active states, then clear the enable and wait for DISABLED.  RX:
	 * clear the enable and wait for DISABLED.  Then let the last backplane
	 * transaction drain (300us).  Mirrors brcmsmac exactly, including the
	 * 10k * 1us spinwait bound.
	 */
	if (ring->r_tx) {
		bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL,
		    BCM4313_D64_XC_SE);
		for (i = 0; i < 10000; i++) {
			uint32_t st = bcm4313_read_4(sc,
			    ring->r_base + BCM4313_DMA64_STATUS0);
			st &= BCM4313_D64_XS0_XS_MASK;
			if (st == BCM4313_D64_XS0_XS_DISABLED ||
			    st == BCM4313_D64_XS0_XS_IDLE ||
			    st == BCM4313_D64_XS0_XS_STOPPED)
				break;
			DELAY(1);
		}
	}
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL, 0);
	for (i = 0; i < 10000; i++) {
		uint32_t st = bcm4313_read_4(sc,
		    ring->r_base + BCM4313_DMA64_STATUS0);
		if (ring->r_tx)
			st &= BCM4313_D64_XS0_XS_MASK;
		else
			st &= BCM4313_D64_RS0_RS_MASK;
		if (st == (ring->r_tx ? BCM4313_D64_XS0_XS_DISABLED :
		    BCM4313_D64_RS0_RS_DISABLED))
			break;
		DELAY(1);
	}
	DELAY(300);
	if (ring->r_tx) {
		bcm4313_tx_reclaim(sc, ring, 1);
		return;
	}
	/* RX: free every posted buffer. */
	while (ring->r_in != ring->r_out) {
		i = ring->r_in;
		if (ring->r_slots[i].s_m != NULL) {
			bus_dmamap_sync(sc->sc_bufdtag,
			    ring->r_slots[i].s_dmap, BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->sc_bufdtag,
			    ring->r_slots[i].s_dmap);
			m_freem(ring->r_slots[i].s_m);
			ring->r_slots[i].s_m = NULL;
		}
		ring->r_in = (ring->r_in + 1) % ring->r_nslots;
	}
}

/*
 * Post receive buffers to an RX ring.  Keeps ring->r_nslots - 1 slots
 * posted (the hardware requires one slot of slack).
 */
static void
bcm4313_rx_refill(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	struct bcm4313_dma64desc *desc;
	struct bcm4313_slot *slot;
	struct mbuf *m;
	bus_addr_t pa, da;
	int active, i, n, error;

	active = (ring->r_out - ring->r_in + ring->r_nslots) % ring->r_nslots;
	n = ring->r_nslots - 1 - active;
	for (i = 0; i < n; i++) {
		slot = &ring->r_slots[ring->r_out];
		/*
		 * Page-size jumbo clusters for the data RX ring
		 * (BCM4313_RXBUFSZ = 4096 does not fit MCLBYTES); the
		 * conditional remains for any future small-buffer ring.
		 * Requires a 4K-page architecture, which every bhnd PCIe
		 * host is.
		 */
		if (ring->r_bufsz <= MCLBYTES)
			m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		else
			m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR,
			    MJUMPAGESIZE);
		if (m == NULL) {
			sc->sc_rxnobuf++;
			break;
		}
		if ((error = bus_dmamap_load_mbuf(sc->sc_bufdtag,
		    slot->s_dmap, m, bcm4313_dma_addr_cb2, &pa,
		    BUS_DMA_NOWAIT)) != 0) {
			m_freem(m);
			sc->sc_rxnobuf++;
			break;
		}
		bus_dmamap_sync(sc->sc_bufdtag, slot->s_dmap,
		    BUS_DMASYNC_PREREAD);
		slot->s_m = m;
		da = bcm4313_dma_addr(sc, pa);
		desc = &ring->r_desc[ring->r_out];
		desc->ctrl1 = htole32((ring->r_out == ring->r_nslots - 1) ?
		    BCM4313_D64_CTRL1_EOT : 0);
		desc->ctrl2 = htole32(ring->r_bufsz & BCM4313_D64_CTRL2_BC_MASK);
		desc->addrlow = htole32((uint32_t)da);
		desc->addrhigh = htole32((uint32_t)((uint64_t)da >> 32));
		ring->r_out = (ring->r_out + 1) % ring->r_nslots;
	}
	bus_dmamap_sync(ring->r_dtag, ring->r_dmap, BUS_DMASYNC_PREWRITE);
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR,
	    ring->r_out * sizeof(struct bcm4313_dma64desc));
}

/*
 * Reclaim completed TX descriptors.  force=1 reclaims everything.
 */
static void
bcm4313_tx_reclaim(struct bcm4313_softc *sc, struct bcm4313_ring *ring,
    int force)
{
	uint32_t status0;
	uint32_t xstate;
	int i, end;

	if (ring->r_in == ring->r_out)
		return;
	status0 = bcm4313_read_4(sc, ring->r_base + BCM4313_DMA64_STATUS0);
	xstate = status0 & BCM4313_D64_XS0_XS_MASK;
	if (force || xstate == BCM4313_D64_XS0_XS_IDLE ||
	    xstate == BCM4313_D64_XS0_XS_STOPPED)
		end = ring->r_out;
	else
		end = (status0 & BCM4313_D64_XS0_CD_MASK) /
		    sizeof(struct bcm4313_dma64desc);

	while (ring->r_in != end) {
		i = ring->r_in;
		if (ring->r_slots[i].s_type == BCM4313_SLOT_BODY &&
		    ring->r_slots[i].s_m != NULL) {
			bus_dmamap_sync(sc->sc_bufdtag,
			    ring->r_slots[i].s_dmap, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_bufdtag,
			    ring->r_slots[i].s_dmap);
			m_freem(ring->r_slots[i].s_m);
			if (ring->r_slots[i].s_ni != NULL) {
				ieee80211_free_node(ring->r_slots[i].s_ni);
				ring->r_slots[i].s_ni = NULL;
			}
			ring->r_slots[i].s_m = NULL;
			sc->sc_txreclaimed++;
		}
		ring->r_in = (ring->r_in + 1) % ring->r_nslots;
	}
	sc->sc_last_reclaim = ticks;
}

/*
 * LCN-PHY packet RSSI gain-index correction (brcmsmac phy_cmn.c
 * lcnphy_gain_index_offset_for_pkt_rssi, byte-for-byte).  Indexed by the
 * RX gain index the ucode reports in PhyRxStatus_2 bits 15:10.
 */
static const int8_t bcm4313_lcn_gidx_pkt_rssi[] = {
	8, 8, 8, 8, 8, 8, 8, 9, 10, 8, 8, 7, 7, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	1, 1, 0, 0, 0, 0
};

/*
 * Harvest received frames from an RX ring and feed them to net80211.
 *
 * Buffer layout (brcmsmac brcms_c_recv/brcms_c_recvctl, see
 * if_bcm4313var.h): [0,38) d11 rx header region, [+2 pad if RXS_PBPRES],
 * [+6 PLCP] [802.11 frame] [+4 FCS]; RxFrameSize counts everything after
 * the header region.
 */
static void
bcm4313_rx_harvest(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct bcm4313_d11rxhdr *rxh;
	struct bcm4313_slot *slot;
	struct mbuf *m;
	uint32_t hw;
	uint16_t rxlen, macrxs1, macrxs2, prxs1, prxs2;
	int gidx, rssi;
	int i;

	hw = (bcm4313_read_4(sc, ring->r_base + BCM4313_DMA64_STATUS0) &
	    BCM4313_D64_RS0_CD_MASK) / sizeof(struct bcm4313_dma64desc);
	while (ring->r_in != (int)hw) {
		i = ring->r_in;
		slot = &ring->r_slots[i];
		m = slot->s_m;
		slot->s_m = NULL;
		if (m != NULL) {
			bus_dmamap_sync(sc->sc_bufdtag, slot->s_dmap,
			    BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->sc_bufdtag, slot->s_dmap);
			rxh = mtod(m, struct bcm4313_d11rxhdr *);
			rxlen = le16toh(rxh->RxFrameSize);
			macrxs1 = le16toh(rxh->RxStatus1);
			macrxs2 = le16toh(rxh->RxStatus2);
			prxs1 = le16toh(rxh->PhyRxStatus_1);
			prxs2 = le16toh(rxh->PhyRxStatus_2);
			if (rxlen < BCM4313_D11_PHY_HDR_LEN +
			    IEEE80211_MIN_LEN + BCM4313_FCS_LEN ||
			    rxlen > ring->r_bufsz - BCM4313_HWRXOFF ||
			    (macrxs1 & BCM4313_RXS_FCSERR) != 0) {
				/* Giant / short / bad-FCS: drop. */
				if (rxlen > ring->r_bufsz - BCM4313_HWRXOFF)
					sc->sc_rxgiants++;
				m_freem(m);
				counter_u64_add(ic->ic_ierrors, 1);
				goto next;
			}
			/* A-MSDU reassembly is not supported (upstream toss). */
			if ((macrxs2 & BCM4313_RXS_AMSDU) != 0) {
				sc->sc_rxamsdu++;
				m_freem(m);
				counter_u64_add(ic->ic_ierrors, 1);
				goto next;
			}
			/* Frame length after the DMA header region; set both
			 * lengths directly (the cluster holds the whole
			 * frame), then strip the layers: rx header region,
			 * QoS pad, PLCP, FCS. */
			m->m_pkthdr.len = m->m_len =
			    BCM4313_HWRXOFF + rxlen;
			m_adj(m, BCM4313_HWRXOFF);
			if ((macrxs1 & BCM4313_RXS_PBPRES) != 0) {
				/* QoS/A-MSDU/a4 header pad (2 bytes). */
				if (m->m_pkthdr.len < 2 + IEEE80211_MIN_LEN) {
					m_freem(m);
					counter_u64_add(ic->ic_ierrors, 1);
					goto next;
				}
				m_adj(m, 2);
				sc->sc_rxpad++;
			}
			m_adj(m, BCM4313_D11_PHY_HDR_LEN);
			m_adj(m, -BCM4313_FCS_LEN);
			/*
			 * RSSI per brcmsmac wlc_phy_rssi_compute() LCNPHY
			 * branch: JSSI from PhyRxStatus_1 (signed), corrected
			 * by the gain index from PhyRxStatus_2 and the slope
			 * constant (lcnphy_pkteng_rssi_slope is always 0
			 * here).  Without a valid PHY rx status the reading
			 * is meaningless (upstream reports RSSI_INVALID).
			 */
			if ((macrxs2 & BCM4313_RXS_PHYRXST_VALID) == 0) {
				sc->sc_rssiinvalid++;
				rssi = -128;
			} else {
				gidx = (prxs2 & BCM4313_PRXS2_GIDX_MASK) >>
				    BCM4313_PRXS2_GIDX_SHIFT;
				if (gidx >= (int)nitems(bcm4313_lcn_gidx_pkt_rssi))
					gidx = nitems(bcm4313_lcn_gidx_pkt_rssi) - 1;
				rssi = prxs1 & BCM4313_PRXS1_JSSI_MASK;
				if (rssi > 127)
					rssi -= 256;
				rssi += bcm4313_lcn_gidx_pkt_rssi[gidx];
				if (rssi > -46 && gidx > 18)
					rssi += 7;
				rssi += 2;
			}
			sc->sc_rxframes++;
			/*
			 * Deliver to net80211 without the softc lock held:
			 * ieee80211_input() may call back into the driver
			 * (parent, newstate, transmit) which takes the same
			 * mutex (the bwn(4) pattern).
			 */
			wh = mtod(m, struct ieee80211_frame *);
			BCM4313_UNLOCK(sc);
			ni = ieee80211_find_rxnode(ic,
			    (const struct ieee80211_frame_min *)wh);
			if (ni != NULL) {
				ieee80211_input(ni, m, rssi, -95);
				ieee80211_free_node(ni);
			} else
				ieee80211_input_all(ic, m, rssi, -95);
			BCM4313_LOCK(sc);
		}
next:
		ring->r_in = (ring->r_in + 1) % ring->r_nslots;
	}
	bcm4313_rx_refill(sc, ring);
}

/*
 * Drain completed TX statuses from the frmtxstatus/frmtxstatus2 register
 * pair (brcmsmac brcms_b_txstatus: no DMA ring is used for TX statuses --
 * the ucode posts them through these registers).  Each valid status is
 * matched against the in-flight TX ring by frame id and reported to
 * net80211 rate control (the mapping follows brcms_c_dotxstatus()).
 */
static void
bcm4313_txstatus_drain(struct bcm4313_softc *sc)
{
	struct ieee80211_ratectl_tx_status txs;
	struct bcm4313_slot *tslot;
	struct bcm4313_ring *tx = &sc->sc_tx;
	uint32_t s1, s2;
	uint16_t fid, status;
	int j, n, retries;

	for (n = 0; n < BCM4313_TXS_MAX; n++) {
		s1 = bcm4313_read_4(sc, BCM4313_D11_FRMTXSTATUS);
		if (!bcm4313_intstatus_valid(s1)) {
			device_printf(sc->sc_dev,
			    "txstatus: core not responding; MAC stopped, "
			    "re-up to recover\n");
			sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
			bcm4313_write_4(sc, BCM4313_D11_MACCONTROL,
			    bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
			    ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN));
			return;
		}
		if ((s1 & BCM4313_TXS_V) == 0)
			break;			/* no more statuses */
		s2 = bcm4313_read_4(sc, BCM4313_D11_FRMTXSTATUS2);
		status = s1 & BCM4313_TXS_STATUS_MASK;
		fid = (s1 & BCM4313_TXS_FID_MASK) >> BCM4313_TXS_FID_SHIFT;
		if ((status & BCM4313_TX_STATUS_INTERMEDIATE) != 0 &&
		    (status & BCM4313_TX_STATUS_AMPDU) == 0) {
			/*
			 * Partial indication (brcms_c_dotxstatus:
			 * "INTERMEDIATE but not AMPDU", e.g. after a
			 * successful RTS exchange); the frame is completed
			 * by a later status.
			 */
			continue;
		}
		/*
		 * Find the in-flight frame.  Statuses arrive in FIFO order,
		 * so the oldest body slot with a matching frame id is the
		 * one; the scan is bounded in case a status lost its frame
		 * (already reclaimed).
		 */
		tslot = NULL;
		for (j = tx->r_in; j != tx->r_out; j = (j + 1) % tx->r_nslots) {
			if (tx->r_slots[j].s_type == BCM4313_SLOT_BODY &&
			    tx->r_slots[j].s_ni != NULL &&
			    tx->r_slots[j].s_frameid == fid) {
				tslot = &tx->r_slots[j];
				break;
			}
		}
		if (tslot != NULL) {
			memset(&txs, 0, sizeof(txs));
			if (tslot->s_m != NULL) {
				txs.pktlen = tslot->s_m->m_pkthdr.len;
				txs.flags |= IEEE80211_RATECTL_STATUS_PKTLEN;
			}
			if ((status & BCM4313_TX_STATUS_ACK_RCV) != 0)
				txs.status = IEEE80211_RATECTL_TX_SUCCESS;
			else if ((status & BCM4313_TX_STATUS_SUPR_MASK) ==
			    BCM4313_TX_STATUS_SUPR_EXPTIME)
				txs.status = IEEE80211_RATECTL_TX_FAIL_EXPIRED;
			else if ((status & BCM4313_TX_STATUS_SUPR_MASK) != 0)
				txs.status =
				    IEEE80211_RATECTL_TX_FAIL_UNSPECIFIED;
			else {
				txs.status = IEEE80211_RATECTL_TX_FAIL_LONG;
				retries = (status &
				    BCM4313_TX_STATUS_FRM_RTX_MASK) >>
				    BCM4313_TX_STATUS_FRM_RTX_SHIFT;
				txs.flags |=
				    IEEE80211_RATECTL_STATUS_LONG_RETRY;
				txs.long_retries = (retries > 0) ? retries : 1;
			}
			ieee80211_ratectl_tx_complete(tslot->s_ni, &txs);
			ieee80211_free_node(tslot->s_ni);
			tslot->s_ni = NULL;
		} else
			sc->sc_txnomatch++;
		sc->sc_txdone++;
	}
}

/*
 * ---------------------------------------------------------------------------
 * TX path.
 * ---------------------------------------------------------------------------
 */
/* CCK/OFDM PLCP rate codes (b43/bwn conventions). */
static uint16_t
bcm4313_plcp_rate(uint8_t rate)	/* rate in 0.5 Mbps units */
{
	switch (rate) {
	case 2:	return (0x0a);	/* 1 */
	case 4:	return (0x14);	/* 2 */
	case 11:return (0x37);	/* 5.5 */
	case 22:return (0x6e);	/* 11 */
	case 12:return (0x0b);	/* 6 */
	case 18:return (0x0f);	/* 9 */
	case 24:return (0x0a);	/* 12 */
	case 36:return (0x0e);	/* 18 */
	case 48:return (0x09);	/* 24 */
	case 72:return (0x0d);	/* 36 */
	case 96:return (0x08);	/* 48 */
	case 108:return (0x0c);	/* 54 */
	}
	return (0x0b);		/* 6 Mbps fallback */
}

/*
 * Build the 112-byte D11 TX DMA header (struct d11txh).  Field semantics
 * follow brcmsmac brcms_c_sendpkt()/brcms_c_d11hdrs_machdr(); the rate
 * encodings were verified against brcms_c_compute_plcp() (legacy = PLCP
 * signal byte / OFDM rate code, HT = MCS index) and frametype() (CCK=0,
 * OFDM=1, HT=2 in the PhyTxControlWord low bits).
 *
 * The node rate is decoded per net80211 conventions: on FreeBSD >= 15
 * ni_txrate.type says HT vs legacy (ni_txrate.mcs / ni_txrate.dot11rate);
 * on 14 ni_txrate is the rateset value with IEEE80211_RATE_MCS(0x80) set
 * for MCS and the basic-rate bit (same bit) in legacy values.  Rate-based
 * selection (not channel-based) so management frames sent at legacy rates
 * on an HT channel still get CCK/OFDM framing.
 */
static void
bcm4313_set_txhdr(struct bcm4313_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, struct bcm4313_d11txh *txh)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	uint16_t phyctl;
	uint8_t mcs, dot11rate;
	bool is_mcs;

	wh = mtod(m, struct ieee80211_frame *);
	memset(txh, 0, BCM4313_D11_TXH_LEN);
	txh->MacTxControlLow = htole16(BCM4313_TXC_STARTMSDU);
	if ((wh->i_addr1[0] & 1) == 0)
		txh->MacTxControlLow |= htole16(BCM4313_TXC_IMMEDACK);
	txh->MacFrameControl = htole16(wh->i_fc[0] | (wh->i_fc[1] << 8));
	memcpy(txh->TxFrameRA, wh->i_addr1, IEEE80211_ADDR_LEN);
	txh->TxFrameID = htole16(sc->sc_frameid++);
#if __FreeBSD_version >= 1500000
	is_mcs = (ni->ni_txrate.type == IEEE80211_NODE_TXRATE_HT);
	mcs = min(ni->ni_txrate.mcs, 15);
	dot11rate = IEEE80211_RV(ni->ni_txrate.dot11rate);
#else
	/* MCS flag and rate share the 0x80 bit (IEEE80211_RATE_VAL = 0x7f). */
	is_mcs = (ni->ni_txrate & IEEE80211_RATE_MCS) != 0;
	mcs = min(ni->ni_txrate & IEEE80211_RATE_VAL, 15);
	dot11rate = ni->ni_txrate & IEEE80211_RATE_VAL;
#endif
	/*
	 * Framing type in the low bits (brcmsmac frametype()): the ucode
	 * builds the PHY header from it.  The LCN antenna field stays at
	 * ANT_LAST (0).
	 */
	phyctl = BCM4313_PHY_TXC_LCNPHY_ANT_LAST;
	if (is_mcs) {
		/* HT: FT_HT, 1x1, 20MHz, MCS index in MainRates. */
		phyctl |= BCM4313_TXFTYPE_HT;
		txh->MainRates = htole16(mcs);
		txh->PhyTxControlWord_1 = htole16(BCM4313_PHY_TXC1_BW_20MHZ |
		    (BCM4313_PHY_TXC1_MODE_SISO <<
		    BCM4313_PHY_TXC1_MODE_SHIFT));
	} else {
		/* Legacy: PLCP rate code in MainRates. */
		txh->MainRates = htole16(bcm4313_plcp_rate(dot11rate));
		if (BCM4313_IS_CCK_RATE(dot11rate)) {
			/*
			 * Short CCK preamble when both ends support it
			 * (brcmsmac PHY_TXC_SHORT_HDR); 1Mb/s stays long.
			 */
			if (dot11rate != 2 &&
			    (ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_PREAMBLE) &&
			    (ic->ic_flags & IEEE80211_F_SHPREAMBLE))
				phyctl |= BCM4313_PHY_TXC_SHORT_HDR;
		} else
			phyctl |= BCM4313_TXFTYPE_OFDM;
	}
	txh->PhyTxControlWord = htole16(phyctl);
}

/*
 * Post one frame to the TX ring as two descriptors: [SOF] 112-byte D11 TX
 * header, then [EOF|IOC] frame body.  Returns ENOBUFS when the ring is
 * full; the frame is not consumed in that case.
 */
static int
bcm4313_tx_start_locked(struct bcm4313_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m)
{
	struct bcm4313_ring *ring = &sc->sc_tx;
	struct bcm4313_dma64desc *descA, *descB;
	struct bcm4313_d11txh *txh;
	bus_dma_segment_t segs[1];
	bus_addr_t hpa, dpa;
	int active, error, slotA, slotB, nsegs, fi;

	if (m->m_pkthdr.len < IEEE80211_MIN_LEN) {
		m_freem(m);
		return (EINVAL);
	}
	active = (ring->r_out - ring->r_in + ring->r_nslots) % ring->r_nslots;
	if (active >= ring->r_nslots - 2)
		return (ENOBUFS);

	slotA = ring->r_out;
	slotB = (slotA + 1) % ring->r_nslots;
	if (ring->r_slots[slotB].s_m != NULL) {
		sc->sc_txnobuf++;
		return (ENOBUFS);
	}

	/* Build the TX header in the (pre-mapped) consistent cache. */
	fi = (slotA >> 1) % ring->r_nframes;
	txh = (struct bcm4313_d11txh *)
	    (ring->r_txhdr + fi * BCM4313_D11_TXH_LEN);
	bcm4313_set_txhdr(sc, ni, m, txh);
	bus_dmamap_sync(sc->sc_bufdtag, ring->r_txhdr_dmap,
	    BUS_DMASYNC_PREWRITE);
	hpa = ring->r_txhdr_pa + fi * BCM4313_D11_TXH_LEN;
	/* The ucode echoes TxFrameID back in the TX status; keep it. */
	ring->r_slots[slotB].s_frameid = le16toh(txh->TxFrameID);

	/* Map the frame body. */
	error = bus_dmamap_load_mbuf_sg(sc->sc_bufdtag,
	    ring->r_slots[slotB].s_dmap, m, segs, &nsegs, BUS_DMA_NOWAIT);
	if (error == EFBIG || (error == 0 && nsegs != 1)) {
		bus_dmamap_unload(sc->sc_bufdtag,
		    ring->r_slots[slotB].s_dmap);
		m = m_defrag(m, M_NOWAIT);
		if (m == NULL) {
			/* m_defrag() freed the original chain. */
			return (EIO);
		}
		if ((error = bus_dmamap_load_mbuf_sg(sc->sc_bufdtag,
		    ring->r_slots[slotB].s_dmap, m, segs, &nsegs,
		    BUS_DMA_NOWAIT)) != 0) {
			m_freem(m);	/* defragged chain */
			return (EIO);
		}
	}
	if (error != 0 || nsegs != 1) {
		/* Original chain; map was not loaded. */
		m_freem(m);
		return (EIO);
	}
	bus_dmamap_sync(sc->sc_bufdtag, ring->r_slots[slotB].s_dmap,
	    BUS_DMASYNC_PREWRITE);

	/* Fill descriptors. */
	dpa = bcm4313_dma_addr(sc, segs[0].ds_addr);
	descA = &ring->r_desc[slotA];
	descA->ctrl1 = htole32(BCM4313_D64_CTRL1_SOF |
	    (slotA == ring->r_nslots - 1 ? BCM4313_D64_CTRL1_EOT : 0));
	descA->ctrl2 = htole32(BCM4313_D11_TXH_LEN);
	descA->addrlow = htole32((uint32_t)bcm4313_dma_addr(sc, hpa));
	descA->addrhigh = htole32((uint32_t)((uint64_t)
	    bcm4313_dma_addr(sc, hpa) >> 32));
	descB = &ring->r_desc[slotB];
	descB->ctrl1 = htole32(BCM4313_D64_CTRL1_EOF |
	    BCM4313_D64_CTRL1_IOC |
	    (slotB == ring->r_nslots - 1 ? BCM4313_D64_CTRL1_EOT : 0));
	descB->ctrl2 = htole32(m->m_pkthdr.len & BCM4313_D64_CTRL2_BC_MASK);
	descB->addrlow = htole32((uint32_t)dpa);
	descB->addrhigh = htole32((uint32_t)((uint64_t)dpa >> 32));

	ring->r_slots[slotA].s_type = BCM4313_SLOT_HEADER;
	ring->r_slots[slotA].s_m = NULL;
	ring->r_slots[slotA].s_ni = NULL;
	ring->r_slots[slotB].s_type = BCM4313_SLOT_BODY;
	ring->r_slots[slotB].s_m = m;
	ring->r_slots[slotB].s_ni = ni;

	bus_dmamap_sync(ring->r_dtag, ring->r_dmap, BUS_DMASYNC_PREWRITE);
	ring->r_out = (slotB + 1) % ring->r_nslots;
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR,
	    ring->r_out * sizeof(struct bcm4313_dma64desc));
	sc->sc_watchdog_timer = 5;
	sc->sc_txframes++;
	return (0);
}

/* Dequeue and transmit queued frames. */
static void
bcm4313_start_locked(struct bcm4313_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ieee80211_key *k;
	struct ieee80211_frame *wh;
	struct mbuf *m;
	int error;

	BCM4313_ASSERT_LOCKED(sc);
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0)
		return;
	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		if ((sc->sc_tx.r_out - sc->sc_tx.r_in + sc->sc_tx.r_nslots) %
		    sc->sc_tx.r_nslots >= sc->sc_tx.r_nslots - 2) {
			mbufq_prepend(&sc->sc_snd, m);
			break;
		}
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		if (ni == NULL) {
			m_freem(m);
			counter_u64_add(ic->ic_oerrors, 1);
			continue;
		}
		wh = mtod(m, struct ieee80211_frame *);
		if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
			k = ieee80211_crypto_encap(ni, m);
			if (k == NULL) {
				ieee80211_free_node(ni);
				m_freem(m);
				counter_u64_add(ic->ic_oerrors, 1);
				continue;
			}
		}
		error = bcm4313_tx_start_locked(sc, ni, m);
		if (error == ENOBUFS) {
			/* Ring filled up; requeue and stop. */
			mbufq_prepend(&sc->sc_snd, m);
			break;
		}
		if (error != 0) {
			/* tx_start_locked() already consumed/freed m. */
			ieee80211_free_node(ni);
			counter_u64_add(ic->ic_oerrors, 1);
		}
	}
}

/*
 * net80211 delivers the destination node attached to the mbuf
 * (m_pkthdr.rcvif) on both FreeBSD 14 and 15; ic_transmit stays
 * (ic, m) on both.
 */
static int
bcm4313_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	struct bcm4313_softc *sc = ic->ic_softc;
	int error;

	error = 0;
	BCM4313_LOCK(sc);
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0) {
		BCM4313_UNLOCK(sc);
		return (ENXIO);
	}
	error = mbufq_enqueue(&sc->sc_snd, m);
	if (error == 0)
		bcm4313_start_locked(sc);
	BCM4313_UNLOCK(sc);
	return (error);
}

/* Raw frame transmit (management frames, monitor mode). */
static int
bcm4313_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params)
{
	struct bcm4313_softc *sc = ni->ni_ic->ic_softc;
	int error;

	if (m->m_pkthdr.len < IEEE80211_MIN_LEN) {
		m_freem(m);
		return (EINVAL);
	}
	BCM4313_LOCK(sc);
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0) {
		BCM4313_UNLOCK(sc);
		m_freem(m);
		return (ENXIO);
	}
	error = bcm4313_tx_start_locked(sc, ni, m);
	BCM4313_UNLOCK(sc);
	if (error != 0) {
		/* ENOBUFS leaves m unconsumed; other errors free it. */
		if (error == ENOBUFS)
			m_freem(m);
		return (error);
	}
	return (0);
}

/*
 * ---------------------------------------------------------------------------
 * D11 microcode upload (brcmsmac ucode_loader.c / main.c).
 *
 * The LCN microcode for BCM4313 (D11 rev 24) is the section of
 * brcm/bcm43xx-0.fw tagged D11UCODE_OVERSIGHT24_LCN (idx 12); its length in
 * bytes lives in the section tagged D11UCODE_OVERSIGHT24_LCNSZ (idx 13).
 * It is uploaded through the OBJADDR microcode window:
 *   objaddr = OBJADDR_AUTO_INC | OBJADDR_UCM_SEL;
 *   for each LE u32: objdata = word;
 * The band-selective and MAC init tweak lists (tags 1 and 2) are then
 * dumped through brcms_c_write_inits() (register writes of 16 or 32 bits).
 * ---------------------------------------------------------------------------
 */

static int
bcm4313_ucode_find_section(uint32_t want, uint32_t *off, uint32_t *len)
{
	const struct bcm4313_fw_hdr *f;
	uint32_t nsec = BCM4313_UCODE_HDR_SZ / sizeof(struct bcm4313_fw_hdr);
	uint32_t i;

	for (i = 0; i < nsec; i++) {
		f = (const struct bcm4313_fw_hdr *)
		    (bcm4313_ucode_hdr + i * sizeof(*f));
		if (le32toh(f->idx) != want)
			continue;
		*off = le32toh(f->offset);
		*len = le32toh(f->len);
		return (0);
	}
	return (ENOENT);
}

/* Dump one d11init list (brcms_c_write_inits). */
static void
bcm4313_dump_inits(struct bcm4313_softc *sc, const unsigned char *list,
    uint32_t nbytes)
{
	const struct bcm4313_d11init *d = (const struct bcm4313_d11init *)list;
	uint32_t i = 0;
	uint16_t addr, size;
	uint32_t value;

	while (i + sizeof(struct bcm4313_d11init) <= nbytes) {
		addr = le16toh(d[i].addr);
		size = le16toh(d[i].size);
		value = le32toh(d[i].value);
		if (addr == 0xffff)
			break;
		if (size == 2)
			bcm4313_write_2(sc, addr, (uint16_t)value);
		else if (size == 4)
			bcm4313_write_4(sc, addr, value);
		i++;
	}
	bcm4313_postwrite_4(sc, BCM4313_D11_MACCONTROL);
}

static int
bcm4313_ucode_download(struct bcm4313_softc *sc)
{
	uint32_t off, len, offsz, lensz, lcnsz;
	uint32_t i, nwords;

	if (sc->sc_ucode_loaded)
		return (0);

	if (bcm4313_ucode_find_section(BCM4313_D11UCODE_OVERSIGHT24_LCN,
	    &off, &len) != 0) {
		device_printf(sc->sc_dev, "firmware: no LCN ucode section\n");
		return (ENOENT);
	}
	if (off > BCM4313_UCODE_BIN_SZ || len > BCM4313_UCODE_BIN_SZ - off) {
		device_printf(sc->sc_dev, "firmware: bad LCN ucode bounds\n");
		return (EIO);
	}
	if (bcm4313_ucode_find_section(BCM4313_D11UCODE_OVERSIGHT24_LCNSZ,
	    &offsz, &lensz) != 0 || lensz != 4 ||
	    offsz + 4 > BCM4313_UCODE_BIN_SZ) {
		device_printf(sc->sc_dev, "firmware: missing LCN ucode size\n");
		return (ENOENT);
	}
	/* The tagged size is a BYTE count (brcms_ucode_init_uint -> nbytes). */
	lcnsz = le32toh(*(const uint32_t *)(bcm4313_ucode_bin + offsz));
	if (lcnsz == 0 || lcnsz > len || (lcnsz % 4) != 0) {
		device_printf(sc->sc_dev, "firmware: invalid LCN ucode size %u\n",
		    lcnsz);
		return (EIO);
	}
	nwords = lcnsz / 4;

	BCM4313_DPRINTF(sc, "uploading LCN ucode: %u bytes at %#x\n", lcnsz,
	    off);

	/* reset PSM and select the microcode window */
	bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL, ~0,
	    BCM4313_MCTL_PSM_JMP_0 | BCM4313_MCTL_SHM_EN);
	bcm4313_write_4(sc, BCM4313_D11_OBJADDR,
	    BCM4313_OBJADDR_AUTO_INC | BCM4313_OBJADDR_UCM_SEL);
	(void)bcm4313_read_4(sc, BCM4313_D11_OBJADDR);
	for (i = 0; i < nwords; i++)
		bcm4313_write_4(sc, BCM4313_D11_OBJDATA,
		    le32toh(*(const uint32_t *)(bcm4313_ucode_bin + off +
		    i * 4)));
	bcm4313_postwrite_4(sc, BCM4313_D11_OBJADDR);

	/* MAC init tweak lists (band-selective then general). */
	if (bcm4313_ucode_find_section(BCM4313_D11LCN0BSINITVALS24,
	    &off, &len) == 0 && off + len <= BCM4313_UCODE_BIN_SZ)
		bcm4313_dump_inits(sc, bcm4313_ucode_bin + off, len);
	if (bcm4313_ucode_find_section(BCM4313_D11LCN0INITVALS24,
	    &off, &len) == 0 && off + len <= BCM4313_UCODE_BIN_SZ)
		bcm4313_dump_inits(sc, bcm4313_ucode_bin + off, len);

	sc->sc_ucode_loaded = 1;
	device_printf(sc->sc_dev, "D11 LCN ucode uploaded (%u words)\n",
	    nwords);
	return (0);
}

/*
 * ---------------------------------------------------------------------------
 * LCN-PHY.
 *
 * The full LCN-PHY implementation (init, channel set, TX power control,
 * IQ/TSSI/tempsense calibration) lives in if_bcm4313_phy_lcn.c, ported
 * from brcmsmac phy/phy_lcn.c; the entry points below are declared in
 * if_bcm4313var.h.  The board-specific BCM4313 switch-control and RX-gain
 * tables are bundled in bcm4313_lcntab.h and the LCN tuning tables in
 * bcm4313_phytbl_lcn.h (both extracted byte-for-byte from brcmsmac
 * phytbl_lcn.c).
 * ---------------------------------------------------------------------------
 */

/*
 * ---------------------------------------------------------------------------
 * MAC control.
 * ---------------------------------------------------------------------------
 */

/*
 * Start the D11 PSM (microcode) and wait for it to reach its auto-init
 * suspended state -- brcmsmac brcms_b_coreinit():
 *
 *	reset PSM state  mctrl  = MCTL_IHR_EN | MCTL_PSM_JMP_0 | MCTL_WAKE
 *	download ucode   bcm4313_ucode_download()
 *	run the PSM      mctrl  = MCTL_IHR_EN | [MCTL_GMODE | MCTL_INFRA |]
 *	                          MCTL_PSM_RUN | MCTL_WAKE
 *	wait             MI_MACSSPNDD (ucode self-suspend handshake)
 *
 * Each bit is load-bearing and none of them were set before:
 *
 *  - MCTL_IHR_EN  opens the indirect-host-register window the ucode and
 *                 the SHM/OBJADDR accesses rely on.
 *  - MCTL_WAKE    keeps the PSM from dozing off the host's clock request.
 *  - MCTL_PSM_RUN releases the PSM.  Without it the microcode stays parked
 *                 at MCTL_PSM_JMP_0 = 0 forever: the MAC reads as enabled
 *                 in net80211 and does absolutely nothing -- no beacons, no
 *                 TX, no RX -- which is the "interface is up and the
 *                 network doesn't work" symptom, followed by a PSM
 *                 watchdog `ifconfig up` that wedges the chip.
 *  - MI_MACSSPNDD is the only positive proof the ucode is alive and has
 *                 taken the PSM; brcmsmac spins 1s for it.
 *
 * A core that returns 0xffffffff from every register has fallen off the
 * backplane; resetting it again only produces the timeout loop, so that is
 * reported instead of retried.
 *
 * Returns 0 once the ucode is suspended and ready for SHM configuration.
 */
static int
bcm4313_psm_start_locked(struct bcm4313_softc *sc)
{
	uint32_t mc, status = 0;
	int i;

	BCM4313_ASSERT_LOCKED(sc);

	/* Drop stale status bits before the PSM is released. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, 0xffffffff);

	/* Hold the PSM at 0 with the host register window open. */
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL,
	    BCM4313_MCTL_IHR_EN | BCM4313_MCTL_PSM_JMP_0 | BCM4313_MCTL_WAKE);

	/*
	 * The core was just reset, so the microcode RAM must be reloaded
	 * (brcms_b_coreinit() downloads after the reset too).  Forcing the
	 * flag keeps this correct even when attach() already uploaded once.
	 */
	sc->sc_ucode_loaded = 0;
	if (bcm4313_ucode_download(sc) != 0)
		return (EIO);

	/* Release the PSM: BSS STA mode, running, woken. */
	mc = BCM4313_MCTL_IHR_EN | BCM4313_MCTL_GMODE | BCM4313_MCTL_PSM_RUN |
	    BCM4313_MCTL_WAKE;
	if (sc->sc_ic.ic_opmode == IEEE80211_M_STA)
		mc |= BCM4313_MCTL_INFRA;
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);

	/* Wait for the ucode to power up and park itself (brcmsmac: 1s). */
	for (i = 0; i < 100000; i++) {
		status = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS);
		if (status == 0xffffffff) {
			device_printf(sc->sc_dev,
			    "D11 core is not responding (macintstatus "
			    "0xffffffff) -- reset cannot help, power-cycle\n");
			return (EIO);
		}
		if (status & BCM4313_MI_MACSSPNDD) {
			/*
			 * The ucode applied its own initvals (register and SHM
			 * locations) on the way up.  Re-apply the driver's view of
			 * the SHM window on top, so those word writes cannot clobber
			 * MAXRXFRM_LEN/SLOT/CHANNEL or the RCM MAC address.
			 */
			bcm4313_shm_write_2(sc, BCM4313_M_MACHW_VER,
			    BCM4313_D11_HWREV);
			bcm4313_shm_write_2(sc, BCM4313_M_MAXRXFRM_LEN, 2346);
			bcm4313_shm_write_2(sc, BCM4313_M_DOT11_SLOT,
			    (sc->sc_ic.ic_flags & IEEE80211_F_SHSLOT) ? 9 : 20);
			bcm4313_shm_write_2(sc, BCM4313_M_CURCHANNEL,
			    sc->sc_curchan);
			bcm4313_set_macaddr(sc, sc->sc_ic.ic_macaddr);
			return (0);
		}
		DELAY(10);
	}
	device_printf(sc->sc_dev,
	    "ucode did not self-suspend (macintstatus %#x maccontrol %#x)\n",
	    status, bcm4313_read_4(sc, BCM4313_D11_MACCONTROL));
	return (EIO);
}

/*
 * Bring the MAC alive (brcmsmac brcms_c_enable_mac()):
 *
 *	1. make sure the D11 PSM is actually executing.  MCTL_PSM_RUN == 0
 *	   means the core was just reset (or the MAC was taken down), so the
 *	   microcode has to be reloaded and released before anything the MAC
 *	   does can work.  This is the one place every bring-up path passes
 *	   through -- brcmsmac does the same work in brcms_b_coreinit().
 *	2. set MCTL_EN_MAC on top of the running PSM without disturbing
 *	   MCTL_IHR_EN / MCTL_WAKE / MCTL_PSM_RUN.
 *	3. clear the ucode's MI_MACSSPNDD parked indication.
 *
 * If the PSM cannot be started the MAC is deliberately left stopped: a D11
 * with no running ucode silently discards everything, and failing here is
 * what stops the watchdog from resetting a chip that is already wedged.
 */
void
bcm4313_mac_enable(struct bcm4313_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t mc;

	mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
	if ((mc & BCM4313_MCTL_PSM_RUN) == 0) {
		if (bcm4313_psm_start_locked(sc) != 0) {
			BCM4313_DPRINTF(sc, "PSM not running; MAC left off\n");
			return;
		}
		mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
	}

	mc |= BCM4313_MCTL_EN_MAC | BCM4313_MCTL_IHR_EN | BCM4313_MCTL_WAKE |
	    BCM4313_MCTL_GMODE | BCM4313_MCTL_PSM_RUN;
	mc &= ~BCM4313_MCTL_PSM_JMP_0;
	if (ic->ic_opmode == IEEE80211_M_STA)
		mc |= BCM4313_MCTL_INFRA;
	else
		mc &= ~BCM4313_MCTL_INFRA;
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);

	/* Clear the ucode's parked indication now that the MAC is enabled. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, BCM4313_MI_MACSSPNDD);
}

void
bcm4313_mac_disable(struct bcm4313_softc *sc)
{
	uint32_t mc;

	mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
	mc &= ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN);
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);
}

/*
 * Program the current BSSID into RCM entry 3 (RCM_BSSID_OFFSET).
 * Entry access follows the d11.h RCM interface (brcmsmac
 * brcms_b_mac_bssid_write()); verify the index select on hardware.
 */
static void
bcm4313_set_bssid(struct bcm4313_softc *sc, const uint8_t *bssid)
{
	uint16_t ctl;
	int i;

	ctl = bcm4313_read_2(sc, BCM4313_D11_RCM_CTL);
	ctl &= ~BCM4313_RCM_INDEX_MASK;
	ctl |= BCM4313_RCM_BSSID_OFFSET | BCM4313_RCM_INC_DATA;
	bcm4313_write_2(sc, BCM4313_D11_RCM_CTL, ctl);
	for (i = 0; i < 6; i += 2)
		bcm4313_write_2(sc, BCM4313_D11_RCM_MAT_DATA,
		    bssid[i] | (bssid[i + 1] << 8));
}

/* Program the station MAC address into RCM entry 0 (RCM_MAC_OFFSET). */
static void
bcm4313_set_macaddr(struct bcm4313_softc *sc, const uint8_t *mac)
{
	uint16_t ctl;
	int i;

	ctl = bcm4313_read_2(sc, BCM4313_D11_RCM_CTL);
	ctl &= ~BCM4313_RCM_INDEX_MASK;
	ctl |= BCM4313_RCM_MAC_OFFSET | BCM4313_RCM_INC_DATA;
	bcm4313_write_2(sc, BCM4313_D11_RCM_CTL, ctl);
	for (i = 0; i < 6; i += 2)
		bcm4313_write_2(sc, BCM4313_D11_RCM_MAT_DATA,
		    mac[i] | (mac[i + 1] << 8));
	/* Match all 6 bytes. */
	ctl &= ~BCM4313_RCM_INDEX_MASK;
	ctl |= BCM4313_RCM_MAC_OFFSET | BCM4313_RCM_INC_MASK_L |
	    BCM4313_RCM_INC_MASK_H;
	bcm4313_write_2(sc, BCM4313_D11_RCM_CTL, ctl);
	for (i = 0; i < 6; i += 2)
		bcm4313_write_2(sc, BCM4313_D11_RCM_MAT_MASK, 0xffff);
}

static void
bcm4313_set_slot(struct bcm4313_softc *sc, uint16_t slot)
{
	bcm4313_shm_write_2(sc, BCM4313_M_DOT11_SLOT, slot);
}

/*
 * ---------------------------------------------------------------------------
 * net80211 glue.
 * ---------------------------------------------------------------------------
 */
/* Build the 2.4GHz channel list (b/g/ng, HT20).  bwn fills its channel
 * table before ieee80211_ifattach(); net80211's ieee80211_chan_init()
 * reads ic_channels/ic_nchans immediately and panics in
 * ieee80211_get_ratetable() ("no rate table for channel; freq 0
 * flags 0x0") if they are not populated yet -- the KASSERT guards
 * are compiled out on release kernels. */
static void
bcm4313_add_channels(struct ieee80211_channel *chans, int maxchans,
    int *nchans)
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	setbit(bands, IEEE80211_MODE_11NG);
	/* Default 2.4GHz list (channels 1-14).  NOTE: passing a NULL
	 * channel list to ieee80211_add_channel_list_2ghz() adds NOTHING
	 * (add_chanlist iterates the supplied table); the default-2ghz
	 * helper is what supplies the standard list.  HT20 comes from
	 * MODE_11NG; cbw_flags=0 keeps HT40 out. */
	ieee80211_add_channels_default_2ghz(chans, maxchans, nchans,
	    bands, 0);
}

static void
bcm4313_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
    struct ieee80211_channel chans[])
{
	bcm4313_add_channels(chans, maxchans, nchans);
}

static void
bcm4313_scan_start(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;

	BCM4313_LOCK(sc);
	/* Defer periodic PHY recalibration while net80211 scans. */
	sc->sc_flags |= BCM4313_FLAG_SCAN;
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) != 0) {
		/*
		 * During a scan the MAC must pass beacons from any BSSID
		 * (not just the RCM-filtered one) up to net80211.  Same
		 * bit bwn sets in bwn_scan_start() and brcmsmac raises
		 * for FIF_BCN_PRBRESP_PROMISC (MCTL_BCNS_PROMISC).
		 */
		bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL, ~0,
		    BCM4313_MCTL_BCNS_PROMISC);
	}
	BCM4313_UNLOCK(sc);
	BCM4313_DPRINTF(sc, "scan start\n");
	if (sc->sc_debug > 0)
		device_printf(sc->sc_dev, "scan start\n");
}

static void
bcm4313_scan_end(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;

	BCM4313_LOCK(sc);
	sc->sc_flags &= ~BCM4313_FLAG_SCAN;
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) != 0) {
		bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL,
		    ~BCM4313_MCTL_BCNS_PROMISC, 0);
	}
	BCM4313_UNLOCK(sc);
	BCM4313_DPRINTF(sc, "scan end\n");
	if (sc->sc_debug > 0)
		device_printf(sc->sc_dev, "scan end\n");
}

static void
bcm4313_update_promisc(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;
	uint32_t bits;

	BCM4313_LOCK(sc);
	/*
	 * MCTL_KEEPCONTROL alongside promisc so monitor mode captures
	 * control frames (brcms_c_mac_promisc: FIF_CONTROL ->
	 * MCTL_KEEPCONTROL).  MCTL_KEEPBADFCS stays clear -- bad-FCS frames
	 * are dropped in the RX path either way.
	 */
	if (ic->ic_promisc > 0)
		bits = BCM4313_MCTL_PROMISC | BCM4313_MCTL_KEEPCONTROL;
	else
		bits = 0;
	bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL,
	    ~(BCM4313_MCTL_PROMISC | BCM4313_MCTL_KEEPCONTROL), bits);
	BCM4313_UNLOCK(sc);
}

static void
bcm4313_set_channel(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;
	uint8_t chan;

	chan = (uint8_t)ieee80211_chan2ieee(ic, ic->ic_curchan);
	BCM4313_LOCK(sc);
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0) {
		BCM4313_UNLOCK(sc);
		return;
	}
	sc->sc_curchan = chan;
	bcm4313_shm_write_2(sc, BCM4313_M_CURCHANNEL, chan);
	bcm4313_lcnphy_set_chanspec(sc, chan);
	BCM4313_UNLOCK(sc);
	if (sc->sc_debug >= 2)
		device_printf(sc->sc_dev, "chan %u\n", chan);
}

static void
bcm4313_parent(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;
	int startall = 0;

	BCM4313_LOCK(sc);
	if (ic->ic_nrunning > 0) {
		if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0) {
			bcm4313_init_locked(sc);
			startall = 1;
		} else {
			/* ic_update_promisc() takes the lock itself. */
			bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL,
			    ~BCM4313_MCTL_PROMISC,
			    ic->ic_promisc > 0 ? BCM4313_MCTL_PROMISC : 0);
		}
	} else if (sc->sc_flags & BCM4313_FLAG_RUNNING) {
		bcm4313_stop_locked(sc);
	}
	BCM4313_UNLOCK(sc);
	if (startall)
		ieee80211_start_all(ic);
}

/*
 * Per-VAP state machine entry point (net80211 calls the saved default
 * handler; we then program the hardware for the resulting state).
 */
static int
bcm4313_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate,
    int arg)
{
	struct ieee80211com *ic = vap->iv_ic;
	struct bcm4313_softc *sc = ic->ic_softc;
	struct bcm4313_vap *bav = BCM4313_VAP(vap);
	int error;

	error = bav->bv_newstate(vap, nstate, arg);
	if (error != 0)
		return (error);

	BCM4313_LOCK(sc);
	switch (nstate) {
	case IEEE80211_S_INIT:
		bcm4313_mac_disable(sc);
		break;
	case IEEE80211_S_RUN:
		if (vap->iv_bss != NULL)
			bcm4313_set_bssid(sc, vap->iv_bss->ni_bssid);
		bcm4313_set_slot(sc,
		    (ic->ic_flags & IEEE80211_F_SHSLOT) ? 9 : 20);
		bcm4313_mac_enable(sc);
		break;
	default:
		break;
	}
	BCM4313_UNLOCK(sc);
	return (0);
}

static struct ieee80211vap *
bcm4313_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ],
    int unit, enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct bcm4313_softc *sc = ic->ic_softc;
	struct bcm4313_vap *bav;
	struct ieee80211vap *vap;
	int error;

	if (opmode != IEEE80211_M_STA && opmode != IEEE80211_M_IBSS &&
	    opmode != IEEE80211_M_MONITOR) {
		device_printf(sc->sc_dev, "%s: unsupported opmode %d\n",
		    __func__, opmode);
		return (NULL);
	}
	bav = malloc(sizeof(*bav), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &bav->bv_vap;
	error = ieee80211_vap_setup(ic, vap, name, unit, opmode, flags, bssid);
	if (error != 0) {
		free(bav, M_80211_VAP);
		return (NULL);
	}
	/* Wrap the default state handler. */
	bav->bv_newstate = vap->iv_newstate;
	vap->iv_newstate = bcm4313_newstate;
	/* Program our MAC address into the RCM. */
	BCM4313_LOCK(sc);
	bcm4313_set_macaddr(sc, mac);
	BCM4313_UNLOCK(sc);
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	return (vap);
}

static void
bcm4313_vap_delete(struct ieee80211vap *vap)
{
	struct bcm4313_vap *bav = BCM4313_VAP(vap);

	ieee80211_vap_detach(vap);
	free(bav, M_80211_VAP);
}

/*
 * ---------------------------------------------------------------------------
 * Interrupts.
 * ---------------------------------------------------------------------------
 */
/*
 * The core answers every read with 0xffffffff once it is wedged past the
 * bus (clock-gated out of reset reach, dead card).  Masked against the
 * interrupt mask that value still looks like "every interrupt", and each
 * handler then services those phantom bits, trips the fifo poll, and
 * orders the very core reset that can never handshake -- the
 * "DMA fifo N: ...; DMA engine error; resetting MAC; BCMA_DMP_RESETSTATUS
 * timeout; core reset failed: 60" dmesg flood.  Mirror brcmsmac's
 * wlc_intstatus(): treat an all-ones macintstatus as "no interrupt".
 */
static __inline int
bcm4313_intstatus_valid(uint32_t is)
{
	return (is != 0xffffffff);
}

static int
bcm4313_intr(void *arg)
{
	struct bcm4313_softc *sc = arg;
	uint32_t reason;

	reason = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS);
	if (!bcm4313_intstatus_valid(reason))
		return (FILTER_STRAY);
	reason &= sc->sc_intr_mask;
	if (reason == 0)
		return (FILTER_STRAY);
	/* Clear by writing back the read value. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, reason);
	taskqueue_enqueue(sc->sc_tq, &sc->sc_intrtask);
	return (FILTER_HANDLED);
}

static void
bcm4313_intrtask(void *arg, int pending)
{
	struct bcm4313_softc *sc = arg;
	uint32_t reason;

	BCM4313_LOCK(sc);
	reason = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS);
	if (!bcm4313_intstatus_valid(reason))
		reason = 0;
	reason &= sc->sc_intr_mask;
	if (reason == 0) {
		BCM4313_UNLOCK(sc);
		return;
	}
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, reason);
	if (reason & BCM4313_MI_DMAINT) {
		bcm4313_tx_reclaim(sc, &sc->sc_tx, 0);
		bcm4313_rx_harvest(sc, &sc->sc_rx);
		bcm4313_txstatus_drain(sc);
		sc->sc_watchdog_timer = 5;
	}
	if (reason & (BCM4313_MI_MACTXERR | BCM4313_MI_PHYTXERR))
		counter_u64_add(sc->sc_ic.ic_oerrors, 1);
	if (reason & BCM4313_MI_TFS) {
		/*
		 * TX statuses pending in the frmtxstatus register pair: drain
		 * them so the ucode can post completions for further frames.
		 * Without this the status path stalls and the watchdog is
		 * forced to reset the MAC.  (brcmsmac: MI_TFS ->
		 * brcms_b_txstatus().)
		 */
		bcm4313_txstatus_drain(sc);
		bcm4313_tx_reclaim(sc, &sc->sc_tx, 0);
		sc->sc_watchdog_timer = 5;
	}
	if (reason & BCM4313_MI_GP0) {
		sc->sc_wdog_fires++;
		/*
		 * D11 PSM microcode watchdog; brcmsmac treats MI_GP0 as fatal
		 * (brcms_fatal_error -> full restart).  Deliberately do NOT feed
		 * the watchdog callout: a fresh wedge on every tick would grind
		 * the core with resets (and DMP handshake timeouts) until the
		 * card is unrecoverable.  Halt the MAC, drop FLAG_RUNNING so
		 * transmit() rejects frames, and unmask GP0; the next
		 * init_locked() (via `ifconfig wlan0 up`) restores the mask and
		 * retries cleanly.
		 */
		device_printf(sc->sc_dev,
		    "PSM microcode watchdog fired; MAC stopped, re-up to recover\n");
		counter_u64_add(sc->sc_ic.ic_oerrors, 1);
		bcm4313_write_4(sc, BCM4313_D11_MACCONTROL,
		    bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
		    ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN));
		sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
		sc->sc_intr_mask &= ~BCM4313_MI_GP0;
		bcm4313_write_4(sc, BCM4313_D11_MACINTMASK, sc->sc_intr_mask);
	}
	if (reason & BCM4313_MI_RFDISABLE) {
		bool blocked;

		/*
		 * Hardware RF kill switch changed state (edge interrupt,
		 * debounced by the rfdisable-delay timer).  net80211 has no
		 * rfkill(4) equivalent, so mirror brcmsmac's reaction as far as
		 * the model allows: remember the state for dev.bcm4313.0.rfkill,
		 * and while blocked stop the MAC and drop FLAG_RUNNING so
		 * transmit() rejects frames -- an empty scan on a 4313 laptop is
		 * usually this switch, not a dead RX path.  Keep MI_RFDISABLE
		 * unmasked so the re-enable edge is caught too.
		 * (brcmsmac: MI_RFDISABLE -> brcms_rfkill_set_hw_state() ->
		 * phydebug & PDBG_RFD.)
		 */
		blocked = (bcm4313_read_4(sc, BCM4313_D11_PHYDEBUG) &
		    BCM4313_PDBG_RFD) != 0;
		if (blocked && !sc->sc_radio_off) {
			device_printf(sc->sc_dev,
			    "hardware radio disabled (RF kill switch); MAC "
			    "stopped -- flip the switch, then re-up the "
			    "interface (ifconfig wlan down/up)\n");
			if (sc->sc_flags & BCM4313_FLAG_RUNNING) {
				bcm4313_write_4(sc, BCM4313_D11_MACCONTROL,
				    bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
				    ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN));
				sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
			}
		} else if (!blocked && sc->sc_radio_off) {
			device_printf(sc->sc_dev,
			    "hardware radio enabled; re-up the interface to "
			    "reconnect\n");
		}
		sc->sc_radio_off = blocked ? 1 : 0;
	}
	BCM4313_UNLOCK(sc);
}

/*
 * Poll the per-DMA-controller error bits once per second, like brcmsmac's
 * brcms_b_fifoerrors() (called from its watchdog).  Every D11 DMA engine
 * has an intstatus register in the core's shared intctrlregs block (d11.h:
 * intctrlregs[8] at 0x20, indexed by FIFO number); the error bits latch
 * whether or not they are enabled in the MAC interrupt path, so polling is
 * sufficient.  Returns -1 when the core stopped answering reads (all-ones
 * register; the MAC has been halted and no register but MACCONTROL is
 * touched), >0 when a controller has faulted (latched bits cleared and
 * counted; recovery is a full stop/disarm + re-init), 0 when healthy.
 */
static int
bcm4313_fifo_errors_locked(struct bcm4313_softc *sc)
{
	static const uint8_t fifos[] = {
	    BCM4313_RX_FIFO,		/* 0: RX data */
	    BCM4313_TX_BE_FIFO		/* 1: TX data */
	};
	uint32_t intstatus;
	int fatal, i;

	BCM4313_ASSERT_LOCKED(sc);
	fatal = 0;
	for (i = 0; i < nitems(fifos); i++) {
		intstatus = bcm4313_read_4(sc,
		    BCM4313_D11_DMA_INTSTATUS(fifos[i]));
		/*
		 * All-ones means the core is not answering reads anymore
		 * (wedged past the bus / clock-gated / dead); per-controller
		 * "fifo %d" diagnostics would be fiction at this point, and
		 * the reset ordered on their behalf can never handshake.
		 */
		if (!bcm4313_intstatus_valid(intstatus)) {
			device_printf(sc->sc_dev, "DMA core not responding "
			    "(intstatus %#x macintstatus %#x maccontrol %#x); "
			    "MAC stopped, re-up to recover\n", intstatus,
			    bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS),
			    bcm4313_read_4(sc, BCM4313_D11_MACCONTROL));
			sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
			bcm4313_write_4(sc, BCM4313_D11_MACCONTROL,
			    bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
			    ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN));
			return (-1);
		}
		intstatus &= BCM4313_DMA_I_ERRORS;
		if (intstatus == 0)
			continue;
		if (intstatus & BCM4313_DMA_I_RO)
			device_printf(sc->sc_dev,
			    "DMA fifo %d: receive fifo overflow\n", fifos[i]);
		if (intstatus & BCM4313_DMA_I_PC)
			device_printf(sc->sc_dev,
			    "DMA fifo %d: descriptor error\n", fifos[i]);
		if (intstatus & BCM4313_DMA_I_PD)
			device_printf(sc->sc_dev,
			    "DMA fifo %d: data error\n", fifos[i]);
		if (intstatus & BCM4313_DMA_I_DE)
			device_printf(sc->sc_dev,
			    "DMA fifo %d: descriptor protocol error\n",
			    fifos[i]);
		if (intstatus & BCM4313_DMA_I_XU)
			device_printf(sc->sc_dev,
			    "DMA fifo %d: transmit fifo underflow\n", fifos[i]);
		/*
		 * Clear the latched bits so a fault that survives the recovery
		 * below cannot re-trigger it on every tick.
		 */
		bcm4313_write_4(sc, BCM4313_D11_DMA_INTSTATUS(fifos[i]),
		    intstatus);
		sc->sc_dmaerr[fifos[i]]++;
		sc->sc_dmaerr_fifo = fifos[i];
		sc->sc_dmaerr_bits = intstatus;
		fatal = 1;
	}
	return (fatal);
}

static void
bcm4313_watchdog(void *arg)
{
	struct bcm4313_softc *sc = arg;
	uint32_t mc;
	int reset;

	BCM4313_LOCK(sc);
	if (sc->sc_flags & BCM4313_FLAG_RUNNING) {
		/*
		 * Check the DMA engines first, before the TX watchdog timer
		 * (brcmsmac: brcms_b_watchdog() -> brcms_b_fifoerrors()).
		 */
		reset = bcm4313_fifo_errors_locked(sc);
		if (reset < 0) {
			/*
			 * Core stopped answering reads: fifo_errors_locked()
			 * has already stopped the MAC.  Do NOT touch any other
			 * register and do not reset here -- a reset ordered
			 * while the core is in this state can never handshake
			 * ("BCMA_DMP_RESETSTATUS timeout", "core reset failed:
			 * 60").  The interface re-up performs the clean
			 * stop -> reset -> init dance.
			 */
			sc->sc_wdog_fires++;
			counter_u64_add(sc->sc_ic.ic_oerrors, 1);
		} else if (reset > 0) {
			/*
			 * A DMA engine faulted.  brcmsmac answers this with a
			 * full stop/start (brcms_fatal_error).  Here the core is
			 * still alive (per-controller status reads sanity-checked
			 * above), but a MAC-forced bus transaction in flight
			 * during the core reset is what historically turned
			 * recovery into "BCMA_DMP_RESETSTATUS timeout" + "core
			 * reset failed: 60" and killed the card.  Halt the PSM,
			 * drop RUNNING so transmit() rejects frames, and let the
			 * user re-up: stop_locked() then disarms every DMA engine
			 * (dma_txreset/dma_rxreset sequence) before the core
			 * reset -- the same ordering brcms_b_corereset() enforces.
			 */
			sc->sc_wdog_fires++;
			counter_u64_add(sc->sc_ic.ic_oerrors, 1);
			device_printf(sc->sc_dev,
			    "DMA engine error; MAC stopped, re-up to recover\n");
			mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
			mc &= ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN);
			mc |= BCM4313_MCTL_PSM_JMP_0;
			bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);
			sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
		} else if (--sc->sc_watchdog_timer <= 0) {
			/*
			 * No frame and no TX completion for 5s while the
			 * interface is up.  Recovery faces the same constraint
			 * as the DMA-fault case above: disarm the engines via a
			 * full stop (next up) instead of resetting the core
			 * under a possibly-live MAC.
			 */
			sc->sc_wdog_fires++;
			counter_u64_add(sc->sc_ic.ic_oerrors, 1);
			device_printf(sc->sc_dev,
			    "watchdog timeout: MAC stopped, re-up to recover\n");
			mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
			mc &= ~(BCM4313_MCTL_EN_MAC | BCM4313_MCTL_PSM_RUN);
			mc |= BCM4313_MCTL_PSM_JMP_0;
			bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);
			sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
		} else {
			/*
			 * Periodic LCN temperature-based TX-power recalibration
			 * (brcmsmac brcms_b_watchdog() -> wlc_phy_watchdog());
			 * runs only on healthy ticks.
			 */
			bcm4313_lcnphy_watchdog(sc);
		}
	}
	callout_schedule(&sc->sc_watchdog_ch, hz);
	BCM4313_UNLOCK(sc);
}

/*
 * ---------------------------------------------------------------------------
 * Device bring-up / teardown.
 * ---------------------------------------------------------------------------
 */
static void
bcm4313_stop_locked(struct bcm4313_softc *sc)
{
	struct ieee80211_node *ni;
	struct mbuf *m;

	BCM4313_ASSERT_LOCKED(sc);
	/*
	 * Always run the teardown below, even when !RUNNING: the watchdog
	 * drops RUNNING *before* the user re-ups, and the up path that
	 * follows must still disarm the DMA engines (suspend -> disable ->
	 * DISABLED, then the 300us drain) before it resets the core.
	 * Skipping the teardown in that state would leave engines live
	 * across bhnd_reset_hw() -- masters stuck on the backplane,
	 * "BCMA_DMP_RESETSTATUS timeout", "core reset failed: 60".
	 * brcmsmac does the same: brcms_b_corereset() runs dma_txreset/
	 * dma_rxreset whenever the core is enabled, RUNNING or not.
	 */
	sc->sc_flags &= ~(BCM4313_FLAG_RUNNING | BCM4313_FLAG_SCAN);
	callout_stop(&sc->sc_watchdog_ch);

	/* Mask and clear interrupts; disable the MAC. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTMASK, 0);
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, 0xffffffff);
	bcm4313_mac_disable(sc);

	/* Stop DMA and reclaim state. */
	bcm4313_ring_stop(sc, &sc->sc_tx);
	bcm4313_ring_stop(sc, &sc->sc_rx);

	/* Drain the transmit queue. */
	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		if (ni != NULL)
			ieee80211_free_node(ni);
		m_freem(m);
	}
}

static void
bcm4313_init_locked(struct bcm4313_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t ctl;
	uint16_t ioctl, ioctl_mask;
	int error;

	BCM4313_ASSERT_LOCKED(sc);
	if (sc->sc_flags & BCM4313_FLAG_DEAD) {
		/*
		 * Latched dead-core state: the core stopped answering reads
		 * and nothing short of a driver re-load (new probe, fresh
		 * core state) will revive it.  Refuse here so the recovery
		 * reset -- which can never handshake in this state -- is not
		 * re-ordered on every re-up.
		 */
		device_printf(sc->sc_dev, "core is dead; reload the driver "
		    "(or power-cycle) to recover\n");
		return;
	}
	if (sc->sc_flags & BCM4313_FLAG_RUNNING)
		bcm4313_stop_locked(sc);

	/* Reset the D11 core (PHY held in reset during the dance). */
	ioctl = BCM4313_IOCTL_PHYRESET | BCM4313_IOCTL_PHYCLOCK_ENABLE |
	    BCM4313_IOCTL_SUPPORT_G;
	if ((error = bhnd_reset_hw(sc->sc_dev, ioctl, ioctl)) != 0) {
		device_printf(sc->sc_dev, "core reset failed: %d\n", error);
		/*
		 * A failed handshake means masters are still holding the
		 * backplane; every further attempt would only repeat the
		 * timeout.  Latch the dead state and refuse future attempts.
		 */
		sc->sc_flags |= BCM4313_FLAG_DEAD;
		return;
	}
	/*
	 * The core came out of reset cleanly -- a previously wedged
	 * state is gone, so recovery attempts are allowed again.
	 */
	sc->sc_flags &= ~BCM4313_FLAG_DEAD;
	DELAY(2000);
	ioctl = BHND_IOCTL_CLK_FORCE;
	ioctl_mask = BHND_IOCTL_CLK_FORCE | BCM4313_IOCTL_PHYRESET |
	    BCM4313_IOCTL_PHYCLOCK_ENABLE;
	if ((error = bhnd_write_ioctl(sc->sc_dev, ioctl, ioctl_mask)) != 0) {
		device_printf(sc->sc_dev, "ioctl(CLK_FORCE) failed: %d\n",
		    error);
		return;
	}
	DELAY(2000);
	ioctl = BCM4313_IOCTL_PHYCLOCK_ENABLE;
	if ((error = bhnd_write_ioctl(sc->sc_dev, ioctl, ioctl_mask)) != 0) {
		device_printf(sc->sc_dev, "ioctl(PHYCLOCK) failed: %d\n",
		    error);
		return;
	}
	DELAY(2000);
	/* reset_hw() invalidated our clock request; request it again. */
	if ((error = bhnd_request_clock(sc->sc_dev, BHND_CLOCK_HT)) != 0) {
		device_printf(sc->sc_dev, "clock request failed: %d\n", error);
		return;
	}

	/*
	 * Refuse to start while the hardware RF kill switch is engaged
	 * (brcmsmac refuses the same way: brcms_b_up_prep() reports
	 * -ENOMEDIUM and brcms_c_up() leaves the radio down).  The core is
	 * enabled and clocked here, so phydebug is valid; reading it at probe
	 * time (before PHYCLOCK_ENABLE) is what the old "unsupported PHY
	 * type 15" bug did wrong.  MI_RFDISABLE keeps the state fresh while
	 * running and logs the re-enable edge.
	 */
	if (bcm4313_read_4(sc, BCM4313_D11_PHYDEBUG) & BCM4313_PDBG_RFD) {
		if (sc->sc_radio_off == 0)
			device_printf(sc->sc_dev,
			    "hardware radio disabled (RF kill switch); "
			    "flip the switch and re-up the interface\n");
		sc->sc_radio_off = 1;
		return;
	}
	sc->sc_radio_off = 0;

	/* Basic MAC control: 2.4GHz (GMODE), MAC disabled. */
	ctl = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
	ctl &= ~BCM4313_MCTL_EN_MAC;
	ctl |= BCM4313_MCTL_GMODE;
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, ctl);

	/* Shared-memory configuration. */
	bcm4313_shm_write_2(sc, BCM4313_M_MACHW_VER, BCM4313_D11_HWREV);
	bcm4313_shm_write_2(sc, BCM4313_M_MAXRXFRM_LEN, 2346);
	bcm4313_shm_write_2(sc, BCM4313_M_DOT11_SLOT,
	    (ic->ic_flags & IEEE80211_F_SHSLOT) ? 9 : 20);
	bcm4313_shm_write_2(sc, BCM4313_M_CURCHANNEL, sc->sc_curchan);
	bcm4313_set_macaddr(sc, ic->ic_macaddr);

	/* PHY. */
	bcm4313_lcnphy_init(sc);

	/* DMA engines. */
	if (bcm4313_ring_tx_init(sc, &sc->sc_tx) != 0)
		goto fail;
	if (bcm4313_ring_rx_init(sc, &sc->sc_rx) != 0)
		goto fail;

	/* Interrupts. */
	sc->sc_intr_mask = BCM4313_MI_DMAINT | BCM4313_MI_TFS |
	    BCM4313_MI_GP0 | BCM4313_MI_MACTXERR | BCM4313_MI_PHYTXERR |
	    BCM4313_MI_RFDISABLE;
	bcm4313_write_4(sc, BCM4313_D11_MACINTMASK, sc->sc_intr_mask);

	/* Start the MAC. */
	bcm4313_mac_enable(sc);
	/*
	 * Enable the ucode's RF-disable debounce timer so MI_RFDISABLE fires
	 * on a settled switch change.  (brcmsmac brcms_c_init: "enable the RF
	 * Disable Delay timer", RFDISABLE_DEFAULT.)
	 */
	bcm4313_write_4(sc, BCM4313_D11_RFDISABLEDLY, BCM4313_RFDISABLE_DEFAULT);
	DELAY(100);

	sc->sc_flags |= BCM4313_FLAG_RUNNING;
	sc->sc_watchdog_timer = 5;
	callout_reset(&sc->sc_watchdog_ch, hz, bcm4313_watchdog, sc);
	BCM4313_DPRINTF(sc, "MAC initialized\n");
	return;
fail:
	device_printf(sc->sc_dev, "DMA ring init failed\n");
}

/*
 * ---------------------------------------------------------------------------
 * Device methods.
 * ---------------------------------------------------------------------------
 */
static const struct bhnd_device bcm4313_devices[] = {
	{ { BHND_MATCH_CORE(BHND_MFGID_BCM, BHND_COREID_D11),
	    BHND_MATCH_CORE_REV(HWREV_EQ(BCM4313_D11_HWREV)) },
	    BCM4313_DEVICE_DESC, NULL, 0 },
	BHND_DEVICE_END
};

static int
bcm4313_probe(device_t dev)
{
	const struct bhnd_device *id;
	const struct bhnd_chipid *cid;

	id = bhnd_device_lookup(dev, bcm4313_devices, sizeof(bcm4313_devices[0]));
	if (id == NULL)
		return (ENXIO);
	/* D11 rev 24 (LCN) cores also back BCM4331; require the BCM4313 chip. */
	cid = bhnd_get_chipid(dev);
	if (cid == NULL || cid->chip_id != BHND_CHIPID_BCM4313)
		return (ENXIO);
	bhnd_set_default_core_desc(dev);
	return (BUS_PROBE_DEFAULT);
}

/*
 * Register dev.bcm4313.X.* diagnostics on the device tree (the same
 * approach bwn(4) uses; mirrors the sibling brcmfmac port's sysctls).
 * Hot-path counters are the first place to look when scan/assoc
 * misbehaves:
 *   txframes - frames posted to the DMA TX ring
 *   rxframes - frames delivered to net80211
 *   txdone   - tx statuses drained from the status ring
 *   wdogfires- watchdog resets + PSM watchdog events
 *   dma{tx,rx,txstatus} - live ring producer/consumer positions
 *   cal*     - periodic temperature-based recalibration loop
 *              (calticks rises 1/s; calfires counts glacial recals)
 */
/* calsample is a uint8_t in the softc; hand it back as an int. */
static int
bcm4313_sysctl_calsample(SYSCTL_HANDLER_ARGS)
{
	struct bcm4313_softc *sc = arg1;
	int sample;

	sample = sc->sc_lcn.lcnphy_cal_counter;
	return (sysctl_handle_int(oidp, &sample, 0, req));
}

/* Per-FIFO DMA fault tally, plus fifo/bits of the most recent fault. */
static int
bcm4313_sysctl_dmaerr(SYSCTL_HANDLER_ARGS)
{
	struct bcm4313_softc *sc = arg1;
	char buf[128];
	int i, len;

	len = snprintf(buf, sizeof(buf), "fifo");
	for (i = 0; i < 8; i++)
		len += snprintf(buf + len, sizeof(buf) - len,
		    " %u", sc->sc_dmaerr[i]);
	snprintf(buf + len, sizeof(buf) - len,
	    " last=%u bits=%#x", sc->sc_dmaerr_fifo,
	    sc->sc_dmaerr_bits);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static void
bcm4313_sysctl_setup(struct bcm4313_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *root;

	ctx = device_get_sysctl_ctx(sc->sc_dev);
	root = device_get_sysctl_tree(sc->sc_dev);

	SYSCTL_ADD_STRING(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "fwinfo", CTLFLAG_RD, sc->sc_fwinfo, 0,
	    "Chip / PHY / radio identity");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "debug", CTLFLAG_RW, &sc->sc_debug, 0,
	    "Log verbosity: 0 off, 1 scan start/end, 2 +channel hops");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "intrmask", CTLFLAG_RD, &sc->sc_intr_mask, 0,
	    "Current MAC interrupt mask");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rfkill", CTLFLAG_RD, &sc->sc_radio_off, 0,
	    "Hardware RF kill state: 0 radio on, 1 hard-blocked");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "dmatx", CTLFLAG_RD, &sc->sc_tx.r_out, 0,
	    "TX ring producer position");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "dmarx", CTLFLAG_RD, &sc->sc_rx.r_in, 0,
	    "RX ring consumer position");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "dmatxstatus", CTLFLAG_RD, &sc->sc_txdone, 0,
	    "TX statuses drained from the frmtxstatus registers");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "txframes", CTLFLAG_RD, &sc->sc_txframes, 0,
	    "Frames posted to the DMA TX ring");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rxframes", CTLFLAG_RD, &sc->sc_rxframes, 0,
	    "Frames delivered to net80211");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "txdone", CTLFLAG_RD, &sc->sc_txdone, 0,
	    "TX statuses harvested");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "wdogfires", CTLFLAG_RD, &sc->sc_wdog_fires, 0,
	    "Watchdog resets + PSM watchdog events");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "txreclaimed", CTLFLAG_RD, &sc->sc_txreclaimed, 0,
	    "TX frames reclaimed");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "txnobuf", CTLFLAG_RD, &sc->sc_txnobuf, 0,
	    "TX ring full / no-buffer drops");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rxnobuf", CTLFLAG_RD, &sc->sc_rxnobuf, 0,
	    "RX refill no-buffer drops");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rxgiants", CTLFLAG_RD, &sc->sc_rxgiants, 0,
	    "Oversized RX frames dropped");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rxpad", CTLFLAG_RD, &sc->sc_rxpad, 0,
	    "RX frames with QoS pad bytes stripped");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rxamsdu", CTLFLAG_RD, &sc->sc_rxamsdu, 0,
	    "A-MSDU frames dropped (unsupported)");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "rssiinvalid", CTLFLAG_RD, &sc->sc_rssiinvalid, 0,
	    "RX frames without a valid PHY status (RSSI unknown)");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "txnomatch", CTLFLAG_RD, &sc->sc_txnomatch, 0,
	    "TX statuses with no matching in-flight frame");
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "dmaerr", CTLTYPE_STRING | CTLFLAG_RD, sc, 0,
	    bcm4313_sysctl_dmaerr, "A",
	    "Per-FIFO DMA error counts + last error (fifo/bits)");

	/*
	 * Periodic temperature-based TX-power recalibration loop
	 * (bcm4313_lcnphy_watchdog, ~every 3.5 min on tempsense boards):
	 * calticks counts per-second ticks while the MAC is up, callastfull
	 * is the tick of the last full calibration, calsample counts the
	 * temperature samples taken since (0..90 before a recalibration)
	 * and calfires counts the glacial IQLO+VCO recalibrations run.
	 * Soak test: calticks must climb 1/s and calfires must increment
	 * roughly every 3-4 minutes while associated.
	 */
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "calticks", CTLFLAG_RD, &sc->sc_lcn.lcnphy_now, 0,
	    "Per-second recalibration ticks while the MAC is up");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "callastfull", CTLFLAG_RD, &sc->sc_lcn.lcnphy_lastcal, 0,
	    "calticks value at the last full calibration");
	SYSCTL_ADD_UINT(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "calfires", CTLFLAG_RD, &sc->sc_lcn.lcnphy_glacial_fires, 0,
	    "Glacial IQLO+VCO recalibrations run (~1 per 3.5 min)");
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "calsample", CTLTYPE_INT | CTLFLAG_RD, sc, 0,
	    bcm4313_sysctl_calsample, "I",
	    "Temp samples since the last glacial recalibration (0..90)");
}

static int
bcm4313_attach(device_t dev)
{
	struct bcm4313_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic;
	struct bcm4313_tx_radiotap_header *tx_th;
	struct bcm4313_rx_radiotap_header *rx_th;
	const char *mac_varname;
	char chip_name[BHND_CHIPID_MAX_NAMELEN];
	uint16_t phyver, ioctl;
	int error, rid;

	sc->sc_dev = dev;
	sc->sc_cid = *bhnd_get_chipid(dev);

	BCM4313_LOCK_INIT(sc);
	mbufq_init(&sc->sc_snd, ifqmaxlen);
	callout_init_mtx(&sc->sc_watchdog_ch, &sc->sc_mtx, 0);
	NET_TASK_INIT(&sc->sc_intrtask, 0, bcm4313_intrtask, sc);
	sc->sc_tq = taskqueue_create_fast("bcm4313_taskq", M_NOWAIT,
	    taskqueue_thread_enqueue, &sc->sc_tq);
	if (sc->sc_tq == NULL) {
		device_printf(dev, "couldn't create taskqueue\n");
		error = ENOMEM;
		goto fail;
	}
	taskqueue_start_threads(&sc->sc_tq, 1, PI_NET, "%s taskq",
	    device_get_nameunit(dev));

	/* Board and chip identification. */
	if (sc->sc_cid.chip_id != BHND_CHIPID_BCM4313) {
		device_printf(dev, "unsupported chip id %#x\n",
		    sc->sc_cid.chip_id);
		error = ENXIO;
		goto fail;
	}
	if ((error = bhnd_read_board_info(dev, &sc->sc_board)) != 0) {
		device_printf(dev, "couldn't read board info\n");
		goto fail;
	}

	/* D11 core register window. */
	rid = 0;
	sc->sc_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->sc_mem_res == NULL) {
		device_printf(dev, "couldn't allocate registers\n");
		error = ENXIO;
		goto fail;
	}
	sc->sc_mem_rid = rid;
	if ((error = bhnd_alloc_pmu(dev)) != 0) {
		device_printf(dev, "couldn't allocate PMU state\n");
		goto fail;
	}

	/* DMA address translation (64-bit preferred, 32-bit fallback). */
	if ((error = bhnd_get_dma_translation(dev, BHND_DMA_ADDR_64BIT, 0,
	    &sc->sc_dmatag, &sc->sc_dma_translation)) != 0) {
		if ((error = bhnd_get_dma_translation(dev,
		    BHND_DMA_ADDR_32BIT, 0, &sc->sc_dmatag,
		    &sc->sc_dma_translation)) != 0) {
			device_printf(dev, "no DMA translation available\n");
			goto fail;
		}
	}
	/* Data-buffer tag: single segment, up to one full frame. */
	if ((error = bus_dma_tag_create(sc->sc_dmatag, 16, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL, BCM4313_TXBUFSZ,
	    1, BCM4313_TXBUFSZ, 0, NULL, NULL, &sc->sc_bufdtag)) != 0) {
		device_printf(dev, "couldn't create buffer dma tag\n");
		goto fail;
	}

	/* Bring the core out of reset so PHYVER is readable.  Match
	 * brcmsmac's probe: wlc_phy_attach() reads phyversion right after
	 * bcma_core_enable(core, SICF_GMODE | SICF_PCLKE) -- PCLKE is forced
	 * for D11 rev >= 18, and PHYRESET is never asserted at probe time
	 * (asserting it leaves the PHY in reset, so PHYVER reads 0xFFFF).
	 * The bhnd backend owns BHND_IOCTL_CLK_EN/BHND_IOCTL_CLK_FORCE
	 * exclusively and returns EINVAL if a driver passes them to
	 * bhnd_reset_hw(); it sets the clock bits itself when releasing reset,
	 * and we re-request the HT clock below. */
	ioctl = BCM4313_IOCTL_PHYCLOCK_ENABLE | BCM4313_IOCTL_SUPPORT_G;
	if ((error = bhnd_reset_hw(dev, ioctl, ioctl)) != 0) {
		device_printf(dev, "core reset failed: %d\n", error);
		goto fail;
	}
	DELAY(2000);
	if ((error = bhnd_request_clock(dev, BHND_CLOCK_HT)) != 0) {
		device_printf(dev, "clock request failed: %d\n", error);
		goto fail;
	}
	DELAY(1000);
	phyver = bcm4313_read_2(sc, BCM4313_D11_PHYVER);
	sc->sc_phy_analog = (phyver >> 12) & 0xf;
	sc->sc_phy_type = (phyver >> 8) & 0xf;
	sc->sc_phy_rev = phyver & 0xf;
	sc->sc_radio_id = bcm4313_radio_read32(sc, 0x1000); /* RFCTL_ID */
	if (sc->sc_phy_type != BCM4313_PHY_TYPE_LCN) {
		device_printf(dev, "unsupported PHY type %u (rev %u)\n",
		    sc->sc_phy_type, sc->sc_phy_rev);
		error = ENXIO;
		goto fail;
	}

	/*
	 * Crystal frequency + SPROM TX-power/tempsense calibration values
	 * (wlc_phy_attach_lcnphy / wlc_phy_txpwr_srom_read_lcnphy in
	 * brcmsmac).  The ALP clock is the crystal frequency used by the
	 * 2064 PLL math.
	 */
	(void)bhnd_get_clock_freq(dev, BHND_CLOCK_ALP, &sc->sc_xtalfreq);
	if (sc->sc_xtalfreq == 0)
		sc->sc_xtalfreq = 40000000; /* 4313 boards use a 40 MHz xtal */

	/* PA0 coefficients (i16) and max TX power (u8, dBm). */
	(void)bhnd_nvram_getvar_int16(dev, BHND_NVAR_PA0B0,
	    &sc->sc_txpa_2g[0]);
	(void)bhnd_nvram_getvar_int16(dev, BHND_NVAR_PA0B1,
	    &sc->sc_txpa_2g[1]);
	(void)bhnd_nvram_getvar_int16(dev, BHND_NVAR_PA0B2,
	    &sc->sc_txpa_2g[2]);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_PA0MAXPWR,
	    &sc->sc_tx_power_min);

	/* RSSI calibration (rssismf2g/rssismc2g/rssisav2g). */
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_RSSISMF2G,
	    &sc->sc_lcn.lcnphy_rssi_vf);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_RSSISMC2G,
	    &sc->sc_lcn.lcnphy_rssi_vc);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_RSSISAV2G,
	    &sc->sc_lcn.lcnphy_rssi_gs);
	sc->sc_lcn.lcnphy_rssi_vf_lowtemp = sc->sc_lcn.lcnphy_rssi_vf;
	sc->sc_lcn.lcnphy_rssi_vc_lowtemp = sc->sc_lcn.lcnphy_rssi_vc;
	sc->sc_lcn.lcnphy_rssi_gs_lowtemp = sc->sc_lcn.lcnphy_rssi_gs;
	sc->sc_lcn.lcnphy_rssi_vf_hightemp = sc->sc_lcn.lcnphy_rssi_vf;
	sc->sc_lcn.lcnphy_rssi_vc_hightemp = sc->sc_lcn.lcnphy_rssi_vc;
	sc->sc_lcn.lcnphy_rssi_gs_hightemp = sc->sc_lcn.lcnphy_rssi_gs;

	/* Tempsense / IQ-cal calibration. */
	(void)bhnd_nvram_getvar_uint16(dev, BHND_NVAR_RAWTEMPSENSE,
	    &sc->sc_lcn.lcnphy_rawtempsense);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_MEASPOWER,
	    &sc->sc_lcn.lcnphy_measPower);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_TEMPSENSE_SLOPE,
	    &sc->sc_lcn.lcnphy_tempsense_slope);
	{
		uint8_t tmp8;

		(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_HW_IQCAL_EN,
		    &tmp8);
		sc->sc_lcn.lcnphy_hw_iqcal_en = (tmp8 != 0);
		(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_IQCAL_SWP_DIS,
		    &tmp8);
		sc->sc_lcn.lcnphy_iqcal_swp_dis = (tmp8 != 0);
	}
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_TEMPCORRX,
	    &sc->sc_lcn.lcnphy_tempcorrx);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_TEMPSENSE_OPTION,
	    &sc->sc_lcn.lcnphy_tempsense_option);
	(void)bhnd_nvram_getvar_uint8(dev, BHND_NVAR_FREQOFFSET_CORR,
	    &sc->sc_lcn.lcnphy_freqoffset_corr);

	/* TX power offset tables (per-rate power-0 offsets). */
	(void)bhnd_nvram_getvar_uint16(dev, BHND_NVAR_CCK2GPO,
	    &sc->sc_cck2gpo);
	(void)bhnd_nvram_getvar_uint32(dev, BHND_NVAR_OFDM2GPO,
	    &sc->sc_ofdm2gpo);
	(void)bhnd_nvram_getvar_uint16(dev, BHND_NVAR_MCS2GPO0,
	    &sc->sc_mcs2gpo[0]);
	(void)bhnd_nvram_getvar_uint16(dev, BHND_NVAR_MCS2GPO1,
	    &sc->sc_mcs2gpo[1]);

	/*
	 * Derive the per-rate TX power maxima and rate offsets from the
	 * SPROM values read above (brcmsmac calls
	 * wlc_phy_txpwr_srom_read_lcnphy() from wlc_phy_attach_lcnphy()).
	 */
	bcm4313_lcnphy_txpwr_srom_read(sc);

	/*
	 * Hardware TX-power control capability (wlc_phy_attach_lcnphy):
	 * rev 1 without tempsense-based pwrctrl uses HW TSSI control;
	 * rev 1 with tempsense (option != 3) is software-controlled.
	 */
	if (sc->sc_board.board_flags & BCM4313_BFL_NOPA) {
		sc->sc_hwpwrctrl_capable = false;
	} else {
		sc->sc_hwpwrctrl_capable = true;
	}
	if (sc->sc_phy_rev == 1) {
		if (sc->sc_lcn.lcnphy_tempsense_option == 3) {
			sc->sc_hwpwrctrl_capable = true;
			sc->sc_temppwrctrl_capable = false;
		} else {
			sc->sc_hwpwrctrl_capable = false;
			sc->sc_temppwrctrl_capable = true;
		}
	}

	/*
	 * D11 rev 24 / LCN needs its microcode before the MAC can run.  The
	 * LCN ucode is bundled (bcm4313_ucode.c) and uploaded through the
	 * OBJADDR microcode window; bail if it cannot be found, since a D11
	 * core without microcode will not suspend/operate.
	 */
	if ((error = bcm4313_ucode_download(sc)) != 0)
		goto fail;

	/* DMA rings. */
	if ((error = bcm4313_ring_alloc(sc, &sc->sc_tx,
	    BCM4313_D11_FIFO_DMA_TX(BCM4313_TX_BE_FIFO), BCM4313_NTX_DESC,
	    1, 0, BCM4313_TX_FRAMES)) != 0)
		goto fail;
	if ((error = bcm4313_ring_alloc(sc, &sc->sc_rx,
	    BCM4313_D11_FIFO_DMA_RX(BCM4313_RX_FIFO), BCM4313_NRX_DESC,
	    0, BCM4313_RXBUFSZ, 0)) != 0)
		goto fail;

	/* net80211. */
	ic = &sc->sc_ic;
	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_caps = IEEE80211_C_STA | IEEE80211_C_IBSS |
	    IEEE80211_C_HOSTAP | IEEE80211_C_MONITOR |
	    IEEE80211_C_SHPREAMBLE | IEEE80211_C_SHSLOT |
	    IEEE80211_C_WME | IEEE80211_C_WPA;
	ic->ic_htcaps = IEEE80211_HTCAP_SHORTGI20;
	ic->ic_rxstream = 1;
	ic->ic_txstream = 1;
	setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11NG);
	ic->ic_flags_ext |= IEEE80211_FEXT_SWBMISS;

	/* SPROM MAC address (bhnd nvram layer). */
	mac_varname = "macaddr";
	if (sc->sc_board.board_srom_rev <= 2)
		mac_varname = (bhnd_get_core_unit(dev) == 0) ?
		    "il0macaddr" : "et1macaddr";
	if ((error = bhnd_nvram_getvar_array(dev, mac_varname,
	    ic->ic_macaddr, sizeof(ic->ic_macaddr),
	    BHND_NVRAM_TYPE_UINT8_ARRAY)) != 0) {
		device_printf(dev, "error reading %s: %d\n", mac_varname,
		    error);
		goto fail;
	}

	/* Channel list: populate ic_channels/ic_nchans BEFORE ifattach
	 * (ieee80211_chan_init() reads them immediately; see
	 * bcm4313_add_channels above).  ic_getradiocaps is still provided
	 * for regdomain updates. */
	ic->ic_getradiocaps = bcm4313_getradiocaps;
	memset(ic->ic_channels, 0, sizeof(ic->ic_channels));
	ic->ic_nchans = 0;
	bcm4313_add_channels(ic->ic_channels, IEEE80211_CHAN_MAX,
	    &ic->ic_nchans);
	device_printf(dev, "channels ready: %d (2.4GHz b/g/ng HT20)\n",
	    ic->ic_nchans);
	ieee80211_ifattach(ic);
	ic->ic_transmit = bcm4313_transmit;
	ic->ic_raw_xmit = bcm4313_raw_xmit;
	ic->ic_parent = bcm4313_parent;
	ic->ic_set_channel = bcm4313_set_channel;
	ic->ic_scan_start = bcm4313_scan_start;
	ic->ic_scan_end = bcm4313_scan_end;
	ic->ic_update_promisc = bcm4313_update_promisc;
	ic->ic_vap_create = bcm4313_vap_create;
	ic->ic_vap_delete = bcm4313_vap_delete;

	tx_th = &sc->sc_tx_th;
	rx_th = &sc->sc_rx_th;
	ieee80211_radiotap_attach(ic, &tx_th->wt_ihdr, sizeof(*tx_th),
	    BCM4313_TX_RADIOTAP_PRESENT, &rx_th->wr_ihdr, sizeof(*rx_th),
	    BCM4313_RX_RADIOTAP_PRESENT);
	if (bootverbose)
		ieee80211_announce(ic);

	sc->sc_flags |= BCM4313_FLAG_ATTACHED;

	/* Interrupt. */
	rid = 0;
	sc->sc_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_irq == NULL) {
		device_printf(dev, "couldn't allocate interrupt\n");
		error = ENXIO;
		goto fail;
	}
	sc->sc_irq_rid = rid;
	if ((error = bus_setup_intr(dev, sc->sc_irq, INTR_TYPE_NET |
	    INTR_MPSAFE, bcm4313_intr, NULL, sc, &sc->sc_ih)) != 0) {
		device_printf(dev, "couldn't setup interrupt\n");
		goto fail;
	}

	bhnd_format_chip_id(chip_name, sizeof(chip_name), sc->sc_cid.chip_id);
	device_printf(dev, "%s (rev %u, sromrev %u) PHY (type %u rev %u "
	    "analog %u) radio (ver %#x) board-flags %#x board-rev %#x\n",
	    chip_name, bhnd_get_hwrev(dev), sc->sc_board.board_srom_rev,
	    sc->sc_phy_type, sc->sc_phy_rev, sc->sc_phy_analog,
	    sc->sc_radio_id, sc->sc_board.board_flags,
	    sc->sc_board.board_rev);

	snprintf(sc->sc_fwinfo, sizeof(sc->sc_fwinfo),
	    "%s rev %u sromrev %u PHY %u/%u/%u radio %#x board %#x/%#x",
	    chip_name, bhnd_get_hwrev(dev), sc->sc_board.board_srom_rev,
	    sc->sc_phy_type, sc->sc_phy_rev, sc->sc_phy_analog,
	    sc->sc_radio_id, sc->sc_board.board_flags,
	    sc->sc_board.board_rev);
	bcm4313_sysctl_setup(sc);

	return (0);
fail:
	bcm4313_detach(dev);
	return (error);
}

static int
bcm4313_detach(device_t dev)
{
	struct bcm4313_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = &sc->sc_ic;

	if (sc->sc_flags & BCM4313_FLAG_ATTACHED) {
		BCM4313_LOCK(sc);
		bcm4313_stop_locked(sc);
		BCM4313_UNLOCK(sc);
		ieee80211_ifdetach(ic);
		sc->sc_flags &= ~BCM4313_FLAG_ATTACHED;
	}
	if (sc->sc_ih != NULL) {
		bus_teardown_intr(dev, sc->sc_irq, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid,
		    sc->sc_irq);
		sc->sc_irq = NULL;
	}
	if (sc->sc_tq != NULL) {
		taskqueue_drain(sc->sc_tq, &sc->sc_intrtask);
		taskqueue_free(sc->sc_tq);
		sc->sc_tq = NULL;
	}
	bcm4313_ring_free(&sc->sc_tx);
	bcm4313_ring_free(&sc->sc_rx);
	if (sc->sc_bufdtag != NULL) {
		bus_dma_tag_destroy(sc->sc_bufdtag);
		sc->sc_bufdtag = NULL;
	}
	callout_drain(&sc->sc_watchdog_ch);
	mbufq_drain(&sc->sc_snd);
	if (sc->sc_mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_mem_rid,
		    sc->sc_mem_res);
		sc->sc_mem_res = NULL;
	}
	bhnd_release_pmu(dev);
	BCM4313_LOCK_DESTROY(sc);
	return (0);
}

static device_method_t bcm4313_methods[] = {
	DEVMETHOD(device_probe, bcm4313_probe),
	DEVMETHOD(device_attach, bcm4313_attach),
	DEVMETHOD(device_detach, bcm4313_detach),
	DEVMETHOD_END
};

static driver_t bcm4313_driver = {
	.name = "bcm4313",
	.methods = bcm4313_methods,
	.size = sizeof(struct bcm4313_softc),
};

DRIVER_MODULE(if_bcm4313, bhnd, bcm4313_driver, 0, 0);
MODULE_VERSION(if_bcm4313, 1);
MODULE_DEPEND(if_bcm4313, bhnd, 1, 1, 1);
/*
 * bhnd(4) is only the bus core.  A PCIe-attached BCM4313 also needs the
 * PCI->bhnd bridge chain before the D11 core is even enumerated (the same
 * set bwn_pci declares): bhndb_pci -> bhndb -> bcma_bhndb (bcma backend
 * over the bridge), plus bhnd_sprom for nvram.  Without these, kldload
 * succeeds but the card never becomes a bhnd bus and the driver never
 * probes -- kldload if_bcm4313 must pull them in automatically.
 */
MODULE_DEPEND(if_bcm4313, bhndb, 1, 1, 1);
MODULE_DEPEND(if_bcm4313, bhndb_pci, 1, 1, 1);
MODULE_DEPEND(if_bcm4313, bcma_bhndb, 1, 1, 1);
MODULE_DEPEND(if_bcm4313, siba_bhndb, 1, 1, 1);
MODULE_DEPEND(if_bcm4313, bhnd_sprom, 1, 1, 1);
MODULE_DEPEND(if_bcm4313, wlan, 1, 1, 1);
