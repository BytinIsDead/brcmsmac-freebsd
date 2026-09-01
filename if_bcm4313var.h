/*-
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
 *
 * Dual-licensed per LICENSE (BSD-2-Clause OR GPL-2.0-or-later for driver
 * code); embeds ISC-licensed tuning data (bcm4313_lcntab.h, stays ISC) and a
 * Broadcom-owned binary microcode.  See LICENSE before distributing.
 *
 * if_bcm4313var.h -- FreeBSD driver for the Broadcom BCM4313 SoftMAC
 * 802.11b/g/n PCIe chipset (D11 MAC core rev 24, LCN-PHY).
 *
 * This is a native FreeBSD driver that glues the brcmsmac-derived hardware
 * programming directly into the bhnd(4) backplane bus, net80211, and
 * bus_dma(9) interfaces.  No linuxkpi.
 *
 * Register offsets and bit definitions below are transcribed from the Linux
 * brcmsmac driver (drivers/net/wireless/broadcom/brcm80211/brcmsmac/d11.h,
 * dma.h, dma.c) and match the D11 core rev 17 register layout.  They are
 * hardware facts, not code.
 *
 * $FreeBSD$
 */
#ifndef	_IF_BCM4313VAR_H_
#define	_IF_BCM4313VAR_H_

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_phy.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_regdomain.h>

#include <dev/bhnd/bhnd.h>
#include <dev/bhnd/bhnd_ids.h>

MALLOC_DECLARE(M_BCM4313);

/*
 * Device identification.  bhnd(4)/bhndb(4) match the PCI vendor:device ID
 * 0x14e4:0x4313 at the bus bridge level; this driver attaches to the
 * enumerated D11 (MAC) core of that chipset.
 */
#define	BCM4313_PCI_VENDOR		0x14e4
#define	BCM4313_PCI_DEVICE		0x4313
#define	BCM4313_DEVICE_DESC		"Broadcom BCM4313 802.11bgn"

/*
 * IEEE 802.11 core (D11) hwrev used by BCM4313.  brcmsmac maps the BCM4313
 * (verified from real hardware as "core revision 24 (LCN)") to D11 rev 24,
 * which is the revision that loads the LCN (bcm43xx_24_lcn) microcode.
 * Revisions 17/23 are N-PHY and are a *different* family of chips.
 */
#define	BCM4313_D11_HWREV		24

/*
 * ---------------------------------------------------------------------------
 * D11 MAC core registers (d11regs, rev >= 11, 32-bit access).
 * Offsets verified against brcmsmac d11.h.
 * ---------------------------------------------------------------------------
 */
#define	BCM4313_D11_MACCONTROL		0x120	/* maccontrol */
#define	BCM4313_D11_MACCOMMAND		0x124	/* maccommand */
#define	BCM4313_D11_MACINTSTATUS	0x128	/* macintstatus */
#define	BCM4313_D11_MACINTMASK		0x12C	/* macintmask */
#define	BCM4313_D11_OBJADDR		0x160	/* objaddr: ucode/shm window */
#define	BCM4313_D11_OBJDATA		0x164	/* objdata */
#define	BCM4313_D11_FRMTXSTATUS		0x170	/* frmtxstatus */
#define	BCM4313_D11_FRMTXSTATUS2	0x174
#define	BCM4313_D11_TSF_TIMERLOW	0x180	/* tsf_timerlow */
#define	BCM4313_D11_TSF_TIMERHIGH	0x184
#define	BCM4313_D11_TSF_CFPSTART	0x18C
#define	BCM4313_D11_MACCONTROL1		0x1A0	/* maccontrol1 */
#define	BCM4313_D11_CLK_CTL_ST		0x1E0	/* clk_ctl_st (HT clock) */
#define	BCM4313_D11_RADIOADDR		0x3D8	/* radioregaddr (16-bit) */
#define	BCM4313_D11_RADIODATA		0x3DA	/* radioregdata (16-bit) */
#define	BCM4313_D11_PHYVER		0x3E0	/* phyversion (16-bit) */
#define	BCM4313_D11_PHY4WADDR		0x3F6	/* phy4waddr (32-bit PHYs) */
#define	BCM4313_D11_PHY4WDATAHI		0x3F8
#define	BCM4313_D11_PHY4WDATALO		0x3FA
#define	BCM4313_D11_PHYREGADDR		0x3FC	/* phyregaddr (16-bit) */
#define	BCM4313_D11_PHYREGDATA		0x3FE	/* phyregdata (16-bit) */

/* macintstatus/macintmask bits. */
#define	BCM4313_MI_MACSSPNDD		(1 << 0)
#define	BCM4313_MI_BCNTPL		(1 << 1)
#define	BCM4313_MI_TBTT			(1 << 2)
#define	BCM4313_MI_BCNSUCCESS		(1 << 3)
#define	BCM4313_MI_BCNCANCLD		(1 << 4)
#define	BCM4313_MI_ATIMWINEND		(1 << 5)
#define	BCM4313_MI_PMQ			(1 << 6)
#define	BCM4313_MI_MACTXERR		(1 << 9)
#define	BCM4313_MI_PHYTXERR		(1 << 11)
#define	BCM4313_MI_PME			(1 << 12)
#define	BCM4313_MI_DMAINT		(1 << 15)
#define	BCM4313_MI_TXSTOP		(1 << 16)
#define	BCM4313_MI_PWRUP		(1 << 21)
#define	BCM4313_MI_TFS			(1 << 29)
#define	BCM4313_MI_PHYCHANGED		(1 << 30)
#define	BCM4313_MI_TO			(1U << 31)

