#!/bin/sh
# install.sh -- turnkey driver install + wifi bring-up on FreeBSD.
#
# Usage (run as root on a FreeBSD 14.x/15.x machine, from this directory):
#   sh install.sh          # build, install, (re)load, create wlan0, up, scan
#   sh install.sh boot     # same, plus load at boot (loader.conf + rc.conf)
#   sh install.sh --skip-build   # install the existing if_bcm4313.ko only
#
# Environment overrides (passed through to build.sh):
#   SYSDIR=/path/to/sys        kernel source tree (default: /usr/src/sys)
#   KERNBUILDDIR=/path         kernel build dir with opt_*.h (auto-detected)
#   JOBS=n                     make -j value
#   WLDEV=bcm43130             net80211 device name (default: from
#                              `sysctl net.wlan.devices`)
#   WLANIF=wlan0               interface to create over WLDEV
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
WLDEV="${WLDEV:-}"
WLANIF="${WLANIF:-wlan0}"

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
        echo "ERROR: could not unload if_bcm4313. Are its wlan interfaces up?" >&2
        echo "       Detach them first, e.g.:  ifconfig $WLANIF down && ifconfig $WLANIF destroy" >&2
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
sleep 1

# --- find the net80211 device -------------------------------------------------
if [ -z "$WLDEV" ]; then
    WLDEV="$(sysctl -n net.wlan.devices 2>/dev/null | tr ' ' '\n' | grep '^bcm4313' | head -1)"
fi
[ -n "$WLDEV" ] || {
    echo "ERROR: no bcm4313* device in: $(sysctl -n net.wlan.devices 2>/dev/null)" >&2
    echo "       Attach failed; check: dmesg | tail -20   (or: sh diag.sh)" >&2
    exit 1
}
echo "==> using wireless device: $WLDEV"

# --- wifi steps ---------------------------------------------------------------
echo "==> creating $WLANIF over $WLDEV"
ifconfig "$WLANIF" destroy 2>/dev/null   # drop a stale copy, if any
ifconfig "$WLANIF" create wlandev "$WLDEV" || {
    echo "ERROR: could not create $WLANIF over $WLDEV" >&2
    exit 1
}

echo "==> bringing $WLANIF up"
ifconfig "$WLANIF" up || {
    echo "ERROR: could not bring $WLANIF up" >&2
    echo "       Diagnose with: dmesg | tail -30" >&2
    exit 1
}

echo "==> MAC address: $(ifconfig "$WLANIF" | awk '/ether/{print $2}')"

echo "==> scanning (3 seconds)..."
ifconfig "$WLANIF" scan >/dev/null 2>&1
sleep 3
echo
echo "==> networks found:"
ifconfig "$WLANIF" list scan 2>/dev/null | head -20
echo

# --- summary -----------------------------------------------------------------
echo "Driver is live. To join a network now:"
echo "    ifconfig $WLANIF ssid YOUR_SSID wpakey YOUR_PASSWORD"
echo "    dhclient $WLANIF          # or: dhcpcd $WLANIF"
echo "Or with wpa_supplicant instead of an inline key:"
echo "    wpa_supplicant -B -i $WLANIF -c /etc/wpa_supplicant.conf"
echo "    dhclient $WLANIF"

# --- optional boot persistence ------------------------------------------------
if [ "$MODE" = "boot" ]; then
    conf="/boot/loader.conf"
    if grep -q '^if_bcm4313_load="YES"' "$conf" 2>/dev/null; then
        echo "==> $conf already enables if_bcm4313 at boot"
    else
        echo "==> appending 'if_bcm4313_load=\"YES\"' to $conf"
        echo 'if_bcm4313_load="YES"' >> "$conf"
    fi

    rcconf="/etc/rc.conf"
    if grep -q "wlans_if_${WLDEV}=" "$rcconf" 2>/dev/null; then
        echo "==> $rcconf already creates a wlan over $WLDEV"
    else
        echo "==> appending 'wlans_if_${WLDEV}=\"$WLANIF\"' to $rcconf"
        echo "wlans_if_${WLDEV}=\"$WLANIF\"" >> "$rcconf"
    fi
    echo
    echo "To also connect + get an address at boot, add to $rcconf:"
    echo "    ifconfig_${WLANIF}=\"WPA DHCP\""
    echo "    wpa_supplicant_enable=\"YES\""
    echo "and put your network in /etc/wpa_supplicant.conf (see INSTALL.md)."
fi