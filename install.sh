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
# unloaded first (stale wlan interfaces over it are destroyed first, because
# net80211 pins the module), so this can be re-run after `git pull` to
# upgrade in place.  If the old session wedged the chip ("core reset
# failed: 60"), the wedge lives in the hardware and survives kldunload, so
# the card gets a PCI-level `devctl reset` before the new module loads.
# bhnd/bhndb/bhndb_pci/bcma_bhndb are pulled in automatically as module
# dependencies when the driver loads.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"

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

WLDEV="${WLDEV:-}"
WLANIF="${WLANIF:-wlan0}"

# --- sanity checks ----------------------------------------------------------
fb_sane_os
fb_sane_root
fb_require_tree   # sets HERE

# --- build (unless told to keep the existing .ko) ----------------------------
if [ "$SKIP_BUILD" = "1" ]; then
    [ -f "$HERE/if_bcm4313.ko" ] || fb_die "--skip-build given but $HERE/if_bcm4313.ko is missing."
    fb_note "skipping build (--skip-build), using existing if_bcm4313.ko"
else
    fb_note "building (sh build.sh install)"
    ( cd "$HERE" && sh build.sh install ) || fb_die "build/install failed; nothing was loaded."
fi

# --- redundant: verify the build actually produced a fresh module -------------
# `make` can exit 0 without a usable .ko; also catch cases where kldinstall
# silently wrote nothing or the tree was never rebuilt.
if [ ! -s "$HERE/if_bcm4313.ko" ]; then
    echo "ERROR: build finished but $HERE/if_bcm4313.ko is missing or empty." >&2
    echo "       Rerun: sh build.sh   (check the make output above)" >&2
    exit 1
fi
newest="$(ls -t "$HERE"/*.c "$HERE"/*.h 2>/dev/null | head -1)"
if [ -n "$newest" ] && [ "$newest" -nt "$HERE/if_bcm4313.ko" ] 2>/dev/null; then
    echo "WARNING: if_bcm4313.ko is older than $newest -- source changed after" >&2
    echo "         the last build. Rebuilding now to be safe." >&2
    ( cd "$HERE" && sh build.sh install ) || fb_die "rebuild after freshness check failed."
fi

# --- tear down any stale state from a previous load --------------------------
# A loaded module cannot be unloaded while a wlan(4) interface still rides on
# it (net80211 holds a reference).  So first destroy every wlan interface over
# bcm4313*, then unload the old module, then load the new one.
PCISEL=""
if kldstat -q -m if_bcm4313; then
    # Remember the PCI selector while our front-end is still attached, so the
    # card can be reset at the bus level after the unload (see below).
    PCISEL="$(fb_pci_selector)"
    fb_destroy_wlans_over 'bcm4313*'
    fb_note "unloading old if_bcm4313"
    kldunload if_bcm4313 || {
        echo "ERROR: could not unload if_bcm4313." >&2
        echo "       Destroy remaining wlan interfaces over it:" >&2
        echo "         for i in \$(ifconfig -g wlan); do ifconfig \$i destroy; done" >&2
        exit 1
    }
    sleep 1
    # --- redundant: kldunload can exit 0 while the module lingers (refs) -----
    if kldstat -q -m if_bcm4313; then
        fb_warn "if_bcm4313 still listed after unload; retrying once..."
        sleep 2
        kldunload if_bcm4313 2>/dev/null || {
            echo "ERROR: module is still pinned. What holds it?" >&2
            echo "       kldstat -m if_bcm4313 ; ifconfig -g wlan" >&2
            exit 1
        }
    fi
fi

# --- clear a wedged chip before the new module loads --------------------------
# The D11's DMP/DMA state survives kldunload: if a previous session crashed
# the core ("BCMA_DMP_RESETSTATUS timeout"), the next driver just retries
# resets against the same wedge.  A PCI-level reset clears the hardware.
fb_pci_reset "$PCISEL"

