#!/bin/sh
# install.sh -- turnkey install + load of the if_bcm4313 driver on FreeBSD.
#
# Usage (run as root on a FreeBSD 14.x/15.x machine, from this directory):
#   sh install.sh          # build, install to /boot/modules, (re)load, verify
#   sh install.sh boot     # same, plus enable loading at boot via loader.conf
#   sh install.sh --skip-build   # install the existing if_bcm4313.ko only
#
# Environment overrides (passed through to build.sh):
#   SYSDIR=/path/to/sys        kernel source tree (default: /usr/src/sys)
#   KERNBUILDDIR=/path         kernel build dir with opt_*.h (auto-detected)
#   JOBS=n                     make -j value
#
# The driver trades itself in safely: any already-loaded if_bcm4313 is
# unloaded first, so this can be re-run after `git pull` to upgrade in
# place.  bhnd/bhndb/bhndb_pci/bcma_bhndb are pulled in automatically as
# module dependencies when the driver loads.

set -u

SKIP_BUILD=0
MODE="install"
for a in "$@"; do
    case "$a" in
    boot)           MODE="boot" ;;
    --skip-build)   SKIP_BUILD=1 ;;
    *)
        echo "ERROR: unknown argument '$a'" >&2
        echo "       usage: sh install.sh [boot] [--skip-build]" >&2
        exit 1
        ;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"

# --- sanity checks ----------------------------------------------------------
case "$(uname -s 2>/dev/null)" in
FreeBSD) ;;
*)
    echo "ERROR: this driver only runs on FreeBSD." >&2
    echo "       (this host reports: $(uname -s) $(uname -r 2>/dev/null))" >&2
    exit 1
    ;;
esac

[ "$(id -u)" = "0" ] || {
    echo "ERROR: install must run as root (kldload/kldunload need it)." >&2
    echo "       Try: sudo sh $0" >&2
    exit 1
}

[ -f "$HERE/if_bcm4313.c" ] || {
    echo "ERROR: run install.sh from the driver directory (could not find" >&2
    echo "       if_bcm4313.c next to $0)." >&2
    exit 1
}

# --- build (unless told to keep the existing .ko) ----------------------------
if [ "$SKIP_BUILD" = "1" ]; then
    [ -f "$HERE/if_bcm4313.ko" ] || {
        echo "ERROR: --skip-build given but $HERE/if_bcm4313.ko is missing." >&2
        exit 1
    }
    echo "==> skipping build (--skip-build), using existing if_bcm4313.ko"
else
    echo "==> building (sh build.sh install)"
    ( cd "$HERE" && sh build.sh install ) || {
        echo "ERROR: build/install failed; nothing was loaded." >&2
        exit 1
    }
fi

# --- unload any old copy so the new module actually takes effect -------------
if kldstat -q -m if_bcm4313; then
    echo "==> unloading old if_bcm4313"
    kldunload if_bcm4313 || {
        echo "ERROR: could not unload if_bcm4313 (in use? try: ifconfig wlan0 destroy)" >&2
        exit 1
    }
fi

# --- load --------------------------------------------------------------------
echo "==> kldload if_bcm4313"
kldload if_bcm4313 || {
    echo "ERROR: kldload failed." >&2
    echo "       Diagnose with: dmesg | tail -30" >&2
    exit 1
}

# --- verify ------------------------------------------------------------------
echo
echo "==> devices:"
sysctl net.wlan.devices

echo
echo "==> attach log:"
dmesg | grep -i 'bcm4313\|bhnd' | tail -8

echo
if sysctl net.wlan.devices | grep -q bcm43130; then
    echo "Driver is live. Create an interface with:"
    echo "    ifconfig wlan0 create wlandev bcm43130"
    echo "    ifconfig wlan0 up"
    echo "    ifconfig wlan0 scan"
else
    echo "NOTE: driver loaded but no bcm43130 device yet; check the attach"
    echo "      log above (or run: sh diag.sh)."
fi

# --- optional boot persistence ------------------------------------------------
if [ "$MODE" = "boot" ]; then
    conf="/boot/loader.conf"
    if grep -q '^if_bcm4313_load="YES"' "$conf" 2>/dev/null; then
        echo "==> $conf already enables if_bcm4313 at boot"
    else
        echo "==> appending 'if_bcm4313_load=\"YES\"' to $conf"
        echo 'if_bcm4313_load="YES"' >> "$conf"
    fi
    echo "Note: add a wlan0 + wlans_if_bcm43130 entry in /etc/rc.conf to"
    echo "      bring the interface up at boot; see INSTALL.md."
fi