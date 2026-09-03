/*-
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
 *
 * if_bcm4313_phy_lcn.c -- BCM4313 LCN-PHY layer for the FreeBSD bcm4313
 * driver (if_bcm4313.c).
 *
 * This file is a faithful FreeBSD port of the LCN-PHY code in the Linux
 * brcmsmac driver (drivers/net/wireless/broadcom/brcm80211/brcmsmac/phy/
 * phy_lcn.c and phy/phy_cmn.c).  The register numbers, table values, and
 * sequence are transcribed from brcmsmac so that a BCM4313 radio tunes,
 * calibrates, and transmits exactly as it does under Linux.  Tuning data
 * lives in bcm4313_lcntab.h / bcm4313_phytbl_lcn.h (extracted verbatim
 * from brcmsmac phy/phytbl_lcn.c).
 *
 * Linux-ism to FreeBSD mapping used throughout:
 *	read_phy_reg(pi, a)	-> bcm4313_phy_read(sc, a)
 *	write_phy_reg(pi, a, v)	-> bcm4313_phy_write(sc, a, v)
 *	mod_phy_reg(pi, a, m, v) -> bcm4313_phy_maskset(sc, a, m, v)
 *	and_phy_reg(pi, a, v)	-> bcm4313_phy_maskset(sc, a, v, 0)
 *	or_phy_reg(pi, a, v)	-> bcm4313_phy_maskset(sc, a, 0xffff, v)
 *	read/write_radio_reg()	-> bcm4313_radio_read/write()
 *	mod/and/or_radio_reg()	-> bcm4313_radio_maskset()
 *	udelay()/mdelay()	-> DELAY()
 *	kmalloc/kfree		-> stack buffers
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/systm.h>
#include <sys/libkern.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/bhnd/bhnd.h>
#include <dev/bhnd/cores/pmu/bhnd_pmu.h>

#include "if_bcm4313var.h"
#include "bcm4313_phytbl_lcn.h"

/* Forward declarations (mutual references among the ported helpers). */
static void bcm4313_lcnphy_start_tx_tone(struct bcm4313_softc *, int32_t,
    uint16_t, bool);
static void bcm4313_lcnphy_stop_tx_tone(struct bcm4313_softc *);
static void bcm4313_lcnphy_set_tx_pwr_by_index(struct bcm4313_softc *, int);
static void bcm4313_lcnphy_txpower_recalc_target_internal(struct bcm4313_softc *);
static uint16_t bcm4313_lcnphy_get_current_tx_pwr_idx_if_pwrctrl_on(
    struct bcm4313_softc *);
static uint16_t bcm4313_lcnphy_get_tx_pwr_npt(struct bcm4313_softc *);
static uint16_t bcm4313_lcnphy_get_target_tx_pwr(struct bcm4313_softc *);
static void bcm4313_lcnphy_set_target_tx_pwr(struct bcm4313_softc *, uint16_t);
static uint16_t bcm4313_lcnphy_tempsense(struct bcm4313_softc *, bool);
static void bcm4313_lcnphy_set_start_tx_pwr_idx(struct bcm4313_softc *, uint16_t);
static void bcm4313_lcnphy_set_tx_pwr_npt(struct bcm4313_softc *, uint16_t);
static void bcm4313_2064_vco_cal(struct bcm4313_softc *);
static bool bcm4313_lcnphy_tempsense_done(struct bcm4313_softc *);
static void bcm4313_lcnphy_tx_power_adjustment(struct bcm4313_softc *);
static uint16_t bcm4313_lcnphy_get_tx_pwr_ctrl(struct bcm4313_softc *);
static void bcm4313_lcnphy_set_tx_pwr_ctrl(struct bcm4313_softc *, uint16_t);
void bcm4313_lcnphy_set_chanspec(struct bcm4313_softc *, uint8_t);

#define	BCM4313_LCNREV_IS(rev, r)	((rev) == (r))
#define	BCM4313_LCN_PHY_TXC_ANT_0	0x0040	/* PHY_TXC_ANT_0 (phy_int.h) */
#define	BCM4313_LCN_PMU_CTL_PLL_UPD	0x00000004 /* BCMA_CC_PMU_CTL bit 2 */

/* wlc_lcnphy_iqcal_active() (phy_lcn.c). */
#define	BCM4313_LCNPHY_IQCAL_ACTIVE(sc)	\
	(bcm4313_phy_read((sc), 0x451) & \
	 ((0x1 << 15) | (0x1 << 14)))

/*
 * Sample-playback CORDIC tone generator (brcmsmac uses lib/math/cordic.c;
 * this is the FreeBSD bwn if_bwn_cordic.h implementation, BSD-licensed).
 * The returned I/Q components are scaled by 2^15; CORDIC_FLOAT() shifts
 * them back to the sample range expected by the sampleplay table.
 */
struct bcm4313_c32 {
	int32_t	i;
	int32_t	q;
};

#define	BCM4313_CORDIC_FLOAT(value)	((value) >> 15)

static const uint32_t bcm4313_arctg[] = {
	2949120, 1740967, 919879, 466945, 234379, 117304, 58666, 29335, 14668,
	7334, 3667, 1833, 917, 458, 229, 115, 57, 29,
};

/*
 * Integer square root, ported from the Linux kernel's int_sqrt()
 * (lib/math/int_sqrt.c); __fls() maps to FreeBSD's fls()-1.
 */
static uint32_t
bcm4313_isqrt(uint32_t x)
{
	uint32_t b, m, y = 0;

	if (x <= 1)
		return (x);

	m = 1UL << ((fls(x) - 1) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return (y);
}

static inline struct bcm4313_c32
bcm4313_cordic(int theta)
{
	uint8_t i;
	int32_t tmp;
	int8_t signx = 1;
	uint32_t angle = 0;
	struct bcm4313_c32 ret = { .i = 39797, .q = 0, };

	while (theta > (180 << 16))
		theta -= (360 << 16);
	while (theta < -(180 << 16))
		theta += (360 << 16);

	if (theta > (90 << 16)) {
		theta -= (180 << 16);
		signx = -1;
	} else if (theta < -(90 << 16)) {
		theta += (180 << 16);
		signx = -1;
	}

	for (i = 0; i <= 17; i++) {
		if (theta > angle) {
			tmp = ret.i - (ret.q >> i);
			ret.q += ret.i >> i;
			ret.i = tmp;
			angle += bcm4313_arctg[i];
		} else {
			tmp = ret.i + (ret.q >> i);
			ret.q -= ret.i >> i;
			ret.i = tmp;
			angle -= bcm4313_arctg[i];
		}
	}

	ret.i *= signx;
	ret.q *= signx;

	return (ret);
}

/*
 * ---------------------------------------------------------------------------
 * Shims for brcmsmac wlapi_* / bcma_* helpers.
 * ---------------------------------------------------------------------------
 */

/*
 * MAC suspend with wait-for-MACSSPNDD, mirroring
 * brcms_c_ucode_mac_suspend() (main.c).
 */
static void
bcm4313_lcnphy_suspend(struct bcm4313_softc *sc)
{
	uint32_t mc, is;
	int i;

	mc = bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);
	mc &= ~BCM4313_MCTL_EN_MAC;
	bcm4313_write_4(sc, BCM4313_D11_MACCONTROL, mc);
	(void)bcm4313_read_4(sc, BCM4313_D11_MACCONTROL);

	for (i = 0; i < 10000; i++) {
		is = bcm4313_read_4(sc, BCM4313_D11_MACINTSTATUS);
		if (is & BCM4313_MI_MACSSPNDD)
			break;
		DELAY(1);
	}
	/* Clear the suspend-done interrupt. */
	bcm4313_write_4(sc, BCM4313_D11_MACINTSTATUS, BCM4313_MI_MACSSPNDD);
}

/* wlapi_enable_mac() -- bcm4313_mac_enable() in if_bcm4313.c. */

/*
 * wlapi_switch_macfreq() -- brcms_b_switch_macfreq() for the LCN PHY
 * (main.c): reprogram the TSF clock dividers for spuravoid.
 */
static void
bcm4313_lcnphy_switch_macfreq(struct bcm4313_softc *sc, bool enable)
{
	if (enable) {		/* 82 MHz */
		bcm4313_write_2(sc, BCM4313_D11_TSF_CLK_FRAC_L, 0x7CE0);
		bcm4313_write_2(sc, BCM4313_D11_TSF_CLK_FRAC_H, 0x000C);
	} else {		/* 80 MHz */
		bcm4313_write_2(sc, BCM4313_D11_TSF_CLK_FRAC_L, 0xCCCD);
		bcm4313_write_2(sc, BCM4313_D11_TSF_CLK_FRAC_H, 0x000C);
	}
}

/*
 * bcma_chipco_pll_write() / bcma_chipco_pll_maskset() through the bhnd
 * PMU interface.  FreeBSD's bhnd_pmu_ind_write() mask semantics differ
 * from Linux, so the read-modify-write is done here and a full-width
 * write issued (semantics identical to the Linux helper).
 */
static void
bcm4313_lcnphy_pll_write(struct bcm4313_softc *sc, uint32_t reg, uint32_t val)
{
	if (sc->sc_pmu == NULL)
		return;
	bhnd_pmu_write_pllctrl(sc->sc_pmu, reg, val, ~0);
}

static void
bcm4313_lcnphy_pll_maskset(struct bcm4313_softc *sc, uint32_t reg,
    uint32_t mask, uint32_t set)
{
	uint32_t cur;

	if (sc->sc_pmu == NULL)
		return;
	cur = bhnd_pmu_read_pllctrl(sc->sc_pmu, reg);
	bhnd_pmu_write_pllctrl(sc->sc_pmu, reg, (cur & mask) | set, ~0);
}

/*
 * bcma_chipco_chipctl_maskset() / bcma_chipco_regctl_maskset() through the
 * bhnd PMU chipctrl/regctrl indirect registers.
 */
static void
bcm4313_lcnphy_chipctl_maskset(struct bcm4313_softc *sc, uint32_t reg,
    uint32_t mask, uint32_t set)
{
	uint32_t cur;

	if (sc->sc_pmu == NULL)
		return;
	cur = bhnd_pmu_read_chipctrl(sc->sc_pmu, reg);
	bhnd_pmu_write_chipctrl(sc->sc_pmu, reg, (cur & mask) | set, ~0);
}

static void
bcm4313_lcnphy_regctl_maskset(struct bcm4313_softc *sc, uint32_t reg,
    uint32_t mask, uint32_t set)
{
	uint32_t cur;

	if (sc->sc_pmu == NULL)
		return;
	cur = bhnd_pmu_read_regctrl(sc->sc_pmu, reg);
	bhnd_pmu_write_regctrl(sc->sc_pmu, reg, (cur & mask) | set, ~0);
}

/*
 * bcma_cc_set32(cc, BCMA_CC_PMU_CTL, BCMA_CC_PMU_CTL_PLL_UPD): assert the
 * chipc PMU "PLL update" latch so the PLL divider values written above
 * take effect.  FreeBSD exposes no public API for raw chipc register
 * access from a non-chipc driver, so this pokes the chipc core's register
 * window through a memory resource allocated on the chipc provider device
 * at attach time (sc_cc_res).  If that window could not be allocated the
 * PLL writes are still applied and only the latch pulse is skipped.
 */
static void
bcm4313_lcnphy_pll_upd(struct bcm4313_softc *sc)
{
	if (sc->sc_cc_res == NULL)
		return;
	bhnd_bus_write_4(sc->sc_cc_res, 0x600,
	    bhnd_bus_read_4(sc->sc_cc_res, 0x600) | BCM4313_LCN_PMU_CTL_PLL_UPD);
}

/* brcms_b_write_template_ram() (main.c). */
static void
bcm4313_lcnphy_write_template_ram(struct bcm4313_softc *sc, int offset,
    int len, const uint32_t *buf)
{
	int i;

	for (i = 0; i < len; i += 4) {
		bcm4313_write_4(sc, BCM4313_D11_TPLATEWRPTR, offset + i);
		bcm4313_write_4(sc, BCM4313_D11_TPLATEWRDATA, buf[i / 4]);
	}
}

/*
 * wlc_phy_do_dummy_tx() (phy_cmn.c): inject a 20-byte dummy frame through
 * the template-RAM transmit path so the TSSI/temp-sense ADCs take a
 * sample.  pa_on only matters for N-PHY (PA override); ignored here.
 */
static void
bcm4313_lcnphy_do_dummy_tx(struct bcm4313_softc *sc, bool ofdm, bool pa_on)
{
#define	BCM4313_DUMMY_PKT_LEN	20
	static const uint8_t ofdmpkt[BCM4313_DUMMY_PKT_LEN] = {
		0xcc, 0x01, 0x02, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
	};
	static const uint8_t cckpkt[BCM4313_DUMMY_PKT_LEN] = {
		0x6e, 0x84, 0x0b, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
	};
	const uint32_t *dummypkt;
	int i, count;

	(void)pa_on;
	dummypkt = (const uint32_t *)(ofdm ? ofdmpkt : cckpkt);
	bcm4313_lcnphy_write_template_ram(sc, 0, BCM4313_DUMMY_PKT_LEN,
	    dummypkt);

	bcm4313_write_2(sc, BCM4313_D11_XMTSEL, 0);
	/* D11REV_GE(corerev, 11) is always true for rev 24. */
	bcm4313_write_2(sc, BCM4313_D11_WEPCTL, 0x100);

	bcm4313_write_2(sc, BCM4313_D11_TXE_PHYCTL,
	    (ofdm ? 1 : 0) | BCM4313_LCN_PHY_TXC_ANT_0);
	bcm4313_write_2(sc, BCM4313_D11_TXE_PHYCTL1, 0x1A02);

	bcm4313_write_2(sc, BCM4313_D11_TXE_WM_0, 0);
	bcm4313_write_2(sc, BCM4313_D11_TXE_WM_1, 0);

	bcm4313_write_2(sc, BCM4313_D11_XMTTPLATETXPTR, 0);
	bcm4313_write_2(sc, BCM4313_D11_XMTTXC_NT, BCM4313_DUMMY_PKT_LEN);

	bcm4313_write_2(sc, BCM4313_D11_XMTSEL,
	    ((8 << 8) | (1 << 5) | (1 << 2) | 2));

	bcm4313_write_2(sc, BCM4313_D11_TXE_CTL, 0);

	bcm4313_write_2(sc, BCM4313_D11_TXE_AUX, 0xD0);
	(void)bcm4313_read_2(sc, BCM4313_D11_TXE_AUX);

	i = 0;
	count = ofdm ? 30 : 250;
	while ((i++ < count) &&
	    (bcm4313_read_2(sc, BCM4313_D11_TXE_STATUS) & (1 << 7)))
		DELAY(10);

	i = 0;
	while ((i++ < 10) &&
	    ((bcm4313_read_2(sc, BCM4313_D11_TXE_STATUS) & (1 << 10)) == 0))
		DELAY(10);

	i = 0;
	while ((i++ < 10) &&
	    (bcm4313_read_2(sc, BCM4313_D11_IFSSTAT) & (1 << 8)))
		DELAY(10);
#undef	BCM4313_DUMMY_PKT_LEN
}

/*
 * ---------------------------------------------------------------------------
 * Indirect table access (wlc_phy_write_table/read_table, phy_cmn.c).
 * The table window is {0x455 = (tbl_id << 10) | index, 0x457 = data hi,
 * 0x456 = data lo}.
 * ---------------------------------------------------------------------------
 */
void
bcm4313_lcnphy_write_table(struct bcm4313_softc *sc,
    const struct bcm4313_phytbl *pt)
{
	const uint8_t *p8 = pt->tbl_ptr;
	const uint16_t *p16 = pt->tbl_ptr;
	const uint32_t *p32 = pt->tbl_ptr;
	uint32_t idx;

	bcm4313_phy_write(sc, BCM4313_LCN_TBLADDR,
	    (uint16_t)((pt->tbl_id << 10) | pt->tbl_offset));

	for (idx = 0; idx < pt->tbl_len; idx++) {
		if (pt->tbl_width == 32) {
			bcm4313_phy_write(sc, BCM4313_LCN_TBLDATAHI,
			    (uint16_t)(p32[idx] >> 16));
			bcm4313_phy_write(sc, BCM4313_LCN_TBLDATALO,
			    (uint16_t)p32[idx]);
		} else if (pt->tbl_width == 16) {
			bcm4313_phy_write(sc, BCM4313_LCN_TBLDATALO, p16[idx]);
		} else {
			bcm4313_phy_write(sc, BCM4313_LCN_TBLDATALO, p8[idx]);
		}
	}
}

void
bcm4313_lcnphy_read_table(struct bcm4313_softc *sc, struct bcm4313_phytbl *pt)
{
	uint8_t *p8 = (uint8_t *)(uintptr_t)pt->tbl_ptr;
	uint16_t *p16 = (uint16_t *)(uintptr_t)pt->tbl_ptr;
	uint32_t *p32 = (uint32_t *)(uintptr_t)pt->tbl_ptr;
	uint32_t idx;

	bcm4313_phy_write(sc, BCM4313_LCN_TBLADDR,
	    (uint16_t)((pt->tbl_id << 10) | pt->tbl_offset));

	for (idx = 0; idx < pt->tbl_len; idx++) {
		if (pt->tbl_width == 32) {
			p32[idx] = ((uint32_t)bcm4313_phy_read(sc,
			    BCM4313_LCN_TBLDATAHI) << 16) |
			    bcm4313_phy_read(sc, BCM4313_LCN_TBLDATALO);
		} else if (pt->tbl_width == 16) {
			p16[idx] = bcm4313_phy_read(sc, BCM4313_LCN_TBLDATALO);
		} else {
			p8[idx] = bcm4313_phy_read(sc, BCM4313_LCN_TBLDATALO);
		}
	}
}

/*
 * ---------------------------------------------------------------------------
 * Small helpers (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

/* wlc_lcnphy_total_tx_frames(): M_UCODE_MACSTAT + macstat.txallfrm. */
static uint16_t
bcm4313_lcnphy_total_tx_frames(struct bcm4313_softc *sc)
{
	return (bcm4313_shm_read_2(sc, BCM4313_M_UCODE_MACSTAT_TXALLFRM));
}

/* wlc_lcnphy_qdiv_roundup(). */
static uint32_t
bcm4313_lcnphy_qdiv_roundup(uint32_t dividend, uint32_t divisor,
    uint8_t precision)
{
	uint32_t quotient, remainder, roundup, rbit;

	quotient = dividend / divisor;
	remainder = dividend % divisor;
	rbit = divisor & 1;
	roundup = (divisor >> 1) + rbit;

	while (precision--) {
		quotient <<= 1;
		if (remainder >= roundup) {
			quotient++;
			remainder = ((remainder - roundup) << 1) + rbit;
		} else {
			remainder <<= 1;
		}
	}

	if (remainder >= roundup)
		quotient++;

	return (quotient);
}

/* wlc_lcnphy_calc_floor(). */
static int
bcm4313_lcnphy_calc_floor(int16_t coeff_x, int type)
{
	int k;

	k = 0;
	if (type == 0) {
		if (coeff_x < 0)
			k = (coeff_x - 1) / 2;
		else
			k = coeff_x / 2;
	}
	if (type == 1) {
		if ((coeff_x + 1) < 0)
			k = (coeff_x) / 2;
		else
			k = (coeff_x + 1) / 2;
	}
	return (k);
}

/* wlc_phy_nbits() (phy_cmn.c). */
static uint8_t
bcm4313_lcnphy_nbits(int32_t value)
{
	int32_t abs_val;
	uint8_t nbits = 0;

	abs_val = value < 0 ? -value : value;
	while ((abs_val >> nbits) > 0)
		nbits++;

	return (nbits);
}

/* wlc_lcnphy_get_dac_gain bits come from 0x439 bits 9:7. */
static void
bcm4313_lcnphy_set_dac_gain(struct bcm4313_softc *sc, uint16_t dac_gain)
{
	bcm4313_phy_maskset(sc, 0x439, 0x380, dac_gain << 7);
}

/* wlc_lcnphy_set_tx_gain_override(). */
static void
bcm4313_lcnphy_set_tx_gain_override(struct bcm4313_softc *sc, bool b_enable)
{
	uint16_t ebit = b_enable ? 1 : 0;

	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 8), ebit << 8);

	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 6), ebit << 6);
}

#define	BCM4313_LCN_ENABLE_TX_GAIN_OVERRIDE(sc) \
	bcm4313_lcnphy_set_tx_gain_override((sc), true)
#define	BCM4313_LCN_DISABLE_TX_GAIN_OVERRIDE(sc) \
	bcm4313_lcnphy_set_tx_gain_override((sc), false)

#define	BCM4313_LCN_TX_GAIN_OVERRIDE_ENABLED(sc) \
	(0 != (bcm4313_phy_read((sc), 0x43b) & (0x1 << 6)))

/* wlc_lcnphy_set_trsw_override() / wlc_lcnphy_clear_trsw_override(). */
static void
bcm4313_lcnphy_set_trsw_override(struct bcm4313_softc *sc, bool tx, bool rx)
{
	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 7), (tx ? 1 : 0) << 7);
	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 6), (rx ? 1 : 0) << 6);
	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 5), (rx ? 1 : 0) << 5);
}

static void
bcm4313_lcnphy_clear_trsw_override(struct bcm4313_softc *sc)
{
	bcm4313_lcnphy_set_trsw_override(sc, false, false);
}

/* wlc_lcnphy_rx_gain_override_enable(). */
static void
bcm4313_lcnphy_rx_gain_override_enable(struct bcm4313_softc *sc, bool enable)
{
	uint16_t ebit = enable ? 1 : 0;

	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 8), ebit << 8);
	bcm4313_phy_maskset(sc, 0x44c, (0x1 << 0), ebit << 0);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1)) {
		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 4), ebit << 4);
		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 6), ebit << 6);
		bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 5), ebit << 5);
		bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 6), ebit << 6);
	} else {
		bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 12), ebit << 12);
		bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 13), ebit << 13);
		bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 5), ebit << 5);
	}

	/* 4313 is always 2.4GHz. */
	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 10), ebit << 10);
	bcm4313_phy_maskset(sc, 0x4e5, (0x1 << 3), ebit << 3);
}

/*
 * wlc_lcnphy_set_rx_gain_by_distribution().  The 4313 is LCN rev 1
 * (LCNREV_LT(rev, 2) path).
 */
static void
bcm4313_lcnphy_set_rx_gain_by_distribution(struct bcm4313_softc *sc,
    uint16_t trsw, uint16_t ext_lna, uint16_t biq2, uint16_t biq1,
    uint16_t tia, uint16_t lna2, uint16_t lna1)
{
	uint16_t gain0_15, gain16_19;

	(void)trsw;
	gain16_19 = biq2 & 0xf;
	gain0_15 = ((biq1 & 0xf) << 12) |
	    ((tia & 0xf) << 8) |
	    ((lna2 & 0x3) << 6) |
	    ((lna2 & 0x3) << 4) |
	    ((lna1 & 0x3) << 2) |
	    ((lna1 & 0x3) << 0);

	bcm4313_phy_maskset(sc, 0x4b6, (0xffff << 0), gain0_15 << 0);
	bcm4313_phy_maskset(sc, 0x4b7, (0xf << 0), gain16_19 << 0);
	bcm4313_phy_maskset(sc, 0x4b1, (0x3 << 11), lna1 << 11);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1)) {
		bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 9), ext_lna << 9);
		bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 10), ext_lna << 10);
	} else {
		bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 10), 0 << 10);
		bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 15), 0 << 15);
		bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 9), ext_lna << 9);
	}
}

/* struct lcnphy_txgains (phy_lcn.h). */
struct bcm4313_lcnphy_txgains {
	uint16_t	gm_gain;
	uint16_t	pga_gain;
	uint16_t	pad_gain;
	uint16_t	dac_gain;
};

/* wlc_lcnphy_get_tx_gain(). */
static void
bcm4313_lcnphy_get_tx_gain(struct bcm4313_softc *sc,
    struct bcm4313_lcnphy_txgains *gains)
{
	uint16_t dac_gain, rfgain0, rfgain1;

