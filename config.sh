#!/bin/sh
# config.sh -- connect a FreeBSD machine to wifi through if_bcm4313.
#
# Usage (run as root, after sh install.sh brought the driver up):
#   sh config.sh                       # TUI (dialog) if on a terminal, else prompts
#   sh config.sh MySSID MyPassword     # one-shot WPA2 join + DHCP
#   sh config.sh --boot MySSID MyPassword         # join now + reconnect at boot
#   sh config.sh --wpa /etc/wpa_supplicant.conf   # join via wpa_supplicant
#   sh config.sh --ping MySSID MyPassword         # join, then ping-test the link
#
# Inline `wpakey` mode works for WPA/WPA2-PSK (no wpa_supplicant needed for
# the live session).  Boot persistence always writes a real
# /etc/wpa_supplicant.conf (generated with wpa_passphrase), because inline
# keys do not survive reboot.
#
# Environment overrides:
#   WLDEV=bcm43130    net80211 device name (default: from `sysctl
#                     net.wlan.devices` -- the first bcm4313* entry)
#   WLANIF=wlan0      interface to create over WLDEV
#   TIMEOUT=30        seconds to wait for association (default: 30)
#   NO_TUI=1          force plain prompts even on a terminal

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"

MODE="once"
WPA_CONF=""
PING=0
SSID=""
PASS=""
WLANIF="${WLANIF:-wlan0}"
TIMEOUT="${TIMEOUT:-30}"
TUI=0
TMP=""
trap 'rm -f "$TMP" 2>/dev/null' EXIT INT TERM

# --- argument parsing --------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
    --boot)        MODE="boot" ;;
    --wpa)         shift; [ $# -gt 0 ] && [ -n "$1" ] || {
                       echo "ERROR: --wpa needs a config file path" >&2
                       exit 1
                   }
                   WPA_CONF="$1" ;;
    --ping)        PING=1 ;;
    -h|--help)     sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --*)
        echo "ERROR: unknown option '$1'" >&2
        echo "       usage: sh config.sh [--boot] [--ping] [--wpa FILE] [SSID PASSWORD]"
        exit 1
        ;;
    *)
        if [ -z "$SSID" ]; then
            SSID="$1"
        elif [ -z "$PASS" ]; then
            PASS="$1"
        else
            echo "ERROR: too many arguments (SSID and PASSWORD already given)" >&2
            exit 1
        fi
        ;;
    esac
    shift
done

# --- helpers ------------------------------------------------------------------
# msg/die: TUI-aware; in TUI mode they render boxes, otherwise plain text.
msg() {
    if [ "$TUI" = "1" ]; then
        dialog --title "BCM4313 wifi" --msgbox "$1" 8 60
    else
        echo "$1"
    fi
}
die() {
    if [ "$TUI" = "1" ]; then
        dialog --title "BCM4313 wifi - ERROR" --msgbox "$1" 8 60
    else
        echo "ERROR: $1" >&2
    fi
    exit 1
}

# --- sanity checks -----------------------------------------------------------
fb_sane_os
fb_sane_root
fb_ensure_driver

# Enable the TUI only when nothing was given on the command line and we are on
# a real terminal with dialog available (dialog(1) ships with FreeBSD base).
if [ -z "$SSID" ] && [ -z "$WPA_CONF" ] && [ -z "${NO_TUI:-}" ] &&
    [ -t 0 ] && [ -t 1 ] && command -v dialog >/dev/null 2>&1; then
    TUI=1
    TMP="$(mktemp /tmp/bcm4313cfg.XXXXXXXX)"
fi

# --- find the net80211 device -------------------------------------------------
WLDEV="$(fb_find_wldev)"
[ -n "$WLDEV" ] || die "no bcm4313* device in: $(sysctl -n net.wlan.devices 2>/dev/null) -- attach failed; check: dmesg | tail -20 (or: sh diag.sh)"

# --- (re)create the wlan interface --------------------------------------------
if [ "$TUI" = "1" ]; then
    dialog --title "BCM4313 wifi" --infobox "Preparing $WLANIF over $WLDEV..." 5 50
fi
ifconfig "$WLANIF" destroy 2>/dev/null   # drop a stale copy, if any
ifconfig "$WLANIF" create wlandev "$WLDEV" || die "could not create $WLANIF over $WLDEV"
ifconfig "$WLANIF" up || {
    die "could not bring $WLANIF up -- diagnose with: dmesg | tail -30"
}

# --- TUI main menu -------------------------------------------------------------
tui_menu() {
    sel=$(dialog --stdout --title "BCM4313 wifi" \
        --menu "Driver: $WLDEV over $WLANIF. What do you want to do?" 12 60 0 \
        1 "Scan for networks and connect" \
        2 "Enter SSID and password manually" \
        3 "Use a wpa_supplicant config file" \
        4 "Quit") || exit 1
    case "$sel" in
    1) tui_scan ;;
    2) tui_manual ;;
    3) tui_wpa ;;
    *) exit 0 ;;
    esac
}