/* maccontrol bits (MCTL_*). */
#define	BCM4313_MCTL_GMODE		(1U << 31)
#define	BCM4313_MCTL_PROMISC		(1 << 24)
#define	BCM4313_MCTL_KEEPBADFCS		(1 << 23)
#define	BCM4313_MCTL_KEEPCONTROL	(1 << 22)
#define	BCM4313_MCTL_PHYLOCK		(1 << 21)
#define	BCM4313_MCTL_AP			(1 << 18)
#define	BCM4313_MCTL_INFRA		(1 << 17)
#define	BCM4313_MCTL_SHM_EN		(1 << 8)
#define	BCM4313_MCTL_PSM_JMP_0		(1 << 2)
#define	BCM4313_MCTL_PSM_RUN		(1 << 1)
#define	BCM4313_MCTL_EN_MAC		(1 << 0)

/* maccommand bits. */
#define	BCM4313_MCMD_BCN0VLD		(1 << 0)
#define	BCM4313_MCMD_BCN1VLD		(1 << 1)
#define	BCM4313_MCMD_DIRFRMQVAL		(1 << 2)
#define	BCM4313_MCMD_CCA		(1 << 3)
#define	BCM4313_MCMD_BG_NOISE		(1 << 4)

/* objaddr register. */
#define	BCM4313_OBJADDR_SEL_MASK	0x000F0000
#define	BCM4313_OBJADDR_UCM_SEL		0x00000000
#define	BCM4313_OBJADDR_SHM_SEL		0x00010000
#define	BCM4313_OBJADDR_SCR_SEL		0x00020000
#define	BCM4313_OBJADDR_IHR_SEL		0x00030000
#define	BCM4313_OBJADDR_WINC		0x01000000
#define	BCM4313_OBJADDR_RINC		0x02000000
#define	BCM4313_OBJADDR_AUTO_INC	0x03000000

/* clk_ctl_st bits (HT clock request/avail, 4313). */
#define	BCM4313_CCS_REQ_HT		0x00000010
#define	BCM4313_CCS_AVAIL_HT		0x00020000

/*
 * D11 registers used by the LCN-PHY code (offsets from brcmsmac d11.h):
 * the spuravoid MAC-frequency switch (tsf_clk_frac_*), the dummy-TX
 * template-RAM path, and the sample-collector used by calibration.
 */
#define	BCM4313_D11_TPLATEWRPTR		0x130	/* tplatewrptr */
#define	BCM4313_D11_TPLATEWRDATA	0x134	/* tplatewrdata */
#define	BCM4313_D11_PSM_PHY_HDR_PARAM	0x492
#define	BCM4313_D11_PSM_CORECTLSTS	0x4f0	/* corerev >= 13 */
#define	BCM4313_D11_TXE_CTL		0x500
#define	BCM4313_D11_TXE_AUX		0x502
#define	BCM4313_D11_TXE_WM_0		0x508
#define	BCM4313_D11_TXE_WM_1		0x50a
#define	BCM4313_D11_TXE_PHYCTL		0x50c
#define	BCM4313_D11_TXE_STATUS		0x50e
#define	BCM4313_D11_TXE_PHYCTL1		0x514
#define	BCM4313_D11_SMPL_CLCT_STRPTR	0x552	/* corerev >= 22 */
#define	BCM4313_D11_SMPL_CLCT_STPPTR	0x554
#define	BCM4313_D11_SMPL_CLCT_CURPTR	0x556
#define	BCM4313_D11_XMTTPLATETXPTR	0x54c
#define	BCM4313_D11_XMTSEL		0x568
#define	BCM4313_D11_XMTTXC_NT		0x56a
#define	BCM4313_D11_IFSSTAT		0x690
#define	BCM4313_D11_WEPCTL		0x7c0
#define	BCM4313_D11_TSF_CLK_FRAC_L	0x62e
#define	BCM4313_D11_TSF_CLK_FRAC_H	0x630

/*
 * Shared-memory counters written/read by the LCN-PHY calibration paths.
 * M_UCODE_MACSTAT = 0x0e0; struct macstat.txallfrm is at +0x80.
 */
#define	BCM4313_M_CTS_DURATION		0x0b8	/* M_PSM_SOFT_REGS + 0x5c*2 */
#define	BCM4313_M_UCODE_MACSTAT_TXALLFRM	0x160

/*
 * Shared-memory (SHM) byte offsets; M_* names and values from brcmsmac
 * d11.h.  SHM is addressed in 16-bit words via the OBJADDR/OBJDATA window
 * (OBJADDR_SHM_SEL | (byteoff >> 1)).
 */
#define	BCM4313_M_MACHW_VER		(0x00b * 2)
#define	BCM4313_M_MACHW_CAP_L		(0x060 * 2)
#define	BCM4313_M_MACHW_CAP_H		(0x061 * 2)
#define	BCM4313_M_EDCF_STATUS_OFF	(0x007 * 2)
#define	BCM4313_M_DOT11_SLOT		(0x008 * 2)
#define	BCM4313_M_DOT11_DTIMPERIOD	(0x009 * 2)
#define	BCM4313_M_BCN0_FRM_BYTESZ	(0x00c * 2)
#define	BCM4313_M_MAXRXFRM_LEN		(0x010 * 2)
#define	BCM4313_M_RSP_PCTLWD		(0x011 * 2)
#define	BCM4313_M_TXPWR_N		(0x012 * 2)
#define	BCM4313_M_TXPWR_TARGET		(0x013 * 2)
#define	BCM4313_M_TXPWR_MAX		(0x014 * 2)
#define	BCM4313_M_TXPWR_CUR		(0x019 * 2)
#define	BCM4313_M_RX_PAD_DATA_OFFSET	(0x01a * 2)
#define	BCM4313_M_PHYVER		(0x028 * 2)
#define	BCM4313_M_PHYTYPE		(0x029 * 2)
#define	BCM4313_M_HOST_FLAGS1		(0x02f * 2)
#define	BCM4313_M_HOST_FLAGS2		(0x030 * 2)
#define	BCM4313_M_FIFOSIZE0		(0x04c * 2)
#define	BCM4313_M_FIFOSIZE1		(0x04d * 2)
#define	BCM4313_M_FIFOSIZE2		(0x04e * 2)
#define	BCM4313_M_FIFOSIZE3		(0x04f * 2)
#define	BCM4313_M_CURCHANNEL		(0x050 * 2)
#define	BCM4313_M_BCMC_FID		(0x054 * 2)
#define	BCM4313_M_EDCF_QINFO		(0x120 * 2)
#define	BCM4313_M_TXF_CUR_INDEX		(0x018 * 2)

