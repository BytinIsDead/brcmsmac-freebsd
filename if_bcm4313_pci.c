/*-
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
 *
 * PCI front-end for the Broadcom BCM4313 (if_bcm4313_pci(4)).
 *
 * Adapted from FreeBSD's if_bwn_pci.c:
 *   Copyright (c) 2015-2016 Landon Fuller <landonf@FreeBSD.org>
 *   All rights reserved.  (BSD-2-Clause)
 * See LICENSE for the combined license terms.
 *
 * Why this file exists: FreeBSD's bwn(4) PCI driver claims the BCM4331 /
 * BCM43224 / BCM43225 PCI ids in its BCMA device table, but NOT the
 * BCM4313 (14e4:4727).  The bhnd(4) bridge chain is created by a PCI
 * front-end (bwn_pci) attaching to the PCI device and instantiating a
 * bhndb bridge child; without a front-end that lists the 4313, the chip
 * never becomes a bhnd bus, the D11 core is never enumerated, and the
 * if_bcm4313(4) D11 driver has nothing to attach to -- the module loads
 * silently but nothing ever shows up.
 *
 * This front-end mirrors if_bwn_pci.c for the single 4313 device: it
 * probes 14e4:4727, attaches a bhndb(4) bridge (bhndb_pci_driver) with
 * the generic BCMA hardware config, and the bcma_bhndb/bhnd(4) stack
 * then enumerates the D11 core that if_bcm4313.c binds to.  Loading
 * if_bcm4313.ko now pulls the whole chain via MODULE_DEPEND.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/bhnd/bhndb/bhndb_pcivar.h>
#include <dev/bhnd/bhndb/bhndb_hwdata.h>
#include <dev/bhnd/bhndb/bhndb_pci_hwdata.h>
#include <dev/bhnd/bhnd_ids.h>

#include "bhndb_bus_if.h"

#include "if_bcm4313_pcivar.h"

/* BCMA devices: the BCM4313 (PCIe, 802.11n 2.4GHz single-band). */
static const struct bcm4313_pci_device bcm4313_bcma_devices[] = {
	BCM4313_BCM_DEV(BCM4313_D11N2G, "BCM4313 802.11n 2GHz"),
	{ 0, 0, NULL }
};

/* Device configuration table (BCMA only; the 4313 is a PCIe/BCMA chip). */
static const struct bcm4313_pci_devcfg bcm4313_pci_devcfgs[] = {
	{
		.bridge_hwcfg	= &bhndb_pci_bcma_generic_hwcfg,
		.bridge_hwtable	= bhndb_pci_generic_hw_table,
		.bridge_hwprio	= bhndb_bcma_priority_table,
		.devices	= bcm4313_bcma_devices
	},
	{ NULL, NULL, NULL, NULL }
};

/** Search the device configuration table for an entry matching @p dev. */
static int
bcm4313_pci_find_devcfg(device_t dev, const struct bcm4313_pci_devcfg **cfg,
    const struct bcm4313_pci_device **device)
{
	const struct bcm4313_pci_devcfg	*dvc;
	const struct bcm4313_pci_device	*dv;

	for (dvc = bcm4313_pci_devcfgs; dvc->devices != NULL; dvc++) {
		for (dv = dvc->devices; dv->device != 0; dv++) {
			if (pci_get_vendor(dev) == dv->vendor &&
			    pci_get_device(dev) == dv->device) {
				if (cfg != NULL)
					*cfg = dvc;
				if (device != NULL)
					*device = dv;
				return (0);
			}
		}
	}

	return (ENOENT);
}

static int
bcm4313_pci_probe(device_t dev)
{
	const struct bcm4313_pci_device	*ident;

	if (bcm4313_pci_find_devcfg(dev, NULL, &ident))
		return (ENXIO);

	device_set_desc(dev, ident->desc);
	return (BUS_PROBE_DEFAULT);
}