	dac_gain = bcm4313_phy_read(sc, 0x439) >> 0;
	gains->dac_gain = (dac_gain & 0x380) >> 7;

	rfgain0 = (bcm4313_phy_read(sc, 0x4b5) & (0xffff << 0)) >> 0;
	rfgain1 = (bcm4313_phy_read(sc, 0x4fb) & (0x7fff << 0)) >> 0;

	gains->gm_gain = rfgain0 & 0xff;
	gains->pga_gain = (rfgain0 >> 8) & 0xff;
	gains->pad_gain = rfgain1 & 0xff;
}

/* wlc_lcnphy_get_pa_gain(). */
static uint16_t
bcm4313_lcnphy_get_pa_gain(struct bcm4313_softc *sc)
{
	uint16_t pa_gain;

	pa_gain = (bcm4313_phy_read(sc, 0x4fb) & (0x7f << 8)) >> 8;

	return (pa_gain);
}

/* wlc_lcnphy_set_pa_gain(). */
static void
bcm4313_lcnphy_set_pa_gain(struct bcm4313_softc *sc, uint16_t gain)
{
	bcm4313_phy_maskset(sc, 0x4fb, (0x7f << 8), gain << 8);
	bcm4313_phy_maskset(sc, 0x4fd, (0x7f << 8), gain << 8);
}

/* wlc_lcnphy_set_tx_gain(). */
static void
bcm4313_lcnphy_set_tx_gain(struct bcm4313_softc *sc,
    const struct bcm4313_lcnphy_txgains *target_gains)
{
	uint16_t pa_gain = bcm4313_lcnphy_get_pa_gain(sc);

	bcm4313_phy_maskset(sc, 0x4b5, (0xffff << 0),
	    ((target_gains->gm_gain) | (target_gains->pga_gain << 8)) << 0);
	bcm4313_phy_maskset(sc, 0x4fb, (0x7fff << 0),
	    ((target_gains->pad_gain) | (pa_gain << 8)) << 0);

	bcm4313_phy_maskset(sc, 0x4fc, (0xffff << 0),
	    ((target_gains->gm_gain) | (target_gains->pga_gain << 8)) << 0);
	bcm4313_phy_maskset(sc, 0x4fd, (0x7fff << 0),
	    ((target_gains->pad_gain) | (pa_gain << 8)) << 0);

	bcm4313_lcnphy_set_dac_gain(sc, target_gains->dac_gain);

	BCM4313_LCN_ENABLE_TX_GAIN_OVERRIDE(sc);
}

/* wlc_lcnphy_get_bbmult() / wlc_lcnphy_set_bbmult(). */
static uint8_t
bcm4313_lcnphy_get_bbmult(struct bcm4313_softc *sc)
{
	uint16_t m0m1;
	struct bcm4313_phytbl tab;

	tab.tbl_ptr = &m0m1;
	tab.tbl_len = 1;
	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_offset = 87;
	tab.tbl_width = 16;
	bcm4313_lcnphy_read_table(sc, &tab);

	return ((uint8_t)((m0m1 & 0xff00) >> 8));
}

static void
bcm4313_lcnphy_set_bbmult(struct bcm4313_softc *sc, uint8_t m0)
{
	uint16_t m0m1 = (uint16_t)m0 << 8;
	struct bcm4313_phytbl tab;

	tab.tbl_ptr = &m0m1;
	tab.tbl_len = 1;
	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_offset = 87;
	tab.tbl_width = 16;
	bcm4313_lcnphy_write_table(sc, &tab);
}

/* wlc_lcnphy_clear_tx_power_offsets(). */
static void
bcm4313_lcnphy_clear_tx_power_offsets(struct bcm4313_softc *sc)
{
	uint32_t data_buf[64];
	struct bcm4313_phytbl tab;

	memset(data_buf, 0, sizeof(data_buf));

	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_ptr = data_buf;

	if (!sc->sc_temppwrctrl_capable) {
		tab.tbl_len = 30;
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_RATE_OFFSET;
		bcm4313_lcnphy_write_table(sc, &tab);
	}

	tab.tbl_len = 64;
	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_MAC_OFFSET;
	bcm4313_lcnphy_write_table(sc, &tab);
}

/* wlc_lcnphy_set_tx_iqcc() / wlc_lcnphy_get_tx_iqcc(). */
static void
bcm4313_lcnphy_set_tx_iqcc(struct bcm4313_softc *sc, uint16_t a, uint16_t b)
{
	uint16_t iqcc[2];
	struct bcm4313_phytbl tab;

	iqcc[0] = a;
	iqcc[1] = b;

	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_width = 16;
	tab.tbl_ptr = iqcc;
	tab.tbl_len = 2;
	tab.tbl_offset = 80;
	bcm4313_lcnphy_write_table(sc, &tab);
}

static void
bcm4313_lcnphy_get_tx_iqcc(struct bcm4313_softc *sc, uint16_t *a, uint16_t *b)
{
	uint16_t iqcc[2];
	struct bcm4313_phytbl tab;

	tab.tbl_ptr = iqcc;
	tab.tbl_len = 2;
	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_offset = 80;
	tab.tbl_width = 16;
	bcm4313_lcnphy_read_table(sc, &tab);

	*a = iqcc[0];
	*b = iqcc[1];
}

/* wlc_lcnphy_set_tx_locc() / wlc_lcnphy_get_tx_locc(). */
static void
bcm4313_lcnphy_set_tx_locc(struct bcm4313_softc *sc, uint16_t didq)
{
	struct bcm4313_phytbl tab;

	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_width = 16;
	tab.tbl_ptr = &didq;
	tab.tbl_len = 1;
	tab.tbl_offset = 85;
	bcm4313_lcnphy_write_table(sc, &tab);
}

static uint16_t
bcm4313_lcnphy_get_tx_locc(struct bcm4313_softc *sc)
{
	struct bcm4313_phytbl tab;
	uint16_t didq;

	tab.tbl_id = BCM4313_LCN_TBL_ID_IQLOCAL;
	tab.tbl_width = 16;
	tab.tbl_ptr = &didq;
	tab.tbl_len = 1;
	tab.tbl_offset = 85;
	bcm4313_lcnphy_read_table(sc, &tab);

	return (didq);
}

/* wlc_lcnphy_get_radio_loft(). */
static void
bcm4313_lcnphy_get_radio_loft(struct bcm4313_softc *sc, uint8_t *ei0,
    uint8_t *eq0, uint8_t *fi0, uint8_t *fq0)
{
#define	BCM4313_LCN_IQLOCC_READ(val) \
	((uint8_t)(-((int8_t)(((val) & 0xf0) >> 4)) + (int8_t)((val) & 0x0f)))
	*ei0 = BCM4313_LCN_IQLOCC_READ(bcm4313_radio_read(sc, RADIO_2064_REG089));
	*eq0 = BCM4313_LCN_IQLOCC_READ(bcm4313_radio_read(sc, RADIO_2064_REG08A));
	*fi0 = BCM4313_LCN_IQLOCC_READ(bcm4313_radio_read(sc, RADIO_2064_REG08B));
	*fq0 = BCM4313_LCN_IQLOCC_READ(bcm4313_radio_read(sc, RADIO_2064_REG08C));
#undef	BCM4313_LCN_IQLOCC_READ
}

/* wlc_lcnphy_set_cc(). */
static void
bcm4313_lcnphy_set_cc(struct bcm4313_softc *sc, int cal_type,
    int16_t coeff_x, int16_t coeff_y)
{
	uint16_t di0dq0;
	uint16_t x, y, data_rf;
	int k;

	switch (cal_type) {
	case 0:
		bcm4313_lcnphy_set_tx_iqcc(sc, coeff_x, coeff_y);
		break;
	case 2:
		di0dq0 = (coeff_x & 0xff) << 8 | (coeff_y & 0xff);
		bcm4313_lcnphy_set_tx_locc(sc, di0dq0);
		break;
	case 3:
		k = bcm4313_lcnphy_calc_floor(coeff_x, 0);
		y = 8 + k;
		k = bcm4313_lcnphy_calc_floor(coeff_x, 1);
		x = 8 - k;
		data_rf = (x * 16 + y);
		bcm4313_radio_write(sc, RADIO_2064_REG089, data_rf);
		k = bcm4313_lcnphy_calc_floor(coeff_y, 0);
		y = 8 + k;
		k = bcm4313_lcnphy_calc_floor(coeff_y, 1);
		x = 8 - k;
		data_rf = (x * 16 + y);
		bcm4313_radio_write(sc, RADIO_2064_REG08A, data_rf);
		break;
	case 4:
		k = bcm4313_lcnphy_calc_floor(coeff_x, 0);
		y = 8 + k;
		k = bcm4313_lcnphy_calc_floor(coeff_x, 1);
		x = 8 - k;
		data_rf = (x * 16 + y);
		bcm4313_radio_write(sc, RADIO_2064_REG08B, data_rf);
		k = bcm4313_lcnphy_calc_floor(coeff_y, 0);
		y = 8 + k;
		k = bcm4313_lcnphy_calc_floor(coeff_y, 1);
		x = 8 - k;
		data_rf = (x * 16 + y);
		bcm4313_radio_write(sc, RADIO_2064_REG08C, data_rf);
		break;
	}
}

/* wlc_lcnphy_get_cc(). */
struct bcm4313_lcnphy_unsign16 {
	uint16_t	re;
	uint16_t	im;
};

static struct bcm4313_lcnphy_unsign16
bcm4313_lcnphy_get_cc(struct bcm4313_softc *sc, int cal_type)
{
	uint16_t a, b, didq;
	uint8_t di0, dq0, ei, eq, fi, fq;
	struct bcm4313_lcnphy_unsign16 cc;

	cc.re = 0;
	cc.im = 0;
	switch (cal_type) {
	case 0:
		bcm4313_lcnphy_get_tx_iqcc(sc, &a, &b);
		cc.re = a;
		cc.im = b;
		break;
	case 2:
		didq = bcm4313_lcnphy_get_tx_locc(sc);
		di0 = (((didq & 0xff00) << 16) >> 24);
		dq0 = (((didq & 0x00ff) << 24) >> 24);
		cc.re = (uint16_t)di0;
		cc.im = (uint16_t)dq0;
		break;
	case 3:
		bcm4313_lcnphy_get_radio_loft(sc, &ei, &eq, &fi, &fq);
		cc.re = (uint16_t)ei;
		cc.im = (uint16_t)eq;
		break;
	case 4:
		bcm4313_lcnphy_get_radio_loft(sc, &ei, &eq, &fi, &fq);
		cc.re = (uint16_t)fi;
		cc.im = (uint16_t)fq;
		break;
	}
	return (cc);
}

/*
 * ---------------------------------------------------------------------------
 * RX IQ estimation and calibration (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

/* wlc_lcnphy_set_rx_iq_comp(). */
static void
bcm4313_lcnphy_set_rx_iq_comp(struct bcm4313_softc *sc, uint16_t a, uint16_t b)
{
	bcm4313_phy_maskset(sc, 0x645, (0x3ff << 0), (a) << 0);
	bcm4313_phy_maskset(sc, 0x646, (0x3ff << 0), (b) << 0);
	bcm4313_phy_maskset(sc, 0x647, (0x3ff << 0), (a) << 0);
	bcm4313_phy_maskset(sc, 0x648, (0x3ff << 0), (b) << 0);
	bcm4313_phy_maskset(sc, 0x649, (0x3ff << 0), (a) << 0);
	bcm4313_phy_maskset(sc, 0x64a, (0x3ff << 0), (b) << 0);
}

/* struct lcnphy_iq_est. */
struct bcm4313_lcnphy_iq_est {
	uint32_t	iq_prod;
	uint32_t	i_pwr;
	uint32_t	q_pwr;
};

/* wlc_lcnphy_rx_iq_est(). */
static bool
bcm4313_lcnphy_rx_iq_est(struct bcm4313_softc *sc, uint16_t num_samps,
    uint8_t wait_time, struct bcm4313_lcnphy_iq_est *iq_est)
{
	int wait_count = 0;
	bool result = true;

	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 5), (1) << 5);
	bcm4313_phy_maskset(sc, 0x410, (0x1 << 3), (0) << 3);
	bcm4313_phy_maskset(sc, 0x482, (0xffff << 0), (num_samps) << 0);
	bcm4313_phy_maskset(sc, 0x481, (0xff << 0), ((uint16_t)wait_time) << 0);
	bcm4313_phy_maskset(sc, 0x481, (0x1 << 8), (0) << 8);
	bcm4313_phy_maskset(sc, 0x481, (0x1 << 9), (1) << 9);

	while (bcm4313_phy_read(sc, 0x481) & (0x1 << 9)) {
		if (wait_count > (10 * 500)) {
			result = false;
			goto cleanup;
		}
		DELAY(100);
		wait_count++;
	}

	iq_est->iq_prod = ((uint32_t)bcm4313_phy_read(sc, 0x483) << 16) |
	    (uint32_t)bcm4313_phy_read(sc, 0x484);
	iq_est->i_pwr = ((uint32_t)bcm4313_phy_read(sc, 0x485) << 16) |
	    (uint32_t)bcm4313_phy_read(sc, 0x486);
	iq_est->q_pwr = ((uint32_t)bcm4313_phy_read(sc, 0x487) << 16) |
	    (uint32_t)bcm4313_phy_read(sc, 0x488);

cleanup:
	bcm4313_phy_maskset(sc, 0x410, (0x1 << 3), (1) << 3);
	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 5), (0) << 5);

	return (result);
}

/* wlc_lcnphy_calc_rx_iq_comp(). */
static bool
bcm4313_lcnphy_calc_rx_iq_comp(struct bcm4313_softc *sc, uint16_t num_samps)
{
#define	BCM4313_LCN_MIN_RXIQ_PWR	2
	bool result;
	uint16_t a0_new, b0_new;
	struct bcm4313_lcnphy_iq_est iq_est = { 0, 0, 0 };
	int32_t a, b, temp;
	int16_t iq_nbits, qq_nbits, arsh, brsh;
	int32_t iq;
	uint32_t ii, qq;
	struct bcm4313_softc *sc_p = sc;

	a0_new = ((bcm4313_phy_read(sc, 0x645) & (0x3ff << 0)) >> 0);
	b0_new = ((bcm4313_phy_read(sc, 0x646) & (0x3ff << 0)) >> 0);
	bcm4313_phy_maskset(sc, 0x6d1, (0x1 << 2), (0) << 2);
	bcm4313_phy_maskset(sc, 0x64b, (0x1 << 6), (1) << 6);

	bcm4313_lcnphy_set_rx_iq_comp(sc, 0, 0);

	result = bcm4313_lcnphy_rx_iq_est(sc, num_samps, 32, &iq_est);
	if (!result)
		goto cleanup;

	iq = (int32_t)iq_est.iq_prod;
	ii = iq_est.i_pwr;
	qq = iq_est.q_pwr;

	if ((ii + qq) < BCM4313_LCN_MIN_RXIQ_PWR) {
		result = false;
		goto cleanup;
	}

	iq_nbits = bcm4313_lcnphy_nbits(iq);
	qq_nbits = bcm4313_lcnphy_nbits(qq);

	arsh = 10 - (30 - iq_nbits);
	if (arsh >= 0) {
		a = (-(iq << (30 - iq_nbits)) + (ii >> (1 + arsh)));
		temp = (int32_t)(ii >> arsh);
		if (temp == 0)
			return (false);
	} else {
		a = (-(iq << (30 - iq_nbits)) + (ii << (-1 - arsh)));
		temp = (int32_t)(ii << -arsh);
		if (temp == 0)
			return (false);
	}
	a /= temp;
	brsh = qq_nbits - 31 + 20;
	if (brsh >= 0) {
		b = (qq << (31 - qq_nbits));
		temp = (int32_t)(ii >> brsh);
		if (temp == 0)
			return (false);
	} else {
		b = (qq << (31 - qq_nbits));
		temp = (int32_t)(ii << -brsh);
		if (temp == 0)
			return (false);
	}
	b /= temp;
	b -= a * a;
	b = (int32_t)bcm4313_isqrt((uint32_t)b);
	b -= (1 << 10);
	a0_new = (uint16_t)(a & 0x3ff);
	b0_new = (uint16_t)(b & 0x3ff);

cleanup:
	bcm4313_lcnphy_set_rx_iq_comp(sc, a0_new, b0_new);
	bcm4313_phy_maskset(sc, 0x64b, (0x1 << 0), (1) << 0);
	bcm4313_phy_maskset(sc, 0x64b, (0x1 << 3), (1) << 3);

	sc_p->sc_lcn.lcnphy_cal_results.rxiqcal_coeff_a0 = a0_new;
	sc_p->sc_lcn.lcnphy_cal_results.rxiqcal_coeff_b0 = b0_new;

	return (result);
}

/* wlc_lcnphy_rx_iq_cal_gain(). */
static bool
bcm4313_lcnphy_rx_iq_cal_gain(struct bcm4313_softc *sc, uint16_t biq1_gain,
    uint16_t tia_gain, uint16_t lna2_gain)
{
	uint32_t i_thresh_l, q_thresh_l;
	uint32_t i_thresh_h, q_thresh_h;
	struct bcm4313_lcnphy_iq_est iq_est_h, iq_est_l;

	bcm4313_lcnphy_set_rx_gain_by_distribution(sc, 0, 0, 0, biq1_gain,
	    tia_gain, lna2_gain, 0);

	bcm4313_lcnphy_rx_gain_override_enable(sc, true);
	bcm4313_lcnphy_start_tx_tone(sc, 2000, (40 >> 1), 0);
	DELAY(500);
	bcm4313_radio_write(sc, RADIO_2064_REG112, 0);
	if (!bcm4313_lcnphy_rx_iq_est(sc, 1024, 32, &iq_est_l))
		return (false);

	bcm4313_lcnphy_start_tx_tone(sc, 2000, 40, 0);
	DELAY(500);
	bcm4313_radio_write(sc, RADIO_2064_REG112, 0);
	if (!bcm4313_lcnphy_rx_iq_est(sc, 1024, 32, &iq_est_h))
		return (false);

	i_thresh_l = (iq_est_l.i_pwr << 1);
	i_thresh_h = (iq_est_l.i_pwr << 2) + iq_est_l.i_pwr;

	q_thresh_l = (iq_est_l.q_pwr << 1);
	q_thresh_h = (iq_est_l.q_pwr << 2) + iq_est_l.q_pwr;
	if ((iq_est_h.i_pwr > i_thresh_l) &&
	    (iq_est_h.i_pwr < i_thresh_h) &&
	    (iq_est_h.q_pwr > q_thresh_l) &&
	    (iq_est_h.q_pwr < q_thresh_h))
		return (true);

	return (false);
}

/* struct lcnphy_rx_iqcomp. */
struct bcm4313_lcnphy_rx_iqcomp {
	uint8_t	chan;
	int16_t	a;
	int16_t	b;
};

/*
 * wlc_lcnphy_rx_iq_cal().  The 4313 is LCN rev 1 and always 2.4GHz, so
 * only the module == 1 branch is reachable; module == 2 (per-channel
 * pre-stored RX IQ compensation) is kept for completeness.
 */
static bool
bcm4313_lcnphy_rx_iq_cal(struct bcm4313_softc *sc,
    const struct bcm4313_lcnphy_rx_iqcomp *iqcomp, int iqcomp_sz,
    bool tx_switch, bool rx_switch, int module, int tx_gain_idx)
{
	struct bcm4313_lcnphy_txgains old_gains;
	uint16_t tx_pwr_ctrl;
	uint8_t tx_gain_index_old = 0;
	bool result = false, tx_gain_override_old = false;
	uint16_t i, Core1TxControl_old,
	    RFOverrideVal0_old, rfoverride2_old, rfoverride2val_old,
	    rfoverride3_old, rfoverride3val_old, rfoverride4_old,
	    rfoverride4val_old, afectrlovr_old, afectrlovrval_old;
	int tia_gain, lna2_gain, biq1_gain;
	bool set_gain;
	uint16_t old_sslpnCalibClkEnCtrl, old_sslpnRxFeClkEnCtrl;
	uint16_t values_to_save[11];
	int16_t ptr[131];

	if (module == 2) {
		while (iqcomp_sz--) {
			if (iqcomp[iqcomp_sz].chan == sc->sc_curchan) {
				bcm4313_lcnphy_set_rx_iq_comp(sc,
				    (uint16_t)iqcomp[iqcomp_sz].a,
				    (uint16_t)iqcomp[iqcomp_sz].b);
				result = true;
				break;
			}
		}
		goto cal_done;
	}

	/* WARN_ON(module != 1) in brcmsmac; never reached with our callers. */
	tx_pwr_ctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);

	for (i = 0; i < 11; i++)
		values_to_save[i] =
		    bcm4313_radio_read(sc, rxiq_cal_rf_reg[i]);
	Core1TxControl_old = bcm4313_phy_read(sc, 0x631);

	bcm4313_phy_maskset(sc, 0x631, 0xffff, 0x0015 |
	    (bcm4313_phy_read(sc, 0x631) & ~(uint16_t)0x0015));
	(void)bcm4313_phy_read(sc, 0x44c); /* RFOverride0_old */
	RFOverrideVal0_old = bcm4313_phy_read(sc, 0x44d);
	rfoverride2_old = bcm4313_phy_read(sc, 0x4b0);
	rfoverride2val_old = bcm4313_phy_read(sc, 0x4b1);
	rfoverride3_old = bcm4313_phy_read(sc, 0x4f9);
	rfoverride3val_old = bcm4313_phy_read(sc, 0x4fa);
	rfoverride4_old = bcm4313_phy_read(sc, 0x938);
	rfoverride4val_old = bcm4313_phy_read(sc, 0x939);
	afectrlovr_old = bcm4313_phy_read(sc, 0x43b);
	afectrlovrval_old = bcm4313_phy_read(sc, 0x43c);
	old_sslpnCalibClkEnCtrl = bcm4313_phy_read(sc, 0x6da);
	old_sslpnRxFeClkEnCtrl = bcm4313_phy_read(sc, 0x6db);

	tx_gain_override_old = BCM4313_LCN_TX_GAIN_OVERRIDE_ENABLED(sc);
	if (tx_gain_override_old) {
		bcm4313_lcnphy_get_tx_gain(sc, &old_gains);
		tx_gain_index_old = sc->sc_lcn.lcnphy_current_index;
	}

	bcm4313_lcnphy_set_tx_pwr_by_index(sc, tx_gain_idx);

	bcm4313_phy_maskset(sc, 0x4f9, (0x1 << 0), 1 << 0);
	bcm4313_phy_maskset(sc, 0x4fa, (0x1 << 0), 0 << 0);

	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 1), 1 << 1);
	bcm4313_phy_maskset(sc, 0x43c, (0x1 << 1), 0 << 1);

	bcm4313_radio_write(sc, RADIO_2064_REG116, 0x06);
	bcm4313_radio_write(sc, RADIO_2064_REG12C, 0x07);
	bcm4313_radio_write(sc, RADIO_2064_REG06A, 0xd3);
	bcm4313_radio_write(sc, RADIO_2064_REG098, 0x03);
	bcm4313_radio_write(sc, RADIO_2064_REG00B, 0x7);
	bcm4313_radio_maskset(sc, RADIO_2064_REG113, 1 << 4, 1 << 4);
	bcm4313_radio_write(sc, RADIO_2064_REG01D, 0x01);
	bcm4313_radio_write(sc, RADIO_2064_REG114, 0x01);
	bcm4313_radio_write(sc, RADIO_2064_REG02E, 0x10);
	bcm4313_radio_write(sc, RADIO_2064_REG12A, 0x08);

	bcm4313_phy_maskset(sc, 0x938, (0x1 << 0), 1 << 0);
	bcm4313_phy_maskset(sc, 0x939, (0x1 << 0), 0 << 0);
	bcm4313_phy_maskset(sc, 0x938, (0x1 << 1), 1 << 1);
	bcm4313_phy_maskset(sc, 0x939, (0x1 << 1), 1 << 1);
	bcm4313_phy_maskset(sc, 0x938, (0x1 << 2), 1 << 2);
	bcm4313_phy_maskset(sc, 0x939, (0x1 << 2), 1 << 2);
	bcm4313_phy_maskset(sc, 0x938, (0x1 << 3), 1 << 3);
	bcm4313_phy_maskset(sc, 0x939, (0x1 << 3), 1 << 3);
	bcm4313_phy_maskset(sc, 0x938, (0x1 << 5), 1 << 5);
	bcm4313_phy_maskset(sc, 0x939, (0x1 << 5), 0 << 5);

	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 0), 1 << 0);
	bcm4313_phy_maskset(sc, 0x43c, (0x1 << 0), 0 << 0);

	bcm4313_phy_write(sc, 0x6da, 0xffff);
	bcm4313_phy_maskset(sc, 0x6db, 0xffff, 0x3 |
	    (bcm4313_phy_read(sc, 0x6db) & ~(uint16_t)0x3));

	bcm4313_lcnphy_set_trsw_override(sc, tx_switch, rx_switch);
	for (lna2_gain = 3; lna2_gain >= 0; lna2_gain--) {
		for (tia_gain = 4; tia_gain >= 0; tia_gain--) {
			for (biq1_gain = 6; biq1_gain >= 0; biq1_gain--) {
				set_gain = bcm4313_lcnphy_rx_iq_cal_gain(sc,
				    (uint16_t)biq1_gain,
				    (uint16_t)tia_gain,
				    (uint16_t)lna2_gain);
				if (!set_gain)
					continue;

				result = bcm4313_lcnphy_calc_rx_iq_comp(sc,
				    1024);
				goto stop_tone;
			}
		}
	}