/*
 * ---------------------------------------------------------------------------
 * DMA (dma64) -- fifo64 register blocks at 0x200, one per FIFO.
 * Each FIFO: dma64regs (tx) + pio (tx) + dma64regs (rx) + pio (rx) = 0x40.
 * Verified against brcmsmac d11.h/dma.h.
 * ---------------------------------------------------------------------------
 */
#define	BCM4313_D11_DMA_BASE		0x200
#define	BCM4313_D11_FIFO_STRIDE		0x40
#define	BCM4313_D11_FIFO_DMA_TX(fifo)	(BCM4313_D11_DMA_BASE + \
    (fifo) * BCM4313_D11_FIFO_STRIDE)
#define	BCM4313_D11_FIFO_DMA_RX(fifo)	(BCM4313_D11_DMA_BASE + \
    (fifo) * BCM4313_D11_FIFO_STRIDE + 0x20)

/* FIFO numbers (brcmsmac d11.h). */
#define	BCM4313_RX_FIFO			0	/* data + ctl frames */
#define	BCM4313_TX_BE_FIFO		1	/* best-effort data */
#define	BCM4313_RX_TXSTATUS_FIFO	3	/* tx status packets */

/* dma64 per-channel registers (struct dma64regs). */
#define	BCM4313_DMA64_CTL		0x00	/* control */
#define	BCM4313_DMA64_PTR		0x04	/* last descriptor posted */
#define	BCM4313_DMA64_ADDRLOW		0x08	/* ring base low (8K align) */
#define	BCM4313_DMA64_ADDRHIGH		0x0C	/* ring base high */
#define	BCM4313_DMA64_STATUS0		0x10	/* current descriptor */
#define	BCM4313_DMA64_STATUS1		0x14	/* active descriptor / error */

/* dma64 transmit channel control. */
#define	BCM4313_D64_XC_XE		0x00000001 /* transmit enable */
#define	BCM4313_D64_XC_SE		0x00000002 /* suspend request */
#define	BCM4313_D64_XC_LE		0x00000004 /* loopback */
#define	BCM4313_D64_XC_FL		0x00000010 /* flush request */
#define	BCM4313_D64_XC_PD		0x00000800 /* parity check disable */
#define	BCM4313_D64_XC_AE		0x00030000 /* addr ext bits */

/* dma64 receive channel control. */
#define	BCM4313_D64_RC_RE		0x00000001 /* receive enable */
#define	BCM4313_D64_RC_RO_MASK		0x000000fe /* rx frame offset */
#define	BCM4313_D64_RC_RO_SHIFT		1
#define	BCM4313_D64_RC_FM		0x00000100 /* direct fifo mode */
#define	BCM4313_D64_RC_SH		0x00000200 /* separate rx hdr desc */
#define	BCM4313_D64_RC_OC		0x00000400 /* overflow continue */
#define	BCM4313_D64_RC_PD		0x00000800 /* parity check disable */
#define	BCM4313_D64_RC_AE		0x00030000 /* addr ext bits */

/* dma64 channel status. */
#define	BCM4313_D64_XS0_CD_MASK		0x00001fff /* current descriptor */
#define	BCM4313_D64_XS0_XS_MASK		0xf0000000 /* transmit state */
#define	BCM4313_D64_XS0_XS_SHIFT	28
#define	BCM4313_D64_XS0_XS_DISABLED	0x00000000
#define	BCM4313_D64_XS0_XS_ACTIVE	0x10000000
#define	BCM4313_D64_XS0_XS_IDLE		0x20000000
#define	BCM4313_D64_XS0_XS_STOPPED	0x30000000
#define	BCM4313_D64_RS0_CD_MASK		0x00001fff /* receive: current desc */
#define	BCM4313_D64_RS0_RS_MASK		0xf0000000 /* receive state */

/* dma64 descriptor control words. */
#define	BCM4313_D64_CTRL1_EOT		(1U << 28) /* end of descriptor table */
#define	BCM4313_D64_CTRL1_IOC		(1U << 29) /* interrupt on completion */
#define	BCM4313_D64_CTRL1_EOF		(1U << 30) /* end of frame */
#define	BCM4313_D64_CTRL1_SOF		(1U << 31) /* start of frame */
#define	BCM4313_D64_CTRL2_BC_MASK	0x00007fff /* buffer byte count */
#define	BCM4313_D64_CTRL2_AE		0x00030000 /* address extension */
#define	BCM4313_D64_CTRL2_AE_SHIFT	16

/* Descriptor rings must be 8KB-aligned and fit within 8KB. */
#define	BCM4313_D64_RINGALIGN		8192

struct bcm4313_dma64desc {
	uint32_t	ctrl1;		/* control word 1 (EOT/IOC/EOF/SOF) */
	uint32_t	ctrl2;		/* control word 2 (byte count) */
	uint32_t	addrlow;	/* data buffer address, bits 31:0 */
	uint32_t	addrhigh;	/* data buffer address, bits 63:32 */
} __packed;

/*
 * ---------------------------------------------------------------------------
 * D11 TX DMA header (struct d11txh) -- 112 bytes, placed at the start of
 * the TX buffer, immediately followed by the 802.11 frame.  The MAC core
 * parses this header and prepends the PLCP itself.  Layout verified against
 * brcmsmac d11.h (D11_TXH_LEN == 112).
 * ---------------------------------------------------------------------------
 */