# scan -> menu of APs -> credentials -> connect
tui_scan() {
    dialog --title "BCM4313 wifi" --infobox "Scanning (3s)..." 5 40
    ifconfig "$WLANIF" scan >/dev/null 2>&1
    sleep 3
    ifconfig "$WLANIF" list scan 2>/dev/null | awk '
        /^[[:space:]]*$/ { next }
        NR <= 2 { next }                      # header lines
        {
            bssid = ""
            for (i = 1; i <= NF; i++)
                if ($i ~ /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/) { bssid = $i; break }
            if (bssid == "") next
            ssid = ""; for (j = 1; j < i; j++) ssid = ssid $j (j < i - 1 ? " " : "")
            sec = ($0 ~ /WPA/) ? "WPA" : "open"
            printf "%s\t%s\t%s\t%s\t%s\n", bssid, ssid, $(i + 1), $(i + 3), sec
        }' > "$TMP"

    [ -s "$TMP" ] || {
        msg "No networks found by the scan.\n\nTry: 1) move closer to the AP  2) rerun this  3) enter the SSID manually.\ndmesg | tail -30 may also show a driver problem."
        tui_manual
        return
    }

    set --
    while IFS="$(printf '\t')" read -r bssid ssid ch sig sec; do
        [ -n "$bssid" ] || continue
        set -- "$@" "$bssid" "${ssid}  (ch ${ch}, sig ${sig}, ${sec})"
    done < "$TMP"
    sel=$(dialog --stdout --title "BCM4313 wifi" \
        --menu "Networks found -- pick one:" 0 0 0 "$@") || tui_menu
    line=$(grep "^${sel}" "$TMP" | head -1)
    SSID=$(printf '%s' "$line" | cut -f2)
    sec=$(printf '%s' "$line" | cut -f5)
    [ -n "$SSID" ] && [ "$SSID" != "NULL" ] || die "could not read the SSID for $sel"
    if [ "$sec" = "open" ]; then
        PASS=""
    else
        PASS=$(dialog --stdout --title "BCM4313 wifi" \
            --passwordbox "WPA password for '$SSID':" 8 60) || tui_menu
    fi
    tui_askboot
}

tui_manual() {
    SSID=$(dialog --stdout --title "BCM4313 wifi" \
        --inputbox "SSID:" 8 60) || tui_menu
    [ -n "$SSID" ] || die "empty SSID."
    PASS=$(dialog --stdout --title "BCM4313 wifi" \
        --passwordbox "WPA password for '$SSID' (empty = open network):" 8 60) || tui_menu
    tui_askboot
}

tui_wpa() {
    WPA_CONF=$(dialog --stdout --title "BCM4313 wifi" \
        --fselect /etc/wpa_supplicant.conf 12 70) || tui_menu
    WPA_CONF=$(printf '%s' "$WPA_CONF" | sed 's/[[:space:]]*$//')  # fselect pads
    [ -f "$WPA_CONF" ] || die "wpa_supplicant config not found: $WPA_CONF"
    tui_askboot
}

tui_askboot() {
    if [ "$MODE" = "boot" ]; then
        return
    fi
    if dialog --stdout --title "BCM4313 wifi" \
        --yesno "Connect now, and also reconnect at every boot?" 8 60; then
        MODE="boot"
    fi
}

# --- join the network -----------------------------------------------------------
if [ "$TUI" = "1" ]; then
    tui_menu
fi

# plain-prompt fallback (no tty / NO_TUI=1)
if [ -z "$SSID" ] && [ -z "$WPA_CONF" ]; then
    printf "SSID: "
    read SSID
fi
[ -n "$SSID" ] || [ -n "$WPA_CONF" ] || die "no SSID given."
if [ -z "$WPA_CONF" ] && [ -z "$PASS" ]; then
    printf "WPA password (hidden; empty = open network): "
    stty -echo 2>/dev/null
    read PASS
    stty echo 2>/dev/null
    echo
fi

if [ -n "$WPA_CONF" ]; then
    [ -f "$WPA_CONF" ] || die "wpa_supplicant config not found: $WPA_CONF"
    [ "$TUI" = "1" ] || echo "==> joining via wpa_supplicant (-c $WPA_CONF)"
    ifconfig "$WLANIF" up
    wpa_supplicant -B -i "$WLANIF" -c "$WPA_CONF" || {
        die "wpa_supplicant failed to start.\nCheck:  wpa_supplicant -i $WLANIF -c $WPA_CONF -dd"
    }
    SSID="$(wpa_cli -i "$WLANIF" status 2>/dev/null | awk -F= '/^ssid=/{print $2}')"
    [ -n "$SSID" ] || SSID="(wpa_supplicant)"