stop_tone:
	bcm4313_lcnphy_stop_tx_tone(sc);

	bcm4313_phy_write(sc, 0x631, Core1TxControl_old);

	bcm4313_phy_write(sc, 0x44c, RFOverrideVal0_old);
	bcm4313_phy_write(sc, 0x44d, RFOverrideVal0_old);
	bcm4313_phy_write(sc, 0x4b0, rfoverride2_old);
	bcm4313_phy_write(sc, 0x4b1, rfoverride2val_old);
	bcm4313_phy_write(sc, 0x4f9, rfoverride3_old);
	bcm4313_phy_write(sc, 0x4fa, rfoverride3val_old);
	bcm4313_phy_write(sc, 0x938, rfoverride4_old);
	bcm4313_phy_write(sc, 0x939, rfoverride4val_old);
	bcm4313_phy_write(sc, 0x43b, afectrlovr_old);
	bcm4313_phy_write(sc, 0x43c, afectrlovrval_old);
	bcm4313_phy_write(sc, 0x6da, old_sslpnCalibClkEnCtrl);
	bcm4313_phy_write(sc, 0x6db, old_sslpnRxFeClkEnCtrl);

	bcm4313_lcnphy_clear_trsw_override(sc);

	bcm4313_phy_maskset(sc, 0x44c, (0x1 << 2), 0 << 2);

	for (i = 0; i < 11; i++)
		bcm4313_radio_write(sc, rxiq_cal_rf_reg[i],
		    values_to_save[i]);

	if (tx_gain_override_old)
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, tx_gain_index_old);
	else
		BCM4313_LCN_DISABLE_TX_GAIN_OVERRIDE(sc);

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, tx_pwr_ctrl);
	bcm4313_lcnphy_rx_gain_override_enable(sc, false);

cal_done:
	(void)ptr;
	return (result);
}

/*
 * ---------------------------------------------------------------------------
 * TX power control (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

#define	BCM4313_LCN_TXPWRCTRL_OFF(sc)	\
	(0x7 != ((bcm4313_phy_read((sc), 0x4a4) & 0xE000) >> 13))

/* wlc_lcnphy_get_current_tx_pwr_idx(). */
static int8_t
bcm4313_lcnphy_get_current_tx_pwr_idx(struct bcm4313_softc *sc)
{
	int8_t index;

	if (BCM4313_LCN_TXPWRCTRL_OFF(sc))
		index = (int8_t)sc->sc_lcn.lcnphy_current_index;
	else if (sc->sc_hwpwrctrl_capable)
		index = (int8_t)(bcm4313_lcnphy_get_current_tx_pwr_idx_if_pwrctrl_on(sc) / 2);
	else
		index = (int8_t)sc->sc_lcn.lcnphy_current_index;
	return (index);
}

/* wlc_lcnphy_tx_pwr_update_npt(). */
static void
bcm4313_lcnphy_tx_pwr_update_npt(struct bcm4313_softc *sc)
{
	uint16_t tx_cnt, tx_total, npt;

	tx_total = bcm4313_lcnphy_total_tx_frames(sc);
	tx_cnt = tx_total - sc->sc_lcn.lcnphy_tssi_tx_cnt;
	npt = bcm4313_lcnphy_get_tx_pwr_npt(sc);

	if (tx_cnt > (1 << npt)) {
		sc->sc_lcn.lcnphy_tssi_tx_cnt = tx_total;
		sc->sc_lcn.lcnphy_tssi_idx =
		    bcm4313_lcnphy_get_current_tx_pwr_idx(sc);
		sc->sc_lcn.lcnphy_tssi_npt = npt;
	}
}

/* wlc_lcnphy_tssi2dbm(). */
static int32_t
bcm4313_lcnphy_tssi2dbm(int32_t tssi, int32_t a1, int32_t b0, int32_t b1)
{
	int32_t a, b, p;

	a = 32768 + (a1 * tssi);
	b = (1024 * b0) + (64 * b1 * tssi);
	p = ((2 * b) + a) / (2 * a);

	return (p);
}

/* wlc_lcnphy_txpower_reset_npt(). */
static void
bcm4313_lcnphy_txpower_reset_npt(struct bcm4313_softc *sc)
{
	if (sc->sc_temppwrctrl_capable)
		return;

	sc->sc_lcn.lcnphy_tssi_idx = BCM4313_LCN_TX_PWR_CTRL_START_INDEX_2G_4313;
	sc->sc_lcn.lcnphy_tssi_npt = BCM4313_LCN_TX_PWR_CTRL_START_NPT;
}

/* wlc_lcnphy_txpower_recalc_target() (internal; see also the exported
 * bcm4313_lcnphy_txpower_recalc_target wrapper below). */
static void
bcm4313_lcnphy_txpower_recalc_target_internal(struct bcm4313_softc *sc)
{
	struct bcm4313_phytbl tab;
	uint32_t rate_table[BCM4313_NUM_RATES_CCK + BCM4313_NUM_RATES_OFDM +
	    BCM4313_NUM_RATES_MCS_1_STREAM];
	u_int i, j;

	if (sc->sc_temppwrctrl_capable)
		return;

	for (i = 0, j = 0; i < nitems(rate_table); i++, j++) {
		if (i == BCM4313_NUM_RATES_CCK + BCM4313_NUM_RATES_OFDM)
			j = BCM4313_TXP_FIRST_SISO_MCS_20;

		rate_table[i] = (uint32_t)((int32_t)(-sc->sc_lcn.tx_power_offset[j]));
	}

	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_len = nitems(rate_table);
	tab.tbl_ptr = rate_table;
	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_RATE_OFFSET;
	bcm4313_lcnphy_write_table(sc, &tab);

	if (bcm4313_lcnphy_get_target_tx_pwr(sc) != sc->sc_tx_power_min) {
		bcm4313_lcnphy_set_target_tx_pwr(sc, sc->sc_tx_power_min);
		bcm4313_lcnphy_txpower_reset_npt(sc);
	}
}

/* wlc_lcnphy_set_tx_pwr_soft_ctrl(). */
static void
bcm4313_lcnphy_set_tx_pwr_soft_ctrl(struct bcm4313_softc *sc, int8_t index)
{
	uint32_t cck_offset[4] = { 22, 22, 22, 22 };
	uint32_t ofdm_offset, reg_offset_cck;
	int i;
	uint16_t index2;
	struct bcm4313_phytbl tab;

	if (sc->sc_hwpwrctrl_capable)
		return;

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 14), (0x1) << 14);
	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 14), (0x0) << 14);
	bcm4313_phy_maskset(sc, 0x6da, 0xffff, 0x0040 |
	    (bcm4313_phy_read(sc, 0x6da) & ~(uint16_t)0x0040));

	reg_offset_cck = 0;
	for (i = 0; i < 4; i++)
		cck_offset[i] -= reg_offset_cck;
	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_len = 4;
	tab.tbl_ptr = cck_offset;
	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_RATE_OFFSET;
	bcm4313_lcnphy_write_table(sc, &tab);
	ofdm_offset = 0;
	tab.tbl_len = 1;
	tab.tbl_ptr = &ofdm_offset;
	for (i = 836; i < 862; i++) {
		tab.tbl_offset = i;
		bcm4313_lcnphy_write_table(sc, &tab);
	}

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 15), (0x1) << 15);
	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 14), (0x1) << 14);
	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 13), (0x1) << 13);

	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 7), (0) << 7);
	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 6), (0) << 6);
	bcm4313_phy_maskset(sc, 0x4a9, (0x1 << 15), (1) << 15);

	index2 = (uint16_t)(index * 2);
	bcm4313_phy_maskset(sc, 0x4a9, (0x1ff << 0), (index2) << 0);

	bcm4313_phy_maskset(sc, 0x6a3, (0x1 << 4), (0) << 4);
}

/* wlc_lcnphy_tempcompensated_txpwrctrl(). */
static int8_t
bcm4313_lcnphy_tempcompensated_txpwrctrl(struct bcm4313_softc *sc)
{
	int8_t index, delta_brd, delta_temp, new_index, tempcorrx;
	int16_t manp, meas_temp, temp_diff;
	bool neg = false;
	uint16_t temp;

	if (sc->sc_hwpwrctrl_capable)
		return ((int8_t)sc->sc_lcn.lcnphy_current_index);

	index = BCM4313_LCN_FIXED_TXPWR;

	if (sc->sc_lcn.lcnphy_tempsense_slope == 0)
		return (index);

	temp = (uint16_t)bcm4313_lcnphy_tempsense(sc, 0);
	meas_temp = BCM4313_LCN_TEMPSENSE(temp);

	if (sc->sc_tx_power_min != 0)
		delta_brd = (sc->sc_lcn.lcnphy_measPower - sc->sc_tx_power_min);
	else
		delta_brd = 0;

	manp = BCM4313_LCN_TEMPSENSE(sc->sc_lcn.lcnphy_rawtempsense);
	temp_diff = manp - meas_temp;
	if (temp_diff < 0) {
		neg = true;
		temp_diff = -temp_diff;
	}

	delta_temp = (int8_t)bcm4313_lcnphy_qdiv_roundup(
	    (uint32_t)(temp_diff * 192),
	    (uint32_t)(sc->sc_lcn.lcnphy_tempsense_slope * 10), 0);
	if (neg)
		delta_temp = -delta_temp;

	/* 4313 is LCN rev 1: tempcorrx is fixed at 4 below. */
	if (sc->sc_lcn.lcnphy_tempcorrx > 31)
		tempcorrx = (int8_t)(sc->sc_lcn.lcnphy_tempcorrx - 64);
	else
		tempcorrx = (int8_t)sc->sc_lcn.lcnphy_tempcorrx;
	tempcorrx = 4;
	new_index = index + delta_brd + delta_temp -
	    sc->sc_lcn.lcnphy_bandedge_corr;
	new_index += tempcorrx;

	index = 127;

	if (new_index < 0 || new_index > 126)
		return (index);

	return (new_index);
}

/* wlc_lcnphy_set_tx_pwr_ctrl_mode() / get/set_tx_pwr_ctrl(). */
static uint16_t
bcm4313_lcnphy_set_tx_pwr_ctrl_mode(struct bcm4313_softc *sc, uint16_t mode)
{
	uint16_t current_mode = mode;

	if (sc->sc_temppwrctrl_capable && mode == BCM4313_LCN_TX_PWR_CTRL_HW)
		current_mode = BCM4313_LCN_TX_PWR_CTRL_TEMPBASED;
	if (sc->sc_hwpwrctrl_capable &&
	    mode == BCM4313_LCN_TX_PWR_CTRL_TEMPBASED)
		current_mode = BCM4313_LCN_TX_PWR_CTRL_HW;
	return (current_mode);
}

static uint16_t
bcm4313_lcnphy_get_tx_pwr_ctrl(struct bcm4313_softc *sc)
{
	return (bcm4313_phy_read(sc, 0x4a4) &
	    ((0x1 << 15) | (0x1 << 14) | (0x1 << 13)));
}

static void
bcm4313_lcnphy_set_tx_pwr_ctrl(struct bcm4313_softc *sc, uint16_t mode)
{
	uint16_t old_mode = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	int8_t index;

	mode = bcm4313_lcnphy_set_tx_pwr_ctrl_mode(sc, mode);
	old_mode = bcm4313_lcnphy_set_tx_pwr_ctrl_mode(sc, old_mode);

	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 6),
	    ((BCM4313_LCN_TX_PWR_CTRL_HW == mode) ? 1 : 0) << 6);

	bcm4313_phy_maskset(sc, 0x6a3, (0x1 << 4),
	    ((BCM4313_LCN_TX_PWR_CTRL_HW == mode) ? 0 : 1) << 4);

	if (old_mode != mode) {
		if (BCM4313_LCN_TX_PWR_CTRL_HW == old_mode) {
			bcm4313_lcnphy_tx_pwr_update_npt(sc);
			bcm4313_lcnphy_clear_tx_power_offsets(sc);
		}
		if (BCM4313_LCN_TX_PWR_CTRL_HW == mode) {
			bcm4313_lcnphy_txpower_recalc_target_internal(sc);
			bcm4313_lcnphy_set_start_tx_pwr_idx(sc,
			    sc->sc_lcn.lcnphy_tssi_idx);
			bcm4313_lcnphy_set_tx_pwr_npt(sc,
			    sc->sc_lcn.lcnphy_tssi_npt);
			bcm4313_radio_maskset(sc, RADIO_2064_REG11F, 0x4, 0);

			sc->sc_lcn.lcnphy_tssi_tx_cnt =
			    bcm4313_lcnphy_total_tx_frames(sc);

			BCM4313_LCN_DISABLE_TX_GAIN_OVERRIDE(sc);
			sc->sc_lcn.lcnphy_tx_power_idx_override = -1;
		} else
			BCM4313_LCN_ENABLE_TX_GAIN_OVERRIDE(sc);

		bcm4313_phy_maskset(sc, 0x4a4,
		    ((0x1 << 15) | (0x1 << 14) | (0x1 << 13)), mode);
		if (mode == BCM4313_LCN_TX_PWR_CTRL_TEMPBASED) {
			index = bcm4313_lcnphy_tempcompensated_txpwrctrl(sc);
			bcm4313_lcnphy_set_tx_pwr_soft_ctrl(sc, index);
			sc->sc_lcn.lcnphy_current_index = (int8_t)
			    ((bcm4313_phy_read(sc, 0x4a9) & 0xFF) / 2);
		}
	}
}

/* wlc_lcnphy_set_start_tx_pwr_idx() (0x4a4 bits 8:0). */
static void
bcm4313_lcnphy_set_start_tx_pwr_idx(struct bcm4313_softc *sc, uint16_t idx)
{
	bcm4313_phy_maskset(sc, 0x4a4, (0x1ff << 0), (uint16_t)(idx) << 0);
}

/* wlc_lcnphy_set/get_tx_pwr_npt() (0x4a5 bits 10:8). */
static void
bcm4313_lcnphy_set_tx_pwr_npt(struct bcm4313_softc *sc, uint16_t npt)
{
	bcm4313_phy_maskset(sc, 0x4a5, (0x7 << 8), (uint16_t)(npt) << 8);
}

static uint16_t
bcm4313_lcnphy_get_tx_pwr_npt(struct bcm4313_softc *sc)
{
	return ((bcm4313_phy_read(sc, 0x4a5) & (0x7 << 8)) >> 8);
}

/* wlc_lcnphy_get/set_target_tx_pwr() (0x4a7 bits 7:0). */
static uint16_t
bcm4313_lcnphy_get_target_tx_pwr(struct bcm4313_softc *sc)
{
	return ((bcm4313_phy_read(sc, 0x4a7) & (0xff << 0)) >> 0);
}

static void
bcm4313_lcnphy_set_target_tx_pwr(struct bcm4313_softc *sc, uint16_t target)
{
	bcm4313_phy_maskset(sc, 0x4a7, (0xff << 0), (uint16_t)(target) << 0);
}

/* wlc_lcnphy_get_current_tx_pwr_idx_if_pwrctrl_on() (0x473 bits 8:0). */
static uint16_t
bcm4313_lcnphy_get_current_tx_pwr_idx_if_pwrctrl_on(struct bcm4313_softc *sc)
{
	return (bcm4313_phy_read(sc, 0x473) & 0x1ff);
}

/* wlc_lcnphy_set_tx_pwr_by_index(). */
static void
bcm4313_lcnphy_set_tx_pwr_by_index(struct bcm4313_softc *sc, int index)
{
	struct bcm4313_phytbl tab;
	uint16_t a, b;
	uint8_t bb_mult;
	uint32_t bbmultiqcomp, txgain, locoeffs, rfpower;
	struct bcm4313_lcnphy_txgains gains;

	sc->sc_lcn.lcnphy_tx_power_idx_override = (int8_t)index;
	sc->sc_lcn.lcnphy_current_index = (uint8_t)index;

	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_len = 1;

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);

	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_IQ_OFFSET + index;
	tab.tbl_ptr = &bbmultiqcomp;
	bcm4313_lcnphy_read_table(sc, &tab);

	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_GAIN_OFFSET + index;
	tab.tbl_width = 32;
	tab.tbl_ptr = &txgain;
	bcm4313_lcnphy_read_table(sc, &tab);

	gains.gm_gain = (uint16_t)(txgain & 0xff);
	gains.pga_gain = (uint16_t)(txgain >> 8) & 0xff;
	gains.pad_gain = (uint16_t)(txgain >> 16) & 0xff;
	gains.dac_gain = (uint16_t)(bbmultiqcomp >> 28) & 0x07;
	bcm4313_lcnphy_set_tx_gain(sc, &gains);
	bcm4313_lcnphy_set_pa_gain(sc, (uint16_t)(txgain >> 24) & 0x7f);

	bb_mult = (uint8_t)((bbmultiqcomp >> 20) & 0xff);
	bcm4313_lcnphy_set_bbmult(sc, bb_mult);

	BCM4313_LCN_ENABLE_TX_GAIN_OVERRIDE(sc);

	if (!sc->sc_temppwrctrl_capable) {
		a = (uint16_t)((bbmultiqcomp >> 10) & 0x3ff);
		b = (uint16_t)(bbmultiqcomp & 0x3ff);
		bcm4313_lcnphy_set_tx_iqcc(sc, a, b);

		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_LO_OFFSET + index;
		tab.tbl_ptr = &locoeffs;
		bcm4313_lcnphy_read_table(sc, &tab);

		bcm4313_lcnphy_set_tx_locc(sc, (uint16_t)locoeffs);

		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_PWR_OFFSET + index;
		tab.tbl_ptr = &rfpower;
		bcm4313_lcnphy_read_table(sc, &tab);
		bcm4313_phy_maskset(sc, 0x6a6, (0x1fff << 0),
		    (rfpower * 8) << 0);
	}
}

/*
 * ---------------------------------------------------------------------------
 * Q-format math (brcmsmac phy/phy_qmath.c), used by load_rfpower().
 * ---------------------------------------------------------------------------
 */
static uint16_t
bcm4313_qm_mulu16(uint16_t op1, uint16_t op2)
{
	return ((uint16_t)(((uint32_t)op1 * (uint32_t)op2) >> 16));
}

static int16_t
bcm4313_qm_muls16(int16_t op1, int16_t op2)
{
	int32_t result;

	if (op1 == (int16_t)0x8000 && op2 == (int16_t)0x8000)
		result = 0x7fffffff;
	else
		result = ((int32_t)op1 * (int32_t)op2);
	return ((int16_t)(result >> 15));
}

static int32_t
bcm4313_qm_add32(int32_t op1, int32_t op2)
{
	int32_t result;

	result = op1 + op2;
	if (op1 < 0 && op2 < 0 && result > 0)
		result = 0x80000000;
	else if (op1 > 0 && op2 > 0 && result < 0)
		result = 0x7fffffff;
	return (result);
}

static int16_t
bcm4313_qm_add16(int16_t op1, int16_t op2)
{
	int16_t result;
	int32_t temp = (int32_t)op1 + (int32_t)op2;

	if (temp > (int32_t)0x7fff)
		result = (int16_t)0x7fff;
	else if (temp < (int32_t)0xffff8000)
		result = (int16_t)0xffff8000;
	else
		result = (int16_t)temp;
	return (result);
}

static int16_t
bcm4313_qm_sub16(int16_t op1, int16_t op2)
{
	int16_t result;
	int32_t temp = (int32_t)op1 - (int32_t)op2;

	if (temp > (int32_t)0x7fff)
		result = (int16_t)0x7fff;
	else if (temp < (int32_t)0xffff8000)
		result = (int16_t)0xffff8000;
	else
		result = (int16_t)temp;
	return (result);
}

static int32_t
bcm4313_qm_shl32(int32_t op, int shift)
{
	int i;
	int32_t result;

	result = op;
	if (shift > 31)
		shift = 31;
	else if (shift < -31)
		shift = -31;
	if (shift >= 0) {
		for (i = 0; i < shift; i++)
			result = bcm4313_qm_add32(result, result);
	} else {
		result = result >> (-shift);
	}
	return (result);
}

static int16_t
bcm4313_qm_shl16(int16_t op, int shift)
{
	int i;
	int16_t result;

	result = op;
	if (shift > 15)
		shift = 15;
	else if (shift < -15)
		shift = -15;
	if (shift > 0) {
		for (i = 0; i < shift; i++)
			result = bcm4313_qm_add16(result, result);
	} else {
		result = result >> (-shift);
	}
	return (result);
}

static int16_t
bcm4313_qm_shr16(int16_t op, int shift)
{
	return (bcm4313_qm_shl16(op, -shift));
}

static int16_t
bcm4313_qm_norm32(int32_t op)
{
	uint16_t u16extraSignBits;

	if (op == 0) {
		return (31);
	} else {
		u16extraSignBits = 0;
		while ((op >> 31) == (op >> 30)) {
			u16extraSignBits++;
			op = op << 1;
		}
	}
	return (u16extraSignBits);
}

/* log2(1+(i/32)), i=[0:1:32], q.15. */
static const int16_t bcm4313_log_table[] = {
	0, 1455, 2866, 4236, 5568, 6863, 8124, 9352, 10549, 11716, 12855,
	13968, 15055, 16117, 17156, 18173, 19168, 20143, 21098, 22034, 22952,
	23852, 24736, 25604, 26455, 27292, 28114, 28922, 29717, 30498, 31267,
	32024, 32767
};
#define	BCM4313_LOG_TABLE_SIZE		32
#define	BCM4313_LOG2_LOG_TABLE_SIZE	5
#define	BCM4313_LOG10_2			19728	/* log10(2) in q.16 */

/* qm_log10(): log10(N) with qN in, log10N/qLog10N out. */
static void
bcm4313_qm_log10(int32_t n, int16_t qn, int16_t *log10n, int16_t *qlog10n)
{
	int16_t s16norm, s16tableIndex, s16errorApproximation;
	uint16_t u16offset;
	int32_t s32log;

	s16norm = bcm4313_qm_norm32(n);
	n = n << s16norm;
	qn = qn + s16norm - 30;
	s16tableIndex = (int16_t)(n >> (32 - (2 + BCM4313_LOG2_LOG_TABLE_SIZE)));
	s16tableIndex =
	    s16tableIndex & (int16_t)((1 << BCM4313_LOG2_LOG_TABLE_SIZE) - 1);
	n = n & ((1 << (32 - (2 + BCM4313_LOG2_LOG_TABLE_SIZE))) - 1);
	u16offset = (uint16_t)(n >> (32 - (2 + BCM4313_LOG2_LOG_TABLE_SIZE + 16)));
	s32log = bcm4313_log_table[s16tableIndex];
	s16errorApproximation = (int16_t)bcm4313_qm_mulu16(u16offset,
	    (uint16_t)(bcm4313_log_table[s16tableIndex + 1] -
	    bcm4313_log_table[s16tableIndex]));
	s32log = bcm4313_qm_add16((int16_t)s32log, s16errorApproximation);
	s32log = bcm4313_qm_add32(s32log, ((int32_t)-qn) << 15);
	s16norm = bcm4313_qm_norm32(s32log);
	s32log = bcm4313_qm_shl32(s32log, s16norm - 16);
	*log10n = bcm4313_qm_muls16((int16_t)s32log, (int16_t)BCM4313_LOG10_2);
	*qlog10n = 15 + s16norm - 16 + 1;
}