struct bcm4313_d11txh {
	uint16_t	MacTxControlLow;	/* 0x00 */
	uint16_t	MacTxControlHigh;	/* 0x02 */
	uint16_t	MacFrameControl;	/* 0x04 */
	uint16_t	TxFesTimeNormal;	/* 0x06 */
	uint16_t	PhyTxControlWord;	/* 0x08 */
	uint16_t	PhyTxControlWord_1;	/* 0x0a */
	uint16_t	PhyTxControlWord_1_Fbr;	/* 0x0c */
	uint16_t	PhyTxControlWord_1_Rts;	/* 0x0e */
	uint16_t	PhyTxControlWord_1_FbrRts; /* 0x10 */
	uint16_t	MainRates;		/* 0x12 */
	uint16_t	XtraFrameTypes;		/* 0x14 */
	uint8_t		IV[16];			/* 0x16 */
	uint8_t		TxFrameRA[6];		/* 0x26 */
	uint16_t	TxFesTimeFallback;	/* 0x2c */
	uint8_t		RTSPLCPFallback[6];	/* 0x2e */
	uint16_t	RTSDurFallback;		/* 0x34 */
	uint8_t		FragPLCPFallback[6];	/* 0x36 */
	uint16_t	FragDurFallback;	/* 0x3c */
	uint16_t	MModeLen;		/* 0x3e */
	uint16_t	MModeFbrLen;		/* 0x40 */
	uint16_t	TstampLow;		/* 0x42 */
	uint16_t	TstampHigh;		/* 0x44 */
	uint16_t	ABI_MimoAntSel;		/* 0x46 */
	uint16_t	PreloadSize;		/* 0x48 */
	uint16_t	AmpduSeqCtl;		/* 0x4a */
	uint16_t	TxFrameID;		/* 0x4c */
	uint16_t	TxStatus;		/* 0x4e */
	uint16_t	MaxNMpdus;		/* 0x50 */
	uint16_t	MaxABytes_MRT;		/* 0x52 */
	uint16_t	MaxABytes_FBR;		/* 0x54 */
	uint16_t	MinMBytes;		/* 0x56 */
	uint8_t		RTSPhyHeader[6];	/* 0x58 */
	uint8_t		rts_frame[16];		/* 0x5e */
	uint16_t	PAD;			/* 0x6e */
} __packed __aligned(2);
#define	BCM4313_D11_TXH_LEN		(sizeof(struct bcm4313_d11txh)) /* 112 */

/* MacTxControlLow bits (TXC_*). */
#define	BCM4313_TXC_AMIC		0x8000
#define	BCM4313_TXC_SENDCTS		0x0800
#define	BCM4313_TXC_AMPDU_MASK		0x0600
#define	BCM4313_TXC_BW_40		0x0100
#define	BCM4313_TXC_FREQBAND_5G		0x0080
#define	BCM4313_TXC_DFCS		0x0040
#define	BCM4313_TXC_IGNOREPMQ		0x0020
#define	BCM4313_TXC_HWSEQ		0x0010
#define	BCM4313_TXC_STARTMSDU		0x0008
#define	BCM4313_TXC_SENDRTS		0x0004
#define	BCM4313_TXC_LONGFRAME		0x0002
#define	BCM4313_TXC_IMMEDACK		0x0001

/* MacTxControlHigh bits. */
#define	BCM4313_TXC_PREAMBLE_RTS_FB_SHORT	0x8000
#define	BCM4313_TXC_PREAMBLE_RTS_MAIN_SHORT	0x4000
#define	BCM4313_TXC_PREAMBLE_DATA_FB_SHORT	0x2000

/* PhyTxControlWord bits (LCN PHY). */
#define	BCM4313_PHY_TXC_PWR_MASK	0xFC00
#define	BCM4313_PHY_TXC_PWR_SHIFT	10
#define	BCM4313_PHY_TXC_ANT_MASK	0x03C0
#define	BCM4313_PHY_TXC_ANT_SHIFT	6
#define	BCM4313_PHY_TXC_LCNPHY_ANT_LAST	0x0000	/* auto, last rx ant */
#define	BCM4313_PHY_TXC_SHORT_HDR	0x0010

/* PhyTxControlWord_1 bits. */
#define	BCM4313_PHY_TXC1_BW_MASK	0x0007
#define	BCM4313_PHY_TXC1_BW_20MHZ	2
#define	BCM4313_PHY_TXC1_MODE_SHIFT	3
#define	BCM4313_PHY_TXC1_MODE_MASK	0x0038
#define	BCM4313_PHY_TXC1_MODE_SISO	0

/* XtraFrameTypes. */
#define	BCM4313_XFTS_CHANNEL_SHIFT	8

/*
 * ---------------------------------------------------------------------------
 * D11 RX DMA header (struct d11rxhdr) -- 24 bytes at the start of every
 * received buffer, followed by the 802.11 frame (FCS already stripped when
 * MCTL_KEEPBADFCS is clear).  Layout verified against brcmsmac d11.h.
 * ---------------------------------------------------------------------------
 */
struct bcm4313_d11rxhdr {
	uint16_t	RxFrameSize;	/* actual frame length */
	uint16_t	PAD;
	uint16_t	PhyRxStatus_0;
	uint16_t	PhyRxStatus_1;
	uint16_t	PhyRxStatus_2;
	uint16_t	PhyRxStatus_3;
	uint16_t	PhyRxStatus_4;
	uint16_t	PhyRxStatus_5;
	uint16_t	RxStatus1;	/* ucode MAC rx status */
	uint16_t	RxStatus2;	/* extended MAC rx status */
	uint16_t	RxTSFTime;
	uint16_t	RxChan;
} __packed;
#define	BCM4313_D11_RXH_LEN		(sizeof(struct bcm4313_d11rxhdr)) /* 24 */

/* RxStatus1 (RXS_*). */
#define	BCM4313_RXS_BCNSENT		0x8000
#define	BCM4313_RXS_DECERR		(1 << 4)
#define	BCM4313_RXS_DECATMPT		(1 << 3)
#define	BCM4313_RXS_PBPRES		(1 << 2) /* pad bytes present */
#define	BCM4313_RXS_RESPFRAMETX		(1 << 1)
#define	BCM4313_RXS_FCSERR		(1 << 0)

