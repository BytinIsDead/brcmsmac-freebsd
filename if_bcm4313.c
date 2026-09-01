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
static void	bcm4313_tx_reclaim(struct bcm4313_softc *,
		    struct bcm4313_ring *, int);
static void	bcm4313_init_locked(struct bcm4313_softc *);
static void	bcm4313_stop_locked(struct bcm4313_softc *);
static int	bcm4313_detach(device_t);
static int	bcm4313_ucode_download(struct bcm4313_softc *);

/* Ring sizes. */
#define	BCM4313_NTX_DESC		64	/* 32 frames, 2 slots each */
#define	BCM4313_NRX_DESC		64
#define	BCM4313_NTXSTATUS_DESC		16
#define	BCM4313_RXBUFSZ			2048
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

/* Initialize a TX dma64 channel. */
static int
bcm4313_ring_tx_init(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	bus_addr_t pa;

	ring->r_in = ring->r_out = 0;
	memset(ring->r_desc, 0, ring->r_nslots *
	    sizeof(struct bcm4313_dma64desc));
	/* Enable the channel (parity disabled; not implemented). */
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL,
	    BCM4313_D64_XC_XE | BCM4313_D64_XC_PD);
	/* Descriptor table base (8KB-aligned ring). */
	pa = bcm4313_dma_addr(sc, ring->r_paddr);
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
	    (uint32_t)pa);
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
	    (uint32_t)((uint64_t)pa >> 32));
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR, 0);
	return (0);
}

/* Initialize an RX dma64 channel and post the initial buffers. */
static int
bcm4313_ring_rx_init(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	bus_addr_t pa;

	ring->r_in = ring->r_out = 0;
	memset(ring->r_desc, 0, ring->r_nslots *
	    sizeof(struct bcm4313_dma64desc));
	/* Enable RX; frame offset 0 (inline rx header). */
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL,
	    BCM4313_D64_RC_RE | BCM4313_D64_RC_PD);
	pa = bcm4313_dma_addr(sc, ring->r_paddr);
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRLOW,
	    (uint32_t)pa);
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_ADDRHIGH,
	    (uint32_t)((uint64_t)pa >> 32));
	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_PTR, 0);
	bcm4313_rx_refill(sc, ring);
	return (0);
}

/* Disable a channel; reclaim posted buffers. */
static void
bcm4313_ring_stop(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
	int i;

	bcm4313_write_4(sc, ring->r_base + BCM4313_DMA64_CTL, 0);
	DELAY(100);
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
		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
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
 * Harvest received frames from an RX ring and feed them to net80211.
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
	uint16_t rxlen, rxs1, rxs2;
	int rssi;
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
			rxs1 = le16toh(rxh->RxStatus1);
			rxs2 = le16toh(rxh->PhyRxStatus_2);
			if (rxlen < IEEE80211_MIN_LEN ||
			    rxlen > ring->r_bufsz - BCM4313_D11_RXH_LEN ||
			    (rxs1 & BCM4313_RXS_FCSERR) != 0) {
				/* Giant / short / bad-FCS: drop. */
				if (rxlen > ring->r_bufsz - BCM4313_D11_RXH_LEN)
					sc->sc_rxgiants++;
				m_freem(m);
				counter_u64_add(ic->ic_ierrors, 1);
				goto next;
			}
			/* Strip the rx header, trim to frame length. */
			m->m_pkthdr.len = rxlen + BCM4313_D11_RXH_LEN;
			m_adj(m, -(MCLBYTES - (rxlen + BCM4313_D11_RXH_LEN)));
			m_adj(m, BCM4313_D11_RXH_LEN);
			/*
			 * RSSI from HTPHY/LCN Rx power (antenna 0) in
			 * PhyRxStatus_2; sign/scale per brcmsmac's
			 * wlc_lcnphy_rx_signal_strength().
			 */
			rssi = -(int)((rxs2 & BCM4313_PRXS2_HTPHY_RXPWR_ANT0) >> 8);
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
 * Drain the TX-status FIFO (RX_TXSTATUS_FIFO).  The ucode posts one
 * 16-byte struct bcm4313_tx_status per completed frame; ACK/retry
 * accounting can be added here (see brcms_c_dotxstatus()).
 */
static void
bcm4313_txstatus_harvest(struct bcm4313_softc *sc, struct bcm4313_ring *ring)
{
#ifdef BCM4313_DEBUG
	struct bcm4313_tx_status *ts;
#endif
	struct bcm4313_slot *slot;
	struct mbuf *m;
	uint32_t hw;
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
			#ifdef BCM4313_DEBUG
			if (m->m_len >= BCM4313_TXSTATUS_LEN) {
				ts = mtod(m, struct bcm4313_tx_status *);
				BCM4313_DPRINTF(sc,
				    "txstatus: fid %#x status %#x\n",
				    le16toh(ts->frameid),
				    le16toh(ts->status));
			}
			#endif
			m_freem(m);
		}
		ring->r_in = (ring->r_in + 1) % ring->r_nslots;
	}
	bcm4313_rx_refill(sc, ring);
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
 * follow brcmsmac brcms_c_sendpkt(); the rate encoding should be verified
 * against that function when bring-up begins.
 */