/*
 * ---------------------------------------------------------------------------
 * Channel-set path (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

/* wlc_lcnphy_toggle_afe_pwdn(). */
static void
bcm4313_lcnphy_toggle_afe_pwdn(struct bcm4313_softc *sc)
{
	uint16_t save_AfeCtrlOvrVal, save_AfeCtrlOvr;

	save_AfeCtrlOvrVal = bcm4313_phy_read(sc, 0x43c);
	save_AfeCtrlOvr = bcm4313_phy_read(sc, 0x43b);

	bcm4313_phy_write(sc, 0x43c, save_AfeCtrlOvrVal | 0x1);
	bcm4313_phy_write(sc, 0x43b, save_AfeCtrlOvr | 0x1);

	bcm4313_phy_write(sc, 0x43c, save_AfeCtrlOvrVal & 0xfffe);
	bcm4313_phy_write(sc, 0x43b, save_AfeCtrlOvr & 0xfffe);

	bcm4313_phy_write(sc, 0x43c, save_AfeCtrlOvrVal);
	bcm4313_phy_write(sc, 0x43b, save_AfeCtrlOvr);
}

/* wlc_lcnphy_txrx_spur_avoidance_mode(). */
static void
bcm4313_lcnphy_txrx_spur_avoidance_mode(struct bcm4313_softc *sc, bool enable)
{
	if (enable) {
		bcm4313_phy_write(sc, 0x942, 0x7);
		bcm4313_phy_write(sc, 0x93b, ((1 << 13) + 23));
		bcm4313_phy_write(sc, 0x93c, ((1 << 13) + 1989));

		bcm4313_phy_write(sc, 0x44a, 0x084);
		bcm4313_phy_write(sc, 0x44a, 0x080);
		bcm4313_phy_write(sc, 0x6d3, 0x2222);
		bcm4313_phy_write(sc, 0x6d3, 0x2220);
	} else {
		bcm4313_phy_write(sc, 0x942, 0x0);
		bcm4313_phy_write(sc, 0x93b, ((0 << 13) + 23));
		bcm4313_phy_write(sc, 0x93c, ((0 << 13) + 1989));
	}
	bcm4313_lcnphy_switch_macfreq(sc, enable);
}

/* wlc_lcnphy_set_chanspec_tweaks(). */
static void
bcm4313_lcnphy_set_chanspec_tweaks(struct bcm4313_softc *sc, uint8_t channel)
{
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	if (channel == 14)
		bcm4313_phy_maskset(sc, 0x448, (0x3 << 8), (2) << 8);
	else
		bcm4313_phy_maskset(sc, 0x448, (0x3 << 8), (1) << 8);

	lcn->lcnphy_bandedge_corr = 2;
	if (channel == 1)
		lcn->lcnphy_bandedge_corr = 4;

	if (channel == 1 || channel == 2 || channel == 3 ||
	    channel == 4 || channel == 9 ||
	    channel == 10 || channel == 11 || channel == 12) {
		bcm4313_lcnphy_pll_write(sc, 0x2, 0x03000c04);
		bcm4313_lcnphy_pll_maskset(sc, 0x3, ~0x00ffffff, 0x0);
		bcm4313_lcnphy_pll_write(sc, 0x4, 0x200005c0);

		bcm4313_lcnphy_pll_upd(sc);
		bcm4313_phy_write(sc, 0x942, 0);
		bcm4313_lcnphy_txrx_spur_avoidance_mode(sc, false);
		lcn->lcnphy_spurmod = false;
		bcm4313_phy_maskset(sc, 0x424, (0xff << 8), (0x1b) << 8);

		bcm4313_phy_write(sc, 0x425, 0x5907);
	} else {
		bcm4313_lcnphy_pll_write(sc, 0x2, 0x03140c04);
		bcm4313_lcnphy_pll_maskset(sc, 0x3, ~0x00ffffff, 0x333333);
		bcm4313_lcnphy_pll_write(sc, 0x4, 0x202c2820);

		bcm4313_lcnphy_pll_upd(sc);
		bcm4313_phy_write(sc, 0x942, 0);
		bcm4313_lcnphy_txrx_spur_avoidance_mode(sc, true);

		lcn->lcnphy_spurmod = false;
		bcm4313_phy_maskset(sc, 0x424, (0xff << 8), (0x1f) << 8);

		bcm4313_phy_write(sc, 0x425, 0x590a);
	}

	bcm4313_phy_maskset(sc, 0x44a, 0xffff, 0x44);
	bcm4313_phy_write(sc, 0x44a, 0x80);
}

/* wlc_lcnphy_radio_2064_channel_tune_4313(). */
static void
bcm4313_lcnphy_radio_2064_channel_tune_4313(struct bcm4313_softc *sc,
    uint8_t channel)
{
	uint32_t i;
	const struct chan_info_2064_lcnphy *ci;
	uint8_t pll_pwrup, pll_pwrup_ovr;
	int32_t qFcal;
	uint8_t d15, d16, f16, e44, e45;
	uint32_t div_int, div_frac, fvco3, fpfd, fref3, fcal_div;
	uint16_t loop_bw, d30, setCount;

	uint8_t h29, h28_ten, e30, h30_ten, cp_current;
	uint16_t g30, d28;

	ci = &chan_info_2064_lcnphy[0];

	bcm4313_radio_maskset(sc, 0x09d, 0x4, 0x1 << 2);

	bcm4313_radio_write(sc, 0x09e, 0xf);
	loop_bw = BCM4313_LCN_PLL_2064_LOOP_BW_DOUBLER;
	d30 = BCM4313_LCN_PLL_2064_D30_DOUBLER;

	/* 4313 is 2.4GHz-only; the 5G scan loop in brcmsmac is skipped. */
	for (i = 0; i < nitems(chan_info_2064_lcnphy); i++)
		if (chan_info_2064_lcnphy[i].chan == channel)
			break;

	if (i >= nitems(chan_info_2064_lcnphy))
		return;

	ci = &chan_info_2064_lcnphy[i];

	bcm4313_radio_write(sc, 0x02a, ci->logen_buftune);

	bcm4313_radio_maskset(sc, 0x030, 0x3, ci->logen_rccr_tx);

	bcm4313_radio_maskset(sc, 0x091, 0x3, ci->txrf_mix_tune_ctrl);

	bcm4313_radio_maskset(sc, 0x038, 0xf, ci->pa_input_tune_g);

	bcm4313_radio_maskset(sc, 0x030, 0x3 << 2, (ci->logen_rccr_rx) << 2);

	bcm4313_radio_maskset(sc, 0x05e, 0xf, ci->pa_rxrf_lna1_freq_tune);

	bcm4313_radio_maskset(sc, 0x05e, (0xf) << 4,
	    (ci->pa_rxrf_lna2_freq_tune) << 4);

	bcm4313_radio_write(sc, 0x06c, ci->rxrf_rxrf_spare1);

	pll_pwrup = (uint8_t)bcm4313_radio_read(sc, 0x044);
	pll_pwrup_ovr = (uint8_t)bcm4313_radio_read(sc, 0x12b);

	bcm4313_radio_maskset(sc, 0x044, 0xffff, bcm4313_radio_read(sc, 0x044) | 0x07);

	bcm4313_radio_maskset(sc, 0x12b, 0xffff, bcm4313_radio_read(sc, 0x12b) | (0x07) << 1);
	e44 = 0;
	e45 = 0;

	fpfd = sc->sc_xtalfreq << 1;
	if (sc->sc_xtalfreq > 26000000)
		e44 = 1;
	if (sc->sc_xtalfreq > 52000000)
		e45 = 1;
	if (e44 == 0)
		fcal_div = 1;
	else if (e45 == 0)
		fcal_div = 2;
	else
		fcal_div = 4;
	fvco3 = (ci->freq * 3);
	fref3 = 2 * fpfd;

	qFcal = sc->sc_xtalfreq * fcal_div / BCM4313_LCN_PLL_2064_MHZ;

	bcm4313_radio_write(sc, 0x04f, 0x02);

	d15 = (sc->sc_xtalfreq * fcal_div * 4 / 5) / BCM4313_LCN_PLL_2064_MHZ - 1;
	bcm4313_radio_write(sc, 0x052, (0x07 & (d15 >> 2)));
	bcm4313_radio_write(sc, 0x053, (d15 & 0x3) << 5);

	d16 = (qFcal * 8 / (d15 + 1)) - 1;
	bcm4313_radio_write(sc, 0x051, d16);

	f16 = ((d16 + 1) * (d15 + 1)) / qFcal;
	setCount = f16 * 3 * (ci->freq) / 32 - 1;
	bcm4313_radio_maskset(sc, 0x053, (0x0f << 0), (uint8_t)(setCount >> 8));

	bcm4313_radio_maskset(sc, 0x053, 0xffff, bcm4313_radio_read(sc, 0x053) | 0x10);
	bcm4313_radio_write(sc, 0x054, (uint8_t)(setCount & 0xff));

	div_int = ((fvco3 * (BCM4313_LCN_PLL_2064_MHZ >> 4)) / fref3) << 4;

	div_frac = ((fvco3 * (BCM4313_LCN_PLL_2064_MHZ >> 4)) % fref3) << 4;
	while (div_frac >= fref3) {
		div_int++;
		div_frac -= fref3;
	}
	div_frac = bcm4313_lcnphy_qdiv_roundup(div_frac, fref3, 20);

	bcm4313_radio_maskset(sc, 0x045, (0x1f << 0), (uint8_t)(div_int >> 4));
	bcm4313_radio_maskset(sc, 0x046, (0x1f << 4), (uint8_t)(div_int << 4));
	bcm4313_radio_maskset(sc, 0x046, (0x0f << 0), (uint8_t)(div_frac >> 16));
	bcm4313_radio_write(sc, 0x047, (uint8_t)(div_frac >> 8) & 0xff);
	bcm4313_radio_write(sc, 0x048, (uint8_t)div_frac & 0xff);

	bcm4313_radio_write(sc, 0x040, 0xfb);

	bcm4313_radio_write(sc, 0x041, 0x9a);
	bcm4313_radio_write(sc, 0x042, 0xa3);
	bcm4313_radio_write(sc, 0x043, 0x0c);

	h29 = BCM4313_LCN_BW_LMT / loop_bw;
	d28 = (((BCM4313_LCN_PLL_2064_HIGH_END_KVCO - BCM4313_LCN_PLL_2064_LOW_END_KVCO) *
	    (fvco3 / 2 - BCM4313_LCN_PLL_2064_LOW_END_VCO)) /
	    (BCM4313_LCN_PLL_2064_HIGH_END_VCO - BCM4313_LCN_PLL_2064_LOW_END_VCO))
	    + BCM4313_LCN_PLL_2064_LOW_END_KVCO;
	h28_ten = (d28 * 10) / BCM4313_LCN_VCO_DIV;
	e30 = (d30 - BCM4313_LCN_OFFSET) / BCM4313_LCN_FACT;
	g30 = BCM4313_LCN_OFFSET + (e30 * BCM4313_LCN_FACT);
	h30_ten = (g30 * 10) / BCM4313_LCN_CUR_DIV;
	cp_current = ((BCM4313_LCN_CUR_LMT * h29 * BCM4313_LCN_MULT * 100) / h28_ten) / h30_ten;
	bcm4313_radio_maskset(sc, 0x03c, 0x3f, cp_current);

	if (channel >= 1 && channel <= 5)
		bcm4313_radio_write(sc, 0x03c, 0x8);
	else
		bcm4313_radio_write(sc, 0x03c, 0x7);
	bcm4313_radio_write(sc, 0x03d, 0x3);

	bcm4313_radio_maskset(sc, 0x044, 0x0c, 0x0c);
	DELAY(1);

	bcm4313_2064_vco_cal(sc);

	bcm4313_radio_write(sc, 0x044, pll_pwrup);
	bcm4313_radio_write(sc, 0x12b, pll_pwrup_ovr);
	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1)) {
		bcm4313_radio_write(sc, 0x038, 3);
		bcm4313_radio_write(sc, 0x091, 7);
	}

	if (!(sc->sc_board.board_flags & BCM4313_BFL_FEM)) {
		static const uint8_t reg038[14] = {
			0xd, 0xe, 0xd, 0xd, 0xd, 0xc, 0xa,
			0xb, 0xb, 0x3, 0x3, 0x2, 0x0, 0x0
		};

		bcm4313_radio_write(sc, 0x02a, 0xf);
		bcm4313_radio_write(sc, 0x091, 0x3);
		bcm4313_radio_write(sc, 0x038, 0x3);

		bcm4313_radio_write(sc, 0x038, reg038[channel - 1]);
	}
}

/* wlc_2064_vco_cal(). */
static void
bcm4313_2064_vco_cal(struct bcm4313_softc *sc)
{
	uint8_t calnrst;

	bcm4313_radio_maskset(sc, 0x057, 1 << 3, 1 << 3);
	calnrst = (uint8_t)bcm4313_radio_read(sc, 0x056) & 0xf8;
	bcm4313_radio_write(sc, 0x056, calnrst);
	DELAY(1);
	bcm4313_radio_write(sc, 0x056, calnrst | 0x03);
	DELAY(1);
	bcm4313_radio_write(sc, 0x056, calnrst | 0x07);
	DELAY(300);
	bcm4313_radio_maskset(sc, 0x057, 1 << 3, 0);
}

/* wlc_lcnphy_load_tx_iir_filter(). */
static int
bcm4313_lcnphy_load_tx_iir_filter(struct bcm4313_softc *sc, bool is_ofdm,
    int16_t filt_type)
{
	int16_t filt_index = -1;
	int j;

	static const uint16_t addr[] = {
		0x910, 0x91e, 0x91f, 0x924, 0x925, 0x926, 0x920, 0x921,
		0x927, 0x928, 0x929, 0x922, 0x923, 0x930, 0x931, 0x932
	};

	static const uint16_t addr_ofdm[] = {
		0x90f, 0x900, 0x901, 0x906, 0x907, 0x908, 0x902, 0x903,
		0x909, 0x90a, 0x90b, 0x904, 0x905, 0x90c, 0x90d, 0x90e
	};

	if (!is_ofdm) {
		for (j = 0; j < LCNPHY_NUM_TX_DIG_FILTERS_CCK; j++) {
			if (filt_type == LCNPHY_txdigfiltcoeffs_cck[j][0]) {
				filt_index = (int16_t)j;
				break;
			}
		}

		if (filt_index != -1) {
			for (j = 0; j < LCNPHY_NUM_DIG_FILT_COEFFS; j++)
				bcm4313_phy_write(sc, addr[j],
				    LCNPHY_txdigfiltcoeffs_cck[filt_index][j + 1]);
		}
	} else {
		for (j = 0; j < LCNPHY_NUM_TX_DIG_FILTERS_OFDM; j++) {
			if (filt_type == LCNPHY_txdigfiltcoeffs_ofdm[j][0]) {
				filt_index = (int16_t)j;
				break;
			}
		}

		if (filt_index != -1) {
			for (j = 0; j < LCNPHY_NUM_DIG_FILT_COEFFS; j++)
				bcm4313_phy_write(sc, addr_ofdm[j],
				    LCNPHY_txdigfiltcoeffs_ofdm[filt_index][j + 1]);
		}
	}

	return ((filt_index != -1) ? 0 : -1);
}

/*
 * ---------------------------------------------------------------------------
 * TSSI / RSSI setup (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

/* wlc_lcnphy_rfseq_tbl_adc_pwrup(). */
static uint16_t
bcm4313_lcnphy_rfseq_tbl_adc_pwrup(struct bcm4313_softc *sc)
{
	uint16_t N1, N2, N3, N4, N5, N6, N;
	N1 = ((bcm4313_phy_read(sc, 0x4a5) & (0xff << 0)) >> 0);
	N2 = 1 << ((bcm4313_phy_read(sc, 0x4a5) & (0x7 << 12)) >> 12);
	N3 = ((bcm4313_phy_read(sc, 0x40d) & (0xff << 0)) >> 0);
	N4 = 1 << ((bcm4313_phy_read(sc, 0x40d) & (0x7 << 8)) >> 8);
	N5 = ((bcm4313_phy_read(sc, 0x4a2) & (0xff << 0)) >> 0);
	N6 = 1 << ((bcm4313_phy_read(sc, 0x4a2) & (0x7 << 8)) >> 8);
	N = 2 * (N1 + N2 + N3 + N4 + 2 * (N5 + N6)) + 80;
	if (N < 1600)
		N = 1600;
	return (N);
}

/* wlc_lcnphy_pwrctrl_rssiparams(). */
static void
bcm4313_lcnphy_pwrctrl_rssiparams(struct bcm4313_softc *sc)
{
	uint16_t auxpga_vmid, auxpga_vmid_temp, auxpga_gain_temp;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	auxpga_vmid = (2 << 8) | (lcn->lcnphy_rssi_vc << 4) | lcn->lcnphy_rssi_vf;
	auxpga_vmid_temp = (2 << 8) | (8 << 4) | 4;
	auxpga_gain_temp = 2;

	bcm4313_phy_maskset(sc, 0x4d8, (0x1 << 0), (0) << 0);

	bcm4313_phy_maskset(sc, 0x4d8, (0x1 << 1), (0) << 1);

	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 3), (0) << 3);

	bcm4313_phy_maskset(sc, 0x4db, (0x3ff << 0) | (0x7 << 12),
	    (auxpga_vmid << 0) | (lcn->lcnphy_rssi_gs << 12));

	bcm4313_phy_maskset(sc, 0x4dc, (0x3ff << 0) | (0x7 << 12),
	    (auxpga_vmid << 0) | (lcn->lcnphy_rssi_gs << 12));

	bcm4313_phy_maskset(sc, 0x40a, (0x3ff << 0) | (0x7 << 12),
	    (auxpga_vmid << 0) | (lcn->lcnphy_rssi_gs << 12));

	bcm4313_phy_maskset(sc, 0x40b, (0x3ff << 0) | (0x7 << 12),
	    (auxpga_vmid_temp << 0) | (auxpga_gain_temp << 12));

	bcm4313_phy_maskset(sc, 0x40c, (0x3ff << 0) | (0x7 << 12),
	    (auxpga_vmid_temp << 0) | (auxpga_gain_temp << 12));

	bcm4313_radio_maskset(sc, 0x082, (1 << 5), (1 << 5));
	bcm4313_radio_maskset(sc, 0x07c, (1 << 0), (1 << 0));
}

/* wlc_lcnphy_set_tssi_mux(). */
static void
bcm4313_lcnphy_set_tssi_mux(struct bcm4313_softc *sc, int pos)
{
	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 0), (0x1) << 0);

	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 6), (1) << 6);

	if (BCM4313_LCNPHY_TSSI_POST_PA == pos) {
		bcm4313_phy_maskset(sc, 0x4d9, (0x1 << 2), (0) << 2);

		bcm4313_phy_maskset(sc, 0x4d9, (0x1 << 3), (1) << 3);

		if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
			bcm4313_radio_maskset(sc, 0x086, 0x4, 0x4);
		} else {
			bcm4313_radio_maskset(sc, 0x03a, 1, 0x1);
			bcm4313_radio_maskset(sc, 0x11a, 0x8, 0x8);
			bcm4313_radio_maskset(sc, 0x028, 0x1, 0x0);
			bcm4313_radio_maskset(sc, 0x11a, 0x4, 1 << 2);
			bcm4313_radio_maskset(sc, 0x036, 0x10, 0x0);
			bcm4313_radio_maskset(sc, 0x11a, 0x10, 1 << 4);
			bcm4313_radio_maskset(sc, 0x036, 0x3, 0x0);
			bcm4313_radio_maskset(sc, 0x035, 0xff, 0x77);
			bcm4313_radio_maskset(sc, 0x028, 0x1e, 0xe << 1);
			bcm4313_radio_maskset(sc, 0x112, 0x80, 1 << 7);
			bcm4313_radio_maskset(sc, 0x005, 0x7, 1 << 1);
			bcm4313_radio_maskset(sc, 0x029, 0xf0, 0 << 4);
		}
	} else {
		bcm4313_phy_maskset(sc, 0x4d9, (0x1 << 2), (0x1) << 2);

		bcm4313_phy_maskset(sc, 0x4d9, (0x1 << 3), (0) << 3);

		if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
			bcm4313_radio_maskset(sc, 0x086, 0x4, 0x4);
		} else {
			bcm4313_radio_maskset(sc, 0x03a, 1, 0);
			bcm4313_radio_maskset(sc, 0x11a, 0x8, 0x8);
		}
	}
	bcm4313_phy_maskset(sc, 0x637, (0x3 << 14), (0) << 14);

	if (BCM4313_LCNPHY_TSSI_EXT == pos) {
		bcm4313_radio_write(sc, 0x07f, 1);
		bcm4313_radio_maskset(sc, 0x005, 0x7, 0x2);
		bcm4313_radio_maskset(sc, 0x112, 0x80, 0x1 << 7);
		bcm4313_radio_maskset(sc, 0x028, 0x1f, 0x3);
	}
}

