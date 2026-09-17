#!/bin/sh
# build.sh -- build the if_bcm4313 kernel module on FreeBSD.
#
# Usage (run on a FreeBSD 14.x/15.x machine):
#   sh build.sh            # build the module (default)
#   sh build.sh debug      # build with BCM4313_DEBUG + -g (more dmesg output)
#   sh build.sh install    # build, then `make install` (copies the .ko into /boot)
#   sh build.sh clean      # remove generated object files
#   sh build.sh check      # dry run: print the exact make commands
#   sh build.sh --help     # this text
#
# Environment overrides:
#   SYSDIR=/path/to/sys        kernel source tree (default: /usr/src/sys)
#   KERNBUILDDIR=/path         kernel build dir with opt_*.h (auto-detected)
#   JOBS=n                     make -j value (default: hw.ncpu)
#
# Example with a custom kernel:
#   KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/MYKERNEL sh build.sh
#
# After a successful build, the fastest path from .ko to online Wi-Fi is:
#     sh install.sh                  # (re)load the driver, create wlan0, scan
#     sh config.sh MySSID MyPassword # join your network and get an IP
#   (every tool also accepts --help for its full manual)


set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"

# --- defaults -------------------------------------------------------------
SYSDIR="${SYSDIR:-/usr/src/sys}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
MODE="${1:-build}"

# --- mode -------------------------------------------------------------------
case "$MODE" in
-h|--help)
    fb_usage
    exit 0
    ;;
build|debug|install|clean|check) ;;
*)
    echo "ERROR: unknown mode '$MODE' (expected: build, debug, install, clean, check)" >&2
    echo "       full usage: sh build.sh --help" >&2
    exit 1
    ;;
esac

# --- sanity checks ----------------------------------------------------------
fb_sane_os

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
    fb_sane_root   # kldinstall writes /boot/modules
    echo "==> make (default)"
    make $MAKEARGS -j"$JOBS" depend all || exit 1
    echo "==> make install (kldinstall into /boot/kernel)"
    make $MAKEARGS install || exit 1
    ;;
build)
    echo "==> make (default build; use 'debug' or 'install' variants)"
    make $MAKEARGS -j"$JOBS" depend all || exit 1
    ;;

esac

case "$MODE" in
build|debug|install)
    echo
    echo "Done -- if_bcm4313.ko is ready."
    echo
    echo "Next steps (fastest path from .ko to online Wi-Fi):"
    echo "    sh install.sh                   # (re)load the driver, create wlan0, scan"
    echo "    sh config.sh MySSID MyPassword  # join your network and get an IP"
    echo "                                    # (plain 'sh config.sh' opens a scan-and-pick"
    echo "                                    #  menu; add --boot to reconnect at boot)"
    echo
    echo "Or load it by hand and verify:"
    echo "    kldload ./if_bcm4313.ko     # pulls in bhnd/bhndb/bhndb_pci/bcma_bhndb automatically"
    echo "    sysctl net.wlan.devices     # expect: bcm43130"
    ;;
esac