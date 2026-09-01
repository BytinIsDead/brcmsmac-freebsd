/*-
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
 *
 * PCI front-end for the BCM4313 (if_bcm4313_pci(4)).
 *
 * Adapted from FreeBSD's if_bwn_pcivar.h:
 *   Copyright (c) 2015-2016 Landon Fuller <landonf@FreeBSD.org>
 *   All rights reserved.  (BSD-2-Clause)
 * Redistributions retain the above copyright notice and this list of
 * conditions (see the license text in the original source, or LICENSE).
 *
 * The FreeBSD bwn(4) PCI driver (if_bwn_pci.c) only lists BCM4331 /
 * BCM43224 / BCM43225 in its BCMA device table, so a BCM4313 (14e4:4727)
 * never gets a PCI->bhnd bridge: no bhnd bus is created, the D11 core is
 * never enumerated, and no driver can attach.  This front-end claims the
 * 4313's PCI id and creates the bhnd(4) bridge chain itself, so that
 * "kldload if_bcm4313" is sufficient to bring up the whole stack.
 *
 * $FreeBSD$
 */

#ifndef	_IF_BCM4313_PCIVAR_H_
#define	_IF_BCM4313_PCIVAR_H_

struct bcm4313_pci_devcfg;

/** bcm4313_pci per-instance state. */
struct bcm4313_pci_softc {
	device_t			dev;	/**< device */
	device_t			bhndb_dev;	/**< bhnd bridge device */
	const struct bcm4313_pci_devcfg	*devcfg;	/**< device config */
};

/* PCI device descriptor */
struct bcm4313_pci_device {
	uint16_t	vendor;
	uint16_t	device;
	const char	*desc;
};

#define	BCM4313_BCM_DEV(_devid, _desc)		\
    { PCI_VENDOR_BROADCOM, PCI_DEVID_ ## _devid, \
        "Broadcom " _desc " Wireless" }

/* Supported device table */
struct bcm4313_pci_devcfg {
	const struct bhndb_hwcfg	*bridge_hwcfg;
	const struct bhndb_hw		*bridge_hwtable;
	const struct bhndb_hw_priority	*bridge_hwprio;
	const struct bcm4313_pci_device	*devices;
};

#endif /* _IF_BCM4313_PCIVAR_H_ */