/* wlc_lcnphy_tssi_setup(). */
static void
bcm4313_lcnphy_tssi_setup(struct bcm4313_softc *sc)
{
	struct bcm4313_phytbl tab;
	uint32_t rfseq, ind;
	int mode;
	uint8_t tssi_sel;

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM) {
		tssi_sel = 0x1;
		mode = BCM4313_LCNPHY_TSSI_EXT;
	} else {
		tssi_sel = 0xe;
		mode = BCM4313_LCNPHY_TSSI_POST_PA;
	}
	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_ptr = &ind;
	tab.tbl_len = 1;
	tab.tbl_offset = 0;
	for (ind = 0; ind < 128; ind++) {
		bcm4313_lcnphy_write_table(sc, &tab);
		tab.tbl_offset++;
	}
	tab.tbl_offset = 704;
	for (ind = 0; ind < 128; ind++) {
		bcm4313_lcnphy_write_table(sc, &tab);
		tab.tbl_offset++;
	}
	bcm4313_phy_maskset(sc, 0x503, (0x1 << 0), (0) << 0);

	bcm4313_phy_maskset(sc, 0x503, (0x1 << 2), (0) << 2);

	bcm4313_phy_maskset(sc, 0x503, (0x1 << 4), (1) << 4);

	bcm4313_lcnphy_set_tssi_mux(sc, mode);
	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 14), (0) << 14);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 15), (1) << 15);

	bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 5), (0) << 5);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1ff << 0), (0) << 0);

	bcm4313_phy_maskset(sc, 0x4a5, (0xff << 0), (255) << 0);

	bcm4313_phy_maskset(sc, 0x4a5, (0x7 << 12), (5) << 12);

	bcm4313_phy_maskset(sc, 0x4a5, (0x7 << 8), (0) << 8);

	bcm4313_phy_maskset(sc, 0x40d, (0xff << 0), (64) << 0);

	bcm4313_phy_maskset(sc, 0x40d, (0x7 << 8), (4) << 8);

	bcm4313_phy_maskset(sc, 0x4a2, (0xff << 0), (64) << 0);

	bcm4313_phy_maskset(sc, 0x4a2, (0x7 << 8), (4) << 8);

	bcm4313_phy_maskset(sc, 0x4d0, (0x1ff << 6), (0) << 6);

	bcm4313_phy_maskset(sc, 0x4a8, (0xff << 0), (0x1) << 0);

	bcm4313_lcnphy_clear_tx_power_offsets(sc);

	bcm4313_phy_maskset(sc, 0x4a6, (0x1 << 15), (1) << 15);

	bcm4313_phy_maskset(sc, 0x4a6, (0x1ff << 0), (0xff) << 0);

	bcm4313_phy_maskset(sc, 0x49a, (0x1ff << 0), (0xff) << 0);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
		bcm4313_radio_maskset(sc, 0x028, 0xf, tssi_sel);
		bcm4313_radio_maskset(sc, 0x086, 0x4, 0x4);
	} else {
		bcm4313_radio_maskset(sc, 0x028, 0x1e, tssi_sel << 1);
		bcm4313_radio_maskset(sc, 0x03a, 0x1, 1);
		bcm4313_radio_maskset(sc, 0x11a, 0x8, 1 << 3);
	}

	bcm4313_radio_write(sc, 0x025, 0xc);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
		bcm4313_radio_maskset(sc, 0x03a, 0x1, 1);
	} else {
		/* 4313 is 2.4GHz-only. */
		bcm4313_radio_maskset(sc, 0x03a, 0x2, 1 << 1);
	}

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2))
		bcm4313_radio_maskset(sc, 0x03a, 0x2, 1 << 1);
	else
		bcm4313_radio_maskset(sc, 0x03a, 0x4, 1 << 2);

	bcm4313_radio_maskset(sc, 0x11a, 0x1, 1 << 0);

	bcm4313_radio_maskset(sc, 0x005, 0x8, 1 << 3);

	if (!sc->sc_temppwrctrl_capable)
		bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 3) | (0x7 << 12),
		    0 << 3 | 2 << 12);

	rfseq = bcm4313_lcnphy_rfseq_tbl_adc_pwrup(sc);
	tab.tbl_id = BCM4313_LCN_TBL_ID_RFSEQ;
	tab.tbl_width = 16;
	tab.tbl_ptr = &rfseq;
	tab.tbl_len = 1;
	tab.tbl_offset = 6;
	bcm4313_lcnphy_write_table(sc, &tab);

	bcm4313_phy_maskset(sc, 0x938, (0x1 << 2), (1) << 2);

	bcm4313_phy_maskset(sc, 0x939, (0x1 << 2), (1) << 2);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 12), (1) << 12);

	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 2), (1) << 2);

	bcm4313_phy_maskset(sc, 0x4d7, (0xf << 8), (0) << 8);

	bcm4313_radio_maskset(sc, 0x035, 0xff, 0x0);
	bcm4313_radio_maskset(sc, 0x036, 0x3, 0x0);
	bcm4313_radio_maskset(sc, 0x11a, 0x8, 0x8);

	bcm4313_lcnphy_pwrctrl_rssiparams(sc);
}

/*
 * ---------------------------------------------------------------------------
 * IQ calibration (phy_lcn.c).
 * ---------------------------------------------------------------------------
 */

/* wlc_lcnphy_common_read_table() / common_write_table(). */
static void
bcm4313_lcnphy_common_read_table(struct bcm4313_softc *sc, uint32_t tbl_id,
    uint16_t *tbl_ptr, uint32_t tbl_len, uint32_t tbl_width,
    uint32_t tbl_offset)
{
	struct bcm4313_phytbl tab;
	tab.tbl_id = tbl_id;
	tab.tbl_ptr = tbl_ptr;
	tab.tbl_len = tbl_len;
	tab.tbl_width = tbl_width;
	tab.tbl_offset = tbl_offset;
	bcm4313_lcnphy_read_table(sc, &tab);
}

static void
bcm4313_lcnphy_common_write_table(struct bcm4313_softc *sc, uint32_t tbl_id,
    const uint16_t *tbl_ptr, uint32_t tbl_len, uint32_t tbl_width,
    uint32_t tbl_offset)
{
	struct bcm4313_phytbl tab;
	tab.tbl_id = tbl_id;
	tab.tbl_ptr = tbl_ptr;
	tab.tbl_len = tbl_len;
	tab.tbl_width = tbl_width;
	tab.tbl_offset = tbl_offset;
	bcm4313_lcnphy_write_table(sc, &tab);
}

/* wlc_lcnphy_tx_iqlo_loopback(). */
static void
bcm4313_lcnphy_tx_iqlo_loopback(struct bcm4313_softc *sc, uint16_t *values_to_save)
{
	uint16_t vmid;
	int i;

	for (i = 0; i < 20; i++)
		values_to_save[i] = bcm4313_radio_read(sc, iqlo_loopback_rf_regs[i]);

	bcm4313_phy_maskset(sc, 0x44c, (0x1 << 12), 1 << 12);
	bcm4313_phy_maskset(sc, 0x44d, (0x1 << 14), 1 << 14);

	bcm4313_phy_maskset(sc, 0x44c, (0x1 << 11), 1 << 11);
	bcm4313_phy_maskset(sc, 0x44d, (0x1 << 13), 0 << 13);

	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 1), 1 << 1);
	bcm4313_phy_maskset(sc, 0x43c, (0x1 << 1), 0 << 1);

	bcm4313_phy_maskset(sc, 0x43b, (0x1 << 0), 1 << 0);
	bcm4313_phy_maskset(sc, 0x43c, (0x1 << 0), 0 << 0);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2))
		bcm4313_radio_maskset(sc, 0x03a, 0xffff, bcm4313_radio_read(sc, 0x03a) & 0xFD);
	else
		bcm4313_radio_maskset(sc, 0x03a, 0xffff, bcm4313_radio_read(sc, 0x03a) & 0xF9);
	bcm4313_radio_maskset(sc, 0x11a, 0xffff, bcm4313_radio_read(sc, 0x11a) | 0x1);

	bcm4313_radio_maskset(sc, 0x036, 0xffff, bcm4313_radio_read(sc, 0x036) | 0x01);
	bcm4313_radio_maskset(sc, 0x11a, 0xffff, bcm4313_radio_read(sc, 0x11a) | 0x18);
	DELAY(20);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
		/* 4313 is 2.4GHz-only. */
		bcm4313_radio_maskset(sc, 0x03a, 0xffff, bcm4313_radio_read(sc, 0x03a) | 1);
	} else {
		bcm4313_radio_maskset(sc, 0x03a, 0xffff, bcm4313_radio_read(sc, 0x03a) | 0x3);
	}

	DELAY(20);

	bcm4313_radio_write(sc, 0x025, 0xf);
	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 2)) {
		bcm4313_radio_maskset(sc, 0x028, 0xf, 0x6);
	} else {
		bcm4313_radio_maskset(sc, 0x028, 0x1e, 0x6 << 1);
	}

	DELAY(20);

	bcm4313_radio_write(sc, 0x005, 0x8);
	bcm4313_radio_maskset(sc, 0x112, 0xffff, bcm4313_radio_read(sc, 0x112) | 0x80);
	DELAY(20);

	bcm4313_radio_maskset(sc, 0x0ff, 0xffff, bcm4313_radio_read(sc, 0x0ff) | 0x10);
	bcm4313_radio_maskset(sc, 0x11f, 0xffff, bcm4313_radio_read(sc, 0x11f) | 0x44);
	DELAY(20);

	bcm4313_radio_maskset(sc, 0x00b, 0xffff, bcm4313_radio_read(sc, 0x00b) | 0x7);
	bcm4313_radio_maskset(sc, 0x113, 0xffff, bcm4313_radio_read(sc, 0x113) | 0x10);
	DELAY(20);

	bcm4313_radio_write(sc, 0x007, 0x1);
	DELAY(20);

	vmid = 0x2a6;
	bcm4313_radio_maskset(sc, 0x0fc, 0x3 << 0, (vmid >> 8) & 0x3);
	bcm4313_radio_write(sc, 0x0fd, (vmid & 0xff));
	bcm4313_radio_maskset(sc, 0x11f, 0xffff, bcm4313_radio_read(sc, 0x11f) | 0x44);
	DELAY(20);

	bcm4313_radio_maskset(sc, 0x0ff, 0xffff, bcm4313_radio_read(sc, 0x0ff) | 0x10);
	DELAY(20);
	bcm4313_radio_write(sc, 0x012, 0x02);
	bcm4313_radio_maskset(sc, 0x112, 0xffff, bcm4313_radio_read(sc, 0x112) | 0x06);
	bcm4313_radio_write(sc, 0x036, 0x11);
	bcm4313_radio_write(sc, 0x059, 0xcc);
	bcm4313_radio_write(sc, 0x05c, 0x2e);
	bcm4313_radio_write(sc, 0x078, 0xd7);
	bcm4313_radio_write(sc, 0x092, 0x15);
}

/* wlc_lcnphy_tx_iqlo_loopback_cleanup(). */
static void
bcm4313_lcnphy_tx_iqlo_loopback_cleanup(struct bcm4313_softc *sc,
    uint16_t *values_to_save)
{
	int i;

	bcm4313_phy_maskset(sc, 0x44c, 0xffff, bcm4313_phy_read(sc, 0x44c) & (0x0 >> 11));

	bcm4313_phy_maskset(sc, 0x43b, 0xffff, bcm4313_phy_read(sc, 0x43b) & 0xC);

	for (i = 0; i < 20; i++)
		bcm4313_radio_write(sc, iqlo_loopback_rf_regs[i], values_to_save[i]);
}

/* wlc_lcnphy_iqcal_wait(). */
static bool
bcm4313_lcnphy_iqcal_wait(struct bcm4313_softc *sc)
{
	uint32_t delay_count = 0;

	while (BCM4313_LCNPHY_IQCAL_ACTIVE(sc)) {
		DELAY(100);
		delay_count++;

		if (delay_count > (10 * 500))
			break;
	}

	return (0 == BCM4313_LCNPHY_IQCAL_ACTIVE(sc));
}

/* wlc_lcnphy_tx_iqlo_cal(). */
static void
bcm4313_lcnphy_tx_iqlo_cal(struct bcm4313_softc *sc,
    const struct bcm4313_lcnphy_txgains *target_gains, int cal_mode,
    bool keep_tone)
{
	struct bcm4313_lcnphy_txgains cal_gains, temp_gains;
	uint16_t hash;
	int j;
	uint16_t ncorr_override[5];
	static const uint16_t syst_coeffs[11] = {
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000
	};

	static const uint16_t commands_fullcal[] = {
		0x8434, 0x8334, 0x8084, 0x8267, 0x8056, 0x8234
	};

	static const uint16_t commands_recal[] = {
		0x8434, 0x8334, 0x8084, 0x8267, 0x8056, 0x8234
	};

	static const uint16_t command_nums_fullcal[] = {
		0x7a97, 0x7a97, 0x7a97, 0x7a87, 0x7a87, 0x7b97
	};

	static const uint16_t command_nums_recal[] = {
		0x7a97, 0x7a97, 0x7a97, 0x7a87, 0x7a87, 0x7b97
	};
	const uint16_t *command_nums = command_nums_fullcal;

	const uint16_t *start_coeffs = NULL, *cal_cmds = NULL;
	uint16_t cal_type, diq_start;
	uint16_t tx_pwr_ctrl_old, save_txpwrctrlrfctrl2;
	uint16_t save_sslpnCalibClkEnCtrl, save_sslpnRxFeClkEnCtrl;
	bool tx_gain_override_old;
	struct bcm4313_lcnphy_txgains old_gains;
	uint32_t i, n_cal_cmds = 0, n_cal_start = 0;
	uint16_t values_to_save[20];
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	/* 4313 is 2.4GHz-only (WARN_ON(CHSPEC_IS5G) dropped). */

	save_sslpnRxFeClkEnCtrl = bcm4313_phy_read(sc, 0x6db);
	save_sslpnCalibClkEnCtrl = bcm4313_phy_read(sc, 0x6da);

	bcm4313_phy_maskset(sc, 0x6da, 0xffff, bcm4313_phy_read(sc, 0x6da) | 0x40);
	bcm4313_phy_maskset(sc, 0x6db, 0xffff, bcm4313_phy_read(sc, 0x6db) | 0x3);

	switch (cal_mode) {
	case BCM4313_LCNPHY_CAL_FULL:
		start_coeffs = syst_coeffs;
		cal_cmds = commands_fullcal;
		n_cal_cmds = nitems(commands_fullcal);
		break;

	case BCM4313_LCNPHY_CAL_RECAL:
		start_coeffs = syst_coeffs;
		cal_cmds = commands_recal;
		n_cal_cmds = nitems(commands_recal);
		command_nums = command_nums_recal;
		break;

	default:
		break;
	}

	bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    start_coeffs, 11, 16, 64);

	bcm4313_phy_write(sc, 0x6da, 0xffff);
	bcm4313_phy_maskset(sc, 0x503, (0x1 << 3), (1) << 3);

	tx_pwr_ctrl_old = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 12), (1) << 12);

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);

	save_txpwrctrlrfctrl2 = bcm4313_phy_read(sc, 0x4db);

	bcm4313_phy_maskset(sc, 0x4db, (0x3ff << 0), (0x2a6) << 0);

	bcm4313_phy_maskset(sc, 0x4db, (0x7 << 12), (2) << 12);

	bcm4313_lcnphy_tx_iqlo_loopback(sc, values_to_save);

	tx_gain_override_old = BCM4313_LCN_TX_GAIN_OVERRIDE_ENABLED(sc);
	if (tx_gain_override_old)
		bcm4313_lcnphy_get_tx_gain(sc, &old_gains);

	if (target_gains == NULL) {
		if (!tx_gain_override_old)
			bcm4313_lcnphy_set_tx_pwr_by_index(sc,
			    lcn->lcnphy_tssi_idx);
		bcm4313_lcnphy_get_tx_gain(sc, &temp_gains);
		target_gains = &temp_gains;
	}

	hash = (target_gains->gm_gain << 8) |
	    (target_gains->pga_gain << 4) | (target_gains->pad_gain);

	cal_gains = *target_gains;
	memset(ncorr_override, 0, sizeof(ncorr_override));
	for (j = 0; j < iqcal_gainparams_numgains_lcnphy[0]; j++) {
		if (hash == tbl_iqcal_gainparams_lcnphy[0][j][0]) {
			cal_gains.gm_gain = tbl_iqcal_gainparams_lcnphy[0][j][1];
			cal_gains.pga_gain = tbl_iqcal_gainparams_lcnphy[0][j][2];
			cal_gains.pad_gain = tbl_iqcal_gainparams_lcnphy[0][j][3];
			memcpy(ncorr_override,
			    &tbl_iqcal_gainparams_lcnphy[0][j][3],
			    sizeof(ncorr_override));
			break;
		}
	}

	bcm4313_lcnphy_set_tx_gain(sc, &cal_gains);

	bcm4313_phy_write(sc, 0x453, 0xaa9);
	bcm4313_phy_write(sc, 0x93d, 0xc0);

	bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    lcnphy_iqcal_loft_gainladder, nitems(lcnphy_iqcal_loft_gainladder),
	    16, 0);

	bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    lcnphy_iqcal_ir_gainladder, nitems(lcnphy_iqcal_ir_gainladder),
	    16, 32);

	bcm4313_lcnphy_start_tx_tone(sc, 3750, 88, 1);

	bcm4313_phy_write(sc, 0x6da, 0xffff);

	for (i = n_cal_start; i < n_cal_cmds; i++) {
		uint16_t zero_diq = 0;
		uint16_t best_coeffs[11];
		uint16_t command_num;

		cal_type = (cal_cmds[i] & 0x0f00) >> 8;

		command_num = command_nums[i];
		if (ncorr_override[cal_type])
			command_num =
			    ncorr_override[cal_type] << 8 | (command_num & 0xff);

		bcm4313_phy_write(sc, 0x452, command_num);

		if ((cal_type == 3) || (cal_type == 4)) {
			bcm4313_lcnphy_common_read_table(sc,
			    BCM4313_LCN_TBL_ID_IQLOCAL, &diq_start, 1, 16, 69);

			bcm4313_lcnphy_common_write_table(sc,
			    BCM4313_LCN_TBL_ID_IQLOCAL, &zero_diq, 1, 16, 69);
		}

		bcm4313_phy_write(sc, 0x451, cal_cmds[i]);

		if (!bcm4313_lcnphy_iqcal_wait(sc))
			goto cleanup;

		bcm4313_lcnphy_common_read_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
		    best_coeffs, nitems(best_coeffs), 16, 96);
		bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
		    best_coeffs, nitems(best_coeffs), 16, 64);

		if ((cal_type == 3) || (cal_type == 4))
			bcm4313_lcnphy_common_write_table(sc,
			    BCM4313_LCN_TBL_ID_IQLOCAL, &diq_start, 1, 16, 69);
		bcm4313_lcnphy_common_read_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
		    lcn->lcnphy_cal_results.txiqlocal_bestcoeffs,
		    nitems(lcn->lcnphy_cal_results.txiqlocal_bestcoeffs), 16, 96);
	}

	bcm4313_lcnphy_common_read_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    lcn->lcnphy_cal_results.txiqlocal_bestcoeffs,
	    nitems(lcn->lcnphy_cal_results.txiqlocal_bestcoeffs), 16, 96);
	lcn->lcnphy_cal_results.txiqlocal_bestcoeffs_valid = true;

	bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    &lcn->lcnphy_cal_results.txiqlocal_bestcoeffs[0], 4, 16, 80);

	bcm4313_lcnphy_common_write_table(sc, BCM4313_LCN_TBL_ID_IQLOCAL,
	    &lcn->lcnphy_cal_results.txiqlocal_bestcoeffs[5], 2, 16, 85);

cleanup:
	bcm4313_lcnphy_tx_iqlo_loopback_cleanup(sc, values_to_save);

	if (!keep_tone)
		bcm4313_lcnphy_stop_tx_tone(sc);

	bcm4313_phy_write(sc, 0x4db, save_txpwrctrlrfctrl2);

	bcm4313_phy_write(sc, 0x453, 0);

	if (tx_gain_override_old)
		bcm4313_lcnphy_set_tx_gain(sc, &old_gains);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, tx_pwr_ctrl_old);

	bcm4313_phy_write(sc, 0x6da, save_sslpnCalibClkEnCtrl);
	bcm4313_phy_write(sc, 0x6db, save_sslpnRxFeClkEnCtrl);
}

/* wlc_lcnphy_tx_pu(). */
static void
bcm4313_lcnphy_tx_pu(struct bcm4313_softc *sc, bool b_enable)
{
	if (!b_enable) {
		bcm4313_phy_maskset(sc, 0x43b, 0xffff,
		    bcm4313_phy_read(sc, 0x43b) & ~(uint16_t)((0x1 << 1) | (0x1 << 4)));

		bcm4313_phy_maskset(sc, 0x43c, (0x1 << 1), 1 << 1);

		bcm4313_phy_maskset(sc, 0x44c, 0xffff,
		    bcm4313_phy_read(sc, 0x44c) & ~(uint16_t)((0x1 << 3) |
		    (0x1 << 5) | (0x1 << 12) | (0x1 << 0) | (0x1 << 1) | (0x1 << 2)));

		bcm4313_phy_maskset(sc, 0x44d, 0xffff,
		    bcm4313_phy_read(sc, 0x44d) & ~(uint16_t)((0x1 << 3) |
		    (0x1 << 5) | (0x1 << 14)));
		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 2), 1 << 2);

		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 1) | (0x1 << 0), (0x1 << 0));

		bcm4313_phy_maskset(sc, 0x4f9, 0xffff,
		    bcm4313_phy_read(sc, 0x4f9) & ~(uint16_t)((0x1 << 0) |
		    (0x1 << 1) | (0x1 << 2)));

		bcm4313_phy_maskset(sc, 0x4fa, 0xffff,
		    bcm4313_phy_read(sc, 0x4fa) & ~(uint16_t)((0x1 << 0) |
		    (0x1 << 1) | (0x1 << 2)));
	} else {
		bcm4313_phy_maskset(sc, 0x43b, (0x1 << 1), 1 << 1);
		bcm4313_phy_maskset(sc, 0x43c, (0x1 << 1), 0 << 1);

		bcm4313_phy_maskset(sc, 0x43b, (0x1 << 4), 1 << 4);
		bcm4313_phy_maskset(sc, 0x43c, (0x1 << 6), 0 << 6);

		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 12), 1 << 12);
		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 14), 1 << 14);

		bcm4313_lcnphy_set_trsw_override(sc, true, false);

		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 2), 0 << 2);
		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 2), 1 << 2);

		/* 4313 is 2.4GHz-only (CHSPEC_IS2G). */
		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 3), 1 << 3);
		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 3), 1 << 3);

		bcm4313_phy_maskset(sc, 0x44c, (0x1 << 5), 1 << 5);
		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 5), 0 << 5);

		bcm4313_phy_maskset(sc, 0x4f9, (0x1 << 1), 1 << 1);
		bcm4313_phy_maskset(sc, 0x4fa, (0x1 << 1), 1 << 1);

		bcm4313_phy_maskset(sc, 0x4f9, (0x1 << 2), 1 << 2);
		bcm4313_phy_maskset(sc, 0x4fa, (0x1 << 2), 1 << 2);

		bcm4313_phy_maskset(sc, 0x4f9, (0x1 << 0), 1 << 0);
		bcm4313_phy_maskset(sc, 0x4fa, (0x1 << 0), 1 << 0);
	}
}

/* wlc_lcnphy_run_samples(). */
static void
bcm4313_lcnphy_run_samples(struct bcm4313_softc *sc, uint16_t num_samps,
    uint16_t num_loops, uint16_t wait, bool iqcalmode)
{
	bcm4313_phy_maskset(sc, 0x6da, 0xffff, bcm4313_phy_read(sc, 0x6da) | 0x8080);

	bcm4313_phy_maskset(sc, 0x642, (0x7f << 0), (num_samps - 1) << 0);
	if (num_loops != 0xffff)
		num_loops--;
	bcm4313_phy_maskset(sc, 0x640, (0xffff << 0), num_loops << 0);

	bcm4313_phy_maskset(sc, 0x641, (0xffff << 0), wait << 0);

	if (iqcalmode) {
		bcm4313_phy_maskset(sc, 0x453, 0xffff, bcm4313_phy_read(sc, 0x453) & ~(0x1 << 15));
		bcm4313_phy_maskset(sc, 0x453, 0xffff, bcm4313_phy_read(sc, 0x453) | (0x1 << 15));
	} else {
		bcm4313_phy_write(sc, 0x63f, 1);
		bcm4313_lcnphy_tx_pu(sc, 1);
	}

	bcm4313_radio_maskset(sc, 0x112, 0xffff, bcm4313_radio_read(sc, 0x112) | 0x6);
}

/* wlc_lcnphy_deaf_mode(). */
static void
bcm4313_lcnphy_deaf_mode(struct bcm4313_softc *sc, bool mode)
{
	/* 4313 is 20MHz-only (phybw40 = 0). */
	bcm4313_phy_maskset(sc, 0x4b0, (0x1 << 5), (mode) << 5);
	bcm4313_phy_maskset(sc, 0x4b1, (0x1 << 9), 0 << 9);

	/* 4313 is 2.4GHz (CHSPEC_IS2G). */
	bcm4313_phy_maskset(sc, 0x410, (0x1 << 6) | (0x1 << 5),
	    ((!mode)) << 6 | (!mode) << 5);
	bcm4313_phy_maskset(sc, 0x410, (0x1 << 7), (mode) << 7);
}