/* PhyRxStatus_2 (HTPHY/LCN). */
#define	BCM4313_PRXS2_HTPHY_RXPWR_ANT0	0xFF00

/* TX status packet (16 bytes, delivered via RX_TXSTATUS_FIFO). */
struct bcm4313_tx_status {
	uint16_t	framelen;
	uint16_t	PAD;
	uint16_t	frameid;
	uint16_t	status;
	uint16_t	lasttxtime;
	uint16_t	sequence;
	uint16_t	phyerr;
	uint16_t	ackphyrxsh;
} __packed;
#define	BCM4313_TXSTATUS_LEN		16

/*
 * ---------------------------------------------------------------------------
 * LCN-PHY.
 *
 * BCM4313 uses the "LCN" PHY (PHY_TYPE_LCN = 8, rev 1).  16-bit PHY
 * registers are accessed indirectly through D11_PHYREGADDR/D11_PHYREGDATA
 * (0x3FC/0x3FE); the 2.4GHz radio (BCM2056) is accessed through
 * D11_RADIOADDR/D11_RADIODATA (0x3D8/0x3DA).
 *
 * The LCN-PHY uses an *indirect table* write mechanism (brcmsmac
 * wlc_phy_write_table with tblAddr=0x455, tblDataHi=0x457,
 * tblDataLo=0x456): phyreg 0x455 is loaded with (tbl_id << 10) | index and
 * the 8/16/32-bit data is then written through 0x456/0x457.
 *
 * The BCM4313 tuning data (switch-control tables and RX-gain tables) is
 * bundled byte-for-byte in bcm4313_lcntab.h, extracted verbatim from
 * brcmsmac phy/phytbl_lcn.c, and selected below from the SPROM board flags
 * exactly as brcmsmac does in wlc_lcnphy_init().
 * ---------------------------------------------------------------------------
 */
#define	BCM4313_PHY_TYPE_LCN		8	/* PHY_TYPE_LCN, brcmsmac */
#define	BCM4313_PHY_REV_LCN		1

/* LCN-PHY indirect table-write phyregs (phy_cmn.c). */
#define	BCM4313_LCN_TBLADDR		0x455
#define	BCM4313_LCN_TBLDATAHI		0x457
#define	BCM4313_LCN_TBLDATALO		0x456

/*
 * SPROM board flags used to pick the BCM4313 tuning (brcmsmac types.h).
 * Values verified against brcmsmac/types.h (BFL_FEM=0x800, BFL_FEM_BT=
 * 0x00400000, BFL_EXTLNA=0x1000).
 */
#define	BCM4313_BFL_FEM			0x00000800	/* FEM 2.4GHz */
#define	BCM4313_BFL_FEM_BT		0x00400000	/* FEM + Bluetooth */
#define	BCM4313_BFL_EXTLNA		0x00001000	/* external LNA 2.4GHz */
#define	BCM4313_BFL_NOPA		0x00010000	/* no on-board PA */
#define	BCM4313_BFL_EXTLNA_5GHz		0x10000000

/*
 * LCN-PHY constants (brcmsmac phy/phy_lcn.c and phy/phy_int.h).
 */
#define	BCM4313_LCN_AFE_CLK_INIT_MODE_TXRX2X	1
#define	BCM4313_LCN_AFE_CLK_INIT_MODE_PAPD	0

#define	BCM4313_LCN_TBL_ID_IQLOCAL		0x00
#define	BCM4313_LCN_TBL_ID_TXPWRCTL		0x07
#define	BCM4313_LCN_TBL_ID_RFSEQ		0x08
#define	BCM4313_LCN_TBL_ID_GAIN_IDX		0x0d
#define	BCM4313_LCN_TBL_ID_SW_CTRL		0x0f
#define	BCM4313_LCN_TBL_ID_GAIN_TBL		0x12
#define	BCM4313_LCN_TBL_ID_SPUR			0x14
#define	BCM4313_LCN_TBL_ID_SAMPLEPLAY		0x15
#define	BCM4313_LCN_TBL_ID_SAMPLEPLAY1		0x16
#define	BCM4313_LCN_TBL_ID_PAPDCOMPDELTATBL	0x18

#define	BCM4313_LCN_TX_PWR_CTRL_RATE_OFFSET	832
#define	BCM4313_LCN_TX_PWR_CTRL_MAC_OFFSET	128
#define	BCM4313_LCN_TX_PWR_CTRL_GAIN_OFFSET	192
#define	BCM4313_LCN_TX_PWR_CTRL_IQ_OFFSET	320
#define	BCM4313_LCN_TX_PWR_CTRL_LO_OFFSET	448
#define	BCM4313_LCN_TX_PWR_CTRL_PWR_OFFSET	576
#define	BCM4313_LCN_TX_PWR_CTRL_START_INDEX_2G_4313	140
#define	BCM4313_LCN_TX_PWR_CTRL_START_NPT	1
#define	BCM4313_LCN_TX_PWR_CTRL_MAX_NPT		7
#define	BCM4313_LCN_NOISE_SAMPLES_DEFAULT	5000

#define	BCM4313_LCN_TX_PWR_CTRL_OFF		0
#define	BCM4313_LCN_TX_PWR_CTRL_SW		(0x1 << 15)
#define	BCM4313_LCN_TX_PWR_CTRL_HW		(0x1 << 15 | 0x1 << 14 | 0x1 << 13)
#define	BCM4313_LCN_TX_PWR_CTRL_TEMPBASED	0xE001

#define	BCM4313_LCN_FIXED_TXPWR		78
#define	BCM4313_LCN_TARGET_PWR		60
#define	BCM4313_LCN_TEMPSENSE(val)	\
	((int16_t)(((val) > 255) ? ((val) - 512) : (val)))