static void
bcm4313_set_txhdr(struct bcm4313_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, struct bcm4313_d11txh *txh)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	uint8_t rate;

	wh = mtod(m, struct ieee80211_frame *);
	memset(txh, 0, BCM4313_D11_TXH_LEN);
	txh->MacTxControlLow = htole16(BCM4313_TXC_STARTMSDU);
	txh->MacFrameControl = htole16(wh->i_fc[0] | (wh->i_fc[1] << 8));
	memcpy(txh->TxFrameRA, wh->i_addr1, IEEE80211_ADDR_LEN);
	txh->TxFrameID = htole16(sc->sc_frameid++);
	txh->PhyTxControlWord = htole16(BCM4313_PHY_TXC_LCNPHY_ANT_LAST);
	if (IEEE80211_IS_CHAN_HT(ic->ic_curchan)) {
		/* HT: 1x1, 20MHz, MCS in MainRates. */
#if __FreeBSD_version >= 1500000
		/* ni_txrate is now struct ieee80211_node_txrate. */
		rate = min(ni->ni_txrate.mcs, 15);
#else
		rate = min(ni->ni_txrate, 15);
#endif
		txh->MainRates = htole16(rate);
		txh->PhyTxControlWord_1 = htole16(BCM4313_PHY_TXC1_BW_20MHZ |
		    (BCM4313_PHY_TXC1_MODE_SISO <<
		    BCM4313_PHY_TXC1_MODE_SHIFT));
	} else {
		/* Legacy: PLCP rate code in MainRates. */
#if __FreeBSD_version >= 1500000
		/* dot11rate already carries the dot11 rate code. */
		txh->MainRates = htole16(bcm4313_plcp_rate(
		    ni->ni_txrate.dot11rate));
#else
		rate = ni->ni_txrate;
		if (rate >= ic->ic_rt->rateCount)
			rate = 0;
		txh->MainRates = htole16(bcm4313_plcp_rate(
		    ic->ic_rt->info[rate].dot11Rate));
#endif
	}
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
void
bcm4313_mac_enable(struct bcm4313_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t mc;

	mc = BCM4313_MCTL_EN_MAC | BCM4313_MCTL_GMODE;
	if (ic->ic_opmode == IEEE80211_M_STA)
		mc |= BCM4313_MCTL_INFRA;
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);
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
}

static void
bcm4313_scan_end(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;

	BCM4313_LOCK(sc);
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) != 0) {
		bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL,
		    ~BCM4313_MCTL_BCNS_PROMISC, 0);
	}
	BCM4313_UNLOCK(sc);
	BCM4313_DPRINTF(sc, "scan end\n");
}

