#!/bin/sh
# config.sh -- connect a FreeBSD machine to wifi through if_bcm4313.
#
# Usage (run as root, after sh install.sh brought the driver up):
#   sh config.sh                       # interactive: prompts for SSID + password
#   sh config.sh MySSID MyPassword     # one-shot WPA2 join + DHCP
#   sh config.sh --wpa /etc/wpa_supplicant.conf   # join via wpa_supplicant
#   sh config.sh --boot MySSID MyPassword         # join now + reconnect at boot
#   sh config.sh --ping MySSID MyPassword         # join, then ping-test the link
#
# Inline `wpakey` mode works for WPA/WPA2-PSK (no wpa_supplicant needed for
# the live session).  --boot always writes a real /etc/wpa_supplicant.conf
# (generated with wpa_passphrase), because inline keys do not survive reboot.
#
# Environment overrides:
#   WLDEV=bcm43130    net80211 device name (default: from `sysctl
#                     net.wlan.devices` -- the first bcm4313* entry)
#   WLANIF=wlan0      interface to create over WLDEV
#   TIMEOUT=30        seconds to wait for association (default: 30)

set -u

MODE="once"
WPA_CONF=""
PING=0
SSID=""
PASS=""
WLANIF="${WLANIF:-wlan0}"
TIMEOUT="${TIMEOUT:-30}"

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
    -h|--help)     sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# --- sanity checks -----------------------------------------------------------
case "$(uname -s 2>/dev/null)" in
FreeBSD) ;;
*)
    echo "ERROR: this driver only runs on FreeBSD." >&2
    echo "       (this host reports: $(uname -s) $(uname -r 2>/dev/null))" >&2
    exit 1
    ;;
esac

[ "$(id -u)" = "0" ] || {
    echo "ERROR: config must run as root (ifconfig/dhclient need it)." >&2
    echo "       Try: sudo sh $0" >&2
    exit 1
}

if ! kldstat -q -m if_bcm4313; then
    echo "ERROR: if_bcm4313 is not loaded. Run 'sh install.sh' first." >&2
    exit 1
fi

# --- interactive credential prompts -------------------------------------------
if [ -z "$SSID" ]; then
    printf "SSID: "
    read SSID
fi
[ -n "$SSID" ] || {
    echo "ERROR: no SSID given." >&2
    exit 1
}

if [ -z "$WPA_CONF" ] && [ -z "$PASS" ]; then
    printf "WPA password (hidden; empty = open network): "
    stty -echo 2>/dev/null
    read PASS
    stty echo 2>/dev/null
    echo
fi

# --- find the net80211 device -------------------------------------------------
WLDEV="${WLDEV:-}"
if [ -z "$WLDEV" ]; then
    WLDEV="$(sysctl -n net.wlan.devices 2>/dev/null | tr ' ' '\n' | grep '^bcm4313' | head -1)"
fi
[ -n "$WLDEV" ] || {
    echo "ERROR: no bcm4313* device in: $(sysctl -n net.wlan.devices 2>/dev/null)" >&2
    echo "       Attach failed; check: dmesg | tail -20   (or: sh diag.sh)" >&2
    exit 1
}
echo "==> using wireless device: $WLDEV"

# --- (re)create the wlan interface --------------------------------------------
echo "==> preparing $WLANIF over $WLDEV"
ifconfig "$WLANIF" destroy 2>/dev/null   # drop a stale copy, if any
ifconfig "$WLANIF" create wlandev "$WLDEV" || {
    echo "ERROR: could not create $WLANIF over $WLDEV" >&2
    exit 1
}
ifconfig "$WLANIF" up || {
    echo "ERROR: could not bring $WLANIF up" >&2
    echo "       Diagnose with: dmesg | tail -30" >&2
    exit 1
}

# --- join the network ---------------------------------------------------------
if [ -n "$WPA_CONF" ]; then
    [ -f "$WPA_CONF" ] || {
        echo "ERROR: wpa_supplicant config not found: $WPA_CONF" >&2
        exit 1
    }
    echo "==> joining via wpa_supplicant (-c $WPA_CONF)"
    ifconfig "$WLANIF" up
    wpa_supplicant -B -i "$WLANIF" -c "$WPA_CONF" || {
        echo "ERROR: wpa_supplicant failed to start." >&2
        echo "       Check the config:  wpa_supplicant -i $WLANIF -c $WPA_CONF -dd" >&2
        exit 1
    }