#define	BCM4313_LCN_TEMPSENSE_OFFSET	80812
#define	BCM4313_LCN_TEMPSENSE_DEN	2647
#define	BCM4313_LCN_VBAT_SCALE_NOM	53
#define	BCM4313_LCN_VBAT_SCALE_DEN	432
#define	BCM4313_LCN_TEMPSENSE_MODE	1	/* TEMPSENSE */
#define	BCM4313_LCN_VBATSENSE_MODE	2	/* VBATSENSE */
#define	BCM4313_LCNPHY_TSSI_PRE_PA	0
#define	BCM4313_LCNPHY_TSSI_POST_PA	1
#define	BCM4313_LCNPHY_TSSI_EXT		2
#define	BCM4313_LCNPHY_CAL_FULL		0
#define	BCM4313_LCNPHY_CAL_RECAL		1

/* Radio 2064 PLL tuning constants (phy_lcn.c). */
#define	BCM4313_LCN_PLL_2064_MHZ		1000000
#define	BCM4313_LCN_PLL_2064_LOOP_BW_DOUBLER	200
#define	BCM4313_LCN_PLL_2064_D30_DOUBLER	10500
#define	BCM4313_LCN_PLL_2064_LOW_END_VCO		3000
#define	BCM4313_LCN_PLL_2064_HIGH_END_VCO	4200
#define	BCM4313_LCN_PLL_2064_LOW_END_KVCO	27
#define	BCM4313_LCN_PLL_2064_HIGH_END_KVCO	68
#define	BCM4313_LCN_BW_LMT			200
#define	BCM4313_LCN_CUR_LMT			1250
#define	BCM4313_LCN_MULT			1
#define	BCM4313_LCN_VCO_DIV			30
#define	BCM4313_LCN_OFFSET			680
#define	BCM4313_LCN_FACT			490
#define	BCM4313_LCN_CUR_DIV			2640

/* TX power table rate indices (brcmsmac main.h). */
#define	BCM4313_NUM_RATES_CCK			4
#define	BCM4313_NUM_RATES_OFDM			8
#define	BCM4313_NUM_RATES_MCS_1_STREAM		8
#define	BCM4313_TXP_NUM_RATES			40
#define	BCM4313_TXP_FIRST_CCK			0
#define	BCM4313_TXP_LAST_CCK			3
#define	BCM4313_TXP_FIRST_OFDM			4
#define	BCM4313_TXP_LAST_OFDM			11
#define	BCM4313_TXP_FIRST_SISO_MCS_20		12
#define	BCM4313_TXP_LAST_SISO_MCS_20		19

/*
 * LCN-PHY driver state (transcribed from struct brcms_phy_lcnphy and
 * struct lcnphy_cal_results; only the fields used by the ported code).
 */
struct bcm4313_lcnphy_cal_results {
	uint16_t	txiqlocal_bestcoeffs[11];
	bool		txiqlocal_bestcoeffs_valid;
	uint16_t	txiqlocal_a;
	uint16_t	txiqlocal_b;
	uint16_t	txiqlocal_didq;
	uint8_t		txiqlocal_ei0;
	uint8_t		txiqlocal_eq0;
	uint8_t		txiqlocal_fi0;
	uint8_t		txiqlocal_fq0;
	uint16_t	rxiqcal_coeff_a0;
	uint16_t	rxiqcal_coeff_b0;
};

struct bcm4313_lcnphy {
	uint8_t		lcnphy_full_cal_channel;
	uint8_t		lcnphy_cal_counter;
	uint16_t	lcnphy_cal_temper;
	bool		lcnphy_recal;

	uint32_t	lcnphy_mcs20_po;

	uint8_t		lcnphy_tr_isolation_mid;
	uint8_t		lcnphy_rx_power_offset;
	uint8_t		lcnphy_rssi_vf;
	uint8_t		lcnphy_rssi_vc;
	uint8_t		lcnphy_rssi_gs;
	uint8_t		lcnphy_rssi_vf_lowtemp;
	uint8_t		lcnphy_rssi_vc_lowtemp;
	uint8_t		lcnphy_rssi_gs_lowtemp;
	uint8_t		lcnphy_rssi_vf_hightemp;
	uint8_t		lcnphy_rssi_vc_hightemp;
	uint8_t		lcnphy_rssi_gs_hightemp;
	int8_t		lcnphy_lastsensed_temperature;

	uint16_t	lcnphy_rawtempsense;
	uint8_t		lcnphy_measPower;
	uint8_t		lcnphy_tempsense_slope;
	uint8_t		lcnphy_tempsense_option;
	uint8_t		lcnphy_tempcorrx;
	uint8_t		lcnphy_freqoffset_corr;
	bool		lcnphy_iqcal_swp_dis;
	bool		lcnphy_hw_iqcal_en;
	int		lcnphy_bandedge_corr;
	bool		lcnphy_spurmod;
	uint16_t	lcnphy_tssi_tx_cnt;
	uint16_t	lcnphy_tssi_idx;
	uint16_t	lcnphy_tssi_npt;
	int16_t		lcnphy_cck_dig_filt_type;

	int8_t		lcnphy_tx_power_idx_override;
	uint16_t	lcnphy_noise_samples;

	uint32_t	lcnphy_gain_idx_14_lowword;
	uint32_t	lcnphy_gain_idx_14_hiword;
	int16_t		lcnphy_ofdmgainidxtableoffset;
	int16_t		lcnphy_dsssgainidxtableoffset;
	uint32_t	lcnphy_tr_R_gain_val;
	uint32_t	lcnphy_tr_T_gain_val;
	int8_t		lcnphy_input_pwr_offset_db;
	uint16_t	lcnphy_Med_Low_Gain_db;
	uint16_t	lcnphy_Very_Low_Gain_db;

	struct bcm4313_lcnphy_cal_results lcnphy_cal_results;

	uint8_t		lcnphy_current_index;

	int8_t		tx_power_offset[BCM4313_TXP_NUM_RATES];
};

/* Forward declarations (must precede the prototypes below). */
struct bcm4313_softc;
struct bcm4313_vap;
struct bcm4313_phytbl;

