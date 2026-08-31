#!/usr/bin/perl
#
# gen_phytbl.pl -- extract the BCM4313 LCN-PHY tuning tables verbatim from the
# bundled Linux brcmsmac sources into bcm4313_phytbl_lcn.h.
#
# The tables are hardware RF tuning data reproduced byte-for-byte from
# brcmsmac/phy/phytbl_lcn.c and brcmsmac/phy/phy_lcn.c (ISC licensed), the
# same way gen_lcntab.pl already extracts the switch-control and RX-gain
# tables into bcm4313_lcntab.h.  Tables already present in bcm4313_lcntab.h
# (the 4313 switch-control variants and the rev0 RX-gain set) are skipped
# here to avoid duplicate definitions.
#
# Usage: perl gen_phytbl.pl > bcm4313_phytbl_lcn.h
#
use strict;
use warnings;

my $SRC_PHYTBL = "brcmsmac/phy/phytbl_lcn.c";
my $SRC_PHYLCN = "brcmsmac/phy/phy_lcn.c";

# Tables to skip -- already defined in bcm4313_lcntab.h.
my %skip = map { $_ => 1 } qw(
    dot11lcn_sw_ctrl_tbl_4313_rev0
    dot11lcn_sw_ctrl_tbl_4313_epa_rev0
    dot11lcn_sw_ctrl_tbl_4313_ipa_rev0_combo
    dot11lcn_sw_ctrl_tbl_4313_epa_rev0_combo
    dot11lcn_sw_ctrl_tbl_4313_bt_epa_p250_rev0
    dot11lcn_gain_tbl_rev0
    dot11lcn_aux_gain_idx_tbl_rev0
    dot11lcn_gain_idx_tbl_rev0
    dot11lcn_gain_val_tbl_rev0
);

# Tables to extract, in emission order.  The phytbl_info array and the TX
# gain tables come from phytbl_lcn.c; the rest from phy_lcn.c.
my @tbl_phytbl = qw(
    dot11lcn_min_sig_sq_tbl_rev0
    dot11lcn_noise_scale_tbl_rev0
    dot11lcn_fltr_ctrl_tbl_rev0
    dot11lcn_ps_ctrl_tbl_rev0
    dot11lcn_sw_ctrl_tbl_rev0
    dot11lcn_nf_table_rev0
    dot11lcn_spur_tbl_rev0
    dot11lcn_unsup_mcs_tbl_rev0
    dot11lcn_iq_local_tbl_rev0
    dot11lcn_papd_compdelta_tbl_rev0
    dot11lcnphytbl_info_rev0
    dot11lcnphy_2GHz_gaintable_rev0
    dot11lcnphy_2GHz_extPA_gaintable_rev0
);

my @tbl_phylcn = qw(
    lcnphy_sfo_cfg
    chan_info_2064_lcnphy
    lcnphy_radio_regs_2064
    lcnphy_23bitgaincode_table
    lcnphy_gain_table
    lcnphy_gain_index_offset_for_rssi
    LCNPHY_txdigfiltcoeffs_cck
    LCNPHY_txdigfiltcoeffs_ofdm
    tbl_iqcal_gainparams_lcnphy_2G
    tbl_iqcal_gainparams_lcnphy
    iqcal_gainparams_numgains_lcnphy
    lcnphy_iqcal_loft_gainladder
    lcnphy_iqcal_ir_gainladder
    lcnphy_spb_tone_3750
    iqlo_loopback_rf_regs
    tempsense_phy_regs
    rxiq_cal_rf_reg
);

sub slurp {
    my ($path) = @_;
    open(my $fh, "<", $path) or die "cannot open $path: $!\n";
    local $/;
    my $text = <$fh>;
    close($fh);
    return $text;
}

