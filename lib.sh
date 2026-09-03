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

# fb_usage: print this script's leading "#" comment block -- its usage/help
# text.  Every tool shows the same text for -h/--help and on bad arguments,
# so the header comment stays the single source of truth for the manual.
fb_usage() {
    tail -n +2 "$0" | sed -n '/^#/p; /^[^#]/q' | sed 's/^# \{0,1\}//'
}

# fb_ask <prompt>: interactive y/N question.  Returns 0 only for an explicit
# "y/yes" on a real terminal; when stdin/stdout are not terminals (piped,
# scripted) or NO_TUI is set, it answers "no" so scripts never hang or ask
# questions no one can answer.
fb_ask() {
    [ -t 0 ] && [ -t 1 ] || return 1
    [ -z "${NO_TUI:-}" ] || return 1
    printf '%s [y/N] ' "$1"
    read _fb_ans
    case "$_fb_ans" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
    esac
}

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

# fb_find_ko: first existing if_bcm4313.ko (kldinstall location, kernel
# location, then the tree next to this script).  Returns the path or nothing.
fb_find_ko() {
    local c
    for c in /boot/modules/if_bcm4313.ko /boot/kernel/if_bcm4313.ko \
        "${HERE:-.}/if_bcm4313.ko"; do
        [ -f "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

# fb_ensure_driver: if if_bcm4313 is not loaded already, try to load it from
# disk (config.sh, diag.sh).  Waits a few seconds for the bcm4313* device to
# attach.  If that fails, dies with the actual evidence -- often the module
# was loaded on a previous boot that did not persist it, or the last attach
# failed ("core reset failed") and the dmesg tail shows why.
fb_ensure_driver() {
    local i ko
    kldstat -q -m if_bcm4313 && return 0
    ko="$(fb_find_ko 2>/dev/null)" || ko=""
    if [ -n "$ko" ]; then
        fb_warn "if_bcm4313 is not loaded -- loading $ko"
        kldload if_bcm4313 2>/dev/null || kldload "$ko" 2>/dev/null || true
        for i in 1 2 3 4; do
            sysctl -n net.wlan.devices 2>/dev/null | grep -q '^bcm4313' && break
            sleep 1
        done
    fi
    kldstat -q -m if_bcm4313 || {
        echo "ERROR: if_bcm4313 is not loaded${ko:+ (tried: $ko)}." >&2
        echo "       kldstat -m if_bcm4313:  $(kldstat -m if_bcm4313 2>&1)" >&2
        echo "       net.wlan.devices:       $(sysctl -n net.wlan.devices 2>/dev/null)" >&2
        echo "       last bcm4313/bhnd logs: " >&2
        dmesg 2>/dev/null | grep -iE 'bcm4313|bhnd' | tail -3 | sed 's/^/         /' >&2
        [ -n "$ko" ] || echo "       No module found on disk -- run 'sh install.sh' first." >&2
        exit 1
    }
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