#!/bin/sh
# build.sh -- build the if_bcm4313 kernel module on FreeBSD.
#
# Usage (run as root on a FreeBSD 14.x/15.x machine):
#   sh build.sh            # build (default)
#   sh build.sh debug      # build with BCM4313_DEBUG + -g (more dmesg output)
#   sh build.sh install    # build, then kldinstall into /boot/kernel
#   sh build.sh clean      # remove generated files
#   sh build.sh check      # dry run: print the exact make command
#
# Environment overrides:
#   SYSDIR=/path/to/sys        kernel source tree (default: /usr/src/sys)
#   KERNBUILDDIR=/path         kernel build dir with opt_*.h (auto-detected)
#   JOBS=n                     make -j value (default: hw.ncpu)
#
# Example with a custom kernel:
#   KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/MYKERNEL sh build.sh

set -u

# --- defaults -------------------------------------------------------------
SYSDIR="${SYSDIR:-/usr/src/sys}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
MODE="${1:-build}"

# --- sanity checks ----------------------------------------------------------
case "$(uname -s 2>/dev/null)" in
FreeBSD) ;;
*)
    echo "ERROR: this driver only builds on FreeBSD." >&2
    echo "       (this host reports: $(uname -s) $(uname -r 2>/dev/null))" >&2
    echo "       Copy the repo to a FreeBSD 14.x/15.x machine and rerun." >&2
    exit 1
    ;;
esac

[ -d "$SYSDIR" ] || {
    echo "ERROR: kernel source not found at $SYSDIR" >&2
    echo "       (set SYSDIR=/path/to/sys or run 'freebsd-update fetch install' /" >&2
    echo "        fetch the matching source per INSTALL.md step 2)" >&2
    exit 1
}
[ -f "$SYSDIR/kern/bus_if.m" ] || {
    echo "ERROR: $SYSDIR does not look like a kernel source tree (missing sys/kern/bus_if.m)" >&2
    exit 1
}

# --- auto-detect KERNBUILDDIR (kernel build dir with opt_*.h) ----------------
if [ -z "${KERNBUILDDIR:-}" ]; then
    ident="$(uname -b 2>/dev/null || echo GENERIC)"
    cand="$(ls -d /usr/obj/usr/src/*/sys/"$ident" 2>/dev/null | head -1)"
    if [ -n "$cand" ]; then
        KERNBUILDDIR="$cand"
        echo "==> auto-detected KERNBUILDDIR=$KERNBUILDDIR"
    fi
fi

MAKEARGS="SYSDIR=$SYSDIR"
[ -n "${KERNBUILDDIR:-}" ] && MAKEARGS="$MAKEARGS KERNBUILDDIR=$KERNBUILDDIR"

# --- modes ------------------------------------------------------------------
case "$MODE" in
check)
    echo "SYSDIR=$SYSDIR"
    [ -n "${KERNBUILDDIR:-}" ] && echo "KERNBUILDDIR=$KERNBUILDDIR"
    echo "make $MAKEARGS -j$JOBS depend all"
    echo "make $MAKEARGS install"
    ;;
clean)
    echo "==> make clean"
    make $MAKEARGS clean || exit 1
    ;;
debug)
    echo "==> make (debug build: BCM4313_DEBUG, -g)"
    make $MAKEARGS -j"$JOBS" CFLAGS+=-DBCM4313_DEBUG DEBUG_FLAGS=-g depend all || exit 1
    ;;
install)
    echo "==> make (default)"
    make $MAKEARGS -j"$JOBS" depend all || exit 1
    echo "==> make install (kldinstall into /boot/kernel)"
    make $MAKEARGS install || exit 1
    ;;
*)
    echo "==> make (default build; use 'debug' or 'install' variants)"
    make $MAKEARGS -j"$JOBS" depend all || exit 1
    ;;
esac

echo
echo "Done. If built, load with:"
echo "    kldload ./if_bcm4313.ko     # pulls bhnd/bhndb/bhndb_pci/bcma_bhndb automatically"
echo "Then verify:  sysctl net.wlan.devices   (expect: bcm43130)"