/* Accessors exported from if_bcm4313.c for the LCN-PHY layer. */
uint16_t bcm4313_read_2(struct bcm4313_softc *, uint16_t);
void bcm4313_write_2(struct bcm4313_softc *, uint16_t, uint16_t);
uint32_t bcm4313_read_4(struct bcm4313_softc *, uint16_t);
void bcm4313_write_4(struct bcm4313_softc *, uint16_t, uint32_t);
uint16_t bcm4313_shm_read_2(struct bcm4313_softc *, uint16_t);
void bcm4313_shm_write_2(struct bcm4313_softc *, uint16_t, uint16_t);
uint16_t bcm4313_phy_read(struct bcm4313_softc *, uint16_t);
void bcm4313_phy_write(struct bcm4313_softc *, uint16_t, uint16_t);
void bcm4313_phy_maskset(struct bcm4313_softc *, uint16_t, uint16_t, uint16_t);
uint16_t bcm4313_radio_read(struct bcm4313_softc *, uint16_t);
void bcm4313_radio_write(struct bcm4313_softc *, uint16_t, uint16_t);
uint32_t bcm4313_radio_read32(struct bcm4313_softc *, uint16_t);
void bcm4313_radio_maskset(struct bcm4313_softc *, uint16_t, uint16_t, uint16_t);
void bcm4313_lcnphy_write_table(struct bcm4313_softc *, const struct bcm4313_phytbl *);
void bcm4313_lcnphy_read_table(struct bcm4313_softc *, struct bcm4313_phytbl *);
void bcm4313_mac_enable(struct bcm4313_softc *);
void bcm4313_mac_disable(struct bcm4313_softc *);

/* LCN-PHY entry points (if_bcm4313_phy_lcn.c). */
void bcm4313_lcnphy_init(struct bcm4313_softc *);
void bcm4313_lcnphy_set_chanspec(struct bcm4313_softc *, uint8_t);
void bcm4313_lcnphy_calib_modes(struct bcm4313_softc *, uint32_t);
void bcm4313_lcnphy_cal_init(struct bcm4313_softc *);
void bcm4313_lcnphy_txpower_recalc_target(struct bcm4313_softc *);
void bcm4313_lcnphy_txpwr_srom_read(struct bcm4313_softc *);
/* Calibration modes (brcmsmac phy_hal.h). */
#define	BCM4313_LCNPHY_PERICAL_WATCHDOG	2	/* PHY_PERICAL_WATCHDOG */
#define	BCM4313_LCNPHY_PERICAL_PHYINIT	3	/* PHY_PERICAL_PHYINIT */
#define	BCM4313_LCNPHY_PERICAL_CHAN	7	/* PHY_PERICAL_CHAN */
#define	BCM4313_LCN_FULLCAL		8	/* PHY_FULLCAL */
#define	BCM4313_LCNPHY_PERICAL_TEMPBASED_TXPWRCTRL	9

/*
 * ---------------------------------------------------------------------------
 * D11 microcode (firmware) upload.
 *
 * BCM4313 (D11 rev 24, LCN PHY) needs the LCN microcode from the bundled
 * brcm/bcm43xx-0.fw (+ _hdr-0.fw).  The firmware image is a TLV blob of
 * ucode sections; the header file is a list of {offset, len, ucode_idx}
 * entries.  brcmsmac selects each section by its numeric tag.  Only the
 * LCN-relevant sections are loaded.
 * ---------------------------------------------------------------------------
 */
struct bcm4313_fw_hdr {
	uint32_t	offset;	/* byte offset of section in the .fw blob */
	uint32_t	len;	/* section length, bytes */
	uint32_t	idx;	/* ucode section tag (see below) */
} __packed;
#define	BCM4313_FW_PATH		"/boot/modules/brcm/"
#define	BCM4313_FW_NAME			"bcm43xx-0.fw"
#define	BCM4313_FW_HDR_NAME		"bcm43xx_hdr-0.fw"

/* Ucode section tags (ucode_loader.h enum). */
#define	BCM4313_D11LCN0BSINITVALS24	1	/* LCN band-selective init */
#define	BCM4313_D11LCN0INITVALS24	2	/* LCN MAC-core init */
#define	BCM4313_D11UCODE_OVERSIGHT24_LCN	12	/* the LCN microcode (ucode_loader.h enum) */
#define	BCM4313_D11UCODE_OVERSIGHT24_LCNSZ 13	/* LCN ucode size (words) */

/* A single ucode/SHM/IHR register-tweak directive (struct d11init). */
struct bcm4313_d11init {
	uint16_t	addr;
	uint16_t	size;	/* 2 or 4 */
	uint32_t	value;
} __packed;

/*
 * ---------------------------------------------------------------------------
 * Driver state.
 * ---------------------------------------------------------------------------
 */

/* One DMA ring slot. */
struct bcm4313_slot {
	bus_dmamap_t		s_dmap;		/* map of s_m (or txhdr) */
	struct mbuf		*s_m;		/* rx mbuf / tx mbuf */
	struct ieee80211_node	*s_ni;		/* tx node (tx rings) */
	uint8_t			s_type;		/* slot type (tx rings) */
#define	BCM4313_SLOT_HEADER	0
#define	BCM4313_SLOT_BODY	1
};

/* A dma64 descriptor ring. */
struct bcm4313_ring {
	struct bcm4313_softc	*r_sc;
	uint16_t		r_base;		/* D11 dma64 reg base */
	int			r_nslots;
	int			r_tx;		/* 1 = tx direction ring */
	uint16_t		r_bufsz;	/* rx buffer size */
	/* ring descriptor memory */
	bus_dma_tag_t		r_dtag;
	bus_dmamap_t		r_dmap;
	struct bcm4313_dma64desc *r_desc;	/* kva of descriptor ring */
	bus_addr_t		r_paddr;	/* host phys of ring */
	/* producer/consumer indexes (slot units) */
	int			r_in;		/* next slot to reclaim/harvest */
	int			r_out;		/* next slot to post */
	/* per-slot state */
	struct bcm4313_slot	*r_slots;
	/* tx header cache (tx rings only) */
	uint8_t			*r_txhdr;	/* consistent txh cache */
	bus_dmamap_t		r_txhdr_dmap;	/* dma map of cache region */
	bus_addr_t		r_txhdr_pa;	/* phys base of cache region */
	int			r_nframes;	/* # frames in txh cache */
};