elif [ -n "$PASS" ]; then
    [ "$TUI" = "1" ] || echo "==> joining SSID '$SSID' with wpakey (WPA2-PSK)"
    ifconfig "$WLANIF" ssid "$SSID" wpakey "$PASS" up || {
        die "could not set ssid/wpakey -- diagnose with: dmesg | tail -30"
    }
else
    [ "$TUI" = "1" ] || echo "==> joining OPEN network '$SSID'"
    ifconfig "$WLANIF" ssid "$SSID" up || {
        die "could not set ssid -- diagnose with: dmesg | tail -30"
    }
fi

# --- wait for association --------------------------------------------------------
if [ "$TUI" = "1" ]; then
    ( i=0
      while [ "$i" -lt "$TIMEOUT" ]; do
          if ifconfig "$WLANIF" 2>/dev/null | grep -q 'status: associated'; then
              echo "100"
              break
          fi
          echo "$(( i * 100 / TIMEOUT ))"
          sleep 1
          i=$((i + 1))
      done ) | dialog --title "BCM4313 wifi" \
        --gauge "Waiting for association with '$SSID'..." 6 60 0
else
    echo "==> waiting for association (up to ${TIMEOUT}s)..."
    i=0
    while [ "$i" -lt "$TIMEOUT" ]; do
        if ifconfig "$WLANIF" | grep -q 'status: associated'; then
            echo "==> associated."
            break
        fi
        i=$((i + 1))
        sleep 1
    done
fi
if ! ifconfig "$WLANIF" | grep -q 'status: associated'; then
    die "no association after ${TIMEOUT}s. Status: $(ifconfig "$WLANIF" | grep 'status:')\n\nAlso: dmesg | tail -30"
fi

# --- DHCP -------------------------------------------------------------------------
[ "$TUI" = "1" ] || echo "==> requesting an address (dhclient $WLANIF)"
dhclient "$WLANIF" 2>/dev/null
sleep 3

ip="$(ifconfig "$WLANIF" | awk '/inet /{print $2; exit}')"
gw="$(netstat -rn 2>/dev/null | awk '$1=="default"{print $2; exit}')"

# --- optional ping test ------------------------------------------------------------
[ "$PING" = "1" ] && ping -c 3 -t 5 8.8.8.8 >/dev/null 2>&1

# --- optional boot persistence -------------------------------------------------------
if [ "$MODE" = "boot" ]; then
    rcconf="/etc/rc.conf"
    wpaconf="/etc/wpa_supplicant.conf"

    # Boot-time joins always go through wpa_supplicant (the rc 'WPA' keyword
    # does not accept inline keys), so generate the config when the user
    # supplied credentials inline just now.
    needs_wpa=0
    if [ -z "$WPA_CONF" ]; then
        [ -f "$wpaconf" ] && grep -q "ssid=\"$SSID\"" "$wpaconf" 2>/dev/null || needs_wpa=1
    fi
    if [ "$needs_wpa" = "1" ]; then
        if command -v wpa_passphrase >/dev/null 2>&1 && [ -n "$PASS" ]; then
            umask 077
            cat > "$wpaconf" <<EOF
# Generated by config.sh for if_bcm4313 -- SSID: $SSID
ctrl_interface=/var/run/wpa_supplicant
network={
EOF
            wpa_passphrase "$SSID" "$PASS" >> "$wpaconf" 2>/dev/null
            echo "}" >> "$wpaconf"
        elif [ -n "$PASS" ]; then
            msg "WARNING: wpa_passphrase not found -- write $wpaconf by hand (see the wpa_supplicant block in INSTALL.md). Boot reconnect will not work."
        fi
    fi

    append_rc() {  # append_rc KEY VALUE  ->  key="value" if absent
        if grep -q "^$1=" "$rcconf" 2>/dev/null; then
            return
        fi
        echo "$1=\"$2\"" >> "$rcconf"
    }
    append_rc "wlans_if_${WLDEV}" "$WLANIF"
    append_rc "ifconfig_${WLANIF}" "WPA DHCP"
    append_rc "wpa_supplicant_enable" "YES"
fi

# --- summary -----------------------------------------------------------------------
if [ "$TUI" = "1" ]; then
    dialog --title "BCM4313 wifi" --msgbox \
"Connected to '$SSID' on $WLANIF.
IP: ${ip:-no address yet (dhclient may still be running)}
Default route: ${gw:-none}
MAC: $(ifconfig "$WLANIF" | awk '/ether/{print $2}')" 10 60
else
    echo
    [ -n "$ip" ] && echo "==> $WLANIF has address: $ip" ||
        echo "WARNING: no inet address yet -- DHCP may still be running. Check: ifconfig $WLANIF inet"
    [ -n "$gw" ] && echo "==> default route: $gw"
    echo
    echo "Done. Quick checks:  ifconfig $WLANIF   |   ping -c3 8.8.8.8"
fi