sub extract_table {
    my ($text, $name) = @_;
    # Match:  [static] const [struct TYPE] TYPE NAME[SIZE] = {
    # Type may be a plain scalar, a struct, or a typedef, and the declarator
    # may be a pointer.  Dimensions may be literals or constant expressions
    # like [LCNPHY_NUM_...] and may span lines (e.g. [X][Y + 1] = {).
    my $re = qr/(?:static\s+)?const\s+(?:(?:struct\s+)?\w+\s+)(?:\*\s*)?\Q$name\E(\s*\[[^\]]*\])*\s*=\s*\{/;
    if ($text =~ $re) {
        my $start = $-[0];
        # Find the opening brace, then brace-match to the terminating "};"
        my $brace = index($text, "{", $start);
        die "no brace for $name\n" if $brace < 0;
        my $depth = 0;
        my $i = $brace;
        my $n = length($text);
        my $in_str = 0;
        for (; $i < $n; $i++) {
            my $c = substr($text, $i, 1);
            if ($in_str) {
                $in_str = 0 if $c eq '"' && substr($text, $i-1, 1) ne "\\";
                next;
            }
            if ($c eq '"') { $in_str = 1; next; }
            if ($c eq '{') { $depth++; }
            elsif ($c eq '}') {
                $depth--;
                if ($depth == 0) {
                    # include the trailing ';'
                    $i++;
                    last;
                }
            }
        }
        die "unbalanced braces for $name\n" if $depth != 0;
        my $decl = substr($text, $start, $i - $start + 1);
        # strip the leading "static "
        $decl =~ s/^static\s+//;
        # strip trailing whitespace / comments that ride along
        $decl =~ s/\s+$//;
        return $decl . "\n";
    }
    return undef;
}

# Translate Linux kernel types to FreeBSD sys/types.h spellings.  The table
# bodies are pure numeric data (with the odd ARRAY_SIZE() / RADIO_2064_REG*
# token), so only declaration type words are affected.
sub translate_types {
    my ($decl) = @_;
    $decl =~ s/\bstruct phytbl_info\b/struct bcm4313_phytbl/g;
    $decl =~ s/\bu8\b/uint8_t/g;
    $decl =~ s/\bu16\b/uint16_t/g;
    $decl =~ s/\bu32\b/uint32_t/g;
    $decl =~ s/\bs8\b/int8_t/g;
    $decl =~ s/\bs16\b/int16_t/g;
    $decl =~ s/\bs32\b/int32_t/g;
    return $decl;
}

sub emit {
    my ($text, $names, $fh) = @_;
    for my $name (@$names) {
        next if $skip{$name};
        my $decl = extract_table($text, $name);
        die "table $name not found\n" unless defined $decl;
        print $fh translate_types($decl), "\n";
    }
}

my $t1 = slurp($SRC_PHYTBL);
my $t2 = slurp($SRC_PHYLCN);

# Collect every RADIO_2064_REGxxx token referenced by the extracted tables so
# the header can define the needed register numbers (they are plain hex
# values from brcmsmac/phy/phy_radio.h).
my %radio_regs;
for my $name (@tbl_phytbl, @tbl_phylcn) {
    next if $skip{$name};
    my $decl = extract_table($t1, $name);
    $decl = extract_table($t2, $name) unless defined $decl;
    while (defined $decl && $decl =~ /RADIO_2064_REG([0-9A-Fa-f]+)/g) {
        $radio_regs{uc($1)} = 1;
    }
}

# Collect the LCNPHY_NUM_* macros used as array dimensions in the extracted
# declarations (they come verbatim from phy_lcn.c).
my %num_defs;
for my $name (@tbl_phylcn) {
    next if $skip{$name};
    my $decl = extract_table($t2, $name);
    while (defined $decl && $decl =~ /\b(LCNPHY_NUM_[A-Z_]+)\b/g) {
        my $m = $1;
        if ($t2 =~ /^#define\s+\Q$m\E\s+(\d+)\s*$/m) {
            $num_defs{$m} = $1;
        }
    }
}

my $out;
open(my $fh, ">", \$out) or die;