/* Radiotap capture (monitor mode). */
#define	BCM4313_RX_RADIOTAP_PRESENT ( \
	(1 << IEEE80211_RADIOTAP_FLAGS) | \
	(1 << IEEE80211_RADIOTAP_RATE) | \
	(1 << IEEE80211_RADIOTAP_CHANNEL) | \
	(1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) | \
	0)
struct bcm4313_rx_radiotap_header {
	struct ieee80211_radiotap_header wr_ihdr;
	uint8_t		wr_flags;
	uint8_t		wr_rate;
	uint16_t	wr_chan_freq;
	uint16_t	wr_chan_flags;
	int8_t		wr_antsignal;
} __packed __aligned(8);

#define	BCM4313_TX_RADIOTAP_PRESENT ( \
	(1 << IEEE80211_RADIOTAP_FLAGS) | \
	(1 << IEEE80211_RADIOTAP_RATE) | \
	(1 << IEEE80211_RADIOTAP_CHANNEL) | \
	0)
struct bcm4313_tx_radiotap_header {
	struct ieee80211_radiotap_header wt_ihdr;
	uint8_t		wt_flags;
	uint8_t		wt_rate;
	uint16_t	wt_chan_freq;
	uint16_t	wt_chan_flags;
} __packed __aligned(8);

struct bcm4313_softc {
	device_t		sc_dev;
	struct resource		*sc_mem_res;	/* D11 core registers */
	int			sc_mem_rid;
	struct resource		*sc_irq;	/* interrupt */
	int			sc_irq_rid;
	void			*sc_ih;

	struct mtx		sc_mtx;
#define	BCM4313_LOCK_INIT(sc)	mtx_init(&(sc)->sc_mtx, \
    device_get_nameunit((sc)->sc_dev), MTX_NETWORK_LOCK, MTX_DEF)
#define	BCM4313_LOCK_DESTROY(sc) mtx_destroy(&(sc)->sc_mtx)
#define	BCM4313_LOCK(sc)	mtx_lock(&(sc)->sc_mtx)
#define	BCM4313_UNLOCK(sc)	mtx_unlock(&(sc)->sc_mtx)
#define	BCM4313_ASSERT_LOCKED(sc) mtx_assert(&(sc)->sc_mtx, MA_OWNED)

	struct task		sc_intrtask;
	struct taskqueue	*sc_tq;
	struct callout		sc_watchdog_ch;
	int			sc_watchdog_timer;
	int			sc_last_reclaim;	/* ticks, tx reclaim */

	struct ieee80211com	sc_ic;
	struct mbufq		sc_snd;

	unsigned		sc_flags;
#define	BCM4313_FLAG_ATTACHED	(1 << 0)
#define	BCM4313_FLAG_RUNNING	(1 << 1)

	/* radiotap capture (monitor mode) */
	struct bcm4313_rx_radiotap_header sc_rx_th;
	struct bcm4313_tx_radiotap_header sc_tx_th;

	/* bus/chip identification */
	struct bhnd_chipid	sc_cid;
	struct bhnd_board_info	sc_board;

	/* DMA */
	bus_dma_tag_t		sc_dmatag;	/* bhnd translation tag */
	bus_dma_tag_t		sc_bufdtag;	/* data-buffer tag */
	struct bhnd_dma_translation sc_dma_translation;
	struct bcm4313_ring	sc_tx;		/* FIFO 1: data */
	struct bcm4313_ring	sc_rx;		/* FIFO 0: receive */
	struct bcm4313_ring	sc_txstatus;	/* FIFO 3: tx status */

	/* PHY / radio */
	uint8_t			sc_phy_type;
	uint8_t			sc_phy_rev;
	uint8_t			sc_phy_analog;
	uint16_t		sc_radio_id;
	unsigned		sc_phy_flags;
#define	BCM4313_PHYF_CALIBRATED	(1 << 0)

	/* LCN-PHY state + tuning (if_bcm4313_phy_lcn.c) */
	struct bcm4313_lcnphy	sc_lcn;
	uint32_t		sc_xtalfreq;
	bool			sc_hwpwrctrl_capable;
	bool			sc_temppwrctrl_capable;
	int16_t			sc_txpa_2g[3];
	uint8_t			sc_tx_power_min;
	uint8_t			sc_tx_srom_max_rate_2g[BCM4313_TXP_NUM_RATES];
	uint16_t		sc_cck2gpo;
	uint32_t		sc_ofdm2gpo;
	uint16_t		sc_mcs2gpo[2];
	/* bhnd service providers retained at attach */
	device_t		sc_chipc;
	device_t		sc_pmu;
	struct bhnd_resource *sc_cc_res;	/* chipc window (PLL_UPD) */

	/* MAC state */
	uint32_t		sc_intr_mask;
	int			sc_ucode_loaded;	/* D11 LCN ucode uploaded */
	char			sc_fw_path[128];	/* (reserved) firmware search dir */
	uint8_t			sc_bssid[IEEE80211_ADDR_LEN];
	uint16_t		sc_curchan;
	uint16_t		sc_frameid;	/* tx frame id counter */

	/* counters */
	uint32_t		sc_rxgiants;
	uint32_t		sc_rxnobuf;
	uint32_t		sc_txnobuf;
	uint32_t		sc_txreclaimed;
};

/* Per-VAP driver state (net80211 state changes are per-VAP). */
struct bcm4313_vap {
	struct ieee80211vap	bv_vap;
	int			(*bv_newstate)(struct ieee80211vap *,
				    enum ieee80211_state, int);
};
#define	BCM4313_VAP(vap)	((struct bcm4313_vap *)(vap))

#endif /* _IF_BCM4313VAR_H_ */
