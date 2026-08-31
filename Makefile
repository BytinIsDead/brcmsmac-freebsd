# $FreeBSD$
#
# Out-of-tree build for the Broadcom BCM4313 SoftMAC 802.11b/g/n driver
# (if_bcm4313(4), bhnd(4) child of the D11 core).
#
# Usage (against a kernel source tree):
#	make SYSDIR=/usr/src/sys
#
# If your kernel was built with a custom KERNCONF that generates opt_*.h
# files referenced by net80211, build against the kernel build directory
# as well:
#	make SYSDIR=/usr/src/sys \
#	    KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/GENERIC
#
# The empty opt_bcm4313.h and opt_wlan.h files shipped in this directory
# are used when no KERNBUILDDIR is given; the driver only uses them to
# keep the standard net80211 include list happy.

SYSDIR?=	/usr/src/sys

KMOD	= if_bcm4313
SRCS	= if_bcm4313.c
SRCS	+= if_bcm4313_phy_lcn.c
SRCS	+= bcm4313_ucode.c
SRCS	+= bhnd_bus_if.h bhnd_chipc_if.h bhnd_pmu_if.h bhnd_pwrctl_if.h
SRCS	+= bhndb_bus_if.h bhndb_if.h
SRCS	+= bhnd_nvram_map.h
SRCS	+= device_if.h bus_if.h

# The *_if.h headers above are generated from .m files with makeobjops.
# In-tree module builds get the .m search path from __MPATH (kernel build
# environment); add the paths here so out-of-tree builds work as well.
# The bhnd/bhndb .m files live in subdirectories (roots identified in both
# FreeBSD 14 and 15); bus_if.m/device_if.m are under sys/kern.
.PATH.m:	${SYSDIR}/dev/bhnd \
		${SYSDIR}/dev/bhnd/bhndb \
		${SYSDIR}/dev/bhnd/cores/chipc \
		${SYSDIR}/dev/bhnd/cores/chipc/pwrctl \
		${SYSDIR}/dev/bhnd/cores/pmu \
		${SYSDIR}/kern

# bhnd(4) headers include each other with quoted, directory-relative
# includes; make sure the bhnd include dir is on the path.
CFLAGS+=	-I${SYSDIR}/dev/bhnd

# The LCN-PHY tuning tables (bcm4313_lcntab.h, bcm4313_phytbl_lcn.h) and
# the embedded D11/LCN microcode (bcm4313_ucode.c/.h) are generated from
# the linux-firmware and brcmsmac files bundled under firmware/ and
# brcmsmac/ in this directory:
#	perl gen_lcntab.pl    # -> bcm4313_lcntab.h (from brcmsmac/phy/phytbl_lcn.c)
#	perl gen_phytbl.pl    # -> bcm4313_phytbl_lcn.h (from brcmsmac/phy/phytbl_lcn.c)
#	perl gen_ucode.pl     # -> bcm4313_ucode.c/.h (from firmware/brcm/*.fw)
# They are checked in, so a fresh clone does not need the generators.

# Keep the generated interface headers out of the source tree on clean.
CLEANFILES+=	bhnd_bus_if.h bhnd_chipc_if.h bhnd_pmu_if.h \
		bhnd_pwrctl_if.h bhndb_bus_if.h bhndb_if.h bhnd_nvram_map.h \
		device_if.h bus_if.h

.include <bsd.kmod.mk>