# --- load --------------------------------------------------------------------
# Prefer the kldinstall location, fall back to the tree's own .ko -- never
# load a module that does not exist.
KO=""
for c in /boot/modules/if_bcm4313.ko /boot/kernel/if_bcm4313.ko "$HERE/if_bcm4313.ko"; do
    [ -f "$c" ] && KO="$c" && break
done
[ -n "$KO" ] || fb_die "no if_bcm4313.ko found (looked in /boot/modules, /boot/kernel, $HERE)."
fb_note "kldload $KO"
kldload if_bcm4313 || fb_die "kldload failed. Diagnose with: dmesg | tail -30"
sleep 1

# --- redundant: confirm the module is really loaded ---------------------------
kldstat -q -m if_bcm4313 || fb_die "kldload reported success but the module is not in kldstat."

# --- find the net80211 device, with one attach retry -------------------------
attempt=0
while [ "$attempt" -lt 2 ]; do
    WLDEV="$(fb_find_wldev)"
    [ -n "$WLDEV" ] && break
    echo "==> bcm43130 not attached yet; reloading and waiting (attempt $((attempt + 1))/2)..."
    kldunload if_bcm4313 2>/dev/null
    sleep 1
    kldload if_bcm4313 2>/dev/null
    sleep 3
    WLDEV=""
    attempt=$((attempt + 1))
done
[ -n "$WLDEV" ] || {
    echo "ERROR: no bcm4313* device in: $(sysctl -n net.wlan.devices 2>/dev/null)" >&2
    echo "       Attach failed after 2 attempts; check: dmesg | tail -20   (or: sh diag.sh)" >&2
    exit 1
}
echo "==> using wireless device: $WLDEV"

# --- wifi steps ---------------------------------------------------------------
echo "==> creating $WLANIF over $WLDEV"
ifconfig "$WLANIF" destroy 2>/dev/null   # drop a stale copy, if any
ifconfig "$WLANIF" create wlandev "$WLDEV" || fb_die "could not create $WLANIF over $WLDEV"
# --- redundant: make sure the vap is really bound to OUR card ---------------
parent="$(fb_if_parent "$WLANIF")"
case "$parent" in
bcm4313*)
    ;;
*)
    echo "ERROR: $WLANIF was created but is bound to '${parent:-nothing}' instead of" >&2
    echo "       $WLDEV. Destroying it and aborting." >&2
    ifconfig "$WLANIF" destroy 2>/dev/null
    exit 1
    ;;
esac

echo "==> bringing $WLANIF up"
ifconfig "$WLANIF" up || fb_die "could not bring $WLANIF up. Diagnose with: dmesg | tail -30"
# --- redundant: flags must actually show UP after the command returned ------
fb_if_is_up "$WLANIF" || fb_die "$WLANIF up returned 0 but the interface is not UP. Diagnose with: dmesg | tail -30"

echo "==> MAC address: $(ifconfig "$WLANIF" | awk '/ether/{print $2}')"

echo "==> scanning (3 seconds)..."
ifconfig "$WLANIF" scan >/dev/null 2>&1
sleep 3
echo
echo "==> networks found:"
ifconfig "$WLANIF" list scan 2>/dev/null | head -20
echo
echo "    ($(fb_scan_count "$WLANIF") network(s) in range -- an empty result is okay if no AP is near)"
echo

# --- summary -----------------------------------------------------------------
echo "==> final self-check:"
kldstat -q -m if_bcm4313 && fb_ok "module if_bcm4313 loaded" || fb_fail "module not loaded"
if sysctl -n net.wlan.devices 2>/dev/null | grep -q '^bcm4313'; then
    fb_ok "device $WLDEV attached"
else
    fb_fail "no bcm4313* in: $(sysctl -n net.wlan.devices 2>/dev/null)"
fi
if fb_if_is_up "$WLANIF"; then
    fb_ok "$WLANIF is up"
else
    fb_fail "$WLANIF not UP"
fi
echo
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