elif [ -n "$PASS" ]; then
    echo "==> joining SSID '$SSID' with wpakey (WPA2-PSK)"
    ifconfig "$WLANIF" ssid "$SSID" wpakey "$PASS" up || {
        echo "ERROR: could not set ssid/wpakey. Diagnose: dmesg | tail -30" >&2
        exit 1
    }
else
    echo "==> joining OPEN network '$SSID'"
    ifconfig "$WLANIF" ssid "$SSID" up || {
        echo "ERROR: could not set ssid. Diagnose: dmesg | tail -30" >&2
        exit 1
    }
fi

# --- wait for association -------------------------------------------------------
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
if ! ifconfig "$WLANIF" | grep -q 'status: associated'; then
    echo "ERROR: no association after ${TIMEOUT}s." >&2
    echo "       Status: $(ifconfig "$WLANIF" | grep 'status:')" >&2
    if [ -n "$WPA_CONF" ]; then
        echo "       Check with:  wpa_cli -i $WLANIF status" >&2
    fi
    echo "       Also: dmesg | tail -30" >&2
    exit 1
fi

# --- DHCP -----------------------------------------------------------------------
echo "==> requesting an address (dhclient $WLANIF)"
dhclient "$WLANIF" 2>/dev/null
sleep 3

ip="$(ifconfig "$WLANIF" | awk '/inet /{print $2; exit}')"
if [ -n "$ip" ]; then
    echo "==> $WLANIF has address: $ip"
else
    echo "WARNING: no inet address yet -- DHCP may still be running." >&2
    echo "         Re-check with:  ifconfig $WLANIF inet" >&2
fi

gw="$(netstat -rn 2>/dev/null | awk '$1=="default"{print $2; exit}')"
[ -n "$gw" ] && echo "==> default route: $gw"

# --- optional ping test ---------------------------------------------------------
if [ "$PING" = "1" ]; then
    echo "==> ping test (8.8.8.8, 3 packets)"
    ping -c 3 -t 5 8.8.8.8
fi

# --- optional boot persistence --------------------------------------------------
if [ "$MODE" = "boot" ]; then
    rcconf="/etc/rc.conf"
    wpaconf="/etc/wpa_supplicant.conf"

    # A boot-time join always goes through wpa_supplicant (rc 'WPA' keyword
    # does not accept inline keys), so generate the config when the user
    # supplied credentials inline just now.
    needs_wpa=0
    if [ -z "$WPA_CONF" ]; then
        [ -f "$wpaconf" ] && grep -q "ssid=\"$SSID\"" "$wpaconf" 2>/dev/null || needs_wpa=1
    fi
    if [ "$needs_wpa" = "1" ]; then
        if command -v wpa_passphrase >/dev/null 2>&1 && [ -n "$PASS" ]; then
            echo "==> writing $wpaconf (generated with wpa_passphrase)"
            cat > "$wpaconf" <<EOF
# Generated by config.sh for if_bcm4313 -- SSID: $SSID
ctrl_interface=/var/run/wpa_supplicant
network={
EOF
            wpa_passphrase "$SSID" "$PASS" >> "$wpaconf" 2>/dev/null
            echo "}" >> "$wpaconf"
            chmod 600 "$wpaconf"
        elif [ -n "$PASS" ]; then
            echo "WARNING: wpa_passphrase not found -- write $wpaconf by hand" >&2
            echo "         (see the wpa_supplicant block in INSTALL.md)." >&2
        fi
    fi

    append_rc() {  # append_rc KEY VALUE  ->  key="value" if absent
        if grep -q "^$1=" "$rcconf" 2>/dev/null; then
            echo "==> $rcconf already has $1"
        else
            echo "==> appending $1=\"$2\" to $rcconf"
            echo "$1=\"$2\"" >> "$rcconf"
        fi
    }

    append_rc "wlans_if_${WLDEV}" "$WLANIF"
    append_rc "ifconfig_${WLANIF}" "WPA DHCP"
    append_rc "wpa_supplicant_enable" "YES"

    echo
    echo "Boot persistence is set up. Recheck with:"
    echo "    service netif restart && service wpa_supplicant restart"
    echo "    ifconfig $WLANIF   # expect status: associated + an inet line"
fi

echo
echo "Done. Quick checks:  ifconfig $WLANIF   |   ping -c3 8.8.8.8"