static void
bcm4313_update_promisc(struct ieee80211com *ic)
{
	struct bcm4313_softc *sc = ic->ic_softc;

	BCM4313_LOCK(sc);
	if (ic->ic_promisc > 0)
		bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL, ~0,
		    BCM4313_MCTL_PROMISC);
	else
		bcm4313_maskset_4(sc, BCM4313_D11_MACCONTROL,
		    ~BCM4313_MCTL_PROMISC, 0);
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
static int
bcm4313_intr(void *arg)
{
	struct bcm4313_softc *sc = arg;
	uint32_t reason;

	reason = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS) &
	    sc->sc_intr_mask;
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
	reason = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS) &
	    sc->sc_intr_mask;
	if (reason == 0) {
		BCM4313_UNLOCK(sc);
		return;
	}
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, reason);
	if (reason & BCM4313_MI_DMAINT) {
		bcm4313_tx_reclaim(sc, &sc->sc_tx, 0);
		bcm4313_rx_harvest(sc, &sc->sc_rx);
		bcm4313_txstatus_harvest(sc, &sc->sc_txstatus);
		sc->sc_watchdog_timer = 5;
	}
	if (reason & (BCM4313_MI_MACTXERR | BCM4313_MI_PHYTXERR))
		counter_u64_add(sc->sc_ic.ic_oerrors, 1);
	BCM4313_UNLOCK(sc);
}

static void
bcm4313_watchdog(void *arg)
{
	struct bcm4313_softc *sc = arg;

	BCM4313_LOCK(sc);
	if (sc->sc_flags & BCM4313_FLAG_RUNNING) {
		if (--sc->sc_watchdog_timer <= 0) {
			device_printf(sc->sc_dev,
			    "watchdog timeout: resetting MAC\n");
			counter_u64_add(sc->sc_ic.ic_oerrors, 1);
			bcm4313_init_locked(sc);
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
	if ((sc->sc_flags & BCM4313_FLAG_RUNNING) == 0)
		return;
	sc->sc_flags &= ~BCM4313_FLAG_RUNNING;
	callout_stop(&sc->sc_watchdog_ch);

	/* Mask and clear interrupts; disable the MAC. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTMASK, 0);
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, 0xffffffff);
	bcm4313_mac_disable(sc);

	/* Stop DMA and reclaim state. */
	bcm4313_ring_stop(sc, &sc->sc_tx);
	bcm4313_ring_stop(sc, &sc->sc_rx);
	bcm4313_ring_stop(sc, &sc->sc_txstatus);

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
	if (sc->sc_flags & BCM4313_FLAG_RUNNING)
		bcm4313_stop_locked(sc);

	/* Reset the D11 core (PHY held in reset during the dance). */
	ioctl = BCM4313_IOCTL_PHYRESET | BCM4313_IOCTL_PHYCLOCK_ENABLE |
	    BCM4313_IOCTL_SUPPORT_G;
	if ((error = bhnd_reset_hw(sc->sc_dev, ioctl, ioctl)) != 0) {
		device_printf(sc->sc_dev, "core reset failed: %d\n", error);
		return;
	}
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
	if (bcm4313_ring_rx_init(sc, &sc->sc_txstatus) != 0)
		goto fail;

	/* Interrupts. */
	sc->sc_intr_mask = BCM4313_MI_DMAINT | BCM4313_MI_MACTXERR |
	    BCM4313_MI_PHYTXERR;
	bcm4313_write_4(sc, BCM4313_D11_MACINTMASK, sc->sc_intr_mask);

	/* Start the MAC. */
	bcm4313_mac_enable(sc);
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
	if ((error = bcm4313_ring_alloc(sc, &sc->sc_txstatus,
	    BCM4313_D11_FIFO_DMA_RX(BCM4313_RX_TXSTATUS_FIFO),
	    BCM4313_NTXSTATUS_DESC, 0, BCM4313_TXSTATUS_LEN, 0)) != 0)
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
	bcm4313_ring_free(&sc->sc_txstatus);
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