/* wlc_lcnphy_start_tx_tone(). */
static void
bcm4313_lcnphy_start_tx_tone(struct bcm4313_softc *sc, int32_t f_khz,
    uint16_t max_val, bool iqcalmode)
{
	uint8_t phy_bw;
	uint16_t num_samps, t, k;
	uint32_t bw;
	int32_t theta = 0, rot = 0;
	struct bcm4313_c32 tone_samp;
	uint32_t data_buf[64];
	uint16_t i_samp, q_samp;
	struct bcm4313_phytbl tab;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	bcm4313_lcnphy_deaf_mode(sc, true);

	phy_bw = 40;
	if (lcn->lcnphy_spurmod) {
		bcm4313_phy_write(sc, 0x942, 0x2);
		bcm4313_phy_write(sc, 0x93b, 0x0);
		bcm4313_phy_write(sc, 0x93c, 0x0);
		bcm4313_lcnphy_txrx_spur_avoidance_mode(sc, false);
	}

	if (f_khz) {
		k = 1;
		do {
			bw = phy_bw * 1000 * k;
			num_samps = bw / (f_khz < 0 ? -f_khz : f_khz);
			k++;
		} while ((num_samps *
		    (uint32_t)(f_khz < 0 ? -f_khz : f_khz)) != bw);
	} else
		num_samps = 2;

	rot = ((f_khz * 36) / phy_bw) / 100;
	theta = 0;

	for (t = 0; t < num_samps; t++) {
		tone_samp = bcm4313_cordic(theta);

		theta += rot;

		i_samp = (uint16_t)(BCM4313_CORDIC_FLOAT(tone_samp.i * max_val) & 0x3ff);
		q_samp = (uint16_t)(BCM4313_CORDIC_FLOAT(tone_samp.q * max_val) & 0x3ff);
		data_buf[t] = (i_samp << 10) | q_samp;
	}

	bcm4313_phy_maskset(sc, 0x6d6, (0x3 << 0), 0 << 0);

	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 3), 1 << 3);

	tab.tbl_ptr = data_buf;
	tab.tbl_len = num_samps;
	tab.tbl_id = BCM4313_LCN_TBL_ID_SAMPLEPLAY;
	tab.tbl_offset = 0;
	tab.tbl_width = 32;
	bcm4313_lcnphy_write_table(sc, &tab);

	bcm4313_lcnphy_run_samples(sc, num_samps, 0xffff, 0, iqcalmode);
}

/* wlc_lcnphy_stop_tx_tone(). */
static void
bcm4313_lcnphy_stop_tx_tone(struct bcm4313_softc *sc)
{
	int16_t playback_status;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	if (lcn->lcnphy_spurmod) {
		bcm4313_phy_write(sc, 0x942, 0x7);
		bcm4313_phy_write(sc, 0x93b, 0x2017);
		bcm4313_phy_write(sc, 0x93c, 0x27c5);
		bcm4313_lcnphy_txrx_spur_avoidance_mode(sc, true);
	}

	playback_status = bcm4313_phy_read(sc, 0x644);
	if (playback_status & (0x1 << 0)) {
		bcm4313_lcnphy_tx_pu(sc, 0);
		bcm4313_phy_maskset(sc, 0x63f, (0x1 << 1), 1 << 1);
	} else if (playback_status & (0x1 << 1))
		bcm4313_phy_maskset(sc, 0x453, (0x1 << 15), 0 << 15);

	bcm4313_phy_maskset(sc, 0x6d6, (0x3 << 0), 1 << 0);

	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 3), 0 << 3);

	bcm4313_phy_maskset(sc, 0x6da, (0x1 << 7), 0 << 7);

	bcm4313_radio_maskset(sc, 0x112, 0xfff9, 0);

	bcm4313_lcnphy_deaf_mode(sc, false);
}

/* wlc_lcnphy_samp_cap(). */
static void
bcm4313_lcnphy_samp_cap(struct bcm4313_softc *sc, int clip_detect_algo,
    uint16_t thresh, int16_t *ptr, int mode)
{
	uint32_t curval1, curval2, stpptr, curptr, strptr, val;
	uint16_t sslpnCalibClkEnCtrl, timer;
	uint16_t old_sslpnCalibClkEnCtrl;
	int16_t imag, real;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	timer = 0;
	old_sslpnCalibClkEnCtrl = bcm4313_phy_read(sc, 0x6da);

	curval1 = bcm4313_read_2(sc, BCM4313_D11_PSM_CORECTLSTS);
	ptr[130] = 0;
	bcm4313_write_2(sc, BCM4313_D11_PSM_CORECTLSTS,
	    (uint16_t)((1 << 6) | curval1));

	bcm4313_write_2(sc, BCM4313_D11_SMPL_CLCT_STRPTR, 0x7e00);
	bcm4313_write_2(sc, BCM4313_D11_SMPL_CLCT_STPPTR, 0x8000);
	DELAY(20);
	curval2 = bcm4313_read_2(sc, BCM4313_D11_PSM_PHY_HDR_PARAM);
	bcm4313_write_2(sc, BCM4313_D11_PSM_PHY_HDR_PARAM,
	    (uint16_t)(curval2 | 0x30));

	bcm4313_phy_write(sc, 0x555, 0x0);
	bcm4313_phy_write(sc, 0x5a6, 0x5);

	bcm4313_phy_write(sc, 0x5a2, (uint16_t)(mode | mode << 6));
	bcm4313_phy_write(sc, 0x5cf, 3);
	bcm4313_phy_write(sc, 0x5a5, 0x3);
	bcm4313_phy_write(sc, 0x583, 0x0);
	bcm4313_phy_write(sc, 0x584, 0x0);
	bcm4313_phy_write(sc, 0x585, 0x0fff);
	bcm4313_phy_write(sc, 0x586, 0x0000);

	bcm4313_phy_write(sc, 0x580, 0x4501);

	sslpnCalibClkEnCtrl = bcm4313_phy_read(sc, 0x6da);
	bcm4313_phy_write(sc, 0x6da, (uint16_t)(sslpnCalibClkEnCtrl | 0x2008));
	stpptr = bcm4313_read_2(sc, BCM4313_D11_SMPL_CLCT_STPPTR);
	curptr = bcm4313_read_2(sc, BCM4313_D11_SMPL_CLCT_CURPTR);
	do {
		DELAY(10);
		curptr = bcm4313_read_2(sc, BCM4313_D11_SMPL_CLCT_CURPTR);
		timer++;
	} while ((curptr != stpptr) && (timer < 500));

	bcm4313_write_2(sc, BCM4313_D11_PSM_PHY_HDR_PARAM, 0x2);
	strptr = 0x7e00;
	bcm4313_write_4(sc, BCM4313_D11_TPLATEWRPTR, strptr);
	while (strptr < 0x8000) {
		val = bcm4313_read_4(sc, BCM4313_D11_TPLATEWRDATA);
		imag = ((val >> 16) & 0x3ff);
		real = ((val) & 0x3ff);
		if (imag > 511)
			imag -= 1024;

		if (real > 511)
			real -= 1024;

		if (lcn->lcnphy_iqcal_swp_dis)
			ptr[(strptr - 0x7e00) / 4] = real;
		else
			ptr[(strptr - 0x7e00) / 4] = imag;

		if (clip_detect_algo) {
			if (imag > thresh || imag < -thresh) {
				strptr = 0x8000;
				ptr[130] = 1;
			}
		}

		strptr += 4;
	}

	bcm4313_phy_write(sc, 0x6da, old_sslpnCalibClkEnCtrl);
	bcm4313_write_2(sc, BCM4313_D11_PSM_PHY_HDR_PARAM, (uint16_t)curval2);
	bcm4313_write_2(sc, BCM4313_D11_PSM_CORECTLSTS, (uint16_t)curval1);
}

/* wlc_lcnphy_a1(): iterative coefficient search. */
static void
bcm4313_lcnphy_a1(struct bcm4313_softc *sc, int cal_type, int num_levels,
    int step_size_lg2)
{
	const struct lcnphy_spb_tone *phy_c1;
	struct lcnphy_spb_tone phy_c2;
	struct bcm4313_lcnphy_unsign16 phy_c3;
	int phy_c4, phy_c5, k, l, j, phy_c6;
	uint16_t phy_c7, phy_c8, phy_c9;
	int16_t phy_c10, phy_c11, phy_c12, phy_c13, phy_c14, phy_c15, phy_c16;
	int16_t *ptr, phy_c17;
	int32_t phy_c18, phy_c19;
	uint32_t phy_c20, phy_c21;
	bool phy_c22, phy_c23, phy_c24, phy_c25;
	uint16_t phy_c26, phy_c27;
	uint16_t phy_c28, phy_c29, phy_c30;
	uint16_t phy_c31;
	uint16_t phy_c32[20];
	int16_t ptr_buf[131];

	phy_c21 = 0;
	phy_c10 = phy_c13 = phy_c14 = phy_c8 = 0;
	ptr = ptr_buf;

	phy_c26 = bcm4313_phy_read(sc, 0x6da);
	phy_c27 = bcm4313_phy_read(sc, 0x6db);
	phy_c31 = bcm4313_radio_read(sc, 0x026);
	bcm4313_phy_write(sc, 0x93d, 0xc0);

	bcm4313_lcnphy_start_tx_tone(sc, 3750, 88, 0);
	bcm4313_phy_write(sc, 0x6da, 0xffff);
	bcm4313_phy_maskset(sc, 0x6db, 0xffff, bcm4313_phy_read(sc, 0x6db) | 0x3);

	bcm4313_lcnphy_tx_iqlo_loopback(sc, phy_c32);
	DELAY(500);
	phy_c28 = bcm4313_phy_read(sc, 0x938);
	phy_c29 = bcm4313_phy_read(sc, 0x4d7);
	phy_c30 = bcm4313_phy_read(sc, 0x4d8);
	bcm4313_phy_maskset(sc, 0x938, 0xffff, bcm4313_phy_read(sc, 0x938) | 0x1 << 2);
	bcm4313_phy_maskset(sc, 0x4d7, 0xffff, bcm4313_phy_read(sc, 0x4d7) | 0x1 << 2);
	bcm4313_phy_maskset(sc, 0x4d7, 0xffff, bcm4313_phy_read(sc, 0x4d7) | 0x1 << 3);
	bcm4313_phy_maskset(sc, 0x4d7, (0x7 << 12), 0x2 << 12);
	bcm4313_phy_maskset(sc, 0x4d8, 0xffff, bcm4313_phy_read(sc, 0x4d8) | 1 << 0);
	bcm4313_phy_maskset(sc, 0x4d8, 0xffff, bcm4313_phy_read(sc, 0x4d8) | 1 << 1);
	bcm4313_phy_maskset(sc, 0x4d8, (0x3ff << 2), 0x23a << 2);
	bcm4313_phy_maskset(sc, 0x4d8, (0x7 << 12), 0x7 << 12);
	phy_c1 = &lcnphy_spb_tone_3750[0];
	phy_c4 = 32;

	if (num_levels == 0) {
		if (cal_type != 0)
			num_levels = 4;
		else
			num_levels = 9;
	}
	if (step_size_lg2 == 0) {
		if (cal_type != 0)
			step_size_lg2 = 3;
		else
			step_size_lg2 = 8;
	}

	phy_c7 = (1 << step_size_lg2);
	phy_c3 = bcm4313_lcnphy_get_cc(sc, cal_type);
	phy_c15 = (int16_t)phy_c3.re;
	phy_c16 = (int16_t)phy_c3.im;
	if (cal_type == 2) {
		if (phy_c3.re > 127)
			phy_c15 = phy_c3.re - 256;
		if (phy_c3.im > 127)
			phy_c16 = phy_c3.im - 256;
	}
	bcm4313_lcnphy_set_cc(sc, cal_type, phy_c15, phy_c16);
	DELAY(20);
	for (phy_c8 = 0; phy_c7 != 0 && phy_c8 < num_levels; phy_c8++) {
		phy_c23 = true;
		phy_c22 = false;
		switch (cal_type) {
		case 0:
			phy_c10 = 511;
			break;
		case 2:
			phy_c10 = 127;
			break;
		case 3:
			phy_c10 = 15;
			break;
		case 4:
			phy_c10 = 15;
			break;
		}

		phy_c9 = bcm4313_phy_read(sc, 0x93d);
		phy_c9 = 2 * phy_c9;
		phy_c24 = false;
		phy_c5 = 7;
		phy_c25 = true;
		while (1) {
			bcm4313_radio_write(sc, 0x026,
			    (uint16_t)((phy_c5 & 0x7) | ((phy_c5 & 0x7) << 4)));
			DELAY(50);
			phy_c22 = false;
			ptr[130] = 0;
			bcm4313_lcnphy_samp_cap(sc, 1, phy_c9, &ptr[0], 2);
			if (ptr[130] == 1)
				phy_c22 = true;
			if (phy_c22)
				phy_c5 -= 1;
			if ((phy_c22 != phy_c24) && (!phy_c25))
				break;
			if (!phy_c22)
				phy_c5 += 1;
			if (phy_c5 <= 0 || phy_c5 >= 7)
				break;
			phy_c24 = phy_c22;
			phy_c25 = false;
		}

		if (phy_c5 < 0)
			phy_c5 = 0;
		else if (phy_c5 > 7)
			phy_c5 = 7;

		for (k = -phy_c7; k <= phy_c7; k += phy_c7) {
			for (l = -phy_c7; l <= phy_c7; l += phy_c7) {
				phy_c11 = phy_c15 + k;
				phy_c12 = phy_c16 + l;

				if (phy_c11 < -phy_c10)
					phy_c11 = -phy_c10;
				else if (phy_c11 > phy_c10)
					phy_c11 = phy_c10;
				if (phy_c12 < -phy_c10)
					phy_c12 = -phy_c10;
				else if (phy_c12 > phy_c10)
					phy_c12 = phy_c10;
				bcm4313_lcnphy_set_cc(sc, cal_type, phy_c11, phy_c12);
				DELAY(20);
				bcm4313_lcnphy_samp_cap(sc, 0, 0, ptr, 2);

				phy_c18 = 0;
				phy_c19 = 0;
				for (j = 0; j < 128; j++) {
					if (cal_type != 0)
						phy_c6 = j % phy_c4;
					else
						phy_c6 = (2 * j) % phy_c4;

					phy_c2.re = phy_c1[phy_c6].re;
					phy_c2.im = phy_c1[phy_c6].im;
					phy_c17 = ptr[j];
					phy_c18 = phy_c18 + phy_c17 * phy_c2.re;
					phy_c19 = phy_c19 + phy_c17 * phy_c2.im;
				}

				phy_c18 = phy_c18 >> 10;
				phy_c19 = phy_c19 >> 10;
				phy_c20 = ((phy_c18 * phy_c18) + (phy_c19 * phy_c19));

				if (phy_c23 || phy_c20 < phy_c21) {
					phy_c21 = phy_c20;
					phy_c13 = phy_c11;
					phy_c14 = phy_c12;
				}
				phy_c23 = false;
			}
		}
		phy_c23 = true;
		phy_c15 = phy_c13;
		phy_c16 = phy_c14;
		phy_c7 = phy_c7 >> 1;
		bcm4313_lcnphy_set_cc(sc, cal_type, phy_c15, phy_c16);
		DELAY(20);
	}
	goto cleanup;
cleanup:
	bcm4313_lcnphy_tx_iqlo_loopback_cleanup(sc, phy_c32);
	bcm4313_lcnphy_stop_tx_tone(sc);
	bcm4313_phy_write(sc, 0x6da, phy_c26);
	bcm4313_phy_write(sc, 0x6db, phy_c27);
	bcm4313_phy_write(sc, 0x938, phy_c28);
	bcm4313_phy_write(sc, 0x4d7, phy_c29);
	bcm4313_phy_write(sc, 0x4d8, phy_c30);
	bcm4313_radio_write(sc, 0x026, phy_c31);
}

/* wlc_lcnphy_tx_iqlo_soft_cal_full(). */
static void
bcm4313_lcnphy_tx_iqlo_soft_cal_full(struct bcm4313_softc *sc)
{
	bcm4313_lcnphy_set_cc(sc, 0, 0, 0);
	bcm4313_lcnphy_set_cc(sc, 2, 0, 0);
	bcm4313_lcnphy_set_cc(sc, 3, 0, 0);
	bcm4313_lcnphy_set_cc(sc, 4, 0, 0);

	bcm4313_lcnphy_a1(sc, 4, 0, 0);
	bcm4313_lcnphy_a1(sc, 3, 0, 0);
	bcm4313_lcnphy_a1(sc, 2, 3, 2);
	bcm4313_lcnphy_a1(sc, 0, 5, 8);
	bcm4313_lcnphy_a1(sc, 2, 2, 1);
	bcm4313_lcnphy_a1(sc, 0, 4, 3);

	bcm4313_lcnphy_get_cc(sc, 0);
	bcm4313_lcnphy_get_cc(sc, 2);
	bcm4313_lcnphy_get_cc(sc, 3);
	bcm4313_lcnphy_get_cc(sc, 4);
}

/* wlc_lcnphy_txpwrtbl_iqlo_cal(). */
static void
bcm4313_lcnphy_txpwrtbl_iqlo_cal(struct bcm4313_softc *sc)
{
	struct bcm4313_lcnphy_txgains target_gains, old_gains;
	uint8_t save_bb_mult;
	uint16_t a, b, didq, save_pa_gain = 0;
	uint32_t idx, save_txpwrindex = 0xff;
	uint32_t val;
	uint16_t save_txpwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	struct bcm4313_phytbl tab;
	uint8_t ei0, eq0, fi0, fq0;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	bcm4313_lcnphy_get_tx_gain(sc, &old_gains);
	save_pa_gain = bcm4313_lcnphy_get_pa_gain(sc);

	save_bb_mult = bcm4313_lcnphy_get_bbmult(sc);

	if (save_txpwrctrl == BCM4313_LCN_TX_PWR_CTRL_OFF)
		save_txpwrindex = bcm4313_lcnphy_get_current_tx_pwr_idx(sc);

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);

	target_gains.gm_gain = 7;
	target_gains.pga_gain = 0;
	target_gains.pad_gain = 21;
	target_gains.dac_gain = 0;
	bcm4313_lcnphy_set_tx_gain(sc, &target_gains);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1) || lcn->lcnphy_hw_iqcal_en) {
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, 30);

		bcm4313_lcnphy_tx_iqlo_cal(sc, &target_gains,
		    (lcn->lcnphy_recal ? BCM4313_LCNPHY_CAL_RECAL :
		    BCM4313_LCNPHY_CAL_FULL), false);
	} else {
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, 16);
		bcm4313_lcnphy_tx_iqlo_soft_cal_full(sc);
	}

	bcm4313_lcnphy_get_radio_loft(sc, &ei0, &eq0, &fi0, &fq0);
	if (((int8_t)fi0 == -15 || (int8_t)fi0 == 15) &&
	    ((int8_t)fq0 == -15 || (int8_t)fq0 == 15)) {
		/* 4313 is 2.4GHz-only. */
		target_gains.gm_gain = 7;
		target_gains.pga_gain = 45;
		target_gains.pad_gain = 186;
		target_gains.dac_gain = 0;

		if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1) ||
		    lcn->lcnphy_hw_iqcal_en) {
			target_gains.pga_gain = 0;
			target_gains.pad_gain = 30;
			bcm4313_lcnphy_set_tx_pwr_by_index(sc, 16);
			bcm4313_lcnphy_tx_iqlo_cal(sc, &target_gains,
			    BCM4313_LCNPHY_CAL_FULL, false);
		} else {
			bcm4313_lcnphy_tx_iqlo_soft_cal_full(sc);
		}
	}

	bcm4313_lcnphy_get_tx_iqcc(sc, &a, &b);

	didq = bcm4313_lcnphy_get_tx_locc(sc);

	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_ptr = &val;

	tab.tbl_len = 1;
	tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_RATE_OFFSET;

	for (idx = 0; idx < 128; idx++) {
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_IQ_OFFSET + idx;

		bcm4313_lcnphy_read_table(sc, &tab);
		val = (val & 0xfff00000) |
		    ((uint32_t)(a & 0x3ff) << 10) | (b & 0x3ff);
		bcm4313_lcnphy_write_table(sc, &tab);

		val = didq;
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_LO_OFFSET + idx;
		bcm4313_lcnphy_write_table(sc, &tab);
	}

	lcn->lcnphy_cal_results.txiqlocal_a = a;
	lcn->lcnphy_cal_results.txiqlocal_b = b;
	lcn->lcnphy_cal_results.txiqlocal_didq = didq;
	lcn->lcnphy_cal_results.txiqlocal_ei0 = ei0;
	lcn->lcnphy_cal_results.txiqlocal_eq0 = eq0;
	lcn->lcnphy_cal_results.txiqlocal_fi0 = fi0;
	lcn->lcnphy_cal_results.txiqlocal_fq0 = fq0;

	bcm4313_lcnphy_set_bbmult(sc, save_bb_mult);
	bcm4313_lcnphy_set_pa_gain(sc, save_pa_gain);
	bcm4313_lcnphy_set_tx_gain(sc, &old_gains);

	if (save_txpwrctrl != BCM4313_LCN_TX_PWR_CTRL_OFF)
		bcm4313_lcnphy_set_tx_pwr_ctrl(sc, save_txpwrctrl);
	else
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, save_txpwrindex);
}

/* wlc_lcnphy_idle_tssi_est(). */
static void
bcm4313_lcnphy_idle_tssi_est(struct bcm4313_softc *sc)
{
	bool suspend, tx_gain_override_old;
	struct bcm4313_lcnphy_txgains old_gains;
	uint16_t idleTssi0_2C, idleTssi0_OB, idleTssi0_regvalue_OB,
	    idleTssi0_regvalue_2C;
	uint16_t save_txpwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	uint16_t save_lpfgain = bcm4313_radio_read(sc, 0x112);
	uint16_t save_jtag_bb_afe_switch =
	    bcm4313_radio_read(sc, 0x007) & 1;
	uint16_t save_jtag_auxpga = bcm4313_radio_read(sc, 0x0ff) & 0x10;
	uint16_t save_iqadc_aux_en = bcm4313_radio_read(sc, 0x11f) & 4;
	uint8_t save_bbmult = bcm4313_lcnphy_get_bbmult(sc);

	(void)bcm4313_phy_read(sc, 0x4ab); /* idleTssi */
	suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
	    BCM4313_MCTL_EN_MAC));
	if (!suspend)
		bcm4313_lcnphy_suspend(sc);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);

	tx_gain_override_old = BCM4313_LCN_TX_GAIN_OVERRIDE_ENABLED(sc);
	bcm4313_lcnphy_get_tx_gain(sc, &old_gains);

	BCM4313_LCN_ENABLE_TX_GAIN_OVERRIDE(sc);
	bcm4313_lcnphy_set_tx_pwr_by_index(sc, 127);
	bcm4313_radio_write(sc, 0x112, 0x6);
	bcm4313_radio_maskset(sc, 0x007, 0x1, 1);
	bcm4313_radio_maskset(sc, 0x0ff, 0x10, 1 << 4);
	bcm4313_radio_maskset(sc, 0x11f, 0x4, 1 << 2);
	bcm4313_lcnphy_tssi_setup(sc);

	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 0), (1 << 0));
	bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 6), (1 << 6));

	bcm4313_lcnphy_set_bbmult(sc, 0x0);

	bcm4313_lcnphy_do_dummy_tx(sc, true, false);
	(void)bcm4313_phy_read(sc, 0x4ab); /* idleTssi */

	idleTssi0_2C = ((bcm4313_phy_read(sc, 0x63e) & (0x1ff << 0)) >> 0);

	if (idleTssi0_2C >= 256)
		idleTssi0_OB = idleTssi0_2C - 256;
	else
		idleTssi0_OB = idleTssi0_2C + 256;

	idleTssi0_regvalue_OB = idleTssi0_OB;
	if (idleTssi0_regvalue_OB >= 256)
		idleTssi0_regvalue_2C = idleTssi0_regvalue_OB - 256;
	else
		idleTssi0_regvalue_2C = idleTssi0_regvalue_OB + 256;
	bcm4313_phy_maskset(sc, 0x4a6, (0x1ff << 0),
	    (idleTssi0_regvalue_2C) << 0);

	bcm4313_phy_maskset(sc, 0x44c, (0x1 << 12), (0) << 12);

	bcm4313_lcnphy_set_bbmult(sc, save_bbmult);
	bcm4313_lcnphy_set_tx_gain_override(sc, tx_gain_override_old);
	bcm4313_lcnphy_set_tx_gain(sc, &old_gains);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, save_txpwrctrl);

	bcm4313_radio_write(sc, 0x112, save_lpfgain);
	bcm4313_radio_maskset(sc, 0x007, 0x1, save_jtag_bb_afe_switch);
	bcm4313_radio_maskset(sc, 0x0ff, 0x10, save_jtag_auxpga);
	bcm4313_radio_maskset(sc, 0x11f, 0x4, save_iqadc_aux_en);
	bcm4313_radio_maskset(sc, 0x112, 0x80, 1 << 7);
	if (!suspend)
		bcm4313_mac_enable(sc);
}

