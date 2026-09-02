#!/bin/sh
# lib.sh -- shared helpers for the if_bcm4313 tooling.
#
# Sourced by build.sh / install.sh / config.sh / diag.sh:
#     HERE="$(cd "$(dirname "$0")" && pwd)"; . "$HERE/lib.sh"
#
# POSIX sh only; all functions run on FreeBSD.  Uses `local`, which FreeBSD
# sh(1) supports (the scripts never run on any other shell).

# --- output helpers -----------------------------------------------------------
fb_note() { echo "==> $*"; }
fb_ok()   { echo "   [ok] $*"; }
fb_fail() { echo "   [FAIL] $*" >&2; }
fb_warn() { echo "   [WARN] $*" >&2; }
fb_die()  { echo "ERROR: $*" >&2; exit 1; }

# --- sanity checks -------------------------------------------------------------
# fb_sane_os: must run on FreeBSD (cheap to call from every script).
fb_sane_os() {
    case "$(uname -s 2>/dev/null)" in
    FreeBSD) ;;
    *)
        fb_die "if_bcm4313 tooling only runs on FreeBSD (this host: $(uname -s) $(uname -r 2>/dev/null))"
        ;;
    esac
}

# fb_sane_root: must run as root (kldload/ifconfig/dhclient all need it).
fb_sane_root() {
    [ "$(id -u)" = "0" ] || {
        echo "ERROR: $0 must run as root." >&2
        echo "       Try: sudo sh $0" >&2
        exit 1
    }
}

# fb_require_driver: the module must be loaded (config.sh, diag.sh).
fb_require_driver() {
    kldstat -q -m if_bcm4313 ||
        fb_die "if_bcm4313 is not loaded. Run 'sh install.sh' first."
}

# fb_require_tree: the caller must run from the driver directory.
fb_require_tree() {
    HERE="$(cd "$(dirname "$0")" && pwd)"
    [ -f "$HERE/if_bcm4313.c" ] ||
        fb_die "run $0 from the driver directory (could not find if_bcm4313.c next to $0)"
}

# --- device helpers -------------------------------------------------------------
# fb_find_wldev: first bcm4313* in net.wlan.devices; honors the WLDEV env var.
fb_find_wldev() {
    if [ -n "${WLDEV:-}" ]; then
        printf '%s\n' "$WLDEV"
        return 0
    fi
    sysctl -n net.wlan.devices 2>/dev/null | tr ' ' '\n' | grep '^bcm4313' | head -1
}

# fb_if_parent <ifname>: underlying device of a wlan vap.  Tolerates both the
# bare-name and "wlandev bcm43130" getter output across FreeBSD versions.
fb_if_parent() {
    local p
    p="$(ifconfig "$1" wlandev 2>/dev/null)"
    [ -n "$p" ] || p="$(ifconfig "$1" 2>/dev/null | awk '/parent interface|wlandev/{print $NF; exit}')"
    printf '%s\n' "${p##* }"
}

# fb_destroy_wlans_over <glob>: down+destroy every wlan vap whose parent
# matches the glob (e.g. bcm4313*) -- net80211 pins the module otherwise.
fb_destroy_wlans_over() {
    local i p
    for i in $(ifconfig -g wlan 2>/dev/null); do
        p="$(fb_if_parent "$i")"
        [ -n "$p" ] || continue
        case "$p" in
        $1)
            ifconfig "$i" down 2>/dev/null
            ifconfig "$i" destroy 2>/dev/null
            fb_note "destroyed stale $i over $p"
            ;;
        esac
    done
}

# fb_if_is_up <ifname>: 0 if the interface's flags contain UP.
fb_if_is_up() {
    ifconfig "$1" 2>/dev/null | grep -q '<UP'
}

# fb_scan_count <ifname>: number of AP lines in `list scan` output.
fb_scan_count() {
    ifconfig "$1" list scan 2>/dev/null | awk '$2 ~ /:/{c++} END{print c+0}'
}

# fb_pci_selector: PCI selector of our card while the front-end is attached.
fb_pci_selector() {
    pciconf -l 2>/dev/null | awk '/bcm4313_pci/{sub(/@.*/, "", $1); print $1; exit}'
}

# fb_pci_reset <selector>: best-effort PCI-level reset (clears wedged DMP/DMA
# state that survives kldunload).  Needs FreeBSD 13+; silent if unavailable.
fb_pci_reset() {
    [ -n "$1" ] || return 0
    command -v devctl >/dev/null 2>&1 || {
        fb_warn "devctl not available -- a reboot clears the same wedge"
        return 0
    }
    fb_note "PCI reset of $1 (clears stale DMP/DMA state)"
    devctl reset "$1" 2>/dev/null ||
        fb_warn "devctl reset failed -- a reboot clears the same state"
    sleep 1
}