print $fh <<'EOF';
/*-
 * bcm4313_phytbl_lcn.h -- BCM4313 LCN-PHY tuning tables, part 2.
 *
 * GENERATED FILE -- do not edit.  Regenerate with:
 *	perl gen_phytbl.pl > bcm4313_phytbl_lcn.h
 *
 * These arrays are extracted VERBATIM from the Linux brcmsmac driver
 * (drivers/net/wireless/broadcom/brcm80211/brcmsmac/phy/phytbl_lcn.c and
 * phy/phy_lcn.c), which are distributed under the ISC license.  They are
 * hardware RF tuning data with no licensing-independent source, so they are
 * reproduced byte-for-byte rather than hand-retyped.
 *
 * Tables that live in bcm4313_lcntab.h (the 4313 switch-control variants and
 * the rev0 RX-gain set) are not duplicated here; the baseband-init table
 * descriptor array (dot11lcnphytbl_info_rev0) references them by name.
 *
 * $FreeBSD$
 */
#ifndef	_BCM4313_PHYTBL_LCN_H_
#define	_BCM4313_PHYTBL_LCN_H_

#include <sys/types.h>
#include <sys/param.h>

#include "bcm4313_lcntab.h"	/* struct bcm4313_phytbl + RX-gain tables */

/* brcmsmac uses the Linux ARRAY_SIZE(); FreeBSD names it nitems(). */
#ifndef	ARRAY_SIZE
#define	ARRAY_SIZE(a)	nitems(a)
#endif

/* LCN-PHY 2.4GHz TX gain table entry (phytbl_lcn.h). */
struct lcnphy_tx_gain_tbl_entry {
	uint8_t	gm;
	uint8_t	pga;
	uint8_t	pad;
	uint8_t	dac;
	uint8_t	bb_mult;
};

/* Radio 2064 per-channel tuning parameters (phy_lcn.c). */
struct chan_info_2064_lcnphy {
	uint32_t	chan;
	uint32_t	freq;
	uint8_t	logen_buftune;
	uint8_t	logen_rccr_tx;
	uint8_t	txrf_mix_tune_ctrl;
	uint8_t	pa_input_tune_g;
	uint8_t	logen_rccr_rx;
	uint8_t	pa_rxrf_lna1_freq_tune;
	uint8_t	pa_rxrf_lna2_freq_tune;
	uint8_t	rxrf_rxrf_spare1;
};

/* One entry of the radio 2064 init register list (phy_lcn.c). */
struct lcnphy_radio_regs {
	uint16_t	address;
	uint8_t	init_a;
	uint8_t	init_g;
	uint8_t	do_init_a;
	uint8_t	do_init_g;
};

/* SFO center-frequency pair (phy_lcn.c). */
struct lcnphy_sfo_cfg {
	uint16_t	ptcentreTs20;
	uint16_t	ptcentreFactor;
};

/* Sample-playback calibration tone (phy_lcn.c). */
struct lcnphy_spb_tone {
	int16_t	re;
	int16_t	im;
};

/* TX IQ-calibration gain ladder entry (phy_lcn.c). */
typedef uint16_t iqcal_gain_params_lcnphy[9];

/* Radio 2064 register numbers (all of 0x000-0x130, values from
 * brcmsmac/phy/phy_radio.h). */
EOF
for my $r (0x000 .. 0x130) {
    printf $fh "#define\tRADIO_2064_REG%03X\t0x%x\n", $r, $r;
}
if (keys %num_defs) {
    print $fh "\n/* Array-dimension constants used by the tables below (from phy_lcn.c). */\n";
    for my $m (sort keys %num_defs) {
        print $fh "#define\t$m\t$num_defs{$m}\n";
    }
}
print $fh "\n";

emit($t1, \@tbl_phytbl, $fh);
print $fh "\n";
emit($t2, \@tbl_phylcn, $fh);

print $fh <<'EOF';

#endif /* _BCM4313_PHYTBL_LCN_H_ */
EOF

print $out;