static int
bcm4313_pci_attach(device_t dev)
{
	struct bcm4313_pci_softc	*sc;
	const struct bcm4313_pci_device	*ident;
	int				 error;

	sc = device_get_softc(dev);
	sc->dev = dev;

	/* Find our hardware config */
	if (bcm4313_pci_find_devcfg(dev, &sc->devcfg, &ident))
		return (ENXIO);

	/* Attach bridge device (DEVICE_UNIT_ANY == -1; -1 keeps 14.x compat). */
	if ((error = bhndb_attach_bridge(dev, &sc->bhndb_dev, -1)) != 0) {
		device_printf(dev, "failed to attach bhnd bridge: %d\n",
		    error);
		return (error);
	}

	return (0);
}

static void
bcm4313_pci_probe_nomatch(device_t dev, device_t child)
{
	const char *name;

	name = device_get_name(child);
	if (name == NULL)
		name = "unknown device";

	device_printf(dev, "<%s> (no driver attached)\n", name);
}

static const struct bhndb_hwcfg *
bcm4313_pci_get_generic_hwcfg(device_t dev, device_t child __unused)
{
	struct bcm4313_pci_softc *sc = device_get_softc(dev);
	return (sc->devcfg->bridge_hwcfg);
}

static const struct bhndb_hw *
bcm4313_pci_get_bhndb_hwtable(device_t dev, device_t child __unused)
{
	struct bcm4313_pci_softc *sc = device_get_softc(dev);
	return (sc->devcfg->bridge_hwtable);
}

static const struct bhndb_hw_priority *
bcm4313_pci_get_bhndb_hwprio(device_t dev, device_t child __unused)
{
	struct bcm4313_pci_softc *sc = device_get_softc(dev);
	return (sc->devcfg->bridge_hwprio);
}

static bool
bcm4313_pci_is_core_disabled(device_t dev __unused, device_t child __unused,
    struct bhnd_core_info *core __unused)
{
	/* The BCM4313 has exactly one WLAN core and no unpopulated cores. */
	return (false);
}

static device_method_t bcm4313_pci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		bcm4313_pci_probe),
	DEVMETHOD(device_attach,	bcm4313_pci_attach),
	DEVMETHOD(device_detach,	bus_generic_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD(device_suspend,	bus_generic_suspend),
	DEVMETHOD(device_resume,	bus_generic_resume),

	/* Bus interface */
	DEVMETHOD(bus_probe_nomatch,	bcm4313_pci_probe_nomatch),

	/* BHNDB_BUS Interface */
	DEVMETHOD(bhndb_bus_get_generic_hwcfg,	bcm4313_pci_get_generic_hwcfg),
	DEVMETHOD(bhndb_bus_get_hardware_table,	bcm4313_pci_get_bhndb_hwtable),
	DEVMETHOD(bhndb_bus_get_hardware_prio,	bcm4313_pci_get_bhndb_hwprio),
	DEVMETHOD(bhndb_bus_is_core_disabled,	bcm4313_pci_is_core_disabled),

	DEVMETHOD_END
};

DEFINE_CLASS_0(bcm4313_pci, bcm4313_pci_driver, bcm4313_pci_methods,
    sizeof(struct bcm4313_pci_softc));
DRIVER_MODULE_ORDERED(bcm4313_pci, pci, bcm4313_pci_driver, NULL, NULL,
    SI_ORDER_ANY);
MODULE_PNP_INFO("U16:vendor;U16:device;D:#", pci, bcm4313_bcma,
    bcm4313_bcma_devices, nitems(bcm4313_bcma_devices) - 1);
DRIVER_MODULE(bhndb, bcm4313_pci, bhndb_pci_driver, NULL, NULL);

/* Mirror if_bwn_pci.c: the full bridge chain must be present before the
 * PCIe BCM4313 can be enumerated (bhnd(4) alone is only the bus core). */
MODULE_DEPEND(bcm4313_pci, bhnd, 1, 1, 1);
MODULE_DEPEND(bcm4313_pci, bhndb, 1, 1, 1);
MODULE_DEPEND(bcm4313_pci, bhndb_pci, 1, 1, 1);
MODULE_DEPEND(bcm4313_pci, bcma_bhndb, 1, 1, 1);
MODULE_DEPEND(bcm4313_pci, siba_bhndb, 1, 1, 1);
MODULE_VERSION(bcm4313_pci, 1);