/* wlc_lcnphy_vbat_temp_sense_setup(). */
static void
bcm4313_lcnphy_vbat_temp_sense_setup(struct bcm4313_softc *sc, uint8_t mode)
{
	bool suspend;
	uint16_t save_txpwrCtrlEn;
	uint8_t auxpga_vmidcourse, auxpga_vmidfine, auxpga_gain;
	uint16_t auxpga_vmid;
	struct bcm4313_phytbl tab;
	uint32_t val;
	uint8_t save_reg007, save_reg0FF, save_reg11F, save_reg005, save_reg025,
	    save_reg112;
	uint16_t values_to_save[14];
	int8_t index;
	int i;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	DELAY(999);

	save_reg007 = (uint8_t)bcm4313_radio_read(sc, 0x007);
	save_reg0FF = (uint8_t)bcm4313_radio_read(sc, 0x0ff);
	save_reg11F = (uint8_t)bcm4313_radio_read(sc, 0x11f);
	save_reg005 = (uint8_t)bcm4313_radio_read(sc, 0x005);
	save_reg025 = (uint8_t)bcm4313_radio_read(sc, 0x025);
	save_reg112 = (uint8_t)bcm4313_radio_read(sc, 0x112);

	for (i = 0; i < 14; i++)
		values_to_save[i] = bcm4313_phy_read(sc, tempsense_phy_regs[i]);
	suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
	    BCM4313_MCTL_EN_MAC));
	if (!suspend)
		bcm4313_lcnphy_suspend(sc);
	save_txpwrCtrlEn = bcm4313_radio_read(sc, 0x4a4);

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);
	index = lcn->lcnphy_current_index;
	bcm4313_lcnphy_set_tx_pwr_by_index(sc, 127);
	bcm4313_radio_maskset(sc, 0x007, 0x1, 0x1);
	bcm4313_radio_maskset(sc, 0x0ff, 0x10, 0x1 << 4);
	bcm4313_radio_maskset(sc, 0x11f, 0x4, 0x1 << 2);
	bcm4313_phy_maskset(sc, 0x503, (0x1 << 0), (0) << 0);

	bcm4313_phy_maskset(sc, 0x503, (0x1 << 2), (0) << 2);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 14), (0) << 14);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 15), (0) << 15);

	bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 5), (0) << 5);

	bcm4313_phy_maskset(sc, 0x4a5, (0xff << 0), (255) << 0);

	bcm4313_phy_maskset(sc, 0x4a5, (0x7 << 12), (5) << 12);

	bcm4313_phy_maskset(sc, 0x4a5, (0x7 << 8), (0) << 8);

	bcm4313_phy_maskset(sc, 0x40d, (0xff << 0), (64) << 0);

	bcm4313_phy_maskset(sc, 0x40d, (0x7 << 8), (6) << 8);

	bcm4313_phy_maskset(sc, 0x4a2, (0xff << 0), (64) << 0);

	bcm4313_phy_maskset(sc, 0x4a2, (0x7 << 8), (6) << 8);

	bcm4313_phy_maskset(sc, 0x4d9, (0x7 << 4), (2) << 4);

	bcm4313_phy_maskset(sc, 0x4d9, (0x7 << 8), (3) << 8);

	bcm4313_phy_maskset(sc, 0x4d9, (0x7 << 12), (1) << 12);

	bcm4313_phy_maskset(sc, 0x4da, (0x1 << 12), (0) << 12);

	bcm4313_phy_maskset(sc, 0x4da, (0x1 << 13), (1) << 13);

	bcm4313_phy_maskset(sc, 0x4a6, (0x1 << 15), (1) << 15);

	bcm4313_radio_write(sc, 0x025, 0xc);

	bcm4313_radio_maskset(sc, 0x005, 0x8, 0x1 << 3);

	bcm4313_phy_maskset(sc, 0x938, (0x1 << 2), (1) << 2);

	bcm4313_phy_maskset(sc, 0x939, (0x1 << 2), (1) << 2);

	bcm4313_phy_maskset(sc, 0x4a4, (0x1 << 12), (1) << 12);

	val = bcm4313_lcnphy_rfseq_tbl_adc_pwrup(sc);
	tab.tbl_id = BCM4313_LCN_TBL_ID_RFSEQ;
	tab.tbl_width = 16;
	tab.tbl_len = 1;
	tab.tbl_ptr = &val;
	tab.tbl_offset = 6;
	bcm4313_lcnphy_write_table(sc, &tab);
	if (mode == BCM4313_LCN_TEMPSENSE_MODE) {
		bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 3), (1) << 3);

		bcm4313_phy_maskset(sc, 0x4d7, (0x7 << 12), (1) << 12);

		auxpga_vmidcourse = 8;
		auxpga_vmidfine = 0x4;
		auxpga_gain = 2;
		bcm4313_radio_maskset(sc, 0x082, 0x20, 1 << 5);
	} else {
		bcm4313_phy_maskset(sc, 0x4d7, (0x1 << 3), (1) << 3);

		bcm4313_phy_maskset(sc, 0x4d7, (0x7 << 12), (3) << 12);

		auxpga_vmidcourse = 7;
		auxpga_vmidfine = 0xa;
		auxpga_gain = 2;
	}
	auxpga_vmid =
	    (uint16_t)((2 << 8) | (auxpga_vmidcourse << 4) | auxpga_vmidfine);
	bcm4313_phy_maskset(sc, 0x4d8, (0x1 << 0), (1) << 0);

	bcm4313_phy_maskset(sc, 0x4d8, (0x3ff << 2), (auxpga_vmid) << 2);

	bcm4313_phy_maskset(sc, 0x4d8, (0x1 << 1), (1) << 1);

	bcm4313_phy_maskset(sc, 0x4d8, (0x7 << 12), (auxpga_gain) << 12);

	bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 5), (1) << 5);

	bcm4313_radio_write(sc, 0x112, 0x6);

	bcm4313_lcnphy_do_dummy_tx(sc, true, false);
	if (!bcm4313_lcnphy_tempsense_done(sc))
		DELAY(10);

	bcm4313_radio_write(sc, 0x007, (uint16_t)save_reg007);
	bcm4313_radio_write(sc, 0x0ff, (uint16_t)save_reg0FF);
	bcm4313_radio_write(sc, 0x11f, (uint16_t)save_reg11F);
	bcm4313_radio_write(sc, 0x005, (uint16_t)save_reg005);
	bcm4313_radio_write(sc, 0x025, (uint16_t)save_reg025);
	bcm4313_radio_write(sc, 0x112, (uint16_t)save_reg112);
	for (i = 0; i < 14; i++)
		bcm4313_phy_write(sc, tempsense_phy_regs[i], values_to_save[i]);
	bcm4313_lcnphy_set_tx_pwr_by_index(sc, (int)index);

	bcm4313_radio_write(sc, 0x4a4, save_txpwrCtrlEn);
	if (!suspend)
		bcm4313_mac_enable(sc);
	DELAY(999);
}

/* wlc_lcnphy_tx_pwr_ctrl_init(). */
static void
bcm4313_lcnphy_tx_pwr_ctrl_init(struct bcm4313_softc *sc)
{
	struct bcm4313_lcnphy_txgains tx_gains;
	uint8_t bbmult;
	struct bcm4313_phytbl tab;
	int32_t a1, b0, b1;
	int32_t tssi, pwr, mintargetpwr;
	bool suspend;

	suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
	    BCM4313_MCTL_EN_MAC));
	if (!suspend)
		bcm4313_lcnphy_suspend(sc);

	if (!sc->sc_hwpwrctrl_capable) {
		/* 4313 is 2.4GHz-only. */
		tx_gains.gm_gain = 4;
		tx_gains.pga_gain = 12;
		tx_gains.pad_gain = 12;
		tx_gains.dac_gain = 0;

		bbmult = 150;

		bcm4313_lcnphy_set_tx_gain(sc, &tx_gains);
		bcm4313_lcnphy_set_bbmult(sc, bbmult);
		bcm4313_lcnphy_vbat_temp_sense_setup(sc, BCM4313_LCN_TEMPSENSE_MODE);
	} else {
		bcm4313_lcnphy_idle_tssi_est(sc);

		bcm4313_lcnphy_clear_tx_power_offsets(sc);

		b0 = sc->sc_txpa_2g[0];
		b1 = sc->sc_txpa_2g[1];
		a1 = sc->sc_txpa_2g[2];
		mintargetpwr = bcm4313_lcnphy_tssi2dbm(125, a1, b0, b1);

		tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
		tab.tbl_width = 32;
		tab.tbl_ptr = &pwr;
		tab.tbl_len = 1;
		tab.tbl_offset = 0;
		for (tssi = 0; tssi < 128; tssi++) {
			pwr = bcm4313_lcnphy_tssi2dbm(tssi, a1, b0, b1);

			pwr = (pwr < mintargetpwr) ? mintargetpwr : pwr;
			bcm4313_lcnphy_write_table(sc, &tab);
			tab.tbl_offset++;
		}
		bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 0), (0) << 0);
		bcm4313_phy_maskset(sc, 0x4d3, (0xff << 0), (0) << 0);
		bcm4313_phy_maskset(sc, 0x4d3, (0xff << 8), (0) << 8);
		bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 4), (0) << 4);
		bcm4313_phy_maskset(sc, 0x4d0, (0x1 << 2), (0) << 2);

		bcm4313_phy_maskset(sc, 0x410, (0x1 << 7), (0) << 7);

		bcm4313_phy_write(sc, 0x4a8, 10);

		bcm4313_lcnphy_set_target_tx_pwr(sc, BCM4313_LCN_TARGET_PWR);

		bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_HW);
	}
	if (!suspend)
		bcm4313_mac_enable(sc);
}

/* tempsense_done() (phy_lcn.c). */
static bool
bcm4313_lcnphy_tempsense_done(struct bcm4313_softc *sc)
{
	return (0x8000 == (bcm4313_phy_read(sc, 0x476) & 0x8000));
}

/* wlc_lcnphy_clear_papd_comptable(). */
static void
bcm4313_lcnphy_clear_papd_comptable(struct bcm4313_softc *sc)
{
	uint32_t j;
	struct bcm4313_phytbl tab;
	uint32_t temp_offset[128];

	tab.tbl_ptr = temp_offset;
	tab.tbl_len = 128;
	tab.tbl_id = BCM4313_LCN_TBL_ID_PAPDCOMPDELTATBL;
	tab.tbl_width = 32;
	tab.tbl_offset = 0;

	memset(temp_offset, 0, sizeof(temp_offset));
	for (j = 1; j < 128; j += 2)
		temp_offset[j] = 0x80000;

	bcm4313_lcnphy_write_table(sc, &tab);
}

/* wlc_lcnphy_tempsense(). */
static uint16_t
bcm4313_lcnphy_tempsense(struct bcm4313_softc *sc, bool mode)
{
	uint16_t tempsenseval1, tempsenseval2;
	int32_t avg = 0;
	bool suspend = false;
	uint16_t save_txpwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	if (mode == 1) {
		suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
		    BCM4313_MCTL_EN_MAC));
		if (!suspend)
			bcm4313_lcnphy_suspend(sc);
		bcm4313_lcnphy_vbat_temp_sense_setup(sc, BCM4313_LCN_TEMPSENSE_MODE);
	}
	tempsenseval1 = bcm4313_phy_read(sc, 0x476) & 0x1ff;
	tempsenseval2 = bcm4313_phy_read(sc, 0x477) & 0x1ff;

	if (tempsenseval1 > 255)
		avg = (int)(tempsenseval1 - 512);
	else
		avg = (int)tempsenseval1;

	if (lcn->lcnphy_tempsense_option == 1 || sc->sc_hwpwrctrl_capable) {
		if (tempsenseval2 > 255)
			avg = (int)(avg - tempsenseval2 + 512);
		else
			avg = (int)(avg - tempsenseval2);
	} else {
		if (tempsenseval2 > 255)
			avg = (int)(avg + tempsenseval2 - 512);
		else
			avg = (int)(avg + tempsenseval2);
		avg = avg / 2;
	}
	if (avg < 0)
		avg = avg + 512;

	if (lcn->lcnphy_tempsense_option == 2)
		avg = tempsenseval1;

	if (mode)
		bcm4313_lcnphy_set_tx_pwr_ctrl(sc, save_txpwrctrl);

	if (mode == 1) {
		bcm4313_phy_maskset(sc, 0x448, (0x1 << 14), (1) << 14);

		DELAY(100);
		bcm4313_phy_maskset(sc, 0x448, (0x1 << 14), (0) << 14);

		if (!suspend)
			bcm4313_mac_enable(sc);
	}
	return ((uint16_t)avg);
}

/* wlc_lcnphy_afe_clk_init(). */
static void
bcm4313_lcnphy_afe_clk_init(struct bcm4313_softc *sc, uint8_t mode)
{
	/* 4313 is 20MHz-only (phybw40 = 0). */
	bcm4313_phy_maskset(sc, 0x6d1, (0x1 << 7), (1) << 7);

	if ((mode == BCM4313_LCN_AFE_CLK_INIT_MODE_PAPD) ||
	    (mode == BCM4313_LCN_AFE_CLK_INIT_MODE_TXRX2X))
		bcm4313_phy_write(sc, 0x6d0, 0x7);

	bcm4313_lcnphy_toggle_afe_pwdn(sc);
}

/* wlc_lcnphy_temp_adj(). */
static void
bcm4313_lcnphy_temp_adj(struct bcm4313_softc *sc)
{
	(void)sc;
}

/* wlc_lcnphy_glacial_timer_based_cal(). */
static void
bcm4313_lcnphy_glacial_timer_based_cal(struct bcm4313_softc *sc)
{
	bool suspend;
	int8_t index;
	uint16_t save_pwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
	    BCM4313_MCTL_EN_MAC));
	if (!suspend)
		bcm4313_lcnphy_suspend(sc);
	bcm4313_lcnphy_deaf_mode(sc, true);
	/* Re-arm the periodic recalibration loop (upstream:
	 * pi->phy_lastcal = pi->sh->now). */
	lcn->lcnphy_lastcal = lcn->lcnphy_now;
	index = lcn->lcnphy_current_index;

	bcm4313_lcnphy_txpwrtbl_iqlo_cal(sc);

	bcm4313_lcnphy_set_tx_pwr_by_index(sc, index);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, save_pwrctrl);
	bcm4313_lcnphy_deaf_mode(sc, false);
	if (!suspend)
		bcm4313_mac_enable(sc);
}

/* wlc_lcnphy_periodic_cal(). */
static void
bcm4313_lcnphy_periodic_cal(struct bcm4313_softc *sc)
{
	bool suspend;
	uint16_t save_pwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
	int8_t index;
	struct bcm4313_phytbl tab;
	int32_t a1, b0, b1;
	int32_t tssi, pwr, mintargetpwr;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	/* Re-arm the periodic recalibration loop (upstream:
	 * pi->phy_lastcal = pi->sh->now). */
	lcn->lcnphy_lastcal = lcn->lcnphy_now;
	lcn->lcnphy_full_cal_channel = sc->sc_curchan;
	index = lcn->lcnphy_current_index;

	suspend = (0 == (bcm4313_read_4(sc, BCM4313_D11_MACCONTROL) &
	    BCM4313_MCTL_EN_MAC));
	if (!suspend) {
		bcm4313_shm_write_2(sc, BCM4313_M_CTS_DURATION, 10000);
		bcm4313_lcnphy_suspend(sc);
	}

	bcm4313_lcnphy_deaf_mode(sc, true);

	bcm4313_lcnphy_txpwrtbl_iqlo_cal(sc);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1))
		bcm4313_lcnphy_rx_iq_cal(sc, NULL, 0, true, false, 1, 40);
	else
		bcm4313_lcnphy_rx_iq_cal(sc, NULL, 0, true, false, 1, 127);

	if (sc->sc_hwpwrctrl_capable) {
		bcm4313_lcnphy_idle_tssi_est(sc);

		b0 = sc->sc_txpa_2g[0];
		b1 = sc->sc_txpa_2g[1];
		a1 = sc->sc_txpa_2g[2];
		mintargetpwr = bcm4313_lcnphy_tssi2dbm(125, a1, b0, b1);

		tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
		tab.tbl_width = 32;
		tab.tbl_ptr = &pwr;
		tab.tbl_len = 1;
		tab.tbl_offset = 0;
		for (tssi = 0; tssi < 128; tssi++) {
			pwr = bcm4313_lcnphy_tssi2dbm(tssi, a1, b0, b1);
			pwr = (pwr < mintargetpwr) ? mintargetpwr : pwr;
			bcm4313_lcnphy_write_table(sc, &tab);
			tab.tbl_offset++;
		}
	}

	bcm4313_lcnphy_set_tx_pwr_by_index(sc, index);
	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, save_pwrctrl);
	bcm4313_lcnphy_deaf_mode(sc, false);
	if (!suspend)
		bcm4313_mac_enable(sc);
}

/* wlc_lcnphy_calib_modes(). */
void
bcm4313_lcnphy_calib_modes(struct bcm4313_softc *sc, uint32_t mode)
{
	uint16_t temp_new;
	int temp1, temp2, temp_diff;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	switch (mode) {
	case BCM4313_LCNPHY_PERICAL_CHAN:
		break;
	case BCM4313_LCN_FULLCAL:
		bcm4313_lcnphy_periodic_cal(sc);
		break;
	case BCM4313_LCNPHY_PERICAL_PHYINIT:
		bcm4313_lcnphy_periodic_cal(sc);
		break;
	case BCM4313_LCNPHY_PERICAL_WATCHDOG:
		if (sc->sc_temppwrctrl_capable) {
			temp_new = bcm4313_lcnphy_tempsense(sc, 0);
			temp1 = BCM4313_LCN_TEMPSENSE(temp_new);
			temp2 = BCM4313_LCN_TEMPSENSE(lcn->lcnphy_cal_temper);
			temp_diff = temp1 - temp2;
			if ((lcn->lcnphy_cal_counter > 90) ||
			    (temp_diff > 60) || (temp_diff < -60)) {
				bcm4313_lcnphy_glacial_timer_based_cal(sc);
				bcm4313_2064_vco_cal(sc);
				lcn->lcnphy_glacial_fires++;
				lcn->lcnphy_cal_temper = temp_new;
				lcn->lcnphy_cal_counter = 0;
			} else
				lcn->lcnphy_cal_counter++;
		}
		break;
	case BCM4313_LCNPHY_PERICAL_TEMPBASED_TXPWRCTRL:
		if (sc->sc_temppwrctrl_capable)
			bcm4313_lcnphy_tx_power_adjustment(sc);
		break;
	}
}

/* wlc_phy_watchdog() -- LCN branch (phy_cmn.c).  Called once per second
 * from bcm4313_watchdog() while the MAC is up. */
void
bcm4313_lcnphy_watchdog(struct bcm4313_softc *sc)
{
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	/* One per-second tick while the MAC is up (upstream sh->now). */
	lcn->lcnphy_now++;

	/*
	 * Defer while net80211 is hopping channels (upstream returns early
	 * for SCAN_RM_IN_PROGRESS / ASSOC_INPROG_PHY).  This port tracks
	 * scans only: between scan_end and S_RUN net80211 does not move the
	 * channel, so there is no separate association window to exclude.
	 */
	if (sc->sc_flags & BCM4313_FLAG_SCAN)
		return;

	/*
	 * Re-evaluate once PHY_SW_TIMER_GLACIAL has elapsed since the last
	 * full calibration, then every second until a glacial calibration
	 * re-arms the timer.  On boards without temperature-based power
	 * control the calib modes below self-gate and this is a no-op.
	 */
	if (lcn->lcnphy_now - lcn->lcnphy_lastcal <
	    BCM4313_LCNPHY_CAL_INTERVAL)
		return;

	bcm4313_lcnphy_calib_modes(sc,
	    BCM4313_LCNPHY_PERICAL_TEMPBASED_TXPWRCTRL);
	bcm4313_lcnphy_calib_modes(sc, BCM4313_LCNPHY_PERICAL_WATCHDOG);
}

/* wlc_lcnphy_tx_power_adjustment(). */
static void
bcm4313_lcnphy_tx_power_adjustment(struct bcm4313_softc *sc)
{
	int8_t index;
	uint16_t index2;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;
	uint16_t save_txpwrctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);

	if (sc->sc_temppwrctrl_capable && save_txpwrctrl) {
		index = bcm4313_lcnphy_tempcompensated_txpwrctrl(sc);
		index2 = (uint16_t)(index * 2);
		bcm4313_phy_maskset(sc, 0x4a9, (0x1ff << 0), (index2) << 0);

		lcn->lcnphy_current_index =
		    (int8_t)((bcm4313_phy_read(sc, 0x4a9) & 0xff) / 2);
	}
}

/* wlc_lcnphy_load_tx_gain_table(). */
static void
bcm4313_lcnphy_load_tx_gain_table(struct bcm4313_softc *sc,
    const struct lcnphy_tx_gain_tbl_entry *gain_table)
{
	uint32_t j;
	struct bcm4313_phytbl tab;
	uint32_t val;
	uint16_t pa_gain;
	uint16_t gm_gain;

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM)
		pa_gain = 0x10;
	else
		pa_gain = 0x60;
	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_len = 1;
	tab.tbl_ptr = &val;

	/* fixed gm_gain value for iPA */
	gm_gain = 15;
	for (j = 0; j < 128; j++) {
		if (sc->sc_board.board_flags & BCM4313_BFL_FEM)
			gm_gain = gain_table[j].gm;
		val = (((uint32_t)pa_gain << 24) |
		    (gain_table[j].pad << 16) |
		    (gain_table[j].pga << 8) | gm_gain);

		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_GAIN_OFFSET + j;
		bcm4313_lcnphy_write_table(sc, &tab);

		val = (gain_table[j].dac << 28) | (gain_table[j].bb_mult << 20);
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_IQ_OFFSET + j;
		bcm4313_lcnphy_write_table(sc, &tab);
	}
}

/* wlc_lcnphy_load_rfpower(). */
static void
bcm4313_lcnphy_load_rfpower(struct bcm4313_softc *sc)
{
	struct bcm4313_phytbl tab;
	uint32_t val, bbmult, rfgain;
	uint8_t index;
	uint8_t scale_factor = 1;
	int16_t temp, temp1, temp2, qQ, qQ1, qQ2, shift;

	tab.tbl_id = BCM4313_LCN_TBL_ID_TXPWRCTL;
	tab.tbl_width = 32;
	tab.tbl_len = 1;

	for (index = 0; index < 128; index++) {
		tab.tbl_ptr = &bbmult;
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_IQ_OFFSET + index;
		bcm4313_lcnphy_read_table(sc, &tab);
		bbmult = bbmult >> 20;

		tab.tbl_ptr = &rfgain;
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_GAIN_OFFSET + index;
		bcm4313_lcnphy_read_table(sc, &tab);

		bcm4313_qm_log10((int32_t)bbmult, 0, &temp1, &qQ1);
		bcm4313_qm_log10((int32_t)(1 << 6), 0, &temp2, &qQ2);

		if (qQ1 < qQ2) {
			temp2 = bcm4313_qm_shr16(temp2, qQ2 - qQ1);
			qQ = qQ1;
		} else {
			temp1 = bcm4313_qm_shr16(temp1, qQ1 - qQ2);
			qQ = qQ2;
		}
		temp = bcm4313_qm_sub16(temp1, temp2);

		if (qQ >= 4)
			shift = qQ - 4;
		else
			shift = 4 - qQ;

		val = (((index << shift) + (5 * temp) +
		    (1 << (scale_factor + shift - 3))) >> (scale_factor +
		    shift - 2));

		tab.tbl_ptr = &val;
		tab.tbl_offset = BCM4313_LCN_TX_PWR_CTRL_PWR_OFFSET + index;
		bcm4313_lcnphy_write_table(sc, &tab);
	}
}

