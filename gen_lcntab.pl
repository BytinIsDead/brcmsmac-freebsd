#!/usr/bin/perl
# Extract the real BCM4313 LCN-PHY tables verbatim from the downloaded
# brcmsmac phy/phytbl_lcn.c and emit them as a C header with FreeBSD types.
use strict; use warnings;
my $src = "brcmsmac/phy/phytbl_lcn.c";
open my $fh, "<", $src or die $!;
local $/; my $d = <$fh>;

my @tables = (
  "dot11lcn_sw_ctrl_tbl_4313_epa_rev0_combo",
  "dot11lcn_sw_ctrl_tbl_4313_bt_epa_p250_rev0",
  "dot11lcn_sw_ctrl_tbl_4313_epa_rev0",
  "dot11lcn_sw_ctrl_tbl_4313_rev0",
  "dot11lcn_sw_ctrl_tbl_4313_ipa_rev0_combo",
  "dot11lcn_gain_tbl_2G",
  "dot11lcn_aux_gain_idx_tbl_2G",
  "dot11lcn_gain_idx_tbl_2G",
  "dot11lcn_gain_val_tbl_2G",
  "dot11lcn_gain_tbl_extlna_2G",
  "dot11lcn_aux_gain_idx_tbl_extlna_2G",
  "dot11lcn_gain_idx_tbl_extlna_2G",
  "dot11lcn_gain_val_tbl_extlna_2G",
  # BCM4313 is LCN rev 1: the RX-gain tables actually used are the _rev0 set.
  "dot11lcn_gain_tbl_rev0",
  "dot11lcn_aux_gain_idx_tbl_rev0",
  "dot11lcn_gain_idx_tbl_rev0",
  "dot11lcn_gain_val_tbl_rev0",
);

sub extract {
  my ($name) = @_;
  if ($d =~ /(static\s+const\s+u(?:8|16|32)\s+${name}\[\]\s*=\s*\{.*?\};)/s) {
    return $1;
  }
  die "cannot find $name\n";
}

open my $out, ">", "bcm4313_lcntab.h" or die $!;
print $out <<'HDR';
/*-
 * bcm4313_lcntab.h -- BCM4313 LCN-PHY tuning tables.
 *
 * These arrays are extracted VERBATIM from the Linux brcmsmac driver
 * (drivers/net/wireless/broadcom/brcm80211/brcmsmac/phy/phytbl_lcn.c),
 * which is distributed under the ISC license.  They are hardware RF tuning
 * data (RX-gain index tables and the BCM4313 antenna/PA switch-control
 * tables) with no licensing-independent source, so they are reproduced
 * here byte-for-byte rather than hand-retyped.
 *
 * Only BCM4313 materials are kept: the dot11lcn_sw_ctrl_tbl_4313_* board
 * variants and the 2.4GHz (2G) RX-gain tables, including the extlna (IPA)
 * variants selected from SPROM.
 *
 * $FreeBSD$
 */
#ifndef	_BCM4313_LCNTAB_H_
#define	_BCM4313_LCNTAB_H_

#include <sys/types.h>

/* One LCN-PHY indirect-table descriptor (parses phytbl_info). */
struct bcm4313_phytbl {
	const void	*tbl_ptr;
	uint32_t	tbl_len;
	uint32_t	tbl_id;
	uint32_t	tbl_offset;
	uint32_t	tbl_width;
};

/* LCNPHY_TBL_ID_* (brcmsmac phy/phy_lcn.c). */
#define	BCM4313_LCN_TBL_IQLOCAL		0x00
#define	BCM4313_LCN_TBL_RFSEQ		0x08
#define	BCM4313_LCN_TBL_GAIN_IDX	0x0d
#define	BCM4313_LCN_TBL_SW_CTRL		0x0f
#define	BCM4313_LCN_TBL_GAIN_TBL	0x12
#define	BCM4313_LCN_TBL_SPUR		0x14
#define	BCM4313_LCN_TBL_SAMPLEPLAY	0x15
#define	BCM4313_LCN_TBL_SAMPLEPLAY1	0x16

HDR

for my $t (@tables) {
  my $blk = extract($t);
  $blk =~ s/\bu(8|16|32)\b/uint$1_t/g;
  # make non-static so calibration can reference them from if_bcm4313.c
  $blk =~ s/^static const /const /;
  print $out $blk, "\n";
}

# sw_ctrl phytbl wrappers (real, from the same file)
my %sw = (
  sw_ctrl_4313_plain   => ["dot11lcn_sw_ctrl_tbl_4313_rev0"],
  sw_ctrl_4313_bt_ipa  => ["dot11lcn_sw_ctrl_tbl_4313_ipa_rev0_combo"],
  sw_ctrl_4313_epa     => ["dot11lcn_sw_ctrl_tbl_4313_epa_rev0"],
  sw_ctrl_4313_bt_epa  => ["dot11lcn_sw_ctrl_tbl_4313_epa_rev0_combo"],
  sw_ctrl_4313_bt_epa_p250 => ["dot11lcn_sw_ctrl_tbl_4313_bt_epa_p250_rev0"],
);
print $out "\n/* BCM4313 switch-control table descriptors (tbl 0x0f, 16-bit). */\n";
for my $k (keys %sw) {
  my ($arr) = @{$sw{$k}};
  my $nm = "bcm4313_${k}_info";
  print $out "static const struct bcm4313_phytbl $nm = {\n";
  print $out "\t&$arr,\n\tnitems($arr), BCM4313_LCN_TBL_SW_CTRL, 0, 16\n};\n\n";
}

# 2G / extlna-2G rx-gain wrappers (gain tbl 0x12/18, aux 0x14? -> real IDs below)
sub gainw {
  my ($name,$gt,$aux,$idx,$val) = @_;
  print $out "static const struct bcm4313_phytbl bcm4313_${name}_gain_info[] = {\n";
  print $out "\t{ &$gt, nitems($gt), BCM4313_LCN_TBL_GAIN_TBL, 0, 32 },\n";
  if ($aux) { print $out "\t{ &$aux, nitems($aux), 0x0e, 0, 16 },\n"; }
  print $out "\t{ &$idx, nitems($idx), BCM4313_LCN_TBL_GAIN_IDX, 0, 32 },\n";
  if ($val) { print $out "\t{ &$val, nitems($val), 0x11, 0, 8 },\n"; }
  print $out "};\n\n";
}
print $out "/* RX-gain table descriptors. */\n";
gainw("rxgain_rev0", "dot11lcn_gain_tbl_rev0", "dot11lcn_aux_gain_idx_tbl_rev0", "dot11lcn_gain_idx_tbl_rev0", "dot11lcn_gain_val_tbl_rev0");
gainw("rxgain_2G",   "dot11lcn_gain_tbl_2G", "dot11lcn_aux_gain_idx_tbl_2G", "dot11lcn_gain_idx_tbl_2G", "dot11lcn_gain_val_tbl_2G");
gainw("rxgain_extlna_2G", "dot11lcn_gain_tbl_extlna_2G", "dot11lcn_aux_gain_idx_tbl_extlna_2G", "dot11lcn_gain_idx_tbl_extlna_2G", "dot11lcn_gain_val_tbl_extlna_2G");

print $out "\n#endif /* _BCM4313_LCNTAB_H_ */\n";
close $out;
print "wrote bcm4313_lcntab.h\n";