/* wlc_lcnphy_bu_tweaks(). */
static void
bcm4313_lcnphy_bu_tweaks(struct bcm4313_softc *sc)
{
	bcm4313_phy_maskset(sc, 0x805, 0xffff, bcm4313_phy_read(sc, 0x805) | 0x1);

	bcm4313_phy_maskset(sc, 0x42f, (0x7 << 0), (0x3) << 0);

	bcm4313_phy_maskset(sc, 0x030, (0x7 << 0), (0x3) << 0);

	bcm4313_phy_write(sc, 0x414, 0x1e10);
	bcm4313_phy_write(sc, 0x415, 0x0640);

	bcm4313_phy_maskset(sc, 0x4df, (0xff << 8), -(9 << 8));

	bcm4313_phy_maskset(sc, 0x44a, 0xffff, bcm4313_phy_read(sc, 0x44a) | 0x44);
	bcm4313_phy_write(sc, 0x44a, 0x80);
	bcm4313_phy_maskset(sc, 0x434, (0xff << 0), (0xfd) << 0);

	bcm4313_phy_maskset(sc, 0x420, (0xff << 0), (16) << 0);

	if (!(sc->sc_board.board_rev < 0x1204))
		bcm4313_radio_maskset(sc, 0x09b, 0xf0, 0xf0);

	bcm4313_phy_write(sc, 0x7d6, 0x0902);
	bcm4313_phy_maskset(sc, 0x429, (0xf << 0), (0x9) << 0);

	bcm4313_phy_maskset(sc, 0x429, (0x3f << 4), (0xe) << 4);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1)) {
		bcm4313_phy_maskset(sc, 0x423, (0xff << 0), (0x46) << 0);

		bcm4313_phy_maskset(sc, 0x411, (0xff << 0), (1) << 0);

		bcm4313_phy_maskset(sc, 0x434, (0xff << 0), (0xff) << 0);

		bcm4313_phy_maskset(sc, 0x656, (0xf << 0), (2) << 0);

		bcm4313_phy_maskset(sc, 0x44d, (0x1 << 2), (1) << 2);

		bcm4313_radio_maskset(sc, 0x0f7, 0x4, 0x4);
		bcm4313_radio_maskset(sc, 0x0f1, 0x3, 0);
		bcm4313_radio_maskset(sc, 0x0f2, 0xf8, 0x90);
		bcm4313_radio_maskset(sc, 0x0f3, 0x3, 0x2);
		bcm4313_radio_maskset(sc, 0x0f3, 0xf0, 0xa0);

		bcm4313_radio_maskset(sc, 0x11f, 0x2, 0x2);

		bcm4313_lcnphy_clear_tx_power_offsets(sc);
		bcm4313_phy_maskset(sc, 0x4d0, (0x1ff << 6), (10) << 6);
	}
}

/* wlc_lcnphy_rcal(). */
static void
bcm4313_lcnphy_rcal(struct bcm4313_softc *sc)
{
	uint8_t rcal_value;

	bcm4313_radio_maskset(sc, 0x05b, 0xffff, bcm4313_radio_read(sc, 0x05b) & 0xfd);

	bcm4313_radio_maskset(sc, 0x004, 0xffff, bcm4313_radio_read(sc, 0x004) | 0x40);
	bcm4313_radio_maskset(sc, 0x120, 0xffff, bcm4313_radio_read(sc, 0x120) | 0x10);

	bcm4313_radio_maskset(sc, 0x078, 0xffff, bcm4313_radio_read(sc, 0x078) | 0x80);
	bcm4313_radio_maskset(sc, 0x129, 0xffff, bcm4313_radio_read(sc, 0x129) | 0x02);

	bcm4313_radio_maskset(sc, 0x057, 0xffff, bcm4313_radio_read(sc, 0x057) | 0x01);

	bcm4313_radio_maskset(sc, 0x05b, 0xffff, bcm4313_radio_read(sc, 0x05b) | 0x02);
	DELAY(5000);
	/* SPINWAIT(!wlc_radio_2064_rcal_done, 10s) with a bounded wait. */
	{
		int spin = 0;
		while (!(bcm4313_radio_read(sc, 0x05c) & 0x20) && spin < 1000000)
			spin++;
	}

	if (bcm4313_radio_read(sc, 0x05c) & 0x20) {
		rcal_value = (uint8_t)bcm4313_radio_read(sc, 0x05c);
		rcal_value = rcal_value & 0x1f;
	}

	bcm4313_radio_maskset(sc, 0x05b, 0xffff, bcm4313_radio_read(sc, 0x05b) & 0xfd);

	bcm4313_radio_maskset(sc, 0x057, 0xffff, bcm4313_radio_read(sc, 0x057) & 0xfe);
}

/* wlc_lcnphy_rc_cal(). */
static void
bcm4313_lcnphy_rc_cal(struct bcm4313_softc *sc)
{
	uint8_t dflt_rc_cal_val;
	uint16_t flt_val;

	dflt_rc_cal_val = 7;
	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1))
		dflt_rc_cal_val = 11;
	flt_val = (dflt_rc_cal_val << 10) | (dflt_rc_cal_val << 5) |
	    dflt_rc_cal_val;
	bcm4313_phy_write(sc, 0x933, flt_val);
	bcm4313_phy_write(sc, 0x934, flt_val);
	bcm4313_phy_write(sc, 0x935, flt_val);
	bcm4313_phy_write(sc, 0x936, flt_val);
	bcm4313_phy_write(sc, 0x937, (flt_val & 0x1ff));
}

/* wlc_radio_2064_init(). */
static void
bcm4313_radio_2064_init(struct bcm4313_softc *sc)
{
	uint32_t i;
	const struct lcnphy_radio_regs *lcnphyregs = NULL;

	lcnphyregs = lcnphy_radio_regs_2064;

	for (i = 0; lcnphyregs[i].address != 0xffff; i++) {
		/* 4313 is 2.4GHz-only (do_init_a branch dropped). */
		if (lcnphyregs[i].do_init_g)
			bcm4313_radio_write(sc,
			    (uint8_t)((lcnphyregs[i].address & 0x3fff) |
			    0x100),
			    (uint16_t)lcnphyregs[i].init_g);
	}

	bcm4313_radio_write(sc, 0x032, 0x62);
	bcm4313_radio_write(sc, 0x033, 0x19);

	bcm4313_radio_write(sc, 0x090, 0x10);

	bcm4313_radio_write(sc, 0x010, 0x00);

	if (BCM4313_LCNREV_IS(sc->sc_phy_rev, 1)) {
		bcm4313_radio_write(sc, 0x060, 0x7f);
		bcm4313_radio_write(sc, 0x061, 0x72);
		bcm4313_radio_write(sc, 0x062, 0x7f);
	}

	bcm4313_radio_write(sc, 0x01d, 0x02);
	bcm4313_radio_write(sc, 0x01e, 0x06);

	bcm4313_phy_maskset(sc, 0x4ea, (0x7 << 0), 0 << 0);

	bcm4313_phy_maskset(sc, 0x4ea, (0x7 << 3), 1 << 3);

	bcm4313_phy_maskset(sc, 0x4ea, (0x7 << 6), 2 << 6);

	bcm4313_phy_maskset(sc, 0x4ea, (0x7 << 9), 3 << 9);

	bcm4313_phy_maskset(sc, 0x4ea, (0x7 << 12), 4 << 12);

	bcm4313_phy_write(sc, 0x4ea, 0x4688);

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM)
		bcm4313_phy_maskset(sc, 0x4eb, (0x7 << 0), 2 << 0);
	else
		bcm4313_phy_maskset(sc, 0x4eb, (0x7 << 0), 3 << 0);

	bcm4313_phy_maskset(sc, 0x4eb, (0x7 << 6), 0 << 6);

	bcm4313_phy_maskset(sc, 0x46a, (0xffff << 0), 25 << 0);

	bcm4313_lcnphy_set_tx_locc(sc, 0);

	bcm4313_lcnphy_rcal(sc);

	bcm4313_lcnphy_rc_cal(sc);

	if (!(sc->sc_board.board_flags & BCM4313_BFL_FEM)) {
		bcm4313_radio_write(sc, 0x032, 0x6f);
		bcm4313_radio_write(sc, 0x033, 0x19);
		bcm4313_radio_write(sc, 0x039, 0xe);
	}
}

/* wlc_lcnphy_radio_init(). */
static void
bcm4313_lcnphy_radio_init(struct bcm4313_softc *sc)
{
	bcm4313_radio_2064_init(sc);
}

/* wlc_lcnphy_tbl_init(). */
static void
bcm4313_lcnphy_tbl_init(struct bcm4313_softc *sc)
{
	uint32_t idx;
	struct bcm4313_phytbl tab;
	const struct bcm4313_phytbl *tb;
	uint32_t val;

	for (idx = 0; idx < nitems(dot11lcnphytbl_info_rev0); idx++)
		bcm4313_lcnphy_write_table(sc, &dot11lcnphytbl_info_rev0[idx]);

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM_BT) {
		tab.tbl_id = BCM4313_LCN_TBL_ID_RFSEQ;
		tab.tbl_width = 16;
		tab.tbl_ptr = &val;
		tab.tbl_len = 1;
		val = 100;
		tab.tbl_offset = 4;
		bcm4313_lcnphy_write_table(sc, &tab);
	}

	if (!(sc->sc_board.board_flags & BCM4313_BFL_FEM)) {
		tab.tbl_id = BCM4313_LCN_TBL_ID_RFSEQ;
		tab.tbl_width = 16;
		tab.tbl_ptr = &val;
		tab.tbl_len = 1;

		val = 150;
		tab.tbl_offset = 0;
		bcm4313_lcnphy_write_table(sc, &tab);

		val = 220;
		tab.tbl_offset = 1;
		bcm4313_lcnphy_write_table(sc, &tab);
	}

	/* 4313 is 2.4GHz-only. */
	if (sc->sc_board.board_flags & BCM4313_BFL_FEM)
		bcm4313_lcnphy_load_tx_gain_table(sc,
		    dot11lcnphy_2GHz_extPA_gaintable_rev0);
	else
		bcm4313_lcnphy_load_tx_gain_table(sc,
		    dot11lcnphy_2GHz_gaintable_rev0);

	/* LCN rev 2-only rx-gain block dropped (4313 is rev 1). */

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM) {
		if (sc->sc_board.board_flags & BCM4313_BFL_FEM_BT) {
			if (sc->sc_board.board_rev < 0x1250)
				tb = &bcm4313_sw_ctrl_4313_bt_epa_info;
			else
				tb = &bcm4313_sw_ctrl_4313_bt_epa_p250_info;
		} else {
			tb = &bcm4313_sw_ctrl_4313_epa_info;
		}
	} else {
		if (sc->sc_board.board_flags & BCM4313_BFL_FEM_BT)
			tb = &bcm4313_sw_ctrl_4313_bt_ipa_info;
		else
			tb = &bcm4313_sw_ctrl_4313_plain_info;
	}
	bcm4313_lcnphy_write_table(sc, tb);
	bcm4313_lcnphy_load_rfpower(sc);

	bcm4313_lcnphy_clear_papd_comptable(sc);
}

/* wlc_lcnphy_rev0_baseband_init(). */
static void
bcm4313_lcnphy_rev0_baseband_init(struct bcm4313_softc *sc)
{
	bcm4313_radio_write(sc, 0x11c, 0x0);

	bcm4313_phy_write(sc, 0x43b, 0x0);
	bcm4313_phy_write(sc, 0x43c, 0x0);
	bcm4313_phy_write(sc, 0x44c, 0x0);
	bcm4313_phy_write(sc, 0x4e6, 0x0);
	bcm4313_phy_write(sc, 0x4f9, 0x0);
	bcm4313_phy_write(sc, 0x4b0, 0x0);
	bcm4313_phy_write(sc, 0x938, 0x0);
	bcm4313_phy_write(sc, 0x4b0, 0x0);
	bcm4313_phy_write(sc, 0x44e, 0);

	bcm4313_phy_maskset(sc, 0x567, 0xffff, bcm4313_phy_read(sc, 0x567) | 0x03);

	bcm4313_phy_maskset(sc, 0x44a, 0xffff, bcm4313_phy_read(sc, 0x44a) | 0x44);
	bcm4313_phy_write(sc, 0x44a, 0x80);

	if (!(sc->sc_board.board_flags & BCM4313_BFL_FEM))
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, 52);

	bcm4313_phy_maskset(sc, 0x634, (0xff << 0), 0xc << 0);
	if (sc->sc_board.board_flags & BCM4313_BFL_FEM) {
		bcm4313_phy_maskset(sc, 0x634, (0xff << 0), 0xa << 0);

		bcm4313_phy_write(sc, 0x910, 0x1);
	}

	bcm4313_phy_maskset(sc, 0x448, (0x3 << 8), 1 << 8);
	bcm4313_phy_maskset(sc, 0x608, (0xff << 0), 0x17 << 0);
	bcm4313_phy_maskset(sc, 0x604, (0x7ff << 0), 0x3ea << 0);
}

/* wlc_lcnphy_rev2_baseband_init() -- 5GHz-only, dropped (4313 is rev 1). */

/* wlc_lcnphy_agc_temp_init(). */
static void
bcm4313_lcnphy_agc_temp_init(struct bcm4313_softc *sc)
{
	int16_t temp;
	struct bcm4313_phytbl tab;
	uint32_t tableBuffer[2];
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	temp = (int16_t)bcm4313_phy_read(sc, 0x4df);
	lcn->lcnphy_ofdmgainidxtableoffset = (temp & (0xff << 0)) >> 0;

	if (lcn->lcnphy_ofdmgainidxtableoffset > 127)
		lcn->lcnphy_ofdmgainidxtableoffset -= 256;

	lcn->lcnphy_dsssgainidxtableoffset = (temp & (0xff << 8)) >> 8;

	if (lcn->lcnphy_dsssgainidxtableoffset > 127)
		lcn->lcnphy_dsssgainidxtableoffset -= 256;

	tab.tbl_ptr = tableBuffer;
	tab.tbl_len = 2;
	tab.tbl_id = 17;
	tab.tbl_offset = 59;
	tab.tbl_width = 32;
	bcm4313_lcnphy_read_table(sc, &tab);

	if (tableBuffer[0] > 63)
		tableBuffer[0] -= 128;
	lcn->lcnphy_tr_R_gain_val = tableBuffer[0];

	if (tableBuffer[1] > 63)
		tableBuffer[1] -= 128;
	lcn->lcnphy_tr_T_gain_val = tableBuffer[1];

	temp = (int16_t)(bcm4313_phy_read(sc, 0x434) & (0xff << 0));
	if (temp > 127)
		temp -= 256;
	lcn->lcnphy_input_pwr_offset_db = (int8_t)temp;

	lcn->lcnphy_Med_Low_Gain_db =
	    (bcm4313_phy_read(sc, 0x424) & (0xff << 8)) >> 8;
	lcn->lcnphy_Very_Low_Gain_db =
	    (bcm4313_phy_read(sc, 0x425) & (0xff << 0)) >> 0;

	tab.tbl_ptr = tableBuffer;
	tab.tbl_len = 2;
	tab.tbl_id = BCM4313_LCN_TBL_ID_GAIN_IDX;
	tab.tbl_offset = 28;
	tab.tbl_width = 32;
	bcm4313_lcnphy_read_table(sc, &tab);

	lcn->lcnphy_gain_idx_14_lowword = tableBuffer[0];
	lcn->lcnphy_gain_idx_14_hiword = tableBuffer[1];
}

/* wlc_lcnphy_baseband_init(). */
static void
bcm4313_lcnphy_baseband_init(struct bcm4313_softc *sc)
{
	bcm4313_lcnphy_tbl_init(sc);
	bcm4313_lcnphy_rev0_baseband_init(sc);
	bcm4313_lcnphy_bu_tweaks(sc);
}

/* wlc_phy_init_lcnphy(). */
void
bcm4313_lcnphy_init(struct bcm4313_softc *sc)
{
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	lcn->lcnphy_cal_counter = 0;
	lcn->lcnphy_cal_temper = lcn->lcnphy_rawtempsense;

	bcm4313_phy_maskset(sc, 0x44a, 0xffff, bcm4313_phy_read(sc, 0x44a) | 0x80);
	bcm4313_phy_maskset(sc, 0x44a, 0x7f, 0);

	bcm4313_lcnphy_afe_clk_init(sc, BCM4313_LCN_AFE_CLK_INIT_MODE_TXRX2X);

	bcm4313_phy_write(sc, 0x60a, 160);

	bcm4313_phy_write(sc, 0x46a, 25);

	bcm4313_lcnphy_baseband_init(sc);

	bcm4313_lcnphy_radio_init(sc);

	/* 4313 is 2.4GHz-only. */
	bcm4313_lcnphy_tx_pwr_ctrl_init(sc);

	bcm4313_lcnphy_set_chanspec(sc, sc->sc_curchan);

	bcm4313_lcnphy_regctl_maskset(sc, 0, ~0xf, 0x9);

	bcm4313_lcnphy_chipctl_maskset(sc, 0, 0x0, 0x03cddddd);

	if ((sc->sc_board.board_flags & BCM4313_BFL_FEM) &&
	    sc->sc_temppwrctrl_capable)
		bcm4313_lcnphy_set_tx_pwr_by_index(sc, BCM4313_LCN_FIXED_TXPWR);

	bcm4313_lcnphy_agc_temp_init(sc);

	bcm4313_lcnphy_temp_adj(sc);

	bcm4313_phy_maskset(sc, 0x448, (0x1 << 14), (1) << 14);

	DELAY(100);
	bcm4313_phy_maskset(sc, 0x448, (0x1 << 14), (0) << 14);

	bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_HW);
	lcn->lcnphy_noise_samples = BCM4313_LCN_NOISE_SAMPLES_DEFAULT;
	bcm4313_lcnphy_calib_modes(sc, BCM4313_LCNPHY_PERICAL_PHYINIT);
}

/* wlc_phy_chanspec_set_lcnphy(). */
void
bcm4313_lcnphy_set_chanspec(struct bcm4313_softc *sc, uint8_t channel)
{
	bcm4313_lcnphy_set_chanspec_tweaks(sc, channel);

	bcm4313_phy_maskset(sc, 0x44a, 0xffff, bcm4313_phy_read(sc, 0x44a) | 0x44);
	bcm4313_phy_write(sc, 0x44a, 0x80);

	bcm4313_lcnphy_radio_2064_channel_tune_4313(sc, channel);
	DELAY(1000);

	bcm4313_lcnphy_toggle_afe_pwdn(sc);

	bcm4313_phy_write(sc, 0x657, lcnphy_sfo_cfg[channel - 1].ptcentreTs20);
	bcm4313_phy_write(sc, 0x658, lcnphy_sfo_cfg[channel - 1].ptcentreFactor);

	if (channel == 14) {
		bcm4313_phy_maskset(sc, 0x448, (0x3 << 8), (2) << 8);

		bcm4313_lcnphy_load_tx_iir_filter(sc, false, 3);
	} else {
		bcm4313_phy_maskset(sc, 0x448, (0x3 << 8), (1) << 8);

		bcm4313_lcnphy_load_tx_iir_filter(sc, false, 2);
	}

	if (sc->sc_board.board_flags & BCM4313_BFL_FEM)
		bcm4313_lcnphy_load_tx_iir_filter(sc, true, 0);
	else
		bcm4313_lcnphy_load_tx_iir_filter(sc, true, 3);

	bcm4313_phy_maskset(sc, 0x4eb, (0x7 << 3), (1) << 3);
	if (sc->sc_hwpwrctrl_capable)
		bcm4313_lcnphy_tssi_setup(sc);
}

/* wlc_phy_txpwr_srom_read_lcnphy() -- SPROM-derived TX power limits.
 * The raw SPROM values are read at attach into the softc (sc_txpa_2g,
 * sc_tx_power_min, sc_cck2gpo, ...); this derives the per-rate maxima and
 * fills the lcnphy state exactly as brcmsmac does. */
void
bcm4313_lcnphy_txpwr_srom_read(struct bcm4313_softc *sc)
{
	int8_t txpwr = 0;
	int i;
	struct bcm4313_lcnphy *lcn = &sc->sc_lcn;

	/* 4313 is 2.4GHz-only. */
	{
		uint16_t cckpo = 0;
		uint32_t offset_ofdm, offset_mcs;

		txpwr = sc->sc_tx_power_min;
		sc->sc_tx_srom_max_rate_2g[0] = txpwr;

		for (i = BCM4313_TXP_FIRST_CCK; i <= BCM4313_TXP_LAST_CCK; i++)
			lcn->tx_power_offset[i] = 0;
		for (i = BCM4313_TXP_FIRST_OFDM; i <= BCM4313_TXP_LAST_OFDM; i++)
			lcn->tx_power_offset[i] = 0;
		for (i = BCM4313_TXP_FIRST_SISO_MCS_20; i <= BCM4313_TXP_LAST_SISO_MCS_20; i++)
			lcn->tx_power_offset[i] = 0;

		cckpo = sc->sc_cck2gpo;
		offset_ofdm = sc->sc_ofdm2gpo;
		if (cckpo) {
			uint32_t max_pwr_chan = txpwr;

			for (i = BCM4313_TXP_FIRST_CCK; i <= BCM4313_TXP_LAST_CCK; i++) {
				sc->sc_tx_srom_max_rate_2g[i] =
				    max_pwr_chan - ((cckpo & 0xf) * 2);
				cckpo >>= 4;
			}

			for (i = BCM4313_TXP_FIRST_OFDM; i <= BCM4313_TXP_LAST_OFDM; i++) {
				sc->sc_tx_srom_max_rate_2g[i] =
				    max_pwr_chan - ((offset_ofdm & 0xf) * 2);
				offset_ofdm >>= 4;
			}
		} else {
			for (i = BCM4313_TXP_FIRST_CCK; i <= BCM4313_TXP_LAST_CCK; i++)
				sc->sc_tx_srom_max_rate_2g[i] = txpwr;

			for (i = BCM4313_TXP_FIRST_OFDM; i <= BCM4313_TXP_LAST_OFDM; i++) {
				sc->sc_tx_srom_max_rate_2g[i] = txpwr -
				    ((offset_ofdm & 0xf) * 2);
				offset_ofdm >>= 4;
			}
			offset_mcs = sc->sc_mcs2gpo[1] << 16;
			offset_mcs |= sc->sc_mcs2gpo[0];
			lcn->lcnphy_mcs20_po = offset_mcs;
			for (i = BCM4313_TXP_FIRST_SISO_MCS_20;
			    i <= BCM4313_TXP_LAST_SISO_MCS_20; i++) {
				sc->sc_tx_srom_max_rate_2g[i] =
				    txpwr - ((offset_mcs & 0xf) * 2);
				offset_mcs >>= 4;
			}
		}
	}
	lcn->lcnphy_cck_dig_filt_type = -1;
}

/* wlc_phy_txpower_recalc_target_lcnphy(). */
void
bcm4313_lcnphy_txpower_recalc_target(struct bcm4313_softc *sc)
{
	uint16_t pwr_ctrl;

	if (sc->sc_temppwrctrl_capable) {
		bcm4313_lcnphy_calib_modes(sc, BCM4313_LCNPHY_PERICAL_TEMPBASED_TXPWRCTRL);
	} else if (sc->sc_hwpwrctrl_capable) {
		pwr_ctrl = bcm4313_lcnphy_get_tx_pwr_ctrl(sc);
		bcm4313_lcnphy_set_tx_pwr_ctrl(sc, BCM4313_LCN_TX_PWR_CTRL_OFF);
		bcm4313_lcnphy_txpower_recalc_target_internal(sc);
		bcm4313_lcnphy_set_tx_pwr_ctrl(sc, pwr_ctrl);
	}
}

/* wlc_phy_cal_init_lcnphy(). */
void
bcm4313_lcnphy_cal_init(struct bcm4313_softc *sc)
{
	(void)sc;
}

